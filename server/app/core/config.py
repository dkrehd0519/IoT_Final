from functools import lru_cache

from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    app_name: str = "IoT FastAPI Server"
    environment: str = "development"
    redis_url: str = "redis://localhost:6379/0"
    mqtt_host: str = "localhost"
    mqtt_port: int = 1883
    mqtt_username: str | None = None
    mqtt_password: str | None = None
    mqtt_keepalive: int = 60
    mqtt_data_topic: str = "marathon/gateways/+/data"
    mqtt_emergency_topic: str = "marathon/gateways/+/emergency"

    model_config = SettingsConfigDict(
        extra="ignore",
    )


@lru_cache
def get_settings() -> Settings:
    return Settings()
