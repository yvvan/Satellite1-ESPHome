#include "kineto_voice.h"

#ifdef USE_ESP32

#include <esp_crt_bundle.h>

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

static const int WS_RECONNECT_TIMEOUT_MS = 5000;  // TODO(T4): exponential backoff on top of this
static const int WS_NETWORK_TIMEOUT_MS = 10000;
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

  this->connect_client_();
}

void KinetoVoice::connect_client_() {
  this->headers_ = "X-Device-Id: " + this->device_id_ + "\r\n";
  if (!this->auth_token_.empty()) {
    this->headers_ += "Authorization: Bearer " + this->auth_token_ + "\r\n";
  }

  esp_websocket_client_config_t ws_cfg = {};
  ws_cfg.uri = this->url_.c_str();
  ws_cfg.headers = this->headers_.c_str();
  ws_cfg.reconnect_timeout_ms = WS_RECONNECT_TIMEOUT_MS;
  ws_cfg.network_timeout_ms = WS_NETWORK_TIMEOUT_MS;
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

void KinetoVoice::loop() {
  // Detect connection edges recorded by the websocket task.
  bool connected = this->ws_connected_.load();
  if (connected && !this->was_ws_connected_) {
    this->was_ws_connected_ = true;
    this->send_hello_();
  } else if (!connected && this->was_ws_connected_) {
    this->was_ws_connected_ = false;
    this->hello_acked_ = false;
    if (this->listening_.load()) {
      this->stop_listening_();
    }
    this->disconnected_callbacks_.call();
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
  });
}

void KinetoVoice::start(const std::string &wake_word) {
  if (this->is_failed())
    return;
  if (!this->ws_connected_.load()) {
    ESP_LOGW(TAG, "Ignoring start: not connected to Kineto backend");
    return;
  }
  if (this->listening_.load()) {
    ESP_LOGD(TAG, "Already listening");
    return;
  }

  ESP_LOGD(TAG, "Start listening (wake word: %s)", wake_word.c_str());
  this->ring_buffer_->reset();
  this->send_json_([&wake_word](JsonObject root) {
    root["type"] = "wake";
    root["wakeWord"] = wake_word;
  });

  this->listening_.store(true);
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
      auto call = this->media_player_->make_call();
      call.set_media_url(url);
      if (strcmp(kind, "TTS_ANNOUNCEMENT") == 0) {
        call.set_announcement(true);
      }
      if (root["volume"].is<float>()) {
        call.set_volume(root["volume"].as<float>() / 100.0f);
      }
      call.perform();
    } else if (strcmp(type, "stop") == 0) {
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
      this_kv->ws_connected_.store(true);
      break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_ERROR:
      if (this_kv->ws_connected_.load()) {
        ESP_LOGD(TAG, "WebSocket disconnected");
      }
      this_kv->ws_connected_.store(false);
      break;
    case WEBSOCKET_EVENT_DATA: {
      if (data->op_code != 0x01) {
        // Binary/control frames from the backend are not part of the protocol (audio comes as URLs).
        break;
      }
      if (data->payload_offset != 0 || data->data_len != data->payload_len) {
        // TODO(T4): reassemble fragmented text frames.
        ESP_LOGW(TAG, "Dropping fragmented text frame (%d bytes)", data->payload_len);
        break;
      }
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
