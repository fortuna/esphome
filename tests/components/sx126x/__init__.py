"""Include the packet-transport adapter and real logger callbacks in radio regression tests."""

from esphome.components.logger import request_log_listener
from esphome.loader import FileResource
from esphome.types import ConfigType
from tests.testing_helpers import ComponentManifestOverride


def override_manifest(manifest: ComponentManifestOverride) -> None:
    async def to_code(config: ConfigType) -> None:
        request_log_listener()

    manifest.dependencies = manifest.dependencies + [
        "packet_transport",
        "sensor",
        "binary_sensor",
    ]
    manifest.add_resources(
        FileResource(
            "esphome.components.sx126x", "packet_transport/sx126x_transport.h"
        ),
        FileResource(
            "esphome.components.sx126x", "packet_transport/sx126x_transport.cpp"
        ),
    )
    manifest.multi_conf = False
    manifest.to_code = to_code
