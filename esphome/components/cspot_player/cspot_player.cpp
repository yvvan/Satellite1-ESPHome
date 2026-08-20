#include "cspot_player.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "esphome/core/log.h"

#include <BellHTTPServer.h>
#include <BellLogger.h>
#include <BellTask.h>
#include <BellUtils.h>
#include <CSpotContext.h>
#include <LoginBlob.h>
#include <MDNSService.h>
#include <ShannonConnection.h>
#include <SpircHandler.h>
#include <TrackPlayer.h>
#include <civetweb.h>

#include "esphome/components/audio/audio.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "micro_mp3/mp3_decoder.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "nvs.h"

namespace esphome {
namespace cspot_player {

static const char *const TAG = "cspot_player";

namespace {

constexpr const char *NVS_NAMESPACE = "cspot";
constexpr const char *NVS_KEY_AUTH = "authblob";

// URL playback. The compressed input is read in chunks this size; bigger buys nothing because
// the decoder consumes at its own pace and the speaker's ring buffer is the real shock absorber.
constexpr size_t URL_INPUT_CHUNK = 4 * 1024;
constexpr size_t URL_INPUT_CAPACITY = 16 * 1024;
constexpr uint32_t URL_TASK_STACK_SIZE = 6 * 1024;
// Below the audio-capture path on purpose: decoding flat out at 4 starved the wake-word ring
// buffer ("Not enough free bytes ... Resetting") within a second of playback starting.
constexpr UBaseType_t URL_TASK_PRIORITY = 3;
constexpr TickType_t URL_SPEAKER_WAIT = pdMS_TO_TICKS(50);
constexpr int64_t URL_SPEAKER_STALL_TIMEOUT_US = 5 * 1000 * 1000;

std::string load_credentials() {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
    return "";
  size_t len = 0;
  if (nvs_get_str(handle, NVS_KEY_AUTH, nullptr, &len) != ESP_OK || len == 0) {
    nvs_close(handle);
    return "";
  }
  std::string json(len, '\0');
  esp_err_t err = nvs_get_str(handle, NVS_KEY_AUTH, json.data(), &len);
  nvs_close(handle);
  if (err != ESP_OK)
    return "";
  json.resize(len - 1);  // drop the trailing NUL nvs_get_str includes
  return json;
}

void save_credentials(const std::string &json) {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
    ESP_LOGE(TAG, "Cannot open NVS to store credentials");
    return;
  }
  if (nvs_set_str(handle, NVS_KEY_AUTH, json.c_str()) == ESP_OK) {
    nvs_commit(handle);
    ESP_LOGI(TAG, "Stored Spotify credentials (%u bytes)", (unsigned) json.size());
  } else {
    ESP_LOGE(TAG, "Failed to store Spotify credentials");
  }
  nvs_close(handle);
}

void erase_credentials() {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
    return;
  nvs_erase_key(handle, NVS_KEY_AUTH);
  nvs_commit(handle);
  nvs_close(handle);
}

bool can_resolve(const char *host) {
  struct addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo *result = nullptr;
  int err = getaddrinfo(host, "443", &hints, &result);
  if (result != nullptr)
    freeaddrinfo(result);
  return err == 0;
}

/**
 * The connection layer aborts on failure (exceptions-free build), so never enter it
 * until DNS provably works. Waits with retries; installs a public fallback resolver
 * after the second failure (DHCP may have handed us nothing usable).
 */
bool wait_for_dns() {
  const char *probe_host = "apresolve.spotify.com";
  for (int attempt = 0; attempt < 60; attempt++) {
    const ip_addr_t *dns0 = dns_getserver(0);
    if (can_resolve(probe_host))
      return true;
    ESP_LOGW(TAG, "DNS resolve of %s failed (attempt %d, DNS0=%s)", probe_host, attempt + 1,
             dns0 != nullptr ? ipaddr_ntoa(dns0) : "none");
    if (attempt == 1) {
      ip_addr_t fallback;
      IP_ADDR4(&fallback, 1, 1, 1, 1);
      dns_setserver(0, &fallback);
      ESP_LOGW(TAG, "Installed fallback DNS 1.1.1.1");
    }
    BELL_SLEEP_MS(5000);
  }
  ESP_LOGE(TAG, "DNS never became usable; giving up this session round");
  return false;
}

void log_heap(const char *when) {
  ESP_LOGI(TAG, "[heap @ %s] internal free=%u largest=%u | psram free=%u", when,
           (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned) heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

}  // namespace

class CSpotPlayer::Runner : public bell::Task {
 public:
  Runner(std::string device_name, uint16_t http_port, speaker::Speaker *media_speaker,
         const std::atomic<bool> *url_playing)
      : bell::Task("cspot_runner", 32 * 1024, 0, 1),
        device_name_(std::move(device_name)),
        http_port_(http_port),
        media_speaker_(media_speaker),
        url_playing_(url_playing) {
    startTask();
  }

  void set_paused(bool paused) {
    auto handler = this->current_handler_();
    if (handler == nullptr) {
      ESP_LOGW(TAG, "No active Spotify session; ignoring %s", paused ? "pause" : "resume");
      return;
    }
    handler->setPause(paused);
  }

  // Whether a transport command has anything to act on — the same predicate set_paused/next_track
  // gate on internally, exposed so a caller can REPORT a no-op instead of silently making one.
  bool has_session() { return this->current_handler_() != nullptr; }

  void next_track() {
    auto handler = this->current_handler_();
    if (handler == nullptr) {
      ESP_LOGW(TAG, "No active Spotify session; ignoring next");
      return;
    }
    handler->nextSong();
  }

  void runTask() override {
    log_heap("runner start");
    // Exceptions-free build: unrecoverable cspot failures abort (reboot); a normal
    // return here (e.g. stale credentials) falls through to a delayed retry.
    while (true) {
      this->run_once_();
      log_heap("session ended");
      BELL_SLEEP_MS(5000);
    }
  }

 private:
  /** One full session: obtain credentials (NVS or zeroconf), authenticate, pump packets. */
  void run_once_() {
    // Let the post-connect Wi-Fi/lwip allocation burst settle so the next probes
    // measure cspot, not the network stack.
    BELL_SLEEP_MS(5000);
    log_heap("settled before blob");
    auto blob = std::make_shared<cspot::LoginBlob>(device_name_);
    log_heap("login blob created");

    std::string stored = load_credentials();
    bool from_zeroconf = stored.empty();
    if (!from_zeroconf) {
      ESP_LOGI(TAG, "Using stored Spotify credentials for '%s'", device_name_.c_str());
      blob->loadJson(stored);
    } else {
      this->run_zeroconf_(blob);  // blocks until the Spotify app hands over credentials
    }

    if (!wait_for_dns())
      return;

    auto ctx = cspot::Context::createFromBlob(blob);
    ESP_LOGI(TAG, "Connecting to Spotify AP...");
    if (!ctx->session->connectWithRandomAp()) {
      ESP_LOGW(TAG, "AP connection failed; retrying shortly");
      return;
    }
    auto token = ctx->session->authenticate(blob);
    if (token.empty()) {
      ESP_LOGE(TAG, "Spotify authentication failed");
      // Only a verdict from a live connection means the stored credentials are
      // stale; a connection lost mid-auth says nothing about them, and erasing
      // would force a needless re-pairing from the phone.
      auto shan_conn = ctx->session->shanConnection();
      bool connection_lost = shan_conn == nullptr || shan_conn->isDisconnected();
      if (!from_zeroconf && !connection_lost) {
        // Stored credentials went stale — drop them so the next round re-pairs.
        erase_credentials();
      }
      return;
    }
    ESP_LOGI(TAG, "Spotify authentication OK (user: %s)", blob->getUserName().c_str());
    log_heap("authenticated");
    if (from_zeroconf)
      save_credentials(ctx->getCredentialsJson());

    ctx->session->startTask();
    auto handler = std::make_shared<cspot::SpircHandler>(ctx);
    handler->subscribeToMercury();
    {
      std::lock_guard<std::mutex> lock(this->handler_mutex_);
      this->active_handler_ = handler;
    }

    // cspot decodes Ogg Vorbis to 44.1 kHz / 16-bit / stereo PCM and pushes it here. Feed it
    // into the ESPHome media speaker (a resampler -> mixer -> I2S chain). play() returns the
    // bytes it accepted; returning that count gives cspot the backpressure it expects (it
    // sleeps and retries the remainder), so a full ring buffer throttles the decoder instead
    // of overflowing. A short ticks_to_wait keeps this off the cspot player task for too long.
    //
    // Do NOT start the media speaker here: cspot authenticates on every boot, and an
    // idle-but-started media mixer input silences the voice announcement channel (wake-word
    // replies never play). Start lazily on the first decoded PCM instead (below).
    handler->getTrackPlayer()->setDataCallback(
        [this](uint8_t *data, size_t bytes, std::string_view track_id) -> size_t {
          // Upstream-starvation detector: during playback this callback fires
          // continuously (backpressure retries every few ms), so a long entry-to-
          // entry gap means the decoder/CDN side stalled — the audible lags.
          int64_t now_us = esp_timer_get_time();
          if (this->last_data_cb_us_ != 0 && now_us - this->last_data_cb_us_ > 250000) {
            ESP_LOGW(TAG, "PCM gap: %d ms without decoded data",
                     (int) ((now_us - this->last_data_cb_us_) / 1000));
            log_heap("pcm gap");
          }
          this->last_data_cb_us_ = now_us;
          this->streamed_bytes_ += bytes;
          if (this->streamed_bytes_ - this->last_report_ >= 1024 * 1024) {
            this->last_report_ = this->streamed_bytes_;
            ESP_LOGI(TAG, "Streaming: %u MB decoded", (unsigned) (this->streamed_bytes_ >> 20));
            log_heap("streaming");
          }
          if (this->media_speaker_ == nullptr)
            return bytes;
          // A URL owns the speaker: swallow Spotify's audio instead of mixing it into ours.
          // Reporting the bytes as accepted (rather than 0) matters — returning 0 would park
          // cspot's player task on a stream that is never going to be heard, and it would
          // resume mid-track when the URL finishes.
          if (this->url_playing_ != nullptr && this->url_playing_->load())
            return bytes;
          this->ensure_stream_started_();
          return this->media_speaker_->play(data, bytes, pdMS_TO_TICKS(20));
        });

    auto *self = this;
    handler->setEventHandler([self, handler](std::unique_ptr<cspot::SpircHandler::Event> event) {
      switch (event->eventType) {
        case cspot::SpircHandler::EventType::TRACK_INFO: {
          auto &info = std::get<cspot::TrackInfo>(event->data);
          ESP_LOGI(TAG, "Track: %s — %s", info.artist.c_str(), info.name.c_str());
          break;
        }
        case cspot::SpircHandler::EventType::PLAY_PAUSE: {
          bool paused = std::get<bool>(event->data);
          ESP_LOGI(TAG, "Play/pause: %s", paused ? "paused" : "playing");
          // Not while a URL owns the speaker. Taking a URL pauses Spotify, and that pause comes
          // back here as an event — applying it would pause the stream we just started, which
          // then accepts nothing and the playback task waits on a speaker that never drains.
          if (self->media_speaker_ != nullptr && !self->url_owns_speaker_())
            self->media_speaker_->set_pause_state(paused);
          break;
        }
        case cspot::SpircHandler::EventType::VOLUME: {
          int volume = std::get<int>(event->data);  // 0..65535
          if (self->media_speaker_ != nullptr)
            self->media_speaker_->set_volume((float) volume / 65535.0f);
          break;
        }
        case cspot::SpircHandler::EventType::FLUSH:
        case cspot::SpircHandler::EventType::SEEK:
        case cspot::SpircHandler::EventType::PLAYBACK_START:
          // Drop buffered audio so the new position starts cleanly — but only if the buffered
          // audio is Spotify's. Mid-URL these arrive as an echo of the pause we asked for.
          if (!self->url_owns_speaker_())
            self->restart_stream_();
          break;
        case cspot::SpircHandler::EventType::DISC:
          ESP_LOGI(TAG, "User disconnected the device");
          if (self->media_speaker_ != nullptr && !self->url_owns_speaker_())
            self->media_speaker_->stop();
          self->stream_started_ = false;
          break;
        default:
          break;
      }
    });

    while (true) {
      ctx->session->handlePacket();
    }
  }

  /** Start the media speaker with cspot's fixed PCM format, once per playback. */
  void ensure_stream_started_() {
    if (media_speaker_ == nullptr || stream_started_)
      return;
    // cspot output is always 44.1 kHz, 16-bit, stereo (Spotify Ogg Vorbis).
    media_speaker_->set_audio_stream_info(audio::AudioStreamInfo(16, 2, 44100));
    media_speaker_->start();
    stream_started_ = true;
  }

  void restart_stream_() {
    if (media_speaker_ == nullptr)
      return;
    media_speaker_->stop();
    stream_started_ = false;
    ensure_stream_started_();
  }

  /** Advertise on mDNS and run the LAN credential hand-off until the app pairs us. */
  void run_zeroconf_(std::shared_ptr<cspot::LoginBlob> blob) {
    std::atomic<bool> got_blob{false};

    auto server = std::make_unique<bell::BellHTTPServer>(http_port_);
    server->registerGet("/spotify_info", [&server, blob](struct mg_connection *conn) {
      return server->makeJsonResponse(blob->buildZeroconfInfo());
    });
    server->registerPost("/spotify_info", [&server, blob, &got_blob](struct mg_connection *conn) {
      std::string body;
      auto *request_info = mg_get_request_info(conn);
      if (request_info->content_length > 0) {
        body.resize(request_info->content_length);
        mg_read(conn, body.data(), request_info->content_length);

        mg_header headers[10];
        int num = mg_split_form_urlencoded(body.data(), headers, 10);
        std::map<std::string, std::string> query;
        for (int i = 0; i < num; i++)
          query[headers[i].name] = headers[i].value;

        blob->loadZeroconfQuery(query);
        got_blob = true;
      }
      nlohmann::json response;
      response["status"] = 101;
      response["spotifyError"] = 0;
      response["statusString"] = "ERROR-OK";
      auto result = server->makeJsonResponse(response.dump());
      // Let civetweb flush the response before the server is torn down.
      BELL_SLEEP_MS(500);
      return result;
    });

    bell::MDNSService::registerService(blob->getDeviceName(), "_spotify-connect", "_tcp", "",
                                       http_port_,
                                       {{"VERSION", "1.0"}, {"CPath", "/spotify_info"}, {"Stack", "SP"}});
    ESP_LOGI(TAG, "Waiting for pairing: pick '%s' in the Spotify app (same Wi-Fi)",
             blob->getDeviceName().c_str());
    while (!got_blob) {
      BELL_SLEEP_MS(1000);
    }
    ESP_LOGI(TAG, "Received Spotify credentials via zeroconf");
  }

  std::shared_ptr<cspot::SpircHandler> current_handler_() {
    std::lock_guard<std::mutex> lock(this->handler_mutex_);
    return this->active_handler_;
  }

  bool url_owns_speaker_() const { return this->url_playing_ != nullptr && this->url_playing_->load(); }

  std::string device_name_;
  uint16_t http_port_;
  speaker::Speaker *media_speaker_;
  const std::atomic<bool> *url_playing_{nullptr};
  bool stream_started_{false};
  size_t streamed_bytes_{0};
  size_t last_report_{0};
  int64_t last_data_cb_us_{0};
  std::mutex handler_mutex_;
  std::shared_ptr<cspot::SpircHandler> active_handler_;
};

void CSpotPlayer::setup() {
  // bell logs through a global logger pointer that is null until installed — the
  // first BELL_LOG without this is a LoadProhibited crash (reference targets install
  // it in main()).
  bell::setDefaultLogger();

  // ESPHome (AFTER_CONNECTION) has Wi-Fi up and the IDF mdns component initialized
  // by now; bell's MDNSService only adds a service record to it.
  this->runner_ = new Runner(this->device_name_, this->http_port_, this->media_speaker_, &this->url_playing_);
}

void CSpotPlayer::dump_config() {
  ESP_LOGCONFIG(TAG, "CSpot player:");
  ESP_LOGCONFIG(TAG, "  Device name: %s", this->device_name_.c_str());
  ESP_LOGCONFIG(TAG, "  Zeroconf HTTP port: %u", this->http_port_);
}

void CSpotPlayer::set_paused(bool paused) {
  if (this->runner_ == nullptr)
    return;
  this->runner_->set_paused(paused);
}

bool CSpotPlayer::has_session() { return this->runner_ != nullptr && this->runner_->has_session(); }

void CSpotPlayer::next_track() {
  if (this->runner_ == nullptr)
    return;
  this->runner_->next_track();
}

bool CSpotPlayer::play_url(const std::string &url, const std::string &auth_token) {
  if (this->media_speaker_ == nullptr) {
    ESP_LOGW(TAG, "Cannot play a URL: no media speaker configured");
    return false;
  }
  if (url.empty()) {
    ESP_LOGW(TAG, "Cannot play an empty URL");
    return false;
  }
  if (this->url_task_handle_ != nullptr) {
    // A previous URL is still running (or draining). Ask it to stop and let the caller retry:
    // tearing it down from here would race its own use of the decoder and the speaker.
    ESP_LOGW(TAG, "A URL is already playing; stopping it, ignoring the new one");
    this->stop_url();
    return false;
  }

  // Take the media speaker away from Spotify before touching it. Pausing through cspot keeps
  // the Connect state honest, so the user's app shows paused rather than silently playing into
  // a stream nobody hears; stop() then drops whatever it had buffered.
  if (this->runner_ != nullptr)
    this->runner_->set_paused(true);
  this->media_speaker_->stop();

  this->url_ = url;
  this->url_auth_token_ = auth_token;
  this->url_stop_requested_.store(false);
  this->url_playing_.store(true);
  if (xTaskCreate(CSpotPlayer::url_task_, "cspot_url_play", URL_TASK_STACK_SIZE, (void *) this,
                  URL_TASK_PRIORITY, &this->url_task_handle_) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create the URL playback task");
    this->url_task_handle_ = nullptr;
    this->url_playing_.store(false);
    return false;
  }
  return true;
}

void CSpotPlayer::stop_url() {
  if (this->url_task_handle_ == nullptr)
    return;
  this->url_stop_requested_.store(true);
}

void CSpotPlayer::url_task_(void *param) {
  auto *self = static_cast<CSpotPlayer *>(param);
  self->run_url_playback_();
  // Published before the handle is cleared: `play_url` treats a non-null handle as "busy",
  // so clearing it last is what makes a new URL acceptable exactly once we are gone.
  self->url_playing_.store(false);
  TaskHandle_t handle = self->url_task_handle_;
  self->url_task_handle_ = nullptr;
  (void) handle;
  vTaskDelete(nullptr);
}

void CSpotPlayer::run_url_playback_() {
  ESP_LOGI(TAG, "URL playback starting: %s", this->url_.c_str());
  log_heap("url playback start");

  esp_http_client_config_t config = {};
  config.url = this->url_.c_str();
  config.timeout_ms = 10000;
  config.crt_bundle_attach = esp_crt_bundle_attach;  // ignored for plain http
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    ESP_LOGE(TAG, "URL playback: cannot init the http client");
    return;
  }
  // The gateway is on the public internet and hands a clip only to the device it was stored for, so
  // the fetch presents the same token this device authenticated its socket with. Without it the
  // request is a 401 — which is the point.
  if (!this->url_auth_token_.empty()) {
    esp_http_client_set_header(client, "X-Device-Token", this->url_auth_token_.c_str());
  } else {
    ESP_LOGW(TAG, "URL playback: no device token to present; the gateway will refuse this fetch");
  }

  bool speaker_started = false;
  // Both buffers live in PSRAM: internal RAM is the scarce one here (Wi-Fi, the wake-word
  // model and the cspot session all want it), and neither of these is touched by DMA.
  auto *input = static_cast<uint8_t *>(heap_caps_malloc(URL_INPUT_CAPACITY, MALLOC_CAP_SPIRAM));
  size_t output_capacity = 0;
  uint8_t *output = nullptr;
  size_t input_length = 0;
  auto decoder = std::make_unique<micro_mp3::Mp3Decoder>();

  auto cleanup = [&]() {
    if (speaker_started) {
      // finish() plays out what is buffered instead of cutting the last second off.
      this->media_speaker_->finish();
      while (this->media_speaker_->has_buffered_data() && !this->url_stop_requested_.load()) {
        vTaskDelay(pdMS_TO_TICKS(20));
      }
      this->media_speaker_->stop();
    }
    if (input != nullptr)
      heap_caps_free(input);
    if (output != nullptr)
      heap_caps_free(output);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
  };

  if (input == nullptr) {
    ESP_LOGE(TAG, "URL playback: cannot allocate the %u byte input buffer", (unsigned) URL_INPUT_CAPACITY);
    cleanup();
    return;
  }

  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "URL playback: open failed: %s", esp_err_to_name(err));
    cleanup();
    return;
  }
  int64_t content_length = esp_http_client_fetch_headers(client);
  int status = esp_http_client_get_status_code(client);
  if (status != 200) {
    ESP_LOGE(TAG, "URL playback: HTTP %d", status);
    cleanup();
    return;
  }
  ESP_LOGI(TAG, "URL playback: HTTP 200, content-length %lld", content_length);

