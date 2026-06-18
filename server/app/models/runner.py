from datetime import datetime

from pydantic import BaseModel, ConfigDict, Field


class RunnerLocation(BaseModel):
    runner_id: str
    latitude: float
    longitude: float
    pace: float | None = None
    battery: int | None = Field(default=None, ge=0, le=100)
    status: str = "normal"
    received_at: str

    model_config = ConfigDict(frozen=True)

    @property
    def received_timestamp_ms(self) -> float:
        try:
            parsed = datetime.fromisoformat(self.received_at)
            return parsed.timestamp() * 1000
        except ValueError:
            return 0


class RunnerRanking(BaseModel):
    rank: int
    runner_id: str
    distance_m: float
    pace: float | None = None
    battery: int | None = Field(default=None, ge=0, le=100)
    status: str = "normal"
    last_updated: str
