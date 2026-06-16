# LoRa Marathon Tracking System

ESP32 LoRa 기반 마라톤 참가자 위치 추적 시스템이다. Gateway 1대, Relay 2대,
Runner 3대를 기준으로 동작한다.

현재 구현은 LoRa 구간의 cycle 제어, Relay 선택, time slot 전송, packet buffering,
순차 forwarding까지 포함한다. 서버 및 MQTT 연동과 실제 pace/배터리 측정은 이후
확장 항목이다.

## 시스템 구성

```text
Runner 1 ─┐
Runner 2 ─┼─ LoRa ─> Relay 1 ─┐
Runner 3 ─┘                    ├─ LoRa ─> Gateway
              LoRa ─> Relay 2 ┘
```

| Node | Board | Source |
|---|---|---|
| Runner | TTGO T-Beam ESP32 LoRa GPS | `runner/runner.ino` |
| Relay 1, 2 | TTGO LoRa32-OLED | `relay/relay.ino` |
| Gateway | TTGO LoRa32-OLED | `gateway/gateway.ino` |

Relay 1과 Relay 2는 동일한 소스를 사용하며 `RELAY_ID`만 다르게 설정한다.
Runner 3대도 동일한 소스를 사용하며 `RUNNER_ID`를 각각 1, 2, 3으로 설정한다.

## 필요한 라이브러리

- Sandeep Mistry `LoRa`
- Mikal Hart `TinyGPSPlus`
- Adafruit `Adafruit GFX Library`
- Adafruit `Adafruit SSD1306`

Arduino IDE에서 ESP32 board package도 설치해야 한다.

## 공통 LoRa 설정

| 항목 | 값 |
|---|---:|
| Frequency | 915 MHz |
| Sync Word | `0x33` |
| TX Power | 20 dBm |
| Spreading Factor | 7 |
| Bandwidth | 125 kHz |
| Coding Rate | 4/5 |
| Preamble Length | 8 |
| CRC | Enabled |

모든 Node에서 위 설정이 같아야 통신할 수 있다.

## 핀 설정

### Runner: TTGO T-Beam

```cpp
#define LORA_SS   18
#define LORA_RST  23
#define LORA_DIO0 26

#define GPS_RX   34
#define GPS_TX   12
#define GPS_BAUD 9600
```

### Relay/Gateway: TTGO LoRa32-OLED

```cpp
#define LORA_SS   18
#define LORA_RST  14
#define LORA_DIO0 26

#define OLED_SDA     4
#define OLED_SCL     15
#define OLED_ADDRESS 0x3C
```

T-Beam과 LoRa32 보드 버전에 따라 LoRa, GPS, OLED 핀이 다를 수 있으므로 업로드
전에 사용 중인 보드의 pin map을 확인해야 한다.

## Node 설정

### Runner

Runner 장치마다 `runner/runner.ino`의 값을 다르게 설정한다.

```cpp
#define RUNNER_ID 1  // 각 장치에서 1, 2, 3 사용
```

현재 파일의 기본값은 `1`이다.

### Relay

Relay 장치마다 `relay/relay.ino`의 값을 변경한다.

```cpp
#define RELAY_ID 1  // Relay1
#define RELAY_ID 2  // Relay2
```

현재 작업 파일은 테스트 과정에서 `RELAY_ID 2`로 설정되어 있다.

Runner 없이 Gateway와 Relay만 시험할 때:

```cpp
#define TEST_DUMMY_MODE true
```

실제 Runner를 사용할 때는 두 Relay 모두 다음과 같이 변경한다.

```cpp
#define TEST_DUMMY_MODE false
```

Dummy mode에서는 Runner phase 종료 시 각 Relay가 테스트 데이터 한 건을 생성한다.

| Relay | Dummy runner_id | 위치 | Pace | Battery |
|---|---:|---|---:|---:|
| Relay 1 | 1 | 36.10321, 129.38712 | 342 | 78 |
| Relay 2 | 2 | 36.10390, 129.38800 | 355 | 80 |

