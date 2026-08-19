#include "kineto_voice.h"

#ifdef USE_ESP32

#include <esp_crt_bundle.h>
#include <nvs.h>
#include <nvs_flash.h>

#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "esphome/core/version.h"

namespace esphome {
namespace kineto_voice {

static const char *const TAG = "kineto_voice";

// 16-bit 16 kHz mono outbound stream: 32000 bytes/s, keep ~500 ms of audio buffered.
static const size_t RING_BUFFER_SIZE = 16000;
static const size_t STREAM_CHUNK_SIZE = 1024;

static const size_t STREAM_TASK_STACK_SIZE = 4096;
static const UBaseType_t STREAM_TASK_PRIORITY = 3;

// Inbound reply audio. 256 KB is ~8 s of 16 kHz mono 16-bit speech — far more than the lead the
// gateway keeps, so a Wi-Fi stall is absorbed rather than heard. It lives in PSRAM.
static const size_t REPLY_BUFFER_SIZE = 256 * 1024;
static const size_t PLAYBACK_CHUNK_SIZE = 1024;
static const size_t PLAYBACK_TASK_STACK_SIZE = 4096;
static const UBaseType_t PLAYBACK_TASK_PRIORITY = 4;
// How much audio to hold before the first sample is played. Buys the amplifier time to wake and
// covers the jitter of the next few frames; the cost is that much added latency, so keep it small.
static const uint32_t REPLY_PREBUFFER_MS = 300;
// How long buffered audio may wait for its stream_start before it is dropped. Generous: the frame
// is only ever a few milliseconds behind, and dropping is better than playing at a guessed rate.
static const uint32_t FORMAT_WAIT_TIMEOUT_MS = 2000;
// How long to let the speaker finish stopping before announcing a new format. Bounded so a speaker
// that never reports stopped costs the reply 100 ms rather than the whole turn.
static const int SPEAKER_STOP_WAIT_STEPS = 10;

static const int WS_RECONNECT_TIMEOUT_MS = 5000;  // TODO(T4): exponential backoff on top of this
static const int WS_NETWORK_TIMEOUT_MS = 10000;
static const int WS_PING_INTERVAL_SEC = 10;
static const int WS_PINGPONG_TIMEOUT_SEC = 20;
static const uint32_t WS_LIVENESS_TIMEOUT_MS = 45000;  // 4+ missed gateway heartbeats
// How long a wake frame may go unanswered before the link counts as dead. The gateway replies
// within milliseconds on a healthy link; the slack is for a loaded Wi-Fi, not for a broken path.
static const uint32_t LISTEN_ACK_TIMEOUT_MS = 1500;
// While the stored identity is being refused, how often to try it again. Generous on purpose:
// the identity only becomes valid again through something slow (its backend coming back, the
// device row being restored), and every retry restarts the socket.
static const uint32_t AUTH_RETRY_INTERVAL_MS = 5 * 60 * 1000;
constexpr const char *NVS_NAMESPACE = "kineto";
constexpr const char *NVS_KEY_DEVICE_ID = "device_id";
constexpr const char *NVS_KEY_TOKEN = "auth_token";
static const int WS_SEND_TIMEOUT_TICKS = pdMS_TO_TICKS(2000);

void KinetoVoice::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Kineto Voice...");

  this->ring_buffer_ = ring_buffer::RingBuffer::create(RING_BUFFER_SIZE);
  if (this->ring_buffer_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate audio ring buffer");
    this->mark_failed();
    return;
  }

  this->microphone_->add_data_callback([this](const std::vector<uint8_t> &data) { this->on_mic_data_(data); });

  xTaskCreate(KinetoVoice::stream_task, "kineto_ws_stream", STREAM_TASK_STACK_SIZE, (void *) this,
              STREAM_TASK_PRIORITY, &this->stream_task_handle_);
  if (this->stream_task_handle_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create audio streaming task");
    this->mark_failed();
    return;
  }

