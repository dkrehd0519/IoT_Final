from app.models.runner import RunnerLocation
from app.repositories.runner_repository import RunnerRepository
from typing import Set


class FakeRedis:
    def __init__(self) -> None:
        self.values: dict[str, str] = {}
        self.sets: dict[str, set[str]] = {}
        self.sorted_sets: dict[str, list[tuple[float, str]]] = {}
        self.expires: dict[str, int] = {}
        self.removed_members: list[tuple[str, str]] = []

    async def set(self, key: str, value: str, ex: int | None = None) -> None:
        self.values[key] = value
        if ex is not None:
            self.expires[key] = ex

    async def get(self, key: str) -> str | None:
        return self.values.get(key)

    async def sadd(self, key: str, *values: str) -> None:
        self.sets.setdefault(key, set()).update(values)

    async def srem(self, key: str, *values: str) -> None:
        for value in values:
            self.sets.setdefault(key, set()).discard(value)
            self.removed_members.append((key, value))

    async def smembers(self, key: str) -> Set[str]:
        return self.sets.get(key, set())

    async def zadd(self, key: str, mapping: dict[str, float]) -> None:
        values = self.sorted_sets.setdefault(key, [])
        for member, score in mapping.items():
            values.append((score, member))
        values.sort(key=lambda item: item[0])

    async def zrange(self, key: str, start: int, end: int) -> list[str]:
        values = self.sorted_sets.get(key, [])
        if end == -1:
            end = len(values) - 1
        return [member for _, member in values[start : end + 1]]

    async def delete(self, *keys: str) -> None:
        for key in keys:
            self.values.pop(key, None)
            self.sorted_sets.pop(key, None)
            self.expires.pop(key, None)

    async def expire(self, key: str, seconds: int) -> None:
        self.expires[key] = seconds


async def test_repository_saves_and_lists_runner_locations() -> None:
    redis = FakeRedis()
    repository = RunnerRepository(redis)
    location = RunnerLocation(
        runner_id="7",
        latitude=36.10321,
        longitude=129.38712,
        pace=5.42,
        battery=78,
        received_at="2026-06-13T00:00:00+00:00",
    )

    await repository.save_location(location)

    assert await repository.get_location("7") == location
    assert await repository.list_locations() == [location]
    assert redis.expires["runner:7:location"] == 300
    assert redis.expires["runner:7:path"] == 300


async def test_repository_returns_runner_path_in_order() -> None:
    redis = FakeRedis()
    repository = RunnerRepository(redis)
    first = RunnerLocation(
        runner_id="7",
        latitude=36.10321,
        longitude=129.38712,
        pace=5.42,
        battery=78,
        received_at="2026-06-13T00:00:00+00:00",
    )
    second = RunnerLocation(
        runner_id="7",
        latitude=36.10331,
        longitude=129.38722,
        pace=5.31,
        battery=77,
        received_at="2026-06-13T00:00:01+00:00",
    )

    await repository.save_location(first)
    await repository.save_location(second)

    assert await repository.get_path("7") == [first, second]


async def test_repository_clears_runner_path() -> None:
    redis = FakeRedis()
    repository = RunnerRepository(redis)
    location = RunnerLocation(
        runner_id="7",
        latitude=36.10321,
        longitude=129.38712,
        received_at="2026-06-13T00:00:00+00:00",
    )

    await repository.save_location(location)
    await repository.clear_path("7")

    assert await repository.get_path("7") == []


async def test_repository_removes_stale_runner_from_set_when_location_expired() -> None:
    redis = FakeRedis()
    repository = RunnerRepository(redis)
    redis.sets["runners"] = {"7"}

    assert await repository.list_locations() == []
    assert ("runners", "7") in redis.removed_members
