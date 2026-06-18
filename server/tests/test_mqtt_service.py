from app.core.config import Settings
from app.services.mqtt_service import MQTTService


async def data_handler(payload: str) -> None:
    return None


async def emergency_handler(payload: str) -> None:
    return None


def test_mqtt_service_selects_data_handler() -> None:
    service = MQTTService(Settings())
    service._data_handler = data_handler
    service._emergency_handler = emergency_handler

    selected = service._select_handler("marathon/gateways/1/data")

    assert selected == data_handler


def test_mqtt_service_selects_emergency_handler() -> None:
    service = MQTTService(Settings())
    service._data_handler = data_handler
    service._emergency_handler = emergency_handler

    selected = service._select_handler("marathon/gateways/1/emergency")

    assert selected == emergency_handler
