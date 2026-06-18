from contextlib import asynccontextmanager
from collections.abc import AsyncIterator

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
from redis.asyncio import Redis

from app.api.routes import health, runners
from app.core.config import get_settings
from app.repositories.runner_repository import RunnerRepository
from app.services.mqtt_service import MQTTService
from app.services.runner_service import RunnerService


@asynccontextmanager
async def lifespan(app: FastAPI) -> AsyncIterator[None]:
    settings = get_settings()
    redis_client = Redis.from_url(settings.redis_url, decode_responses=True)
    runner_service = RunnerService(RunnerRepository(redis_client))
    mqtt_service = MQTTService(settings)

    app.state.redis = redis_client
    app.state.mqtt_service = mqtt_service

    try:
        await mqtt_service.connect(
            runner_service.handle_gateway_payload,
            runner_service.handle_emergency_payload,
        )
        yield
    finally:
        await mqtt_service.disconnect()
        await redis_client.aclose()


app = FastAPI(title="IoT FastAPI Server", lifespan=lifespan)
app.add_middleware(
    CORSMiddleware,
    allow_origins=[
        "http://localhost:3000",
        "http://127.0.0.1:3000",
        "https://iot-final.leehan416.dev",
    ],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)
app.include_router(health.router)
app.include_router(runners.router)
app.include_router(health.router, prefix="/api")
app.include_router(runners.router, prefix="/api")
