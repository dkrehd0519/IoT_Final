# IoT FastAPI Server

Gateway가 MQTT로 publish하는 Runner 위치 CSV를 받아 Redis에 최신 위치를 저장하는 FastAPI 서버입니다.

## 프로젝트 구조

```text
app/
  main.py                    # FastAPI 앱, lifespan, Redis/MQTT 연결 관리
  core/config.py             # pydantic-settings 환경 설정
  api/routes/health.py       # GET /health
  api/routes/runners.py      # Runner 위치 조회 API
  services/runner_service.py # MQTT payload 파싱 및 저장 로직
  services/mqtt_service.py   # MQTT connect/subscribe
  repositories/runner_repository.py # Redis 저장/조회
  models/runner.py           # Pydantic 모델
tests/                       # pytest 테스트
Dockerfile
docker-compose.yml
pyproject.toml
.env.example
```

## 로컬 실행

Python 3.12와 uv가 필요합니다.

```bash
cd Server
cp .env.example .env
uv sync
uv run --env-file .env uvicorn app.main:app --reload
```

로컬 실행 시 Redis와 MQTT broker가 `localhost:6379`, `localhost:1883`에서 실행 중이어야 합니다.

## Docker 실행

```bash
cd Server
cp .env.example .env
docker compose up --build
```

Docker 환경에서는 app 컨테이너가 `redis`, `mqtt` host 이름으로 각 서비스에 연결합니다.

## API 테스트

Health check:

```bash
curl http://localhost:8000/health
```

Runner 전체 최신 위치 조회:

```bash
curl http://localhost:8000/runners
```

Runner 단건 최신 위치 조회:

```bash
curl http://localhost:8000/runners/7
```

Runner 이동 경로 조회:

```bash
curl http://localhost:8000/api/runners/7/path
```

## MQTT publish 테스트

Docker compose 실행 후 별도 터미널에서 MQTT 메시지를 발행합니다.

```bash
docker compose exec mqtt mosquitto_pub \
  -h localhost \
  -t marathon/gateways/gateway-1/data \
  -m 'FORWARD,1,1,7,36.10321,129.38712,5.42,78,10,-91,7.2'
```

저장 여부는 API로 확인합니다.

```bash
curl http://localhost:8000/runners/7
```

Python 테스트 publisher도 사용할 수 있습니다.

```bash
cd server
uv run python scripts/publish_test_runner.py
```

기본값은 runner 7, 8, 9를 함께 publish합니다. runner 목록을 바꾸려면:

```bash
uv run python scripts/publish_test_runner.py --runner-ids 1,2,3,4
```

## Redis 저장 데이터 확인

```bash
docker compose exec redis redis-cli GET runner:7:location
```

저장 key 형식은 `runner:{runner_id}:location`이며 값은 JSON 문자열입니다.
최신 위치와 경로는 5분 동안 새 데이터가 없으면 자동 만료됩니다.
경로 key 형식은 `runner:{runner_id}:path`입니다.

## 테스트

```bash
cd Server
uv run pytest
```
