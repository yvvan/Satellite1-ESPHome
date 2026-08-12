#include "cspot_player.h"

#include <atomic>
#include <map>
#include <memory>
#include <string>

#include "esphome/core/log.h"

#include <BellHTTPServer.h>
#include <BellTask.h>
#include <BellUtils.h>
#include <CSpotContext.h>
#include <LoginBlob.h>
#include <MDNSService.h>
#include <SpircHandler.h>
#include <TrackPlayer.h>
#include <civetweb.h>

#include "esp_heap_caps.h"
#include "nvs.h"

namespace esphome {
namespace cspot_player {

static const char *const TAG = "cspot_player";

namespace {

constexpr const char *NVS_NAMESPACE = "cspot";
constexpr const char *NVS_KEY_AUTH = "authblob";

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

void log_heap(const char *when) {
  ESP_LOGI(TAG, "[heap @ %s] internal free=%u largest=%u | psram free=%u", when,
           (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned) heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

}  // namespace

class CSpotPlayer::Runner : public bell::Task {
 public:
  Runner(std::string device_name, uint16_t http_port)
      : bell::Task("cspot_runner", 32 * 1024, 0, 1),
        device_name_(std::move(device_name)),
        http_port_(http_port) {
    startTask();
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
    auto blob = std::make_shared<cspot::LoginBlob>(device_name_);

    std::string stored = load_credentials();
    bool from_zeroconf = stored.empty();
    if (!from_zeroconf) {
      ESP_LOGI(TAG, "Using stored Spotify credentials for '%s'", device_name_.c_str());
      blob->loadJson(stored);
    } else {
      this->run_zeroconf_(blob);  // blocks until the Spotify app hands over credentials
    }

    auto ctx = cspot::Context::createFromBlob(blob);
    ESP_LOGI(TAG, "Connecting to Spotify AP...");
    ctx->session->connectWithRandomAp();
    auto token = ctx->session->authenticate(blob);
    if (token.empty()) {
      ESP_LOGE(TAG, "Spotify authentication failed");
      if (!from_zeroconf) {
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

    // Spike sink: count and discard PCM (44.1 kHz s16 stereo). Real audio output is M3.
    handler->getTrackPlayer()->setDataCallback(
        [this](uint8_t *data, size_t bytes, std::string_view track_id) -> size_t {
          this->streamed_bytes_ += bytes;
          if (this->streamed_bytes_ - this->last_report_ >= 1024 * 1024) {
            this->last_report_ = this->streamed_bytes_;
            ESP_LOGI(TAG, "Streaming: %u MB decoded", (unsigned) (this->streamed_bytes_ >> 20));
            log_heap("streaming");
          }
          return bytes;
        });

    handler->setEventHandler([](std::unique_ptr<cspot::SpircHandler::Event> event) {
      switch (event->eventType) {
        case cspot::SpircHandler::EventType::TRACK_INFO: {
          auto &info = std::get<cspot::TrackInfo>(event->data);
          ESP_LOGI(TAG, "Track: %s — %s", info.artist.c_str(), info.name.c_str());
          break;
        }
        case cspot::SpircHandler::EventType::PLAY_PAUSE:
          ESP_LOGI(TAG, "Play/pause: %s", std::get<bool>(event->data) ? "paused" : "playing");
          break;
        case cspot::SpircHandler::EventType::DISC:
          ESP_LOGI(TAG, "User disconnected the device");
          break;
        default:
          break;
      }
    });

    while (true) {
      ctx->session->handlePacket();
    }
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

  std::string device_name_;
  uint16_t http_port_;
  size_t streamed_bytes_{0};
  size_t last_report_{0};
};

void CSpotPlayer::setup() {
  // ESPHome (AFTER_CONNECTION) has Wi-Fi up and the IDF mdns component initialized
  // by now; bell's MDNSService only adds a service record to it.
  this->runner_ = new Runner(this->device_name_, this->http_port_);
}

void CSpotPlayer::dump_config() {
  ESP_LOGCONFIG(TAG, "CSpot player:");
  ESP_LOGCONFIG(TAG, "  Device name: %s", this->device_name_.c_str());
  ESP_LOGCONFIG(TAG, "  Zeroconf HTTP port: %u", this->http_port_);
}

}  // namespace cspot_player
}  // namespace esphome
