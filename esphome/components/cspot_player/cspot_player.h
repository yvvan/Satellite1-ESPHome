#pragma once

#include <cstdint>
#include <string>

#include "esphome/core/component.h"

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

 protected:
  class Runner;

  std::string device_name_;
  uint16_t http_port_{8080};
  Runner *runner_{nullptr};
};

}  // namespace cspot_player
}  // namespace esphome
