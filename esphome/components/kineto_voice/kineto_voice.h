#pragma once

#ifdef USE_ESP32

#include <atomic>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_event.h>
#include <esp_websocket_client.h>

#include "esphome/components/json/json_util.h"
#include "esphome/components/media_player/media_player.h"
#include "esphome/components/microphone/microphone.h"
#include "esphome/components/ring_buffer/ring_buffer.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace kineto_voice {

/// Voice satellite client for the Kineto backend.
///
/// Maintains a WebSocket connection to the Kineto voice endpoint:
///  - outbound: JSON control frames (hello / wake / event) + binary 16-bit 16 kHz mono PCM
///  - inbound:  JSON control frames (hello_ack / listen_stop / ack / play / stop / volume / led)
class KinetoVoice : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

  void set_microphone(microphone::Microphone *microphone) { this->microphone_ = microphone; }
  void set_media_player(media_player::MediaPlayer *media_player) { this->media_player_ = media_player; }
  void set_url(const std::string &url) { this->url_ = url; }
  void set_device_id(const std::string &device_id) { this->device_id_ = device_id; }
  void set_auth_token(const std::string &auth_token) { this->auth_token_ = auth_token; }

  /// Start a listening session: sends a "wake" control frame and streams mic audio until stopped.
  void start(const std::string &wake_word);
  /// Stop the current listening session (local decision, e.g. button press).
  void stop();
  /// Send a generic {"type": "event", "kind": ..., "detail": ...} control frame.
  void send_event(const std::string &kind, const std::string &detail);

  bool is_listening() const { return this->listening_.load(); }
  /// True once the backend acknowledged our hello frame.
  bool is_connected() const { return this->hello_acked_; }

  void add_on_connected_callback(std::function<void()> callback) {
    this->connected_callbacks_.add(std::move(callback));
  }
  void add_on_disconnected_callback(std::function<void()> callback) {
    this->disconnected_callbacks_.add(std::move(callback));
  }
  void add_on_listening_start_callback(std::function<void()> callback) {
    this->listening_start_callbacks_.add(std::move(callback));
  }
  void add_on_listening_stop_callback(std::function<void()> callback) {
    this->listening_stop_callbacks_.add(std::move(callback));
  }
  void add_on_set_led_callback(std::function<void(std::string)> callback) {
    this->set_led_callbacks_.add(std::move(callback));
  }
  // Media-control frames (gateway level-2 shortcuts). When a config wires these
  // triggers (e.g. to the cspot player), they own the behaviour; otherwise the
  // frames fall back to pause/play commands on the configured media_player.
  void add_on_media_pause_callback(std::function<void()> callback) {
    this->has_media_control_hooks_ = true;
    this->media_pause_callbacks_.add(std::move(callback));
  }
  void add_on_media_resume_callback(std::function<void()> callback) {
    this->has_media_control_hooks_ = true;
    this->media_resume_callbacks_.add(std::move(callback));
  }
  /** Delivers the project's Spotify account pushed by the gateway. */
  void add_on_spotify_token_callback(std::function<void(std::string)> callback) {
    this->spotify_token_callbacks_.add(std::move(callback));
  }
  void add_on_media_next_callback(std::function<void()> callback) {
    this->has_media_control_hooks_ = true;
    this->media_next_callbacks_.add(std::move(callback));
  }

 protected:
  /// Initializes and starts the esp_websocket_client (auto-reconnects on its own).
  void connect_client_();
  /// Tears the client down and starts a fresh one (liveness watchdog, or a fresh credential).
  void restart_client_(const char *reason);
  /// Reads one string from the kineto NVS namespace; empty when absent.
  std::string load_stored_identity_(const char *key);
  /// Persists the identity issued by pairing, so the device comes back paired after a reboot.
  void store_identity_(const std::string &device_id, const std::string &token);
  /// Serializes a JSON object and sends it as a text frame.
  bool send_json_(const json::json_build_t &func);
  void send_hello_();
  /// Parses an inbound JSON text frame and dispatches it. Runs in the main loop.
  void handle_text_frame_(const std::string &payload);
  void stop_listening_();

  /// Microphone data callback: 32-bit samples at 16 kHz (see Sat1Microphone).
  /// Converts to 16-bit LE mono and pushes into the ring buffer. Runs in the mic task.
  void on_mic_data_(const std::vector<uint8_t> &data);

  /// FreeRTOS task draining the ring buffer into esp_websocket_client_send_bin().
  static void stream_task(void *params);
  /// esp_websocket_client event handler. Runs in the websocket task context, so it only
  /// records state/queues payloads; triggers fire from loop().
  static void ws_event_handler_(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);

  microphone::Microphone *microphone_{nullptr};
  media_player::MediaPlayer *media_player_{nullptr};

  std::string url_;
  std::string device_id_;
  std::string auth_token_;
  std::string headers_;

  esp_websocket_client_handle_t client_{nullptr};

  std::unique_ptr<ring_buffer::RingBuffer> ring_buffer_;
  TaskHandle_t stream_task_handle_{nullptr};

  std::atomic<bool> listening_{false};
  std::atomic<bool> ws_connected_{false};
  bool was_ws_connected_{false};
  bool hello_acked_{false};
  // Liveness: the gateway beacons every 10s. Silence past the timeout means the
  // link is dead even when the socket still looks open (a port-forwarder in the
  // path swallows the close), so the client is restarted.
  std::atomic<uint32_t> last_inbound_ms_{0};
  /// Set when a set_token frame arrives; loop() reconnects with the new credential.
  bool pending_reauth_{false};

  // Inbound text frames queued by the websocket task, drained by loop().
  Mutex inbound_mutex_;
  std::deque<std::string> inbound_frames_;

  CallbackManager<void()> connected_callbacks_;
  CallbackManager<void()> disconnected_callbacks_;
  CallbackManager<void()> listening_start_callbacks_;
  CallbackManager<void()> listening_stop_callbacks_;
  CallbackManager<void(std::string)> set_led_callbacks_;
  CallbackManager<void()> media_pause_callbacks_;
  CallbackManager<void()> media_resume_callbacks_;
  CallbackManager<void()> media_next_callbacks_;
  CallbackManager<void(std::string)> spotify_token_callbacks_;
  bool has_media_control_hooks_{false};
};

}  // namespace kineto_voice
}  // namespace esphome

#endif  // USE_ESP32
