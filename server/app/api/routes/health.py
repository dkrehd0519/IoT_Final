from typing import Literal

from fastapi import APIRouter, Request
from pydantic import BaseModel


class HealthResponse(BaseModel):
    status: Literal["ok"]
    redis: Literal["connected", "disconnected"]
    mqtt: Literal["connected", "disconnected"]


router = APIRouter(prefix="/health", tags=["health"])


@router.get("", response_model=HealthResponse)
async def health_check(request: Request) -> HealthResponse:
    redis_status: Literal["connected", "disconnected"] = "disconnected"
    mqtt_status: Literal["connected", "disconnected"] = "disconnected"

    redis_client = getattr(request.app.state, "redis", None)
    if redis_client is not None:
        try:
            await redis_client.ping()
            redis_status = "connected"
        except Exception:
            redis_status = "disconnected"

    mqtt_service = getattr(request.app.state, "mqtt_service", None)
    if getattr(mqtt_service, "is_connected", False):
        mqtt_status = "connected"

    return HealthResponse(status="ok", redis=redis_status, mqtt=mqtt_status)
