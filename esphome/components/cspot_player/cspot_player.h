#pragma once

#include <cstdint>
#include <string>

#include <functional>

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/speaker/speaker.h"

namespace esphome {
namespace cspot_player {

/**
 * Spotify Connect endpoint on the speaker, backed by the cspot library.
 *
 * Spike scope: pairs via the standard zeroconf flow (the user authorizes the device
 * from the official Spotify app on the same LAN), persists credentials in NVS, and
 * streams the selected track. Decoded PCM is currently discarded and counted — the
 * milestone under test is auth + streaming + memory coexistence, not audio output.
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
  void next_track();

  /**
   * Hands the player the project's Spotify access token (pushed by the gateway). Used once, to log
   * in and obtain this device's own reusable credentials; never stored and never logged.
   */
  void set_spotify_token(const std::string &access_token);

  /** Fires while the player has no Spotify credentials — the wiring asks the gateway for a token. */
  void add_on_spotify_unlinked_callback(std::function<void()> callback) {
    this->spotify_unlinked_callbacks_.add(std::move(callback));
  }

 protected:
  class Runner;

  std::string device_name_;
  uint16_t http_port_{8080};
  speaker::Speaker *media_speaker_{nullptr};
  Runner *runner_{nullptr};
  CallbackManager<void()> spotify_unlinked_callbacks_;
};

class SpotifyUnlinkedTrigger : public Trigger<> {
 public:
  explicit SpotifyUnlinkedTrigger(CSpotPlayer *parent) {
    parent->add_on_spotify_unlinked_callback([this]() { this->trigger(); });
  }
};

}  // namespace cspot_player
}  // namespace esphome
