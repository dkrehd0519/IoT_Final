#include <SPI.h>
#include <LoRa.h>
#include <TinyGPSPlus.h>

// 보드 버전에 따라 수정 필요: T-Beam LoRa 핀
#define LORA_SS 18
#define LORA_RST 23
#define LORA_DIO0 26

// 보드 버전에 따라 수정 필요: T-Beam GPS UART 핀
#define GPS_RX 34
#define GPS_TX 12
#define GPS_BAUD 9600

// Runner 3대에 각각 1, 2, 3 중 고유한 값으로 변경한다.
#define RUNNER_ID 1

#define LORA_FREQUENCY 915E6
#define LORA_SYNC_WORD 0x33
#define LORA_TX_POWER 20
#define LORA_SPREADING_FACTOR 7
#define LORA_BANDWIDTH 125E3
#define LORA_CODING_RATE_DENOMINATOR 5
#define LORA_PREAMBLE_LENGTH 8

#define BEACON_COLLECTION_MS 1500UL
#define PHASE_GUARD_MS 300UL
#define MAX_RELAY_CANDIDATES 2

// GPS fix가 없을 때 packet에 넣을 임시 좌표
#define DUMMY_LAT 36.10321
#define DUMMY_LNG 129.38712

TinyGPSPlus gps;
HardwareSerial GPSserial(1);

enum RunnerState {
  WAIT_BEACON,
  SELECT_RELAY,
  WAIT_MY_SLOT,
  SEND_RUNNER_STATUS,
  CYCLE_DONE
};

struct RelayCandidate {
  int cycleId;
  int relayId;
  int runnerCount;
  int runnerSlotMs;
  int rssi;
  float snr;
  bool valid;
};

RunnerState currentState = WAIT_BEACON;
RelayCandidate relayCandidates[MAX_RELAY_CANDIDATES];

unsigned long firstBeaconTime = 0;
unsigned long sendTime = 0;
int activeCycleId = -1;
int selectedCandidateIndex = -1;
int selectedRelayId = -1;
int selectedRunnerCount = 0;
int selectedRunnerSlotMs = 0;

double lastLat = DUMMY_LAT;
double lastLng = DUMMY_LNG;
bool gpsValid = false;
unsigned long lastValidGpsTime = 0;

unsigned long seq = 1;

void sendLoRaMessage(String msg);
bool receiveLoRaMessage(String &msg, int &rssi, float &snr);
bool startsWithPacket(String msg, String type);
String getField(String msg, int index);
void updateGPS();
int calculatePace();
int readBatteryPercent();
void handleBeaconPacket(String msg, int rssi, float snr);
int selectBestRelay();
void sendRunnerStatus();
void resetBeaconCandidates();
void changeState(RunnerState nextState);
const char *stateName(RunnerState state);

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("========================================");
  Serial.println(" TTGO T-Beam Marathon Runner Node");
  Serial.println("========================================");
  Serial.print("[CONFIG] RUNNER_ID: ");
  Serial.println(RUNNER_ID);

  // 보드 버전에 따라 GPS_RX/GPS_TX 핀 수정이 필요할 수 있다.
  GPSserial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX, GPS_TX);
  Serial.print("[GPS] UART initialized, RX=");
  Serial.print(GPS_RX);
  Serial.print(", TX=");
  Serial.print(GPS_TX);
  Serial.print(", baud=");
  Serial.println(GPS_BAUD);

  // 보드 버전에 따라 LORA_SS/LORA_RST/LORA_DIO0 핀 수정이 필요할 수 있다.
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(LORA_FREQUENCY)) {
    Serial.println("[ERROR] LoRa initialization failed. Check wiring and board pins.");
    while (true) {
      delay(1000);
    }
  }

  LoRa.setSyncWord(LORA_SYNC_WORD);
  LoRa.setTxPower(LORA_TX_POWER);
  LoRa.setSpreadingFactor(LORA_SPREADING_FACTOR);
  LoRa.setSignalBandwidth(LORA_BANDWIDTH);
  LoRa.setCodingRate4(LORA_CODING_RATE_DENOMINATOR);
  LoRa.setPreambleLength(LORA_PREAMBLE_LENGTH);
  LoRa.enableCrc();
  LoRa.receive();

  Serial.println("[LoRa] Initialization complete");
  Serial.println("[LoRa] 915 MHz, SF7, BW125 kHz, CR4/5, SyncWord 0x33, CRC ON");

  resetBeaconCandidates();
  changeState(WAIT_BEACON);
}

