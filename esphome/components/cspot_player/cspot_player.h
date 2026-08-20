#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esphome/core/component.h"
#include "esphome/components/speaker/speaker.h"

namespace esphome {
namespace cspot_player {

/**
 * Owner of the media half of the speaker: Spotify Connect (backed by the cspot library)
 * and playback of a plain audio URL.
 *
 * Both live here on purpose. They write PCM into the SAME `media_speaker_`, and a speaker
 * takes its format from whoever started it, so two independent writers would fight over it
 * and mix a track into a reply. One owner makes "Spotify and our own audio never play at
 * once" a property of the code instead of a rule someone has to remember: starting a URL
 * pauses Spotify, and Spotify's decoded audio is dropped while a URL is playing.
 *
 * The URL side decodes MP3 on the device (micro-mp3) and hands the resulting PCM to the
 * same sink Spotify uses. It deliberately does NOT go through ESPHome's media_player
 * pipeline: that path fetches and decodes an MP3 without ever producing a sample here
 * (measured 2026-08-18 — the decoder reports valid stream info, then the pipeline's own
 * buffer bookkeeping fails silently), while FLAC through the very same pipeline plays.
 *
 * All cspot types stay inside the .cpp (Runner pimpl) so this header compiles
 * without the cspot include paths.
 */
class CSpotPlayer : public Component {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

  void set_device_name(const std::string &name) { this->device_name_ = name; }
  void set_http_port(uint16_t port) { this->http_port_ = port; }
  void set_media_speaker(speaker::Speaker *speaker) { this->media_speaker_ = speaker; }

  // External playback control (voice shortcuts). Routed through cspot's
  // SpircHandler so the Spotify Connect state stays in sync across devices.
  // No-ops when no Spotify session is active.
  void set_paused(bool paused);
  /// True when a Spotify Connect session exists for transport commands to act on — lets a
  /// caller report "nothing to resume" instead of issuing the silent no-op set_paused makes.
  bool has_session();
  void next_track();

  /**
   * Play [url] (MP3) on the media speaker, replacing whatever is playing there —
   * a Spotify track included. Returns false when the component cannot take it at
   * all (no speaker configured, or the previous URL is still shutting down).
   * The fetch and the decode run on their own task, so this returns immediately.
   */
  bool play_url(const std::string &url, const std::string &auth_token);

  /** Stop URL playback, if any. Does not resume Spotify: the user asked for it. */
  void stop_url();

  /** True while a URL owns the media speaker. */
  bool url_playing() const { return this->url_playing_.load(); }

 protected:
  class Runner;

  /** Body of the URL playback task: fetch, decode, feed the speaker. */
  void run_url_playback_();
  static void url_task_(void *param);

  std::string device_name_;
  uint16_t http_port_{8080};
  speaker::Speaker *media_speaker_{nullptr};
  Runner *runner_{nullptr};

  // URL playback state. `url_playing_` is read by the Spotify data callback on the cspot
  // task, so it has to be atomic; the URL itself is only touched while no task runs.
  std::string url_;
  std::string url_auth_token_;
  std::atomic<bool> url_playing_{false};
  std::atomic<bool> url_stop_requested_{false};
  TaskHandle_t url_task_handle_{nullptr};
};

}  // namespace cspot_player
}  // namespace esphome
