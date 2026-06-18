from app.services.runner_service import RunnerService
from app.models.runner import RunnerLocation


class FakeRepository:
    def __init__(self) -> None:
        self.saved = None
        self.paths: dict[str, list[RunnerLocation]] = {}
        self.cleared_paths: list[str] = []

    async def save_location(self, location):
        self.saved = location

    async def get_location(self, runner_id: str):
        return self.saved if self.saved and self.saved.runner_id == runner_id else None

    async def list_locations(self):
        if self.saved:
            return [self.saved]
        return [path[-1] for path in self.paths.values() if path]

    async def get_path(self, runner_id: str):
        return self.paths.get(runner_id, [])

    async def clear_path(self, runner_id: str):
        self.cleared_paths.append(runner_id)
        self.paths[runner_id] = []


async def test_handle_gateway_payload_saves_forward_location() -> None:
    repository = FakeRepository()
    service = RunnerService(repository)

    location = await service.handle_gateway_payload(
        "FORWARD,1,2,7,36.10321,129.38712,5.42,78,10,-91,7.2"
    )

    assert location is not None
    assert location.runner_id == "7"
    assert location.latitude == 36.10321
    assert location.longitude == 129.38712
    assert repository.saved == location


async def test_handle_gateway_payload_saves_forward_data_location() -> None:
    repository = FakeRepository()
    service = RunnerService(repository)

    location = await service.handle_gateway_payload(
        "FORWARD_DATA,1,2,7,36.10321,129.38712,5.42,78,10,1,-91,7.2"
    )

    assert location is not None
    assert location.runner_id == "7"
    assert location.latitude == 36.10321
    assert location.longitude == 129.38712
    assert repository.saved == location


async def test_handle_gateway_payload_saves_gateway_json_location() -> None:
    repository = FakeRepository()
    service = RunnerService(repository)

    location = await service.handle_gateway_payload(
        '{"cycle_id":1,"relay_id":1,"runner_id":7,"lat":36.10321,'
        '"lng":129.38712,"pace":5.42,"battery":78,"seq":10,'
        '"runner_rssi":-91,"runner_snr":7.2,"gw_rssi":-80,"gw_snr":8.1}'
    )

    assert location is not None
    assert location.runner_id == "7"
    assert location.latitude == 36.10321
    assert location.longitude == 129.38712
    assert repository.saved == location


async def test_handle_gateway_payload_ignores_invalid_payload() -> None:
    repository = FakeRepository()
    service = RunnerService(repository)

    location = await service.handle_gateway_payload("DONE,1,2,3")

    assert location is None
    assert repository.saved is None


async def test_handle_emergency_payload_saves_json_location() -> None:
    repository = FakeRepository()
    service = RunnerService(repository)

    location = await service.handle_emergency_payload(
        '{"emergency_id":1,"relay_id":2,"runner_id":7,"lat":36.10321,'
        '"lng":129.38712,"battery":78,"gps_valid":1,"rssi":-91,"snr":7.2}'
    )

    assert location is not None
    assert location.runner_id == "7"
    assert location.status == "emergency"
    assert location.latitude == 36.10321
    assert location.longitude == 129.38712
    assert repository.saved == location


async def test_handle_emergency_payload_saves_csv_location() -> None:
    repository = FakeRepository()
    service = RunnerService(repository)

    location = await service.handle_emergency_payload(
        "EMERGENCY_FORWARD,1,2,7,36.10321,129.38712,78,2026-06-17T00:00:00Z,1,-91,7.2"
    )

    assert location is not None
    assert location.runner_id == "7"
    assert location.status == "emergency"
    assert location.battery == 78
    assert repository.saved == location


async def test_get_runner_rankings_orders_by_distance() -> None:
    repository = FakeRepository()
    repository.paths = {
        "7": [
            RunnerLocation(
                runner_id="7",
                latitude=36.0,
                longitude=129.0,
                received_at="2026-06-16T00:00:00+00:00",
            ),
            RunnerLocation(
                runner_id="7",
                latitude=36.001,
                longitude=129.0,
                pace=5.2,
                battery=70,
                received_at="2026-06-16T00:00:01+00:00",
            ),
        ],
        "8": [
            RunnerLocation(
                runner_id="8",
                latitude=36.0,
                longitude=129.0,
                received_at="2026-06-16T00:00:00+00:00",
            ),
            RunnerLocation(
                runner_id="8",
                latitude=36.002,
                longitude=129.0,
                pace=5.5,
                battery=80,
                received_at="2026-06-16T00:00:01+00:00",
            ),
        ],
    }
    service = RunnerService(repository)

    rankings = await service.get_runner_rankings()

    assert [ranking.runner_id for ranking in rankings] == ["8", "7"]
    assert rankings[0].rank == 1
    assert rankings[0].distance_m > rankings[1].distance_m


async def test_update_resets_runner_path_when_location_jumps_over_20km() -> None:
    repository = FakeRepository()
    repository.saved = RunnerLocation(
        runner_id="7",
        latitude=36.10321,
        longitude=129.38712,
        received_at="2026-06-16T00:00:00+00:00",
    )
    service = RunnerService(repository)

    location = await service.handle_gateway_payload(
        "FORWARD,1,2,7,36.40321,129.38712,5.42,78,10,-91,7.2"
    )

    assert location is not None
    assert repository.cleared_paths == ["7"]