  if (this->accepts_audio_stream_()) {
    this->reply_buffer_ = ring_buffer::RingBuffer::create(REPLY_BUFFER_SIZE);
    if (this->reply_buffer_ == nullptr) {
      // Not fatal: without the buffer the device simply stops advertising the capability and the
      // gateway keeps sending URLs, which is the path that has always worked.
      ESP_LOGE(TAG, "Failed to allocate the reply audio buffer; streamed replies disabled");
      this->announcement_speaker_ = nullptr;
    } else {
      xTaskCreate(KinetoVoice::playback_task, "kineto_ws_play", PLAYBACK_TASK_STACK_SIZE, (void *) this,
                  PLAYBACK_TASK_PRIORITY, &this->playback_task_handle_);
      if (this->playback_task_handle_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create the reply playback task; streamed replies disabled");
        this->reply_buffer_.reset();
        this->announcement_speaker_ = nullptr;
      }
    }
  }

  // A paired device carries a backend-issued identity in NVS; the yaml values are the dev
  // fallback for a speaker that was flashed with a token by hand.
  std::string stored_device_id = load_stored_identity_(NVS_KEY_DEVICE_ID);
  std::string stored_token = load_stored_identity_(NVS_KEY_TOKEN);
  if (!stored_device_id.empty() && !stored_token.empty()) {
    ESP_LOGI(TAG, "Using paired identity from NVS (device %s)", stored_device_id.c_str());
    this->device_id_ = stored_device_id;
    this->auth_token_ = stored_token;
  } else if (this->auth_token_.empty()) {
    ESP_LOGI(TAG, "No device token: connecting in pairing mode");
  }

  this->connect_client_();
}

void KinetoVoice::connect_client_() {
  this->headers_ = "X-Device-Id: " + this->device_id_ + "\r\n";
  // In pairing fallback the token is deliberately left out of the handshake: no token is what
  // puts the gateway session into pairing mode. The identity itself is kept for the retry.
  if (!this->auth_token_.empty() && !this->pairing_fallback_) {
    this->headers_ += "Authorization: Bearer " + this->auth_token_ + "\r\n";
  }

  esp_websocket_client_config_t ws_cfg = {};
  ws_cfg.uri = this->url_.c_str();
  ws_cfg.headers = this->headers_.c_str();
  ws_cfg.reconnect_timeout_ms = WS_RECONNECT_TIMEOUT_MS;
  ws_cfg.network_timeout_ms = WS_NETWORK_TIMEOUT_MS;
  // TCP alone does not notice a dead gateway: with a port-forwarder in the path
  // the socket stays half-open and the client waits forever. Protocol-level
  // ping/pong makes the client drop the connection and reconnect on its own.
  ws_cfg.ping_interval_sec = WS_PING_INTERVAL_SEC;
  ws_cfg.pingpong_timeout_sec = WS_PINGPONG_TIMEOUT_SEC;
  ws_cfg.crt_bundle_attach = esp_crt_bundle_attach;

  this->client_ = esp_websocket_client_init(&ws_cfg);
  if (this->client_ == nullptr) {
    ESP_LOGE(TAG, "Failed to init websocket client");
    this->mark_failed();
    return;
  }

  esp_websocket_register_events(this->client_, WEBSOCKET_EVENT_ANY, KinetoVoice::ws_event_handler_, (void *) this);
  // The client keeps reconnecting on its own (reconnect_timeout_ms) until stopped.
  esp_websocket_client_start(this->client_);
}

std::string KinetoVoice::load_stored_identity_(const char *key) {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
    return "";
  size_t len = 0;
  if (nvs_get_str(handle, key, nullptr, &len) != ESP_OK || len == 0) {
    nvs_close(handle);
    return "";
  }
  std::string value(len, '\0');
  esp_err_t err = nvs_get_str(handle, key, value.data(), &len);
  nvs_close(handle);
  if (err != ESP_OK)
    return "";
  value.resize(len - 1);  // drop the trailing NUL nvs_get_str includes
  return value;
}

void KinetoVoice::store_identity_(const std::string &device_id, const std::string &token) {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
    ESP_LOGE(TAG, "Cannot open NVS to store the device identity");
    return;
  }
  bool ok = nvs_set_str(handle, NVS_KEY_DEVICE_ID, device_id.c_str()) == ESP_OK &&
            nvs_set_str(handle, NVS_KEY_TOKEN, token.c_str()) == ESP_OK;
  if (ok) {
    nvs_commit(handle);
    ESP_LOGI(TAG, "Stored paired identity (device %s)", device_id.c_str());
  } else {
    ESP_LOGE(TAG, "Failed to store the paired identity");
  }
  nvs_close(handle);
}