void loop() {
  // 상태와 관계없이 GPS UART를 자주 비워 최신 fix를 유지한다.
  updateGPS();

  // BEACON 수집 상태에서만 packet을 꺼낸다.
  // CYCLE_DONE에서 다음 cycle의 첫 BEACON을 실수로 읽고 버리는 것을 방지한다.
  if (currentState == WAIT_BEACON || currentState == SELECT_RELAY) {
    String message;
    int rssi = 0;
    float snr = 0.0;

    if (receiveLoRaMessage(message, rssi, snr)) {
      if (startsWithPacket(message, "BEACON")) {
        handleBeaconPacket(message, rssi, snr);
      } else {
        Serial.println("[RX] Ignored: packet type is not BEACON");
      }
    }
  }

  switch (currentState) {
    case WAIT_BEACON:
      // 첫 유효 BEACON을 받으면 handleBeaconPacket()이 수집 상태로 전환한다.
      break;

    case SELECT_RELAY:
      // 첫 BEACON부터 정해진 시간 동안 같은 cycle의 Relay 후보를 모은다.
      if ((unsigned long)(millis() - firstBeaconTime) >= BEACON_COLLECTION_MS) {
        selectedCandidateIndex = selectBestRelay();

        if (selectedCandidateIndex < 0) {
          Serial.println("[SELECT] No valid BEACON. Skip this cycle.");
          changeState(CYCLE_DONE);
          break;
        }

        RelayCandidate &selected = relayCandidates[selectedCandidateIndex];
        selectedRelayId = selected.relayId;
        selectedRunnerCount = selected.runnerCount;
        selectedRunnerSlotMs = selected.runnerSlotMs;

        Serial.print("[SELECT] Relay ");
        Serial.print(selectedRelayId);
        Serial.print(" selected, RSSI=");
        Serial.print(selected.rssi);
        Serial.print(" dBm, SNR=");
        Serial.print(selected.snr, 2);
        Serial.println(" dB");

        if (RUNNER_ID < 1) {
          Serial.println("[ERROR] RUNNER_ID must start at 1. Transmission skipped.");
          changeState(CYCLE_DONE);
          break;
        }

        if (selectedRunnerCount <= 0 || selectedRunnerSlotMs <= 0) {
          Serial.println("[ERROR] Invalid runner_count or runner_slot_ms. Transmission skipped.");
          changeState(CYCLE_DONE);
          break;
        }

        if (RUNNER_ID > selectedRunnerCount) {
          Serial.print("[WARN] RUNNER_ID ");
          Serial.print(RUNNER_ID);
          Serial.print(" exceeds runner_count ");
          Serial.println(selectedRunnerCount);
          Serial.println("[WARN] No assigned slot; transmission skipped to avoid collision.");
          changeState(CYCLE_DONE);
          break;
        }

        unsigned long slotIndex = (unsigned long)(RUNNER_ID - 1);
        unsigned long mySlotDelay = slotIndex * (unsigned long)selectedRunnerSlotMs;
        sendTime = firstBeaconTime + BEACON_COLLECTION_MS + PHASE_GUARD_MS + mySlotDelay;

        Serial.print("[SLOT] index=");
        Serial.print(slotIndex);
        Serial.print(", slot=");
        Serial.print(selectedRunnerSlotMs);
        Serial.print(" ms, send at millis=");
        Serial.println(sendTime);

        changeState(WAIT_MY_SLOT);
      }
      break;

    case WAIT_MY_SLOT:
      // millis() overflow에도 동작하도록 signed difference로 시간을 비교한다.
      if ((long)(millis() - sendTime) >= 0) {
        changeState(SEND_RUNNER_STATUS);
      }
      break;

    case SEND_RUNNER_STATUS:
      sendRunnerStatus();
      changeState(CYCLE_DONE);
      break;

    case CYCLE_DONE:
      Serial.print("[CYCLE] Cycle ");
      Serial.print(activeCycleId);
      Serial.println(" complete. Waiting for the next BEACON.");
      resetBeaconCandidates();
      changeState(WAIT_BEACON);
      break;
  }
}

void sendLoRaMessage(String msg) {
  // 송신 후 다시 continuous receive mode로 복귀한다.
  LoRa.idle();
  LoRa.beginPacket();
  LoRa.print(msg);

  if (LoRa.endPacket() == 1) {
    Serial.print("[TX] ");
    Serial.println(msg);
  } else {
    Serial.println("[TX][ERROR] LoRa transmission failed");
  }

  LoRa.receive();
}