  bool reader_done = false;
  size_t decoded_frames = 0;

  while (!this->url_stop_requested_.load()) {
    // Top up the compressed input. The decoder consumes from the front, so the remainder is
    // moved down rather than dropped — a partially consumed MP3 frame has to survive the read.
    if (!reader_done && input_length < URL_INPUT_CAPACITY) {
      int read = esp_http_client_read(client, reinterpret_cast<char *>(input + input_length),
                                     std::min(URL_INPUT_CHUNK, URL_INPUT_CAPACITY - input_length));
      if (read > 0) {
        input_length += (size_t) read;
      } else if (read == 0 && esp_http_client_is_complete_data_received(client)) {
        reader_done = true;
        ESP_LOGD(TAG, "URL playback: download complete");
      } else if (read < 0) {
        ESP_LOGE(TAG, "URL playback: read error");
        break;
      }
    }

    if (input_length == 0) {
      if (reader_done)
        break;
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }

    if (output == nullptr) {
      // The decoder documents one worst-case output size (MPEG1 stereo, 4608 bytes) and never
      // needs more, so this is allocated once and MP3_OUTPUT_BUFFER_TOO_SMALL below can only
      // mean the decoder's contract changed under us.
      output_capacity = decoder->get_min_output_buffer_bytes();
      output = static_cast<uint8_t *>(heap_caps_malloc(output_capacity, MALLOC_CAP_SPIRAM));
      if (output == nullptr) {
        ESP_LOGE(TAG, "URL playback: cannot allocate the %u byte output buffer", (unsigned) output_capacity);
        break;
      }
    }

    size_t consumed = 0;
    size_t samples = 0;
    micro_mp3::Mp3Result result =
        decoder->decode(input, input_length, output, output_capacity, consumed, samples);

    if (consumed > 0) {
      input_length -= consumed;
      if (input_length > 0)
        std::memmove(input, input + consumed, input_length);
    }

    if (result == micro_mp3::MP3_STREAM_INFO_READY) {
      ESP_LOGI(TAG, "URL playback: %u Hz, %u ch", (unsigned) decoder->get_sample_rate(),
               (unsigned) decoder->get_channels());
      // The mixer wants 48 kHz; the resampler in front of it converts whatever we announce,
      // exactly as it does for Spotify's 44.1 kHz. So announce the file's real rate.
      this->media_speaker_->set_audio_stream_info(
          audio::AudioStreamInfo(16, decoder->get_channels(), decoder->get_sample_rate()));
      this->media_speaker_->start();
      // stop() does not clear a pause, so a speaker Spotify left paused would swallow this
      // stream: it accepts nothing and never drains.
      this->media_speaker_->set_pause_state(false);
      speaker_started = true;
      continue;
    }
    if (result == micro_mp3::MP3_NEED_MORE_DATA) {
      if (reader_done && input_length == 0)
        break;
      if (consumed == 0 && reader_done)
        break;
      continue;
    }
    if (result == micro_mp3::MP3_DECODE_ERROR) {
      // One corrupt frame is not the end of a track; the decoder resynchronizes itself.
      ESP_LOGW(TAG, "URL playback: skipped a corrupt frame");
      continue;
    }
    if (result != micro_mp3::MP3_OK) {
      // MP3_INPUT_INVALID, MP3_ALLOCATION_FAILED, MP3_OUTPUT_BUFFER_TOO_SMALL: none recoverable.
      ESP_LOGE(TAG, "URL playback: decoder failed: %d", (int) result);
      break;
    }

    if (samples == 0)
      continue;
    if (!speaker_started) {
      // Should not happen (stream info precedes PCM), but writing to a speaker that was never
      // told its format plays the previous stream's settings — the bug this design avoids.
      ESP_LOGE(TAG, "URL playback: PCM before stream info; dropping the stream");
      break;
    }

    // Feed the speaker, respecting backpressure: play() takes what fits in the ring buffer and
    // the remainder has to be re-offered, or the gaps land in the middle of the music.
    size_t pcm_length = samples * decoder->get_channels() * sizeof(int16_t);
    size_t offset = 0;
    int64_t stalled_since_us = 0;
    while (offset < pcm_length && !this->url_stop_requested_.load()) {
      size_t written = this->media_speaker_->play(output + offset, pcm_length - offset, URL_SPEAKER_WAIT);
      if (written == 0) {
        // A full ring buffer is normal backpressure and clears within milliseconds. A speaker that
        // takes nothing for seconds is broken (paused, stopped underneath us, downstream gone), and
        // waiting on it is how a hang becomes indistinguishable from silence.
        int64_t now_us = esp_timer_get_time();
        if (stalled_since_us == 0) {
          stalled_since_us = now_us;
        } else if (now_us - stalled_since_us > URL_SPEAKER_STALL_TIMEOUT_US) {
          ESP_LOGE(TAG, "URL playback: the speaker accepted nothing for %d s; giving up",
                   (int) (URL_SPEAKER_STALL_TIMEOUT_US / 1000000));
          this->url_stop_requested_.store(true);
          break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
      stalled_since_us = 0;
      offset += written;
    }
    decoded_frames += samples;
  }

  ESP_LOGI(TAG, "URL playback finished: %u frames decoded%s", (unsigned) decoded_frames,
           this->url_stop_requested_.load() ? " (stopped)" : "");
  log_heap("url playback end");
  cleanup();
}

}  // namespace cspot_player
}  // namespace esphome
