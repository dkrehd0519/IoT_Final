from httpx import ASGITransport, AsyncClient

from app.api.routes.runners import get_runner_service
from app.main import app
from app.models.runner import RunnerLocation, RunnerRanking
from app.services.mqtt_service import MQTTService


class FakeRedis:
    async def ping(self) -> bool:
        return True


class FakeRunnerService:
    async def list_runner_locations(self) -> list[RunnerLocation]:
        return [
            RunnerLocation(
                runner_id="7",
                latitude=36.10321,
                longitude=129.38712,
                pace=5.42,
                battery=78,
                received_at="2026-06-13T00:00:00+00:00",
            )
        ]

    async def get_runner_location(self, runner_id: str) -> RunnerLocation | None:
        if runner_id != "7":
            return None
        return (await self.list_runner_locations())[0]

    async def get_runner_path(self, runner_id: str) -> list[RunnerLocation]:
        if runner_id != "7":
            return []
        return await self.list_runner_locations()

    async def get_runner_rankings(self) -> list[RunnerRanking]:
        return [
            RunnerRanking(
                rank=1,
                runner_id="7",
                distance_m=123.4,
                pace=5.42,
                battery=78,
                status="normal",
                last_updated="2026-06-13T00:00:00+00:00",
            )
        ]


async def test_health() -> None:
    app.state.redis = FakeRedis()
    app.state.mqtt_service = MQTTService.__new__(MQTTService)
    app.state.mqtt_service.is_connected = True

    async with AsyncClient(
        transport=ASGITransport(app=app), base_url="http://test"
    ) as client:
        response = await client.get("/health")

    assert response.status_code == 200
    assert response.json()["redis"] == "connected"
    assert response.json()["mqtt"] == "connected"


async def test_list_runners() -> None:
    app.dependency_overrides[get_runner_service] = lambda: FakeRunnerService()

    async with AsyncClient(
        transport=ASGITransport(app=app), base_url="http://test"
    ) as client:
        response = await client.get("/runners")

    app.dependency_overrides.clear()

    assert response.status_code == 200
    assert response.json()[0]["runner_id"] == "7"


async def test_list_runners_with_api_prefix() -> None:
    app.dependency_overrides[get_runner_service] = lambda: FakeRunnerService()

    async with AsyncClient(
        transport=ASGITransport(app=app), base_url="http://test"
    ) as client:
        response = await client.get("/api/runners")

    app.dependency_overrides.clear()

    assert response.status_code == 200
    assert response.json()[0]["runner_id"] == "7"


async def test_get_runner_404() -> None:
    app.dependency_overrides[get_runner_service] = lambda: FakeRunnerService()

    async with AsyncClient(
        transport=ASGITransport(app=app), base_url="http://test"
    ) as client:
        response = await client.get("/runners/unknown")

    app.dependency_overrides.clear()

    assert response.status_code == 404


async def test_get_runner_path() -> None:
    app.dependency_overrides[get_runner_service] = lambda: FakeRunnerService()

    async with AsyncClient(
        transport=ASGITransport(app=app), base_url="http://test"
    ) as client:
        response = await client.get("/api/runners/7/path")

    app.dependency_overrides.clear()

    assert response.status_code == 200
    assert response.json()[0]["runner_id"] == "7"


async def test_get_runner_rankings() -> None:
    app.dependency_overrides[get_runner_service] = lambda: FakeRunnerService()

    async with AsyncClient(
        transport=ASGITransport(app=app), base_url="http://test"
    ) as client:
        response = await client.get("/api/runners/rankings")

    app.dependency_overrides.clear()

    assert response.status_code == 200
    assert response.json()[0]["rank"] == 1
    assert response.json()[0]["runner_id"] == "7"