bool receiveLoRaMessage(String &msg, int &rssi, float &snr) {
  int packetSize = LoRa.parsePacket();
  if (packetSize == 0) {
    return false;
  }

  msg = "";
  while (LoRa.available()) {
    msg += (char)LoRa.read();
  }
  msg.trim();

  rssi = LoRa.packetRssi();
  snr = LoRa.packetSnr();

  Serial.print("[RX] ");
  Serial.print(msg);
  Serial.print(" | RSSI=");
  Serial.print(rssi);
  Serial.print(" dBm, SNR=");
  Serial.print(snr, 2);
  Serial.println(" dB");

  return true;
}

bool startsWithPacket(String msg, String type) {
  msg.trim();
  type.trim();
  return msg == type || msg.startsWith(type + ",");
}

String getField(String msg, int index) {
  if (index < 0) {
    return "";
  }

  int fieldStart = 0;
  int currentIndex = 0;

  for (int i = 0; i <= msg.length(); i++) {
    if (i == msg.length() || msg.charAt(i) == ',') {
      if (currentIndex == index) {
        String field = msg.substring(fieldStart, i);
        field.trim();
        return field;
      }
      currentIndex++;
      fieldStart = i + 1;
    }
  }

  return "";
}

void updateGPS() {
  while (GPSserial.available() > 0) {
    gps.encode(GPSserial.read());
  }

  if (gps.location.isValid()) {
    lastLat = gps.location.lat();
    lastLng = gps.location.lng();
    gpsValid = true;
    lastValidGpsTime = millis();
  } else {
    lastLat = DUMMY_LAT;
    lastLng = DUMMY_LNG;
    gpsValid = false;
  }
}

int calculatePace() {
  // TODO: GPS 이동 거리와 경과 시간을 이용한 실제 초/km 페이스 계산
  int dummyPace = 342;
  return dummyPace;
}

int readBatteryPercent() {
  // TODO: T-Beam 보드 버전에 맞는 ADC/PMU 방식으로 배터리 잔량 측정
  int battery = 78;
  return battery;
}

void handleBeaconPacket(String msg, int rssi, float snr) {
  // BEACON,cycle_id,relay_id,runner_count,runner_slot_ms
  String cycleField = getField(msg, 1);
  String relayField = getField(msg, 2);
  String countField = getField(msg, 3);
  String slotField = getField(msg, 4);

  if (getField(msg, 0) != "BEACON" ||
      cycleField.length() == 0 ||
      relayField.length() == 0 ||
      countField.length() == 0 ||
      slotField.length() == 0 ||
      getField(msg, 5).length() != 0) {
    Serial.println("[BEACON][ERROR] Invalid CSV format");
    return;
  }

  int cycleId = cycleField.toInt();
  int relayId = relayField.toInt();
  int runnerCount = countField.toInt();
  int runnerSlotMs = slotField.toInt();

  if (cycleId < 0 || relayId <= 0 || runnerCount <= 0 || runnerSlotMs <= 0) {
    Serial.println("[BEACON][ERROR] Invalid field value");
    return;
  }

  if (currentState == WAIT_BEACON) {
    resetBeaconCandidates();
    activeCycleId = cycleId;
    firstBeaconTime = millis();
    Serial.print("[BEACON] Collection started for cycle ");
    Serial.println(activeCycleId);
    changeState(SELECT_RELAY);
  }

  if (currentState != SELECT_RELAY) {
    Serial.println("[BEACON] Ignored: current cycle is no longer collecting candidates");
    return;
  }

  if (cycleId != activeCycleId) {
    Serial.print("[BEACON] Ignored: cycle ");
    Serial.print(cycleId);
    Serial.print(" does not match active cycle ");
    Serial.println(activeCycleId);
    return;
  }

  int candidateIndex = -1;
  int emptyIndex = -1;

  for (int i = 0; i < MAX_RELAY_CANDIDATES; i++) {
    if (relayCandidates[i].valid &&
        relayCandidates[i].cycleId == cycleId &&
        relayCandidates[i].relayId == relayId) {
      candidateIndex = i;
      break;
    }

    if (!relayCandidates[i].valid && emptyIndex < 0) {
      emptyIndex = i;
    }
  }

  if (candidateIndex >= 0) {
    RelayCandidate &existing = relayCandidates[candidateIndex];
    if (rssi > existing.rssi || (rssi == existing.rssi && snr > existing.snr)) {
      existing.runnerCount = runnerCount;
      existing.runnerSlotMs = runnerSlotMs;
      existing.rssi = rssi;
      existing.snr = snr;
      Serial.print("[BEACON] Duplicate updated with stronger signal: relay ");
      Serial.println(relayId);
    } else {
      Serial.print("[BEACON] Duplicate kept existing stronger signal: relay ");
      Serial.println(relayId);
    }
    return;
  }

  if (emptyIndex < 0) {
    Serial.print("[BEACON] Candidate list full; relay ");
    Serial.print(relayId);
    Serial.println(" ignored");
    return;
  }

  RelayCandidate &candidate = relayCandidates[emptyIndex];
  candidate.cycleId = cycleId;
  candidate.relayId = relayId;
  candidate.runnerCount = runnerCount;
  candidate.runnerSlotMs = runnerSlotMs;
  candidate.rssi = rssi;
  candidate.snr = snr;
  candidate.valid = true;

  Serial.print("[BEACON] Saved candidate: cycle=");
  Serial.print(cycleId);
  Serial.print(", relay=");
  Serial.print(relayId);
  Serial.print(", runners=");
  Serial.print(runnerCount);
  Serial.print(", slot=");
  Serial.print(runnerSlotMs);
  Serial.println(" ms");
}

