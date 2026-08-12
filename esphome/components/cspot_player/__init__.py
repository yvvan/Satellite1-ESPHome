from pathlib import Path

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32
from esphome.const import CONF_ID, CONF_NAME, CONF_PORT

CODEOWNERS = ["@yvvan"]
DEPENDENCIES = ["wifi"]

CONF_CSPOT_COMPONENT_PATH = "cspot_component_path"

cspot_player_ns = cg.esphome_ns.namespace("cspot_player")
CSpotPlayer = cspot_player_ns.class_("CSpotPlayer", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(CSpotPlayer),
        # Spotify Connect device name shown in the user's Spotify app.
        cv.Optional(CONF_NAME, default="Kinetik Speaker"): cv.string,
        # Port for the zeroconf credential hand-off HTTP server (LAN only, used
        # while pairing; the official Spotify app POSTs the encrypted blob here).
        cv.Optional(CONF_PORT, default=8080): cv.port,
        # Local path to the cspot fork's esp32-component directory. A path (not a
        # git ref) so spike iterations on the library don't need commits.
        cv.Required(CONF_CSPOT_COMPONENT_PATH): cv.string,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_device_name(config[CONF_NAME]))
    cg.add(var.set_http_port(config[CONF_PORT]))

    esp32.add_idf_component(name="cspot", path=config[CONF_CSPOT_COMPONENT_PATH])

