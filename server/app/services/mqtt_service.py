import asyncio
from collections.abc import Awaitable, Callable

import paho.mqtt.client as mqtt

from app.core.config import Settings

MessageHandler = Callable[[str], Awaitable[None]]


class MQTTService:
    def __init__(self, settings: Settings) -> None:
        self._settings = settings
        self._client: mqtt.Client | None = None
        self._data_handler: MessageHandler | None = None
        self._emergency_handler: MessageHandler | None = None
        self._loop: asyncio.AbstractEventLoop | None = None
        self.is_connected = False

    async def connect(
        self,
        data_handler: MessageHandler,
        emergency_handler: MessageHandler | None = None,
    ) -> None:
        self._data_handler = data_handler
        self._emergency_handler = emergency_handler
        self._loop = asyncio.get_running_loop()

        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        if self._settings.mqtt_username:
            client.username_pw_set(
                self._settings.mqtt_username,
                self._settings.mqtt_password,
            )

        client.on_connect = self._on_connect
        client.on_disconnect = self._on_disconnect
        client.on_message = self._on_message
        self._client = client

        await asyncio.to_thread(
            client.connect,
            self._settings.mqtt_host,
            self._settings.mqtt_port,
            self._settings.mqtt_keepalive,
        )
        client.loop_start()

    async def disconnect(self) -> None:
        if self._client is None:
            return

        self._client.loop_stop()
        await asyncio.to_thread(self._client.disconnect)
        self.is_connected = False

    def _on_connect(
        self,
        client: mqtt.Client,
        userdata: object,
        flags: mqtt.ConnectFlags,
        reason_code: mqtt.ReasonCode,
        properties: mqtt.Properties | None,
    ) -> None:
        self.is_connected = reason_code == 0
        if self.is_connected:
            client.subscribe(self._settings.mqtt_data_topic)
            client.subscribe(self._settings.mqtt_emergency_topic)

    def _on_disconnect(
        self,
        client: mqtt.Client,
        userdata: object,
        disconnect_flags: mqtt.DisconnectFlags,
        reason_code: mqtt.ReasonCode,
        properties: mqtt.Properties | None,
    ) -> None:
        self.is_connected = False

    def _on_message(
        self,
        client: mqtt.Client,
        userdata: object,
        message: mqtt.MQTTMessage,
    ) -> None:
        if self._loop is None:
            return

        try:
            payload = message.payload.decode("utf-8")
        except UnicodeDecodeError:
            return

        topic = message.topic
        handler = self._select_handler(topic)
        if handler is None:
            return

        asyncio.run_coroutine_threadsafe(handler(payload), self._loop)

    def _select_handler(self, topic: str) -> MessageHandler | None:
        if mqtt.topic_matches_sub(self._settings.mqtt_emergency_topic, topic):
            return self._emergency_handler
        if mqtt.topic_matches_sub(self._settings.mqtt_data_topic, topic):
            return self._data_handler
        return None
