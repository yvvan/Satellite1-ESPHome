import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components import esp32, media_player, microphone
from esphome.const import (
    CONF_ID,
    CONF_MEDIA_PLAYER,
    CONF_MICROPHONE,
    CONF_TRIGGER_ID,
    CONF_URL,
)

CODEOWNERS = ["@yvvan"]
DEPENDENCIES = ["microphone", "media_player", "network", "json"]
AUTO_LOAD = ["ring_buffer"]

CONF_DEVICE_ID = "device_id"
CONF_AUTH_TOKEN = "auth_token"
CONF_WAKE_WORD = "wake_word"
CONF_KIND = "kind"
CONF_DETAIL = "detail"

CONF_ON_CONNECTED = "on_connected"
CONF_ON_DISCONNECTED = "on_disconnected"
CONF_ON_LISTENING_START = "on_listening_start"
CONF_ON_LISTENING_STOP = "on_listening_stop"
CONF_ON_SET_LED = "on_set_led"
CONF_ON_MEDIA_PAUSE = "on_media_pause"
CONF_ON_MEDIA_RESUME = "on_media_resume"
CONF_ON_MEDIA_NEXT = "on_media_next"
CONF_ON_SPOTIFY_TOKEN = "on_spotify_token"

ESP_WEBSOCKET_CLIENT_VERSION = "1.5.0"

kineto_voice_ns = cg.esphome_ns.namespace("kineto_voice")
KinetoVoice = kineto_voice_ns.class_("KinetoVoice", cg.Component)

StartAction = kineto_voice_ns.class_(
    "StartAction", automation.Action, cg.Parented.template(KinetoVoice)
)
StopAction = kineto_voice_ns.class_(
    "StopAction", automation.Action, cg.Parented.template(KinetoVoice)
)
SendEventAction = kineto_voice_ns.class_(
    "SendEventAction", automation.Action, cg.Parented.template(KinetoVoice)
)

ConnectedTrigger = kineto_voice_ns.class_("ConnectedTrigger", automation.Trigger.template())
DisconnectedTrigger = kineto_voice_ns.class_(
    "DisconnectedTrigger", automation.Trigger.template()
)
ListeningStartTrigger = kineto_voice_ns.class_(
    "ListeningStartTrigger", automation.Trigger.template()
)
ListeningStopTrigger = kineto_voice_ns.class_(
    "ListeningStopTrigger", automation.Trigger.template()
)
SetLedTrigger = kineto_voice_ns.class_(
    "SetLedTrigger", automation.Trigger.template(cg.std_string)
)
MediaPauseTrigger = kineto_voice_ns.class_(
    "MediaPauseTrigger", automation.Trigger.template()
)
MediaResumeTrigger = kineto_voice_ns.class_(
    "MediaResumeTrigger", automation.Trigger.template()
)
MediaNextTrigger = kineto_voice_ns.class_(
    "MediaNextTrigger", automation.Trigger.template()
)
SpotifyTokenTrigger = kineto_voice_ns.class_(
    "SpotifyTokenTrigger", automation.Trigger.template(cg.std_string)
)


def _validate_ws_url(value):
    value = cv.string_strict(value)
    if not value.startswith(("ws://", "wss://")):
        raise cv.Invalid("url must start with ws:// or wss://")
    return value


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(KinetoVoice),
        cv.Required(CONF_MICROPHONE): cv.use_id(microphone.Microphone),
        cv.Required(CONF_MEDIA_PLAYER): cv.use_id(media_player.MediaPlayer),
        cv.Required(CONF_URL): _validate_ws_url,
        cv.Required(CONF_DEVICE_ID): cv.string,
        cv.Optional(CONF_AUTH_TOKEN, default=""): cv.string,
        cv.Optional(CONF_ON_CONNECTED): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ConnectedTrigger),
            }
        ),
        cv.Optional(CONF_ON_DISCONNECTED): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(DisconnectedTrigger),
            }
        ),
        cv.Optional(CONF_ON_LISTENING_START): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ListeningStartTrigger),
            }
        ),
        cv.Optional(CONF_ON_LISTENING_STOP): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ListeningStopTrigger),
            }
        ),
        cv.Optional(CONF_ON_SET_LED): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(SetLedTrigger),
            }
        ),
        cv.Optional(CONF_ON_MEDIA_PAUSE): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(MediaPauseTrigger),
            }
        ),
        cv.Optional(CONF_ON_MEDIA_RESUME): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(MediaResumeTrigger),
            }
        ),
        cv.Optional(CONF_ON_MEDIA_NEXT): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(MediaNextTrigger),
            }
        ),
        cv.Optional(CONF_ON_SPOTIFY_TOKEN): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(SpotifyTokenTrigger),
            }
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    mic = await cg.get_variable(config[CONF_MICROPHONE])
    cg.add(var.set_microphone(mic))
    mp = await cg.get_variable(config[CONF_MEDIA_PLAYER])
    cg.add(var.set_media_player(mp))

    cg.add(var.set_url(config[CONF_URL]))
    cg.add(var.set_device_id(config[CONF_DEVICE_ID]))
    cg.add(var.set_auth_token(config[CONF_AUTH_TOKEN]))

    for conf in config.get(CONF_ON_CONNECTED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_DISCONNECTED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_LISTENING_START, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_LISTENING_STOP, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_SET_LED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.std_string, "state")], conf)

    for conf in config.get(CONF_ON_MEDIA_PAUSE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_MEDIA_RESUME, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_MEDIA_NEXT, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_SPOTIFY_TOKEN, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.std_string, "token")], conf)

    # WebSocket client comes from the managed ESP-IDF component registry.
    esp32.add_idf_component(
        name="espressif/esp_websocket_client", ref=ESP_WEBSOCKET_CLIENT_VERSION
    )


START_ACTION_SCHEMA = automation.maybe_simple_id(
    {
        cv.GenerateID(): cv.use_id(KinetoVoice),
        cv.Optional(CONF_WAKE_WORD, default=""): cv.templatable(cv.string),
    }
)


@automation.register_action(
    "kineto_voice.start",
    StartAction,
    START_ACTION_SCHEMA,
    synchronous=True,
)
async def kineto_voice_start_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    template_ = await cg.templatable(config[CONF_WAKE_WORD], args, cg.std_string)
    cg.add(var.set_wake_word(template_))
    return var


STOP_ACTION_SCHEMA = automation.maybe_simple_id(
    {
        cv.GenerateID(): cv.use_id(KinetoVoice),
    }
)


@automation.register_action(
    "kineto_voice.stop",
    StopAction,
    STOP_ACTION_SCHEMA,
    synchronous=True,
)
async def kineto_voice_stop_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


SEND_EVENT_ACTION_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(KinetoVoice),
        cv.Required(CONF_KIND): cv.templatable(cv.string),
        cv.Optional(CONF_DETAIL, default=""): cv.templatable(cv.string),
    }
)


@automation.register_action(
    "kineto_voice.send_event",
    SendEventAction,
    SEND_EVENT_ACTION_SCHEMA,
    synchronous=True,
)
async def kineto_voice_send_event_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    kind_ = await cg.templatable(config[CONF_KIND], args, cg.std_string)
    cg.add(var.set_kind(kind_))
    detail_ = await cg.templatable(config[CONF_DETAIL], args, cg.std_string)
    cg.add(var.set_detail(detail_))
    return var
