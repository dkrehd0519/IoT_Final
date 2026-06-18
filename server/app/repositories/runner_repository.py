import json
from typing import Protocol, Set

from app.models.runner import RunnerLocation


class RedisLike(Protocol):
    async def set(self, key: str, value: str, ex: int | None = None) -> object:
        ...

    async def get(self, key: str) -> str | bytes | None:
        ...

    async def sadd(self, key: str, *values: str) -> object:
        ...

    async def srem(self, key: str, *values: str) -> object:
        ...

    async def smembers(self, key: str) -> Set[str] | Set[bytes]:
        ...

    async def zadd(self, key: str, mapping: dict[str, float]) -> object:
        ...

    async def zrange(self, key: str, start: int, end: int) -> list[str] | list[bytes]:
        ...

    async def delete(self, *keys: str) -> object:
        ...

    async def expire(self, key: str, seconds: int) -> object:
        ...


class RunnerRepository:
    ACTIVE_TTL_SECONDS = 300

    def __init__(self, redis_client: RedisLike) -> None:
        self._redis = redis_client

    async def save_location(self, location: RunnerLocation) -> None:
        location_json = location.model_dump_json()
        await self._redis.set(
            self._location_key(location.runner_id),
            location_json,
            ex=self.ACTIVE_TTL_SECONDS,
        )
        await self._redis.zadd(
            self._path_key(location.runner_id),
            {location_json: self._score(location)},
        )
        await self._redis.expire(
            self._path_key(location.runner_id),
            self.ACTIVE_TTL_SECONDS,
        )
        await self._redis.sadd("runners", location.runner_id)

    async def get_location(self, runner_id: str) -> RunnerLocation | None:
        value = await self._redis.get(self._location_key(runner_id))
        if value is None:
            return None
        if isinstance(value, bytes):
            value = value.decode("utf-8")

        try:
            return RunnerLocation.model_validate(json.loads(value))
        except (json.JSONDecodeError, ValueError, TypeError):
            return None

    async def list_locations(self) -> list[RunnerLocation]:
        runner_ids = await self._redis.smembers("runners")
        locations: list[RunnerLocation] = []

        for runner_id in sorted(self._decode_runner_ids(runner_ids)):
            location = await self.get_location(runner_id)
            if location is not None:
                locations.append(location)
            else:
                await self._redis.srem("runners", runner_id)

        return locations

    async def get_path(self, runner_id: str) -> list[RunnerLocation]:
        values = await self._redis.zrange(self._path_key(runner_id), 0, -1)
        path: list[RunnerLocation] = []

        for value in values:
            if isinstance(value, bytes):
                value = value.decode("utf-8")
            try:
                path.append(RunnerLocation.model_validate(json.loads(value)))
            except (json.JSONDecodeError, ValueError, TypeError):
                continue

        return path

    async def clear_path(self, runner_id: str) -> None:
        await self._redis.delete(self._path_key(runner_id))

    @staticmethod
    def _location_key(runner_id: str) -> str:
        return f"runner:{runner_id}:location"

    @staticmethod
    def _path_key(runner_id: str) -> str:
        return f"runner:{runner_id}:path"

    @staticmethod
    def _score(location: RunnerLocation) -> float:
        return RunnerLocation.model_validate(location).received_timestamp_ms

    @staticmethod
    def _decode_runner_ids(runner_ids: Set[str] | Set[bytes]) -> list[str]:
        return [
            runner_id.decode("utf-8") if isinstance(runner_id, bytes) else runner_id
            for runner_id in runner_ids
        ]
