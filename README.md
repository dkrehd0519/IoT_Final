# LoRa Marathon Tracking Demo

ESP32 LoRa 기반 마라톤 참가자 위치 추적 시스템의 축소 데모 코드이다. 실제
데모 장비는 Runner 3대, Relay 2대, Gateway 2대를 기준으로 한다.

현재 코드는 기존에 통신 테스트를 통과한 하드웨어 설정을 유지한다. Runner는
`RadioLib` 기반 SX1262 T-Beam 설정을 사용하고, Relay/Gateway는 Sandeep Mistry
`LoRa` 기반 TTGO LoRa32-OLED 설정을 사용한다.

## Source Files

| Node | Source | Board |
|---|---|---|
| Runner | `runner/runner.ino` | TTGO T-Beam ESP32 LoRa GPS |
| Relay | `relay/relay.ino` | TTGO LoRa32-OLED |
| Gateway | `gateway/gateway.ino` | TTGO LoRa32-OLED |

역할은 파일로 분리되어 있다. 각 장치에 업로드할 때는 파일 상단의 ID와
`DEMO_SCENARIO`만 바꾼다.

## Preserved Radio Settings

아래 통신 설정은 기존 테스트를 통과한 값을 유지한다.

### Runner: T-Beam SX1262

```cpp
#define LORA_CS    18
#define LORA_DIO1  33
#define LORA_RST   23
#define LORA_BUSY 32
#define LORA_SCK 5
#define LORA_MISO 19
#define LORA_MOSI 27

#define LORA_SYNC_WORD 0x33
#define LORA_TX_POWER 17
#define LORA_SPREADING_FACTOR 7
#define LORA_BANDWIDTH 125.0
#define LORA_CODING_RATE_DENOMINATOR 5
#define LORA_PREAMBLE_LENGTH 8
```

### Relay/Gateway: LoRa32-OLED

```cpp
#define LORA_SS 18
#define LORA_RST 14
#define LORA_DIO0 26

#define LORA_SYNC_WORD 0x33
#define LORA_TX_POWER 20
#define LORA_SPREADING_FACTOR 7
#define LORA_BANDWIDTH 125E3
#define LORA_CODING_RATE_DENOMINATOR 5
#define LORA_PREAMBLE_LENGTH 8
```

## Logical Channels

코드에서는 demo channel id를 사용하지만, 실제 주파수는 기존에 검증한 5개
frequency table에 매핑한다.

| Logical Channel | Demo Purpose | Current Frequency Mapping |
|---:|---|---:|
| 1 | Normal Relay/Gateway 1 | 915 MHz |
| 2 | Scenario 1 Relay/Gateway 2 | 916 MHz |
| 7 | Scenario 2 handover target | 917 MHz |
| 13 | Scenario 3 emergency | 918 MHz |

## Common Packet Format

모든 packet은 디버깅이 쉬운 CSV `String` 형식이다.

```text
SCHEDULE,cycle_id,relay_id,section_id,channel_id,slot_duration_ms,reserve_slot_count,active_runner_count,runner_id_1,...

RUNNER_STATUS,cycle_id,runner_id,current_relay_id,lat,lng,pace,battery,seq,gps_valid,handover_request,handover_request_time

HANDOVER_ACK,cycle_id,runner_id,source_relay_id,next_relay_id,next_channel,reserve_slot_id,valid_from_cycle

HANDOVER_RETRY,cycle_id,runner_id,retry_cycle

HANDOVER_JOIN,cycle_id,runner_id,source_relay_id,target_relay_id,seq

FORWARD_START,cycle_id,relay_id,count

FORWARD_DATA,cycle_id,relay_id,runner_id,lat,lng,pace,battery,seq,gps_valid,rssi,snr

FORWARD_DONE,cycle_id,relay_id,count

EMERGENCY,emergency_id,runner_id,lat,lng,battery,timestamp,gps_valid

EMERGENCY_FORWARD,emergency_id,relay_id,runner_id,lat,lng,battery,timestamp,gps_valid,rssi,snr
```

## Node Configuration

### Runner

`runner/runner.ino` 상단에서 설정한다.

```cpp
#define RUNNER_ID 1       // 1, 2, 3 중 하나
#define DEMO_SCENARIO 1   // 1, 2, 3 중 하나
#define TEST_MODE true
```

Runner는 `SCHEDULE` 안의 active runner list에 자기 `RUNNER_ID`가 있을 때만
자기 slot에서 `RUNNER_STATUS`를 송신한다.