### Gateway

현재 Gateway 설정은 Runner 3명 기준이다.

```cpp
#define RELAY_COUNT 2
#define RUNNER_COUNT 3
#define RUNNER_SLOT_MS 1000

#define RELAY1_TIMEOUT_MS 8000UL
#define RELAY2_TIMEOUT_MS 5000UL
#define CYCLE_GUARD_MS 1000UL
```

Runner 수나 slot 길이를 변경하면 Relay phase 길이와 Gateway timeout도 함께 검토해야
한다.

## 상태 머신

### Gateway

```text
START_CYCLE
  -> WAIT_RELAY1_DATA
  -> SEND_RELAY2_START
  -> WAIT_RELAY2_DATA
  -> CYCLE_DONE
  -> START_CYCLE
```

- 새 cycle에서 `SCHEDULE`을 broadcast한다.
- Relay1의 `FORWARD`와 `DONE`을 기다린다.
- Relay1 완료 또는 timeout 후 Relay2에 `SEND_NOW`를 보낸다.
- Relay2 완료 또는 timeout 후 결과를 출력하고 다음 cycle을 시작한다.

### Relay

```text
WAIT_SCHEDULE
  -> SEND_BEACON
  -> COLLECT_RUNNER_DATA
  -> FORWARD_TO_GATEWAY(Relay1)
  -> WAIT_SEND_NOW -> FORWARD_TO_GATEWAY(Relay2)
  -> CYCLE_DONE
  -> WAIT_SCHEDULE
```

- Relay1은 첫 beacon slot, Relay2는 두 번째 beacon slot을 사용한다.
- 자기 `RELAY_ID`가 target인 Runner packet만 저장한다.
- `cycle_id + runner_id + seq`가 같은 packet은 중복으로 제거한다.
- Relay1은 Runner phase 종료 후 바로 forwarding한다.
- Relay2는 Gateway의 `SEND_NOW`를 받은 후 forwarding한다.
- `SEND_NOW`가 일찍 도착하면 저장했다가 Runner phase 종료 후 처리한다.

### Runner

```text
WAIT_BEACON
  -> SELECT_RELAY
  -> WAIT_MY_SLOT
  -> SEND_RUNNER_STATUS
  -> CYCLE_DONE
  -> WAIT_BEACON
```

- 같은 cycle의 Relay beacon을 1.5초 동안 수집한다.
- RSSI가 높은 Relay를 선택하고, RSSI가 같으면 SNR이 높은 Relay를 선택한다.
- `RUNNER_ID - 1`을 slot index로 사용한다.
- GPS fix가 없더라도 dummy 좌표와 `gps_valid=0`으로 전송한다.

## Packet 프로토콜

모든 packet은 UTF-8/ASCII `String` 기반 CSV 형식이다.

### Gateway -> Relay

```text
SCHEDULE,cycle_id,relay_count,runner_count,runner_slot_ms
SEND_NOW,cycle_id,target_relay_id
```

예:

```text
SCHEDULE,70,2,3,1000
SEND_NOW,70,2
```

### Relay -> Runner

```text
BEACON,cycle_id,relay_id,runner_count,runner_slot_ms
```

### Runner -> Relay

```text
RUNNER,cycle_id,runner_id,target_relay_id,lat,lng,pace,battery,seq,gps_valid
```

Relay는 이전 9-field 형식과 `gps_valid`가 추가된 10-field 형식을 모두 수용한다.

### Relay -> Gateway

```text
FORWARD,cycle_id,relay_id,runner_id,lat,lng,pace,battery,seq,rssi,snr
DONE,cycle_id,relay_id,count
```

`FORWARD`의 RSSI/SNR은 Runner packet을 Relay가 수신했을 때 측정한 값이다.
Gateway는 Relay-to-Gateway RSSI/SNR도 별도로 Serial Monitor에 출력한다.

## Runner 3명 기준 타임테이블

Gateway의 `SCHEDULE` 송신 시점을 `T+0ms`로 본다. 실제 무선 송수신 및 loop 처리로
수십 ms 정도 차이가 발생할 수 있다.