void KinetoVoice::restart_client_(const char *reason) {
  if (this->client_ == nullptr)
    return;
  ESP_LOGW(TAG, "Restarting websocket client: %s", reason);
  esp_websocket_client_stop(this->client_);
  esp_websocket_client_destroy(this->client_);
  this->client_ = nullptr;
  this->ws_connected_.store(false);
  this->connect_client_();
}

void KinetoVoice::loop() {
  // Detect connection edges recorded by the websocket task.
  bool connected = this->ws_connected_.load();

  if (this->pending_reauth_) {
    this->pending_reauth_ = false;
    // Ours, not a failure — see expected_restart_.
    this->expected_restart_ = true;
    this->restart_client_("paired, reconnecting with the issued identity");
    return;
  }

  // The gateway refused the stored identity on the upgrade (401/403: the device row is gone, or
  // this gateway's backend never issued that token). Keep the identity — the old chat may come
  // back — but reconnect tokenless so a new pairing password can adopt the device. Only a
  // successful pairing (set_token) replaces the stored identity; until then it is retried
  // every AUTH_RETRY_INTERVAL_MS. A gateway whose backend is merely down answers 503, not 401,
  // so an outage never lands here.
  int handshake_status = this->last_handshake_status_.exchange(0);
  if ((handshake_status == 401 || handshake_status == 403) && !this->auth_token_.empty() &&
      !this->pairing_fallback_) {
    ESP_LOGW(TAG, "Gateway refused the stored identity (HTTP %d); offering pairing, keeping the identity",
             handshake_status);
    this->pairing_fallback_ = true;
    this->last_auth_retry_ms_ = millis();
    this->restart_client_("identity refused, connecting in pairing mode");
    return;
  }
  if (this->pairing_fallback_ && !this->listening_.load() &&
      millis() - this->last_auth_retry_ms_ > AUTH_RETRY_INTERVAL_MS) {
    // Quietly try the stored identity again; a refusal lands right back in pairing fallback.
    this->pairing_fallback_ = false;
    this->last_auth_retry_ms_ = millis();
    this->restart_client_("retrying the stored identity");
    return;
  }

  // Liveness watchdog: the gateway heartbeats every 10s while connected.
  if (connected) {
    uint32_t last = this->last_inbound_ms_.load();
    if (last != 0 && millis() - last > WS_LIVENESS_TIMEOUT_MS) {
      this->last_inbound_ms_.store(0);
      this->restart_client_("no gateway heartbeat for 45 s");
      return;
    }
  }

  // A wake frame is answered immediately (the gateway sends the listening LED state), so nothing
  // inbound within the timeout means this socket is one-way dead — a half-open connection the
  // port-forwarder never closed. Give up on this turn now instead of streaming a whole utterance
  // into the void and only noticing 45 s later, several lost turns down the line.
  if (this->listening_.load() && this->listen_started_ms_ != 0 &&
      millis() - this->listen_started_ms_ > LISTEN_ACK_TIMEOUT_MS &&
      this->last_inbound_ms_.load() <= this->listen_started_ms_) {
    ESP_LOGW(TAG, "No gateway response to the wake frame; treating the link as dead");
    this->stop_listening_();
    this->turn_failed_callbacks_.call();
    this->restart_client_("no gateway response to the wake frame");
    return;
  }
  if (connected && !this->was_ws_connected_) {
    this->was_ws_connected_ = true;
    this->send_hello_();
  } else if (!connected && this->was_ws_connected_) {
    this->was_ws_connected_ = false;
    this->hello_acked_ = false;
    // Whatever was left of a reply died with the socket; the rest of it is never coming.
    this->abort_reply_stream_();
    if (this->listening_.load()) {
      // The user was mid-sentence; that turn is gone with the socket — unless we are the ones who
      // took the socket away, in which case nothing was lost and there is nothing to announce.
      const bool ours = this->expected_restart_;
      this->stop_listening_();
      if (!ours)
        this->turn_failed_callbacks_.call();
    }
    this->expected_restart_ = false;
    this->disconnected_callbacks_.call();
  }

  // The playback task cannot run ESPHome automations, so it flags the end of a reply and the
  // trigger (undoing the ducking) fires here, on the main thread.
  if (this->reply_finished_.exchange(false)) {
    this->stream_stop_callbacks_.call();
  }

  // Drain inbound text frames queued by the websocket task.
  while (true) {
    std::string payload;
    {
      LockGuard guard(this->inbound_mutex_);
      if (this->inbound_frames_.empty())
        break;
      payload = std::move(this->inbound_frames_.front());
      this->inbound_frames_.pop_front();
    }
    this->handle_text_frame_(payload);
  }
}