int selectBestRelay() {
  int bestIndex = -1;

  for (int i = 0; i < MAX_RELAY_CANDIDATES; i++) {
    if (!relayCandidates[i].valid ||
        relayCandidates[i].cycleId != activeCycleId) {
      continue;
    }

    if (bestIndex < 0 ||
        relayCandidates[i].rssi > relayCandidates[bestIndex].rssi ||
        (relayCandidates[i].rssi == relayCandidates[bestIndex].rssi &&
         relayCandidates[i].snr > relayCandidates[bestIndex].snr)) {
      bestIndex = i;
    }
  }

  return bestIndex;
}

void sendRunnerStatus() {
  if (selectedCandidateIndex < 0 || selectedRelayId <= 0) {
    Serial.println("[TX][ERROR] No selected Relay. Transmission cancelled.");
    return;
  }

  int pace = calculatePace();
  int battery = readBatteryPercent();

  // RUNNER,cycle_id,runner_id,target_relay_id,lat,lng,pace,battery,seq,gps_valid
  String message = "RUNNER,";
  message += String(activeCycleId);
  message += ",";
  message += String(RUNNER_ID);
  message += ",";
  message += String(selectedRelayId);
  message += ",";
  message += String(lastLat, 5);
  message += ",";
  message += String(lastLng, 5);
  message += ",";
  message += String(pace);
  message += ",";
  message += String(battery);
  message += ",";
  message += String(seq);
  message += ",";
  message += gpsValid ? "1" : "0";

  Serial.print("[GPS] valid=");
  Serial.print(gpsValid ? 1 : 0);
  Serial.print(", lat=");
  Serial.print(lastLat, 5);
  Serial.print(", lng=");
  Serial.println(lastLng, 5);

  sendLoRaMessage(message);
  seq++;
}

void resetBeaconCandidates() {
  for (int i = 0; i < MAX_RELAY_CANDIDATES; i++) {
    relayCandidates[i].cycleId = -1;
    relayCandidates[i].relayId = -1;
    relayCandidates[i].runnerCount = 0;
    relayCandidates[i].runnerSlotMs = 0;
    relayCandidates[i].rssi = -999;
    relayCandidates[i].snr = -999.0;
    relayCandidates[i].valid = false;
  }

  firstBeaconTime = 0;
  sendTime = 0;
  activeCycleId = -1;
  selectedCandidateIndex = -1;
  selectedRelayId = -1;
  selectedRunnerCount = 0;
  selectedRunnerSlotMs = 0;
}

void changeState(RunnerState nextState) {
  if (currentState != nextState) {
    Serial.print("[STATE] ");
    Serial.print(stateName(currentState));
    Serial.print(" -> ");
    Serial.println(stateName(nextState));
  }
  currentState = nextState;
}

const char *stateName(RunnerState state) {
  switch (state) {
    case WAIT_BEACON:
      return "WAIT_BEACON";
    case SELECT_RELAY:
      return "SELECT_RELAY";
    case WAIT_MY_SLOT:
      return "WAIT_MY_SLOT";
    case SEND_RUNNER_STATUS:
      return "SEND_RUNNER_STATUS";
    case CYCLE_DONE:
      return "CYCLE_DONE";
    default:
      return "UNKNOWN";
  }
}
