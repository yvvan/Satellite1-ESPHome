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
#include "esphome/components/speaker/speaker.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace kineto_voice {

/// Voice satellite client for the Kineto backend.
///
/// Maintains a WebSocket connection to the Kineto voice endpoint:
///  - outbound: JSON control frames (hello / wake / event) + binary 16-bit 16 kHz mono PCM
///  - inbound:  JSON control frames (hello_ack / listen_stop / ack / play / stop / volume / led /
///              stream_start / stream_end) + binary PCM of a spoken reply
///
/// A spoken reply is always streamed: `stream_start`, then raw PCM in binary frames straight into
/// the announcement speaker, so it starts playing while the rest is still being synthesized. The
/// device advertises that it can do this in its hello.
///
/// A `play` frame carries audio the gateway did not synthesize (a track, a file). Media goes to
/// the cspot player, which owns the media half of the speaker together with Spotify Connect —
/// the media_player pipeline handles the announcement kind only.
class KinetoVoice : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

  void set_microphone(microphone::Microphone *microphone) { this->microphone_ = microphone; }
  void set_media_player(media_player::MediaPlayer *media_player) { this->media_player_ = media_player; }
  /// Where a streamed reply is played. Without one the device cannot accept streams and does not
  /// advertise the capability, so the gateway keeps sending it URLs.
  void set_announcement_speaker(speaker::Speaker *speaker) { this->announcement_speaker_ = speaker; }
  void set_url(const std::string &url) { this->url_ = url; }
  void set_device_id(const std::string &device_id) { this->device_id_ = device_id; }
  void set_auth_token(const std::string &auth_token) { this->auth_token_ = auth_token; }

  /// Start a listening session: sends a "wake" control frame and streams mic audio until stopped.
  void start(const std::string &wake_word);
  /// Stop the current listening session (local decision, e.g. button press).
  void stop();
  /// Send a generic {"type": "event", "kind": ..., "detail": ...} control frame.
  void send_event(const std::string &kind, const std::string &detail);
  /// Reports a transport command this device could not act on (e.g. resume with no Spotify
  /// session) so the gateway can hand the user's request to the agent instead of silence.
  void send_command_failed(const std::string &command, const std::string &reason);
  /// Reconnects the websocket NOW after the network came back, instead of waiting out the client's
  /// own retry timer. Call from the main loop only (wifi's on_connect) — see client_mutex_.
  void network_recovered();

  bool is_listening() const { return this->listening_.load(); }
  /// True while a streamed reply is arriving or still being played out.
  bool is_speaking() const { return this->reply_streaming_.load() || this->reply_playing_.load(); }
  /// Silences a streamed reply the user no longer wants to hear. Safe to call when none is playing.
  void abort_reply() { this->abort_reply_stream_(); }
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
  /// A turn the user started could not be delivered — no link, or the link died mid-listen.
  /// Wire it to something audible: silence is indistinguishable from "did not hear you".
  void add_on_turn_failed_callback(std::function<void()> callback) {
    this->turn_failed_callbacks_.add(std::move(callback));
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
  void add_on_media_next_callback(std::function<void()> callback) {
    this->has_media_control_hooks_ = true;
    this->media_next_callbacks_.add(std::move(callback));
  }
  /// A `play` frame carrying media (a track, a file) rather than a spoken announcement. Wire it to
  /// whoever owns the media half of the speaker — on this hardware the cspot player, which decodes
  /// the file itself and can hand the speaker over from Spotify instead of writing on top of it.
  /// Unwired, media falls back to the media_player pipeline, which plays FLAC but silently
  /// produces nothing from an MP3 (measured 2026-08-18).
  void add_on_media_play_callback(std::function<void(std::string, std::string)> callback) {
    this->has_media_play_hook_ = true;
    this->media_play_callbacks_.add(std::move(callback));
  }
  /// A streamed reply is about to be audible. Wire the same preparation the media_player does for
  /// an announcement: wake the amplifier and duck whatever else is playing.
  void add_on_stream_start_callback(std::function<void()> callback) {
    this->stream_start_callbacks_.add(std::move(callback));
  }
  /// The streamed reply finished playing (or was cut short) — undo the ducking.
  void add_on_stream_stop_callback(std::function<void()> callback) {
    this->stream_stop_callbacks_.add(std::move(callback));
  }

 protected:
  /// Initializes and starts the esp_websocket_client (auto-reconnects on its own).
  void connect_client_();
  /// Tears the client down and starts a fresh one (liveness watchdog, or a fresh credential).
  void restart_client_(const char *reason);
  void begin_offline_capture_(const std::string &wake_word);
  void end_offline_capture_();
  void reset_stored_turn_();
  void log_link_context_();
  void enable_roaming_cooperation_();
  void keep_turn_for_later_(const char *why);
  /// Sends what was kept and clears it. Runs on the audio task — see client_mutex_.
  void send_stored_turn_();
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
  /// FreeRTOS task draining received PCM into the announcement speaker.
  static void playback_task(void *params);
  /// Opens a reply stream in the announced PCM shape. Runs in the main loop.
  void begin_reply_stream_(int sample_rate, int channels, int bits_per_sample);
  /// Drops a reply that is no longer wanted (the user spoke again, or the link died).
  void abort_reply_stream_();
  bool accepts_audio_stream_() const { return this->announcement_speaker_ != nullptr; }
  /// esp_websocket_client event handler. Runs in the websocket task context, so it only
  /// records state/queues payloads; triggers fire from loop().
  static void ws_event_handler_(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);

  microphone::Microphone *microphone_{nullptr};
  media_player::MediaPlayer *media_player_{nullptr};
  speaker::Speaker *announcement_speaker_{nullptr};

  std::string url_;
  std::string device_id_;
  std::string auth_token_;
  std::string headers_;

  esp_websocket_client_handle_t client_{nullptr};

  std::unique_ptr<ring_buffer::RingBuffer> ring_buffer_;
  /**
   * A copy of the turn being spoken right now, kept until the gateway proves it heard it.
   *
   * Its own buffer rather than a snapshot of [ring_buffer_]: a snapshot meant allocating the whole
   * recording at the moment of sending, and on this chip that is a quarter of a megabyte out of an
   * internal heap with tens of kilobytes free — an abort, with exceptions compiled out. Allocated
   * once at boot, in PSRAM like every other audio buffer here, and drained straight to the socket.
   */
  std::unique_ptr<ring_buffer::RingBuffer> stored_ring_;
  TaskHandle_t stream_task_handle_{nullptr};

  /// Received reply audio, waiting to be played. Sized for several seconds so a Wi-Fi stall does
  /// not become a gap in the speech; lives in PSRAM (the ring buffer's default preference).
  std::unique_ptr<ring_buffer::RingBuffer> reply_buffer_;
  TaskHandle_t playback_task_handle_{nullptr};
  /// True between stream_start and stream_end: the gateway is still sending this reply. The
  /// playback task keeps draining after it clears, so the tail is never cut off.
  std::atomic<bool> reply_streaming_{false};
  /** True once THIS stream announced its format. Playing before it does means guessing a rate. */
  std::atomic<bool> reply_format_known_{false};
  /// Announced PCM shape of the reply currently being received.
  std::atomic<uint32_t> reply_bytes_per_second_{32000};
  int reply_sample_rate_{16000};
  int reply_channels_{1};
  int reply_bits_per_sample_{16};
  /// Raised by the playback task when a reply has finished playing, so loop() can fire the stop
  /// trigger from the main thread, where ESPHome automations belong.
  std::atomic<bool> reply_finished_{false};
  /// Asks the playback task to cut the current reply short. Every call into the speaker is left to
  /// that one task, so silencing it from elsewhere is a request rather than an action.
  std::atomic<bool> reply_abort_{false};
  /**
   * Which stream the pending abort belongs to.
   *
   * The abort is a flag for the playback task, so it is applied whenever that task next runs — and a
   * reply that starts in between (the model answering the interruption immediately) was having the
   * abort applied to IT: the chunk in hand discarded and the speaker stopped and restarted a few
   * bytes into the new audio. A 16-bit stream restarted off a sample boundary is a hiss, which is
   * what the room heard on 2026-08-25. Stamped here, checked there: an abort older than the current
   * stream is not this stream's business.
   */
  std::atomic<uint32_t> reply_abort_generation_{0};
  /// Whether the playback task currently owns the announcement speaker.
  std::atomic<bool> reply_playing_{false};
  /**
   * millis() of the last mic frame that arrived while a reply was audible. The uplink mute reads
   * it: frames are dropped while a reply plays and for a short tail after, because the room keeps
   * ringing after the cone stops. The model must never hear this device's own voice — AEC removes
   * the direct sound but a live room's late reflections leak past it, the server VAD reads them as
   * speech, and one wake word becomes the model answering its own echo (prod, 2026-08-25). Wake
   * words are unaffected: micro_wake_word reads the mic on its own path, so "Stop" still interrupts.
   */
  std::atomic<uint32_t> reply_last_audible_ms_{0};
  /// Bumped by every stream_start. The playback task clears the format only if nothing newer has
  /// started while it was draining — without this, a finish racing the NEXT stream's start wiped
  /// the format that start had just announced, and the new reply was dropped as formatless
  /// (measured 2026-08-21 10:00: three seconds of speech thrown away between back-to-back replies).
  std::atomic<uint32_t> reply_stream_generation_{0};

  /// Reassembly for a text frame that arrives in pieces — either split by the client's receive
  /// buffer (payload_offset walks forward) or fragmented at the protocol level (continuation
  /// frames, fin on the last). Touched only by the websocket task, so no lock. Binary needs none
  /// of this: audio is a byte stream and every piece goes into the ring in arrival order.
  std::string text_fragment_;
  bool text_fragment_open_{false};

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
  /// True while a disconnect is OUR OWN doing, so the drop is not reported as a failed turn.
  /// Pairing is the case that needs it: the gateway hands over an identity, this device restarts
  /// its socket to use it, and the drop is otherwise indistinguishable from a link that died
  /// mid-sentence — which answers a pairing that just succeeded with "Kinetik unreachable".
  bool expected_restart_{false};
  /// HTTP status of the last failed WS upgrade, recorded by the websocket task; 0 = none.
  std::atomic<int> last_handshake_status_{0};
  // Consecutive connects that died before hello_ack. Some proxies swallow the upgrade status
  // (through the prod GCLB a 401 surfaces as CONNECTED + an instant drop with no handshake
  // code), so refusal is also inferred from this pattern — see loop().
  std::atomic<uint32_t> unacked_drops_{0};
  /// True while the gateway refuses the stored identity and the client is connected tokenless so
  /// a new pairing password can adopt the device. The stored identity itself stays untouched —
  /// only a successful pairing (set_token) replaces it — and is retried periodically in case the
  /// old chat comes back.
  bool pairing_fallback_{false};
  /// The 802.11k/v station flags are written once per boot; see enable_roaming_cooperation_().
  bool roaming_flags_set_{false};
  uint32_t last_auth_retry_ms_{0};
  /// millis() when the current listen window opened, 0 when not listening. The gateway answers a
  /// wake frame within milliseconds, so silence past ACK_TIMEOUT means the socket is dead in a way
  /// TCP has not noticed yet — waiting for the 45 s liveness watchdog would eat several turns.
  uint32_t listen_started_ms_{0};

  /// Recording a turn that has nowhere to go yet: the wake word was heard with no link to carry it.
  /// The speaker hears its own wake word, so an offline moment costs the connection and not the
  /// question — the words are kept and sent when the socket comes back, seconds later.
  std::atomic<bool> capturing_offline_{false};
  /// millis() when that recording started; it runs for OFFLINE_CAPTURE_MS and no longer.
  uint32_t offline_started_ms_{0};
  /// A finished offline recording sitting in the audio ring, waiting for a link.
  std::atomic<bool> stored_turn_pending_{false};
  /// millis() when it ended — the age the gateway is told, and what makes it too stale to send.
  std::atomic<uint32_t> stored_turn_ended_ms_{0};
  /// Set by loop() once the link is back and the gateway has answered hello; the audio task sends
  /// the recording, because it is the task that owns sending over this socket.
  std::atomic<bool> flush_stored_turn_{false};

  // Inbound text frames queued by the websocket task, drained by loop().
  Mutex inbound_mutex_;
  /// Makes stream_task's send and the client's stop/destroy mutually exclusive. Every other
  /// sender runs in the main loop with restart_client_, so only the audio task can race the
  /// teardown — and over TLS that race walks a freed mbedtls context and reboots the device.
  Mutex client_mutex_;
  std::deque<std::string> inbound_frames_;

  CallbackManager<void()> connected_callbacks_;
  CallbackManager<void()> disconnected_callbacks_;
  CallbackManager<void()> listening_start_callbacks_;
  CallbackManager<void()> listening_stop_callbacks_;
  CallbackManager<void(std::string)> set_led_callbacks_;
  CallbackManager<void()> turn_failed_callbacks_;
  CallbackManager<void()> media_pause_callbacks_;
  CallbackManager<void()> media_resume_callbacks_;
  CallbackManager<void()> media_next_callbacks_;
  /** url + the device token to fetch it with: the gateway serves audio only to the device it is for. */
  CallbackManager<void(std::string, std::string)> media_play_callbacks_;
  CallbackManager<void()> stream_start_callbacks_;
  CallbackManager<void()> stream_stop_callbacks_;
  bool has_media_control_hooks_{false};
  bool has_media_play_hook_{false};
};

}  // namespace kineto_voice
}  // namespace esphome

#endif  // USE_ESP32