void KinetoVoice::dump_config() {
  ESP_LOGCONFIG(TAG, "Kineto Voice:");
  ESP_LOGCONFIG(TAG, "  URL: %s", this->url_.c_str());
  ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
  ESP_LOGCONFIG(TAG, "  Auth token: %s", this->auth_token_.empty() ? "(none)" : "(set)");
}

bool KinetoVoice::send_json_(const json::json_build_t &func) {
  if (this->client_ == nullptr || !this->ws_connected_.load()) {
    ESP_LOGW(TAG, "Cannot send control frame: not connected");
    return false;
  }
  auto buffer = json::build_json(func);
  int sent = esp_websocket_client_send_text(this->client_, buffer.data(), buffer.size(), WS_SEND_TIMEOUT_TICKS);
  if (sent < 0) {
    ESP_LOGW(TAG, "Failed to send control frame");
    return false;
  }
  return true;
}

void KinetoVoice::send_hello_() {
  ESP_LOGD(TAG, "Sending hello frame");
  this->send_json_([this](JsonObject root) {
    root["type"] = "hello";
    root["deviceId"] = this->device_id_;
    root["firmwareVersion"] = ESPHOME_VERSION;
    JsonArray capabilities = root["capabilities"].to<JsonArray>();
    capabilities.add("audio_in_pcm16");
    capabilities.add("media_player");
    capabilities.add("led_ring");
    if (this->accepts_audio_stream_()) {
      capabilities.add("audio_stream_v1");
    }
  });
}

void KinetoVoice::start(const std::string &wake_word) {
  if (this->is_failed())
    return;
  if (!this->ws_connected_.load()) {
    ESP_LOGW(TAG, "Ignoring start: not connected to Kineto backend");
    this->turn_failed_callbacks_.call();
    return;
  }
  if (this->listening_.load()) {
    ESP_LOGD(TAG, "Already listening");
    return;
  }

  ESP_LOGD(TAG, "Start listening (wake word: %s)", wake_word.c_str());
  // The user is talking again, so the previous answer has lost its audience — and leaving it
  // playing would put the speaker's own voice into the microphone.
  this->abort_reply_stream_();
  this->ring_buffer_->reset();
  this->send_json_([&wake_word](JsonObject root) {
    root["type"] = "wake";
    root["wakeWord"] = wake_word;
  });

  this->listening_.store(true);
  this->listen_started_ms_ = millis();
  this->microphone_->start();
  this->listening_start_callbacks_.call();
}

void KinetoVoice::stop() {
  if (!this->listening_.load())
    return;
  ESP_LOGD(TAG, "Stop listening (local request)");
  // TODO(T4): notify the backend that the session was aborted locally.
  this->stop_listening_();
}

void KinetoVoice::stop_listening_() {
  this->listening_.store(false);
  this->listen_started_ms_ = 0;
  this->microphone_->stop();
  this->listening_stop_callbacks_.call();
}

void KinetoVoice::send_event(const std::string &kind, const std::string &detail) {
  this->send_json_([&kind, &detail](JsonObject root) {
    root["type"] = "event";
    root["kind"] = kind;
    if (!detail.empty()) {
      root["detail"] = detail;
    }
  });
}

