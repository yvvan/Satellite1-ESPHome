#pragma once

#ifdef USE_ESP32

#include "kineto_voice.h"

#include "esphome/core/automation.h"

namespace esphome {
namespace kineto_voice {

template<typename... Ts> class StartAction : public Action<Ts...>, public Parented<KinetoVoice> {
 public:
  TEMPLATABLE_VALUE(std::string, wake_word)

  void play(const Ts &...x) override { this->parent_->start(this->wake_word_.value(x...)); }
};

template<typename... Ts> class StopAction : public Action<Ts...>, public Parented<KinetoVoice> {
 public:
  void play(const Ts &...x) override { this->parent_->stop(); }
};

template<typename... Ts> class SendEventAction : public Action<Ts...>, public Parented<KinetoVoice> {
 public:
  TEMPLATABLE_VALUE(std::string, kind)
  TEMPLATABLE_VALUE(std::string, detail)

  void play(const Ts &...x) override {
    this->parent_->send_event(this->kind_.value(x...), this->detail_.value(x...));
  }
};

class ConnectedTrigger : public Trigger<> {
 public:
  explicit ConnectedTrigger(KinetoVoice *parent) {
    parent->add_on_connected_callback([this]() { this->trigger(); });
  }
};

class DisconnectedTrigger : public Trigger<> {
 public:
  explicit DisconnectedTrigger(KinetoVoice *parent) {
    parent->add_on_disconnected_callback([this]() { this->trigger(); });
  }
};

class ListeningStartTrigger : public Trigger<> {
 public:
  explicit ListeningStartTrigger(KinetoVoice *parent) {
    parent->add_on_listening_start_callback([this]() { this->trigger(); });
  }
};

class ListeningStopTrigger : public Trigger<> {
 public:
  explicit ListeningStopTrigger(KinetoVoice *parent) {
    parent->add_on_listening_stop_callback([this]() { this->trigger(); });
  }
};

class SetLedTrigger : public Trigger<std::string> {
 public:
  explicit SetLedTrigger(KinetoVoice *parent) {
    parent->add_on_set_led_callback([this](const std::string &state) { this->trigger(state); });
  }
};

class TurnFailedTrigger : public Trigger<> {
 public:
  explicit TurnFailedTrigger(KinetoVoice *parent) {
    parent->add_on_turn_failed_callback([this]() { this->trigger(); });
  }
};

class MediaPauseTrigger : public Trigger<> {
 public:
  explicit MediaPauseTrigger(KinetoVoice *parent) {
    parent->add_on_media_pause_callback([this]() { this->trigger(); });
  }
};

class MediaResumeTrigger : public Trigger<> {
 public:
  explicit MediaResumeTrigger(KinetoVoice *parent) {
    parent->add_on_media_resume_callback([this]() { this->trigger(); });
  }
};

class MediaNextTrigger : public Trigger<> {
 public:
  explicit MediaNextTrigger(KinetoVoice *parent) {
    parent->add_on_media_next_callback([this]() { this->trigger(); });
  }
};

}  // namespace kineto_voice
}  // namespace esphome

#endif  // USE_ESP32
