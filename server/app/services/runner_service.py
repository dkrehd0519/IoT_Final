import json
import math
from datetime import UTC, datetime

from pydantic import ValidationError

from app.models.runner import RunnerLocation, RunnerRanking
from app.repositories.runner_repository import RunnerRepository

MAX_LOCATION_JUMP_M = 20_000


class RunnerService:
    def __init__(self, repository: RunnerRepository) -> None:
        self._repository = repository

    async def handle_gateway_payload(self, payload: str) -> RunnerLocation | None:
        location = self._parse_forward_payload(payload)
        if location is None:
            return None

        await self._reset_path_if_location_jumped(location)
        await self._repository.save_location(location)
        return location

    async def handle_emergency_payload(self, payload: str) -> RunnerLocation | None:
        location = self._parse_emergency_payload(payload)
        if location is None:
            return None

        await self._reset_path_if_location_jumped(location)
        await self._repository.save_location(location)
        return location

    async def get_runner_location(self, runner_id: str) -> RunnerLocation | None:
        return await self._repository.get_location(runner_id)

    async def list_runner_locations(self) -> list[RunnerLocation]:
        return await self._repository.list_locations()

    async def get_runner_path(self, runner_id: str) -> list[RunnerLocation]:
        return await self._repository.get_path(runner_id)

    async def get_runner_rankings(self) -> list[RunnerRanking]:
        runners = await self._repository.list_locations()
        rankings: list[RunnerRanking] = []

        for runner in runners:
            path = await self._repository.get_path(runner.runner_id)
            rankings.append(
                RunnerRanking(
                    rank=0,
                    runner_id=runner.runner_id,
                    distance_m=round(self._calculate_distance_m(path), 1),
                    pace=runner.pace,
                    battery=runner.battery,
                    status=runner.status,
                    last_updated=runner.received_at,
                )
            )

        rankings.sort(key=lambda ranking: ranking.distance_m, reverse=True)
        return [
            ranking.model_copy(update={"rank": index + 1})
            for index, ranking in enumerate(rankings)
        ]

    def _parse_forward_payload(self, payload: str) -> RunnerLocation | None:
        payload = payload.strip()
        if payload.startswith("{"):
            return self._parse_forward_json(payload)
        return self._parse_forward_csv(payload)

    def _parse_emergency_payload(self, payload: str) -> RunnerLocation | None:
        payload = payload.strip()
        if payload.startswith("{"):
            return self._parse_emergency_json(payload)
        return self._parse_emergency_csv(payload)

    def _parse_forward_csv(self, payload: str) -> RunnerLocation | None:
        fields = [field.strip() for field in payload.split(",")]
        if len(fields) < 8 or fields[0] not in {"FORWARD", "FORWARD_DATA"}:
            return None

        try:
            return RunnerLocation(
                runner_id=fields[3],
                latitude=float(fields[4]),
                longitude=float(fields[5]),
                pace=float(fields[6]),
                battery=int(fields[7]),
                received_at=datetime.now(UTC).isoformat(),
            )
        except (ValueError, ValidationError):
            return None

    def _parse_forward_json(self, payload: str) -> RunnerLocation | None:
        try:
            data = json.loads(payload)
        except json.JSONDecodeError:
            return None

        if not isinstance(data, dict):
            return None

        try:
            return RunnerLocation(
                runner_id=str(data["runner_id"]),
                latitude=float(data["lat"]),
                longitude=float(data["lng"]),
                pace=float(data["pace"]) if data.get("pace") is not None else None,
                battery=int(data["battery"]) if data.get("battery") is not None else None,
                received_at=datetime.now(UTC).isoformat(),
            )
        except (KeyError, TypeError, ValueError, ValidationError):
            return None

    def _parse_emergency_csv(self, payload: str) -> RunnerLocation | None:
        fields = [field.strip() for field in payload.split(",")]
        if len(fields) != 11 or fields[0] != "EMERGENCY_FORWARD":
            return None

        try:
            return RunnerLocation(
                runner_id=fields[3],
                latitude=float(fields[4]),
                longitude=float(fields[5]),
                battery=int(fields[6]),
                status="emergency",
                received_at=datetime.now(UTC).isoformat(),
            )
        except (ValueError, ValidationError):
            return None

    def _parse_emergency_json(self, payload: str) -> RunnerLocation | None:
        try:
            data = json.loads(payload)
        except json.JSONDecodeError:
            return None

        if not isinstance(data, dict):
            return None

        try:
            return RunnerLocation(
                runner_id=str(data["runner_id"]),
                latitude=float(data.get("lat", data.get("latitude"))),
                longitude=float(data.get("lng", data.get("longitude"))),
                pace=float(data["pace"]) if data.get("pace") is not None else None,
                battery=int(data["battery"]) if data.get("battery") is not None else None,
                status="emergency",
                received_at=datetime.now(UTC).isoformat(),
            )
        except (KeyError, TypeError, ValueError, ValidationError):
            return None

    async def _reset_path_if_location_jumped(self, location: RunnerLocation) -> None:
        previous = await self._repository.get_location(location.runner_id)
        if previous is None:
            return

        distance_m = _haversine_m(
            previous.latitude,
            previous.longitude,
            location.latitude,
            location.longitude,
        )
        if distance_m >= MAX_LOCATION_JUMP_M:
            await self._repository.clear_path(location.runner_id)

    @staticmethod
    def _calculate_distance_m(path: list[RunnerLocation]) -> float:
        if len(path) < 2:
            return 0

        total = 0.0
        for previous, current in zip(path, path[1:]):
            total += _haversine_m(
                previous.latitude,
                previous.longitude,
                current.latitude,
                current.longitude,
            )
        return total


def _haversine_m(
    lat1: float,
    lon1: float,
    lat2: float,
    lon2: float,
) -> float:
    radius_m = 6_371_000
    phi1 = math.radians(lat1)
    phi2 = math.radians(lat2)
    delta_phi = math.radians(lat2 - lat1)
    delta_lambda = math.radians(lon2 - lon1)

    a = (
        math.sin(delta_phi / 2) ** 2
        + math.cos(phi1) * math.cos(phi2) * math.sin(delta_lambda / 2) ** 2
    )
    return radius_m * 2 * math.atan2(math.sqrt(a), math.sqrt(1 - a))