void KinetoVoice::handle_text_frame_(const std::string &payload) {
  bool parsed = json::parse_json(payload, [this](JsonObject root) -> bool {
    const char *type = root["type"];
    if (type == nullptr) {
      ESP_LOGW(TAG, "Inbound frame without type");
      return false;
    }

    if (strcmp(type, "hello_ack") == 0) {
      ESP_LOGD(TAG, "Backend acknowledged hello");
      this->hello_acked_ = true;
      this->connected_callbacks_.call();
    } else if (strcmp(type, "listen_stop") == 0) {
      ESP_LOGD(TAG, "Backend requested listen stop");
      if (this->listening_.load()) {
        this->stop_listening_();
      }
    } else if (strcmp(type, "set_token") == 0) {
      const char *device_id = root["deviceId"];
      const char *token = root["token"];
      if (device_id == nullptr || token == nullptr) {
        ESP_LOGW(TAG, "set_token frame without deviceId/token");
        return false;
      }
      ESP_LOGI(TAG, "Paired: got a device identity from the gateway");
      this->store_identity_(device_id, token);
      this->device_id_ = device_id;
      this->auth_token_ = token;
      // A successful pairing is the one thing that replaces a refused identity.
      this->pairing_fallback_ = false;
      // Reconnect so the new credential rides the handshake headers.
      this->pending_reauth_ = true;
    } else if (strcmp(type, "heartbeat") == 0) {
      // Liveness only — receiving it already refreshed the watchdog.
      ESP_LOGV(TAG, "Gateway heartbeat");
    } else if (strcmp(type, "ack") == 0) {
      // Nothing to do yet.
      ESP_LOGV(TAG, "Backend ack");
    } else if (strcmp(type, "play") == 0) {
      const char *url = root["url"];
      if (url == nullptr) {
        ESP_LOGW(TAG, "play frame without url");
        return false;
      }
      const char *kind = root["kind"] | "";
      // Media goes to whoever owns the media speaker, when a config named one. On this hardware
      // that is the cspot player: it decodes the file itself and, being the same component that
      // plays Spotify, it can hand the speaker over instead of writing on top of another stream.
      // The media_player pipeline keeps the announcement kind, whose sources are flash chimes.
      if (strcmp(kind, "MEDIA") == 0 && this->has_media_play_hook_) {
        if (root["volume"].is<float>()) {
          this->media_player_->make_call().set_volume(root["volume"].as<float>() / 100.0f).perform();
        }
        // The token goes with the url: the gateway serves a clip only to the device it was stored
        // for, so the fetch has to identify itself the same way this socket did.
        this->media_play_callbacks_.call(std::string(url), this->auth_token_);
        return true;
      }
      auto call = this->media_player_->make_call();
      call.set_media_url(url);
      if (strcmp(kind, "TTS_ANNOUNCEMENT") == 0) {
        call.set_announcement(true);
      }
      if (root["volume"].is<float>()) {
        call.set_volume(root["volume"].as<float>() / 100.0f);
      }
      call.perform();
    } else if (strcmp(type, "stream_start") == 0) {
      this->begin_reply_stream_(root["sampleRate"] | 16000, root["channels"] | 1,
                                root["bitsPerSample"] | 16);
    } else if (strcmp(type, "stream_end") == 0) {
      // Only stops the arrival of audio: the playback task keeps draining what is buffered, and
      // announces the end itself once the speaker has actually run dry.
      ESP_LOGD(TAG, "Reply stream complete");
      this->reply_streaming_.store(false);
    } else if (strcmp(type, "stop") == 0) {
      this->abort_reply_stream_();
      // Stop whatever is audible: the media_player pipeline (radio/announcement
      // sources) and, when wired, the shortcut hooks' player (Spotify Connect
      // has no stop, so its hook pauses).
      this->media_player_->make_call().set_command(media_player::MEDIA_PLAYER_COMMAND_STOP).perform();
      if (this->has_media_control_hooks_) {
        this->media_pause_callbacks_.call();
      }
    } else if (strcmp(type, "pause") == 0) {
      if (this->has_media_control_hooks_) {
        this->media_pause_callbacks_.call();
      } else {
        this->media_player_->make_call().set_command(media_player::MEDIA_PLAYER_COMMAND_PAUSE).perform();
      }
    } else if (strcmp(type, "resume") == 0) {
      if (this->has_media_control_hooks_) {
        this->media_resume_callbacks_.call();
      } else {
        this->media_player_->make_call().set_command(media_player::MEDIA_PLAYER_COMMAND_PLAY).perform();
      }
    } else if (strcmp(type, "next") == 0) {
      if (this->has_media_control_hooks_) {
        this->media_next_callbacks_.call();
      } else {
        ESP_LOGW(TAG, "next frame: no media-control hooks configured");
      }
    } else if (strcmp(type, "volume") == 0) {
      float level = root["level"] | 0.0f;
      this->media_player_->make_call().set_volume(level / 100.0f).perform();
    } else if (strcmp(type, "led") == 0) {
      const char *state = root["state"] | "";
      this->set_led_callbacks_.call(std::string(state));
    } else {
      ESP_LOGW(TAG, "Unhandled frame type: %s", type);
    }
    return true;
  });

  if (!parsed) {
    ESP_LOGW(TAG, "Failed to parse inbound frame: %s", payload.c_str());
  }
}