### Relay

`relay/relay.ino` 상단에서 설정한다.

```cpp
#define DEMO_SCENARIO 1
#define RELAY_ID 1        // 1 또는 2
#define SECTION_ID 1
#define CHANNEL_ID 1
#define TEST_MODE true
```

`DEMO_SCENARIO`와 `RELAY_ID`에 따라 실제 demo channel과 초기 active runner list가
자동으로 정해진다.

### Gateway

`gateway/gateway.ino` 상단에서 설정한다.

```cpp
#define DEMO_SCENARIO 1
#define GATEWAY_ID 1      // 1 또는 2
#define CHANNEL_ID 1
```

Gateway는 cycle을 시작하지 않는다. Relay가 forwarding하는 `FORWARD_*` packet을
수신하고 Serial Monitor에 출력한다.

## State Machines

### Runner

```text
WAIT_SCHEDULE
-> WAIT_MY_SLOT
-> SEND_STATUS
-> WAIT_HANDOVER_RESPONSE
-> WAIT_VALID_CYCLE
-> SEND_HANDOVER_JOIN
-> NORMAL
```

Emergency는 normal state machine보다 우선 처리된다. Emergency 발생 시 현재
channel을 저장하고 Channel 13으로 이동해 `EMERGENCY` packet을 여러 번 전송한 뒤
원래 channel로 돌아온다.

### Relay

```text
BUILD_SLOT_TABLE
-> SEND_SCHEDULE
-> LISTEN_RESERVE_SLOTS
-> LISTEN_REGULAR_SLOTS
-> PROCESS_HANDOVER
-> SEND_HANDOVER_RESPONSE
-> UPDATE_ACTIVE_RUNNER_LIST
-> FORWARD_TO_GATEWAY
-> CYCLE_DONE
```

Scenario 3의 Relay 2는 emergency 전용이므로 `EMERGENCY_LISTEN` 상태로 동작한다.

### Gateway

```text
WAIT_FORWARD_START
-> RECEIVE_FORWARD_DATA
-> WAIT_FORWARD_DONE
-> PRINT_OR_SEND_TO_SERVER
```

Scenario 3의 Gateway 2는 `EMERGENCY_FORWARD`를 수신하면 긴급 알림을 출력한다.

## Timing Table

현재 demo timing은 `relay/relay.ino` 기준이다.

```cpp
#define SLOT_DURATION_MS 900UL
#define PHASE_GUARD_MS 250UL
#define CYCLE_GUARD_MS 1400UL
#define RESPONSE_GAP_MS 250UL
```

Relay가 `SCHEDULE`을 보낸 시점을 `t0`로 본다.

```text
t0
  Relay -> Runner: SCHEDULE

t0 + 250ms
  Reserve Slot Phase 시작

Reserve Slot j:
  t0 + 250ms + (j - 1) * 900ms

Regular Slot i:
  t0 + 250ms + reserve_slot_count * 900ms + i * 900ms

Regular phase end:
  t0 + 250ms + (reserve_slot_count + active_runner_count) * 900ms

Handover response:
  regular phase end 이후 HANDOVER_ACK / HANDOVER_RETRY를 250ms 간격으로 송신

Forwarding:
  FORWARD_START
  FORWARD_DATA x N
  FORWARD_DONE

Cycle guard:
  forwarding 후 1400ms 대기 후 다음 cycle
```

## Demo Scenario 1: Initial Balancing Demo

목표는 Relay별 `activeRunnerList` 기반 dynamic slot allocation을 보여주는 것이다.

### Device Setup

| Device | File | Required Defines |
|---|---|---|
| Runner 1 | `runner/runner.ino` | `DEMO_SCENARIO 1`, `RUNNER_ID 1` |
| Runner 2 | `runner/runner.ino` | `DEMO_SCENARIO 1`, `RUNNER_ID 2` |
| Runner 3 | `runner/runner.ino` | `DEMO_SCENARIO 1`, `RUNNER_ID 3` |
| Relay 1 | `relay/relay.ino` | `DEMO_SCENARIO 1`, `RELAY_ID 1` |
| Relay 2 | `relay/relay.ino` | `DEMO_SCENARIO 1`, `RELAY_ID 2` |
| Gateway 1 | `gateway/gateway.ino` | `DEMO_SCENARIO 1`, `GATEWAY_ID 1` |
| Gateway 2 | `gateway/gateway.ino` | `DEMO_SCENARIO 1`, `GATEWAY_ID 2` |