| 시각 | 동작 |
|---:|---|
| T+0ms | Gateway가 `SCHEDULE` 송신 |
| T+0ms 부근 | Relay1이 `BEACON` 송신 |
| T+500ms 부근 | Relay2가 `BEACON` 송신 |
| T+1,500ms | Relay 기준 Runner phase 시작 |
| T+1,800ms | Runner 1 전송 |
| T+2,800ms | Runner 2 전송 |
| T+3,800ms | Runner 3 전송 |
| T+4,500ms | Relay Runner phase 종료 |
| T+4,500ms 이후 | Relay1이 buffer를 forwarding하고 `DONE` 송신 |
| Relay1 DONE 직후 | Gateway가 Relay2에 `SEND_NOW` 송신 |
| SEND_NOW 이후 | Relay2가 buffer를 forwarding하고 `DONE` 송신 |
| Cycle 종료 1초 후 | 다음 cycle 시작 |

Runner 전송 시각은 첫 beacon 수신 시각을 기준으로 다음과 같이 계산한다.

```text
sendTime = firstBeaconTime
         + 1500ms beacon collection
         + 300ms guard
         +(RUNNER_ID - 1)* 1000ms
```

Relay는 최대 3개 packet을 400ms 간격으로 forwarding한다. Relay1 8초, Relay2 5초
timeout은 현재 Runner 3명 설정에서 forwarding 시간을 포함하도록 정한 값이다.

## Buffer 및 예외 처리

- Relay buffer 최대 크기: 10개
- Buffer가 가득 차면 추가 Runner packet을 drop한다.
- 다른 cycle 또는 다른 target Relay packet은 drop한다.
- 같은 `cycle_id + runner_id + seq`는 중복으로 drop한다.
- Runner ID가 Gateway의 `runner_count`보다 크면 Runner는 전송하지 않는다.
- Relay1 DONE timeout 시 Gateway는 Relay2 단계로 진행한다.
- Relay2 DONE timeout 시 Gateway는 해당 cycle을 종료한다.
- OLED 초기화가 실패해도 LoRa 기능은 계속 동작한다.

## 테스트 절차

### Gateway + Relay 테스트

1. Gateway를 업로드한다.
2. 첫 Relay에 `RELAY_ID 1`, `TEST_DUMMY_MODE true`를 설정해 업로드한다.
3. 두 번째 Relay에 `RELAY_ID 2`, `TEST_DUMMY_MODE true`를 설정해 업로드한다.
4. Gateway Serial Monitor에서 각 Relay의 `FORWARD=1`, `DONE count=1`,
   `timeout=NO`를 확인한다.

정상 결과 예:

```text
[RESULT] Relay1 FORWARD=1, DONE count=1, timeout=NO
[RESULT] Relay2 FORWARD=1, DONE count=1, timeout=NO
```

Gateway가 Relay의 `BEACON`을 수신한 뒤 `expected FORWARD or DONE`으로 무시하는
로그는 정상이다. BEACON은 Runner 대상 broadcast packet이다.

### 전체 통합 테스트

1. 두 Relay에서 `TEST_DUMMY_MODE false`로 변경한다.
2. Runner 3대의 `RUNNER_ID`를 각각 1, 2, 3으로 설정한다.
3. Gateway 1대, Relay 2대, Runner 3대를 모두 부팅한다.
4. Relay 선택, Runner slot 전송, Relay별 forwarding 결과를 확인한다.

## 현재 미구현 항목

- GPS 이동 거리 기반 실제 pace 계산
- T-Beam 보드별 실제 배터리 잔량 측정
- Gateway의 MQTT/HTTP 서버 전송
- packet ACK 및 재전송
- 영구 저장 및 cycle 통계

현재 pace는 `342`, Runner 배터리는 `78`의 dummy 값을 사용한다. GPS fix가 없으면
좌표 `36.10321, 129.38712`와 `gps_valid=0`을 전송한다.