void KinetoVoice::begin_reply_stream_(int sample_rate, int channels, int bits_per_sample) {
  if (!this->accepts_audio_stream_()) {
    ESP_LOGW(TAG, "Ignoring stream_start: this device has no speaker wired for streamed replies");
    return;
  }
  ESP_LOGD(TAG, "Reply stream starting (%d Hz, %d ch, %d bit)", sample_rate, channels, bits_per_sample);
  this->reply_sample_rate_ = sample_rate;
  this->reply_channels_ = channels;
  this->reply_bits_per_sample_ = bits_per_sample;
  this->reply_bytes_per_second_.store((uint32_t) sample_rate * channels * bits_per_sample / 8);
  this->reply_format_known_.store(true);
  this->reply_streaming_.store(true);
  // Fire before a sample is audible: the amplifier needs waking and the music needs ducking, and
  // the prebuffer is exactly the room this has to happen in.
  this->stream_start_callbacks_.call();
}

void KinetoVoice::abort_reply_stream_() {
  this->reply_format_known_.store(false);
  if (this->reply_buffer_ == nullptr)
    return;
  if (!this->reply_streaming_.load() && this->reply_buffer_->available() == 0)
    return;
  ESP_LOGD(TAG, "Dropping the reply stream");
  this->reply_streaming_.store(false);
  this->reply_buffer_->reset();
  this->reply_abort_.store(true);
}

void KinetoVoice::on_mic_data_(const std::vector<uint8_t> &data) {
  if (!this->listening_.load() || !this->ws_connected_.load())
    return;

  // The satellite1 microphone delivers 32-bit samples at 16 kHz; convert to 16-bit LE mono
  // by taking the first channel of each frame and keeping the top 16 bits (>> 16).
  const auto stream_info = this->microphone_->get_audio_stream_info();
  const size_t bytes_per_sample = stream_info.samples_to_bytes(1);
  if (bytes_per_sample != sizeof(int32_t)) {
    // TODO(T4): support other input formats via audio:: unpack helpers.
    return;
  }
  const size_t channels = stream_info.get_channels();
  const size_t frame_stride = channels * sizeof(int32_t);
  const size_t frames = data.size() / frame_stride;
  if (frames == 0)
    return;

  std::vector<int16_t> converted;
  converted.reserve(frames);
  const int32_t *samples = reinterpret_cast<const int32_t *>(data.data());
  for (size_t i = 0; i < frames; ++i) {
    converted.push_back(static_cast<int16_t>(samples[i * channels] >> 16));
  }

  size_t written = this->ring_buffer_->write(converted.data(), converted.size() * sizeof(int16_t));
  if (written < converted.size() * sizeof(int16_t)) {
    ESP_LOGV(TAG, "Audio ring buffer overflow, oldest samples dropped");
  }
}