### Channel and Active List

```text
Relay 1 / Gateway 1 -> Channel 1
Relay 2 / Gateway 2 -> Channel 2

Relay 1 activeRunnerList: Runner 1, Runner 2
Relay 2 activeRunnerList: Runner 3
```

### Expected Flow

```text
Relay 1 sends SCHEDULE containing Runner 1, Runner 2
Relay 2 sends SCHEDULE containing Runner 3
Runner 1 sends RUNNER_STATUS in Relay 1 slot 1
Runner 2 sends RUNNER_STATUS in Relay 1 slot 2
Runner 3 sends RUNNER_STATUS in Relay 2 slot 1
Relay buffers received packets
Relay forwards FORWARD_START / FORWARD_DATA / FORWARD_DONE
Gateway prints received data
```

### Scenario 1 Timetable

Relay 1:

```text
t0          SCHEDULE
t0 + 250ms  Runner 1 regular slot
t0 + 1150ms Runner 2 regular slot
t0 + 2050ms Regular phase end
```

Relay 2:

```text
t0          SCHEDULE
t0 + 250ms  Runner 3 regular slot
t0 + 1150ms Regular phase end
```

### Serial Monitor Checkpoints

```text
Active Runner List
SLOT_TABLE
RUNNER_STATUS Received Runner Packet
BUFFER Stored runner packet
FORWARD Forwarded Packet Count
Gateway Received Data
```

## Demo Scenario 2: Handover Demo

목표는 Runner가 다음 구간 Relay로 넘어갈 때 reserve slot과 active list update를
보여주는 것이다.

### Device Setup

| Device | File | Required Defines |
|---|---|---|
| Runner 1 | `runner/runner.ino` | `DEMO_SCENARIO 2`, `RUNNER_ID 1` |
| Runner 2 | `runner/runner.ino` | `DEMO_SCENARIO 2`, `RUNNER_ID 2` |
| Runner 3 | `runner/runner.ino` | `DEMO_SCENARIO 2`, `RUNNER_ID 3` |
| Relay 1 | `relay/relay.ino` | `DEMO_SCENARIO 2`, `RELAY_ID 1` |
| Relay 2 | `relay/relay.ino` | `DEMO_SCENARIO 2`, `RELAY_ID 2` |
| Gateway 1 | `gateway/gateway.ino` | `DEMO_SCENARIO 2`, `GATEWAY_ID 1` |
| Gateway 2 | `gateway/gateway.ino` | `DEMO_SCENARIO 2`, `GATEWAY_ID 2` |

### Channel and Active List

```text
Relay 1 / Gateway 1 -> Section 1, Channel 1
Relay 2 / Gateway 2 -> Section 2, Channel 7

Initial Relay 1 activeRunnerList: Runner 1, Runner 2, Runner 3
Initial Relay 2 activeRunnerList: empty
```

실제 24 Relay 확장 구조에서는 Section 2의 target Relay가 Relay 7~12이고, 각 target
Relay가 Channel 7~12를 각각 사용한다. 이 경우 source Relay 1은 한 cycle에 최대
6명을 승인할 수 있다.

```text
Approved 1 -> Relay 7,  Channel 7,  source Relay 1 reserve slot
Approved 2 -> Relay 8,  Channel 8,  source Relay 1 reserve slot
Approved 3 -> Relay 9,  Channel 9,  source Relay 1 reserve slot
Approved 4 -> Relay 10, Channel 10, source Relay 1 reserve slot
Approved 5 -> Relay 11, Channel 11, source Relay 1 reserve slot
Approved 6 -> Relay 12, Channel 12, source Relay 1 reserve slot
Approved 7 -> retry
```

현재 2 Relay 데모에서는 실제 Relay 7~12를 모두 띄울 수 없으므로 Relay 2가
가상 target group 7~12를 대표한다. 데모 충돌을 피하기 위해 Channel 7에서 reserve
slot 1~6을 사용해 `HANDOVER_JOIN`을 순차 수신한다.

### Expected Flow