void KinetoVoice::playback_task(void *params) {
  KinetoVoice *this_kv = (KinetoVoice *) params;
  uint8_t chunk[PLAYBACK_CHUNK_SIZE];
  // Bytes read from the ring but not yet accepted by the speaker. The speaker takes what fits and
  // reports how much, so the remainder has to be kept and offered again — dropping it would punch
  // a hole in the speech.
  size_t pending = 0;
  size_t pending_offset = 0;
  bool playing = false;
  uint32_t format_wait_ms = 0;

  while (true) {
    if (this_kv->reply_abort_.exchange(false)) {
      pending = 0;
      pending_offset = 0;
      if (playing) {
        this_kv->announcement_speaker_->stop();
        playing = false;
        this_kv->reply_playing_.store(false);
        this_kv->reply_finished_.store(true);
        ESP_LOGD(TAG, "Reply playback cut short");
      }
    }

    const bool streaming = this_kv->reply_streaming_.load();
    const size_t buffered = this_kv->reply_buffer_->available();

    if (!playing) {
      if (!streaming && buffered == 0 && pending == 0) {
        format_wait_ms = 0;
        vTaskDelay(pdMS_TO_TICKS(20));
        continue;
      }
      // Audio can reach the ring before its stream_start is parsed: bytes arrive on the socket task
      // and the frame is drained in the main loop. Starting here would use the LAST stream's format
      // — on the first reply after a boot, the defaults — and 24 kHz speech played as 16 kHz is the
      // slow, deep voice. So wait for the format instead of guessing a rate.
      if (!this_kv->reply_format_known_.load()) {
        format_wait_ms += 10;
        if (format_wait_ms > FORMAT_WAIT_TIMEOUT_MS) {
          ESP_LOGE(TAG, "Audio without a stream_start for %u ms; dropping %u buffered bytes",
                   (unsigned) format_wait_ms, (unsigned) buffered);
          this_kv->reply_buffer_->reset();
          format_wait_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
      format_wait_ms = 0;
      // Hold back until there is enough to ride out the first hiccup — unless the whole reply is
      // already here, in which case waiting would only add latency.
      const uint32_t prebuffer =
          this_kv->reply_bytes_per_second_.load() * REPLY_PREBUFFER_MS / 1000;
      if (streaming && buffered < prebuffer) {
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
      // Stop first, even when it looks stopped. A resampler speaker latches its input format at
      // start() and ignores set_audio_stream_info() on a speaker it considers live, so a reply
      // whose rate differs from whatever ran before plays at the OLD rate: 24 kHz speech through
      // a 16 kHz pipeline is the "slow and low" voice (measured 2026-08-18 — the resampler's
      // input ring came out 3200 bytes instead of 4800). cspot does the same stop/start dance
      // around every Spotify track, which is why its 44.1 kHz never had this problem.
      this_kv->announcement_speaker_->stop();
      for (int i = 0; i < SPEAKER_STOP_WAIT_STEPS && !this_kv->announcement_speaker_->is_stopped(); i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
      }
      this_kv->announcement_speaker_->set_audio_stream_info(audio::AudioStreamInfo(
          this_kv->reply_bits_per_sample_, this_kv->reply_channels_, this_kv->reply_sample_rate_));
      this_kv->announcement_speaker_->start();
      playing = true;
      this_kv->reply_playing_.store(true);
      ESP_LOGD(TAG, "Playing the reply (%u bytes buffered)", (unsigned) buffered);
    }

    if (pending == 0) {
      pending = this_kv->reply_buffer_->read((void *) chunk, PLAYBACK_CHUNK_SIZE, pdMS_TO_TICKS(20));
      pending_offset = 0;
    }

    if (pending > 0) {
      size_t written = this_kv->announcement_speaker_->play(chunk + pending_offset, pending - pending_offset,
                                                           pdMS_TO_TICKS(20));
      pending_offset += written;
      if (pending_offset >= pending) {
        pending = 0;
        pending_offset = 0;
      }
      continue;
    }

    // Nothing buffered. While the gateway is still sending this is just the sender's pacing;
    // once it has finished, the reply is over as soon as the speaker has played out.
    if (!streaming) {
      this_kv->announcement_speaker_->finish();
      while (this_kv->announcement_speaker_->has_buffered_data()) {
        vTaskDelay(pdMS_TO_TICKS(20));
      }
      playing = false;
      // The next stream must announce its own format; keeping this one's would let a reply at a
      // different rate start on stale settings, which is the bug this flag exists for.
      this_kv->reply_format_known_.store(false);
      this_kv->reply_playing_.store(false);
      this_kv->reply_finished_.store(true);
      ESP_LOGD(TAG, "Reply playback finished");
    }
  }
}

void KinetoVoice::stream_task(void *params) {
  KinetoVoice *this_kv = (KinetoVoice *) params;
  uint8_t chunk[STREAM_CHUNK_SIZE];

  while (true) {
    if (this_kv->listening_.load() && this_kv->ws_connected_.load()) {
      size_t available = this_kv->ring_buffer_->read((void *) chunk, STREAM_CHUNK_SIZE, pdMS_TO_TICKS(20));
      if (available > 0) {
        int sent = esp_websocket_client_send_bin(this_kv->client_, (const char *) chunk, available,
                                                 WS_SEND_TIMEOUT_TICKS);
        if (sent < 0) {
          ESP_LOGW(TAG, "Failed to send audio chunk");
        }
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }
}

void KinetoVoice::ws_event_handler_(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
  KinetoVoice *this_kv = (KinetoVoice *) handler_args;
  esp_websocket_event_data_t *data = (esp_websocket_event_data_t *) event_data;

  switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
      ESP_LOGD(TAG, "WebSocket connected");
      this_kv->last_inbound_ms_.store(millis());
      this_kv->ws_connected_.store(true);
      break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_ERROR:
      if (this_kv->ws_connected_.load()) {
        ESP_LOGD(TAG, "WebSocket disconnected");
      }
      // A failed upgrade carries its HTTP status; loop() reads 401/403 as "identity refused".
      // The client struct never clears the field, but every acted-on status ends in
      // restart_client_(), which builds a fresh (zeroed) client — so a stale value cannot recur.
      if (data != nullptr && data->error_handle.esp_ws_handshake_status_code > 0) {
        this_kv->last_handshake_status_.store(data->error_handle.esp_ws_handshake_status_code);
      }
      this_kv->ws_connected_.store(false);
      break;
    case WEBSOCKET_EVENT_DATA: {
      // 0x02 is a binary frame — reply audio; 0x00 continues one that did not fit the client's
      // receive buffer. Audio is a byte stream, so every part goes into the ring in arrival order
      // and there is nothing to reassemble. Anything binary outside a stream is not ours to play.
      if (data->op_code == 0x02 || data->op_code == 0x00) {
        this_kv->last_inbound_ms_.store(millis());
        // Buffered without checking whether the stream has been announced yet: `stream_start` is
        // handled by the main loop, which may not have run since it arrived, and audio dropped in
        // that window would clip the first word. The playback task is what waits for the
        // announcement; the buffer is only a pipe, cleared when a reply is abandoned.
        if (this_kv->reply_buffer_ == nullptr || data->data_len <= 0) {
          break;
        }
        size_t written = this_kv->reply_buffer_->write_without_replacement((const void *) data->data_ptr,
                                                                          data->data_len);
        if (written < (size_t) data->data_len) {
          // Dropping the newest audio keeps what is already playing intact; the alternative
          // (overwriting the oldest) would tear a hole in the middle of the sentence.
          ESP_LOGW(TAG, "Reply buffer full, dropped %d bytes", (int) (data->data_len - written));
        }
        break;
      }
      if (data->op_code != 0x01) {
        // Control frames (ping/pong/close) are the client's business, not ours.
        break;
      }
      if (data->payload_offset != 0 || data->data_len != data->payload_len) {
        // TODO(T4): reassemble fragmented text frames.
        ESP_LOGW(TAG, "Dropping fragmented text frame (%d bytes)", data->payload_len);
        break;
      }
      this_kv->last_inbound_ms_.store(millis());
      std::string payload(data->data_ptr, data->data_len);
      {
        LockGuard guard(this_kv->inbound_mutex_);
        this_kv->inbound_frames_.push_back(std::move(payload));
      }
      break;
    }
    default:
      break;
  }
}

}  // namespace kineto_voice
}  // namespace esphome

#endif  // USE_ESP32