```text
Runner 1 and Runner 2 send handover_request=1 from Relay 1
after their accumulated GPS distance reaches 100m
Relay 1 receives multiple handover requests
Relay 1 can approve up to 6 earliest requests in one cycle
Relay 1 sends HANDOVER_ACK to Runner 1
Relay 1 sends HANDOVER_ACK to Runner 2 because target capacity remains
Runner 1 and Runner 2 switch to Channel 7 in this 2 Relay demo
Runner 1 sends HANDOVER_JOIN in Relay 2 reserve slot 1 on the next valid cycle
Runner 2 sends HANDOVER_JOIN in Relay 2 reserve slot 2 on the next valid cycle
Relay 2 adds Runner 1 and Runner 2 to pendingAddRunnerList
Relay 1 removes Runner 1 and Runner 2 from activeRunnerList at cycle update
Final Relay 1 activeRunnerList: Runner 3
Final Relay 2 activeRunnerList: Runner 1, Runner 2
```

Scenario 2의 handover request 기준은 Runner 코드의 아래 값이다.

```cpp
#define HANDOVER_REQUEST_DISTANCE_M 100.0f
```

GPS 이동거리 누적값 `totalDistanceM`이 100m 이상이 되면 Runner 1, Runner 2가
handover request를 보낸다. GPS fix가 없고 `TEST_MODE` dummy 좌표만 사용하는 경우
거리 누적이 자동으로 늘지 않으므로, 실제 이동 테스트 또는 별도 거리 시뮬레이션이
필요하다.

### Scenario 2 Timetable

Relay 1:

```text
t0          SCHEDULE
t0 + 250ms  Runner 1 regular slot
t0 + 1150ms Runner 2 regular slot
t0 + 2050ms Runner 3 regular slot
t0 + 2950ms Regular phase end
afterward   HANDOVER_ACK / HANDOVER_RETRY
```

Relay 2:

```text
t0          SCHEDULE
t0 + 250ms  Reserve Slot 1, source Relay 1
t0 + 1150ms Reserve Slot 2
t0 + 2050ms Reserve Slot 3
t0 + 2950ms Reserve Slot 4
t0 + 3850ms Reserve Slot 5
t0 + 4750ms Reserve Slot 6
t0 + 5650ms Reserve phase end
```

### Serial Monitor Checkpoints

```text
Handover request received
Multiple handover requests detected
Approved runner
Retry runner
HANDOVER_ACK sent
HANDOVER_RETRY sent
HANDOVER_JOIN received
Source relay activeRunnerList updated
Target relay activeRunnerList updated
```

## Demo Scenario 3: Emergency Demo

목표는 emergency packet이 일반 위치 packet과 분리되어 Channel 13으로 전송되는 것을
보여주는 것이다.

### Device Setup

| Device | File | Required Defines |
|---|---|---|
| Runner 1 | `runner/runner.ino` | `DEMO_SCENARIO 3`, `RUNNER_ID 1` |
| Runner 2 | `runner/runner.ino` | `DEMO_SCENARIO 3`, `RUNNER_ID 2` |
| Runner 3 | `runner/runner.ino` | `DEMO_SCENARIO 3`, `RUNNER_ID 3` |
| Relay 1 | `relay/relay.ino` | `DEMO_SCENARIO 3`, `RELAY_ID 1` |
| Relay 2 | `relay/relay.ino` | `DEMO_SCENARIO 3`, `RELAY_ID 2` |
| Gateway 1 | `gateway/gateway.ino` | `DEMO_SCENARIO 3`, `GATEWAY_ID 1` |
| Gateway 2 | `gateway/gateway.ino` | `DEMO_SCENARIO 3`, `GATEWAY_ID 2` |

### Channel Roles

```text
Normal:
Relay 1 / Gateway 1 -> Channel 1

Emergency:
Relay 2 / Gateway 2 -> Channel 13
```

### Expected Flow

```text
Runner sends normal RUNNER_STATUS on Channel 1
Emergency button or TEST_MODE trigger fires
Runner saves current channel
Runner switches to Channel 13
Runner sends EMERGENCY packet 3 times
Emergency Relay receives EMERGENCY
Emergency Relay forwards EMERGENCY_FORWARD
Emergency Gateway prints alert
Runner returns to the original normal channel
```

`TEST_MODE`에서는 Runner 1이 boot 후 약 12초 뒤 emergency를 자동 trigger한다.

### Emergency Serial Output

Gateway 2 Serial Monitor에서 아래 형식을 확인한다.

```text
========== EMERGENCY ALERT ==========
Emergency ID:
Runner ID:
Relay ID:
Latitude:
Longitude:
Battery:
Timestamp:
GPS Valid:
RSSI:
SNR:
=====================================
```

## Load and Collision Notes

현재 demo code의 Relay hard limit:

```text
MAX_ACTIVE_RUNNERS = 6
MAX_BUFFER_SIZE = 8
```

현재 slot 기준:

```text
RUNNER_STATUS packet airtime ~= 120~130ms
SLOT_DURATION_MS = 900ms
slot utilization ~= 14%
```

따라서 3 Runner demo에서는 regular slot collision 가능성이 낮다. 다만 아래 경우에는
collision이 발생할 수 있다.

```text
같은 physical frequency에서 여러 Relay가 동시에 송신
Runner가 SCHEDULE을 늦게 받거나 slot timing이 어긋남
activeRunnerList에 중복 Runner ID가 들어감
Emergency packet이 여러 Runner에서 동시에 발생
```

Emergency는 ALOHA 방식으로 즉시 송신, random backoff, 반복 전송을 사용한다.
3 Runner demo에서는 충분히 잘 도착할 가능성이 높지만, 대규모 동시 emergency에서는
backoff 폭과 재전송 횟수를 늘리는 것이 좋다.

현재 Runner emergency 설정:

```cpp
#define EMERGENCY_TX_TOTAL_COUNT 3
#define EMERGENCY_RETRY_INTERVAL_MS 650UL
#define EMERGENCY_BACKOFF_MAX_MS 150UL
```

대규모 운영 권장 예:

```cpp
#define EMERGENCY_TX_TOTAL_COUNT 5
#define EMERGENCY_RETRY_INTERVAL_MS 1000UL
#define EMERGENCY_BACKOFF_MAX_MS 2000UL
```

## Wave Operation Recommendation

대규모 참가자 운영에서는 전체 인원보다 Relay당 active runner 수를 기준으로 wave를
판단하는 것이 좋다.

```text
Relay당 30명 이하: 안정적
Relay당 30~50명: 현실적 상한
Relay당 60명 이상: cycle 지연 증가
Relay당 100명 이상: wave 또는 slot 재설계 필요
```

실서비스 확장 시 추천 정책:

```text
WAVE_ENABLE_THRESHOLD_TOTAL_RUNNERS = 300
WAVE_SIZE = 100~150
WAVE_INTERVAL_SEC = 120
RELAY_TARGET_ACTIVE_RUNNERS = 40
RELAY_MAX_ACTIVE_RUNNERS = 60

HANDOVER_PREPARE_DISTANCE_M = 8500
HANDOVER_REQUEST_DISTANCE_M = 9500
HANDOVER_FALLBACK_TIME_SEC = 30 * 60
```

구간 2 handover 판단은 5km보다 9~9.5km 기준이 자연스럽다. 모든 wave가 도착할
때까지 handover를 막는 방식은 피하고, wave별 handover window와 Relay별 승인 quota로
부하를 제어하는 것이 좋다.

현재 Scenario 2 데모 코드는 짧은 테스트를 위해 `HANDOVER_REQUEST_DISTANCE_M`을
100m로 낮춰둔 상태이다.

## Quick Upload Checklist

1. 업로드할 파일을 연다: `runner/runner.ino`, `relay/relay.ino`, `gateway/gateway.ino`
2. `DEMO_SCENARIO`를 모든 장치에서 같은 값으로 맞춘다.
3. Runner는 `RUNNER_ID`를 1, 2, 3으로 각각 설정한다.
4. Relay는 `RELAY_ID`를 1, 2로 각각 설정한다.
5. Gateway는 `GATEWAY_ID`를 1, 2로 각각 설정한다.
6. Serial Monitor baud rate는 `115200`으로 연다.
7. Relay Serial에서 `Active Runner List`, `SLOT_TABLE`, `Forwarded Packet Count`를 확인한다.
8. Gateway Serial에서 `Gateway Received Data` 또는 `EMERGENCY ALERT`를 확인한다.

## Serial Monitor Demo Mode

시연 영상 촬영을 위해 기본 Serial Monitor 출력은 핵심 이벤트 위주로 한국어로 표시한다.
원문 packet이나 상태 전환 로그가 필요하면 각 `.ino` 파일 상단의 값을 `true`로 바꾼다.

```cpp
#define SHOW_RAW_PACKETS false
#define SHOW_STATE_LOG false
#define SHOW_DEBUG_LOG false
```

기본값 `false`에서는 다음 정보만 주로 보인다.

```text
Relay: Cycle 시작, active runner list, slot 배정표, Runner 상태 수신, handover 승인, Gateway 전달 개수
Runner: slot 배정, 상태 전송, handover 승인/JOIN, 긴급 전송
Gateway: 수신 시작, Runner 데이터, cycle 요약, 긴급 상황 알림
```
