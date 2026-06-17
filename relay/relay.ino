#include <SPI.h>
#include <Wire.h>
#include <LoRa.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// 기존 통신 테스트를 통과한 TTGO LoRa32-OLED 환경 설정은 유지한다.
#define LORA_SS 18
#define LORA_RST 14
#define LORA_DIO0 26

#define OLED_SDA 4
#define OLED_SCL 15
#define OLED_ADDRESS 0x3C
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1

// 데모 실행 시 Relay 장치마다 아래 값을 변경한다.
#define DEMO_SCENARIO 1
#define RELAY_ID 1
#define SECTION_ID 2
#define CHANNEL_ID 2
#define TEST_MODE true

#define LORA_FREQUENCY 915E6
#define LORA_SYNC_WORD 0x33
#define LORA_TX_POWER 20
#define LORA_SPREADING_FACTOR 7
#define LORA_BANDWIDTH 125E3
#define LORA_CODING_RATE_DENOMINATOR 5
#define LORA_PREAMBLE_LENGTH 8

// 기존에 검증한 실제 주파수 테이블은 유지하고, 데모 channel id만 논리적으로 매핑한다.
const long CHANNEL_FREQ_HZ[] = {
  915000000,
  916000000,
  917000000,
  918000000,
  919000000,
};

#define TOTAL_RUNNERS 3
#define MAX_ACTIVE_RUNNERS 6
#define MAX_BUFFER_SIZE 8
#define MAX_HANDOVER_REQUESTS 8
#define MAX_PENDING_UPDATES 8
#define SLOT_DURATION_MS 900UL
#define PHASE_GUARD_MS 250UL
#define CYCLE_GUARD_MS 1400UL
#define RESPONSE_GAP_MS 250UL
#define TARGET_RELAY_START_ID 7
#define TARGET_RELAY_COUNT 6
#define DEMO_VIRTUAL_TARGET_GROUP true

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
bool oledReady = false;

enum RelayState {
  BUILD_SLOT_TABLE,
  SEND_SCHEDULE,
  LISTEN_RESERVE_SLOTS,
  LISTEN_REGULAR_SLOTS,
  PROCESS_HANDOVER,
  SEND_HANDOVER_RESPONSE,
  UPDATE_ACTIVE_RUNNER_LIST,
  FORWARD_TO_GATEWAY,
  CYCLE_DONE,
  EMERGENCY_LISTEN
};

struct RunnerData {
  int cycleId;
  int runnerId;
  String lat;
  String lng;
  int pace;
  int battery;
  unsigned long seq;
  int gpsValid;
  int rssi;
  float snr;
};

struct HandoverRequest {
  bool valid;
  int runnerId;
  unsigned long requestTime;
};

struct ResponsePacket {
  bool valid;
  String message;
};

RelayState currentState = BUILD_SLOT_TABLE;

int activeRunners[MAX_ACTIVE_RUNNERS];
int activeRunnerCount = 0;
int pendingAdd[MAX_PENDING_UPDATES];
int pendingAddCount = 0;
int pendingRemove[MAX_PENDING_UPDATES];
int pendingRemoveCount = 0;

RunnerData relayBuffer[MAX_BUFFER_SIZE];
int bufferCount = 0;

HandoverRequest handoverRequests[MAX_HANDOVER_REQUESTS];
int handoverRequestCount = 0;
ResponsePacket responsePackets[MAX_HANDOVER_REQUESTS];
int responsePacketCount = 0;
int responseCursor = 0;
unsigned long nextResponseTime = 0;

unsigned long cycleId = 1;
unsigned long stateStartTime = 0;
unsigned long reservePhaseEndTime = 0;
unsigned long regularPhaseEndTime = 0;

int currentChannelId = CHANNEL_ID;
int currentSectionId = SECTION_ID;
int reserveSlotCount = 0;
int approvedRunnerId = -1;
int nextHandoverRelayPointer = TARGET_RELAY_START_ID;

void showOLED(String line1, String line2 = "", String line3 = "", String line4 = "");
void sendLoRaMessage(String msg);
bool receiveLoRaMessage(String &msg, int &rssi, float &snr);
bool startsWithPacket(String msg, String type);
String getField(String msg, int index);
long getFrequencyByChannel(int channelId);
int getEffectiveChannelId();
int getEffectiveSectionId();
bool isEmergencyRelay();
void initScenarioActiveRunners();
void clearCycleData();
void printActiveRunnerList(String label);
void printSlotTable();
bool containsActiveRunner(int runnerId);
void addActiveRunner(int runnerId);
void removeActiveRunner(int runnerId);
void addPendingAdd(int runnerId);
void addPendingRemove(int runnerId);
String buildSchedulePacket();
void handleRunnerStatus(String msg, int rssi, float snr);
void handleHandoverJoin(String msg);
void handleEmergency(String msg, int rssi, float snr);
void addRunnerDataToBuffer(int cycle, int runnerId, String lat, String lng,
                           int pace, int battery, unsigned long seq,
                           int gpsValid, int rssi, float snr);
void addHandoverRequest(int runnerId, unsigned long requestTime);
void processHandoverRequests();
int getNextHandoverRelayId();
int getNextHandoverChannelId(int nextRelayId);
bool isVirtualTargetRelayId(int relayId);
void prepareAckForRunner(int runnerId, int nextRelayId, int nextChannelId,
                         int reserveSlotId);
void prepareRetryForRunner(int runnerId);
void sendNextHandoverResponseIfNeeded();
void applyPendingActiveListUpdates();
void forwardBufferedPacketsToGateway();
void changeState(RelayState nextState);
const char *stateName(RelayState state);

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("========================================");
  Serial.println(" TTGO LoRa32 Marathon Relay Demo");
  Serial.println("========================================");
  Serial.print("[CONFIG] DEMO_SCENARIO=");
  Serial.println(DEMO_SCENARIO);
  Serial.print("[CONFIG] RELAY_ID=");
  Serial.println(RELAY_ID);
  Serial.println("[CONFIG] LoRa/OLED pin and LoRa settings kept from tested code");

  Wire.begin(OLED_SDA, OLED_SCL);
  oledReady = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS);
  if (oledReady) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
  }

  currentChannelId = getEffectiveChannelId();
  currentSectionId = getEffectiveSectionId();
  reserveSlotCount = (currentSectionId >= 2) ? 6 : 0;

  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  long freq = getFrequencyByChannel(currentChannelId);
  Serial.print("[CHANNEL] logical=");
  Serial.print(currentChannelId);
  Serial.print(", freq=");
  Serial.println(freq);

  if (!LoRa.begin(freq)) {
    Serial.println("[ERROR] LoRa initialization failed. Check wiring and board pins.");
    showOLED("LoRa ERROR", "Relay " + String(RELAY_ID), "Check pins");
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

  initScenarioActiveRunners();
  nextHandoverRelayPointer = TARGET_RELAY_START_ID;

  if (isEmergencyRelay()) {
    changeState(EMERGENCY_LISTEN);
  } else {
    changeState(BUILD_SLOT_TABLE);
  }
}

void loop() {
  String message;
  int rssi = 0;
  float snr = 0.0;

  if (currentState == LISTEN_RESERVE_SLOTS ||
      currentState == LISTEN_REGULAR_SLOTS ||
      currentState == EMERGENCY_LISTEN) {
    if (receiveLoRaMessage(message, rssi, snr)) {
      if (startsWithPacket(message, "HANDOVER_JOIN")) {
        handleHandoverJoin(message);
      } else if (startsWithPacket(message, "RUNNER_STATUS")) {
        handleRunnerStatus(message, rssi, snr);
      } else if (startsWithPacket(message, "EMERGENCY")) {
        handleEmergency(message, rssi, snr);
      } else {
        Serial.println("[RX] Ignored packet for Relay");
      }
    }
  }

  switch (currentState) {
    case BUILD_SLOT_TABLE:
      clearCycleData();
      Serial.println();
      Serial.print("[CYCLE] Relay ");
      Serial.print(RELAY_ID);
      Serial.print(" starting cycle ");
      Serial.println(cycleId);
      printActiveRunnerList("Active Runner List");
      printSlotTable();
      changeState(SEND_SCHEDULE);
      break;

    case SEND_SCHEDULE:
      sendLoRaMessage(buildSchedulePacket());
      stateStartTime = millis();
      reservePhaseEndTime = stateStartTime + PHASE_GUARD_MS +
                            (unsigned long)reserveSlotCount * SLOT_DURATION_MS;
      regularPhaseEndTime = reservePhaseEndTime +
                            (unsigned long)activeRunnerCount * SLOT_DURATION_MS;
      if (reserveSlotCount > 0) {
        changeState(LISTEN_RESERVE_SLOTS);
      } else {
        changeState(LISTEN_REGULAR_SLOTS);
      }
      break;

    case LISTEN_RESERVE_SLOTS:
      if ((long)(millis() - reservePhaseEndTime) >= 0) {
        Serial.println("[PHASE] Reserve slot phase complete");
        changeState(LISTEN_REGULAR_SLOTS);
      }
      break;

    case LISTEN_REGULAR_SLOTS:
      if ((long)(millis() - regularPhaseEndTime) >= 0) {
        Serial.println("[PHASE] Regular runner uplink phase complete");
        changeState(PROCESS_HANDOVER);
      }
      break;

    case PROCESS_HANDOVER:
      processHandoverRequests();
      if (responsePacketCount > 0) {
        responseCursor = 0;
        nextResponseTime = millis();
        changeState(SEND_HANDOVER_RESPONSE);
      } else {
        changeState(UPDATE_ACTIVE_RUNNER_LIST);
      }
      break;

    case SEND_HANDOVER_RESPONSE:
      sendNextHandoverResponseIfNeeded();
      if (responseCursor >= responsePacketCount) {
        changeState(UPDATE_ACTIVE_RUNNER_LIST);
      }
      break;

    case UPDATE_ACTIVE_RUNNER_LIST:
      applyPendingActiveListUpdates();
      changeState(FORWARD_TO_GATEWAY);
      break;

    case FORWARD_TO_GATEWAY:
      forwardBufferedPacketsToGateway();
      changeState(CYCLE_DONE);
      break;

    case CYCLE_DONE:
      if ((unsigned long)(millis() - stateStartTime) >= CYCLE_GUARD_MS) {
        cycleId++;
        changeState(BUILD_SLOT_TABLE);
      }
      break;

    case EMERGENCY_LISTEN:
      break;
  }
}

void showOLED(String line1, String line2, String line3, String line4) {
  if (!oledReady) {
    return;
  }
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println(line1);
  display.println(line2);
  display.println(line3);
  display.println(line4);
  display.display();
}

void sendLoRaMessage(String msg) {
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

long getFrequencyByChannel(int channelId) {
  int index = 0;
  if (channelId == 1) {
    index = 0;
  } else if (channelId == 2) {
    index = 1;
  } else if (channelId == 7) {
    index = 2;
  } else if (channelId == 13) {
    index = 3;
  } else {
    index = (channelId - 1) % 5;
    if (index < 0) {
      index = 0;
    }
  }
  return CHANNEL_FREQ_HZ[index];
}

int getEffectiveChannelId() {
  if (DEMO_SCENARIO == 1) {
    return (RELAY_ID == 2) ? 2 : 1;
  }
  if (DEMO_SCENARIO == 2) {
    return (RELAY_ID == 2) ? 7 : 1;
  }
  if (DEMO_SCENARIO == 3) {
    return (RELAY_ID == 2) ? 13 : 1;
  }
  return CHANNEL_ID;
}

int getEffectiveSectionId() {
  if (DEMO_SCENARIO == 2 && RELAY_ID == 2) {
    return 2;
  }
  return SECTION_ID;
}

bool isEmergencyRelay() {
  return DEMO_SCENARIO == 3 && RELAY_ID == 2;
}

void initScenarioActiveRunners() {
  activeRunnerCount = 0;
  for (int i = 0; i < MAX_ACTIVE_RUNNERS; i++) {
    activeRunners[i] = -1;
  }

  if (DEMO_SCENARIO == 1) {
    if (RELAY_ID == 1) {
      activeRunners[activeRunnerCount++] = 1;
      activeRunners[activeRunnerCount++] = 2;
    } else if (RELAY_ID == 2) {
      activeRunners[activeRunnerCount++] = 3;
    }
  } else if (DEMO_SCENARIO == 2) {
    if (RELAY_ID == 1) {
      activeRunners[activeRunnerCount++] = 1;
      activeRunners[activeRunnerCount++] = 2;
      activeRunners[activeRunnerCount++] = 3;
    }
  } else if (DEMO_SCENARIO == 3) {
    if (RELAY_ID == 1) {
      activeRunners[activeRunnerCount++] = 1;
      activeRunners[activeRunnerCount++] = 2;
      activeRunners[activeRunnerCount++] = 3;
    }
  }
}

void clearCycleData() {
  bufferCount = 0;
  pendingAddCount = 0;
  pendingRemoveCount = 0;
  handoverRequestCount = 0;
  responsePacketCount = 0;
  responseCursor = 0;
  approvedRunnerId = -1;
  for (int i = 0; i < MAX_HANDOVER_REQUESTS; i++) {
    handoverRequests[i].valid = false;
    responsePackets[i].valid = false;
    responsePackets[i].message = "";
  }
}

void printActiveRunnerList(String label) {
  Serial.print("[ACTIVE] ");
  Serial.print(label);
  Serial.print(": ");
  if (activeRunnerCount == 0) {
    Serial.println("empty");
    return;
  }
  for (int i = 0; i < activeRunnerCount; i++) {
    Serial.print("Runner ");
    Serial.print(activeRunners[i]);
    if (i < activeRunnerCount - 1) {
      Serial.print(", ");
    }
  }
  Serial.println();
}

void printSlotTable() {
  Serial.println("[SLOT_TABLE] Regular slots");
  if (activeRunnerCount == 0) {
    Serial.println("  no active runners");
  }
  for (int i = 0; i < activeRunnerCount; i++) {
    Serial.print("  Regular Slot ");
    Serial.print(i + 1);
    Serial.print(" -> Runner ");
    Serial.println(activeRunners[i]);
  }

  if (reserveSlotCount > 0) {
    Serial.println("[SLOT_TABLE] Handover reserve slots");
    for (int i = 0; i < reserveSlotCount; i++) {
      Serial.print("  Reserve Slot ");
      Serial.print(i + 1);
      Serial.print(" -> source Relay ");
      Serial.println(i + 1);
    }
  }
}

bool containsActiveRunner(int runnerId) {
  for (int i = 0; i < activeRunnerCount; i++) {
    if (activeRunners[i] == runnerId) {
      return true;
    }
  }
  return false;
}

void addActiveRunner(int runnerId) {
  if (containsActiveRunner(runnerId)) {
    return;
  }
  if (activeRunnerCount >= MAX_ACTIVE_RUNNERS) {
    Serial.println("[ACTIVE][WARN] activeRunnerList full");
    return;
  }
  activeRunners[activeRunnerCount++] = runnerId;
  Serial.print("[ACTIVE_ADD] Target relay activeRunnerList updated: Runner ");
  Serial.println(runnerId);
}

void removeActiveRunner(int runnerId) {
  for (int i = 0; i < activeRunnerCount; i++) {
    if (activeRunners[i] == runnerId) {
      for (int j = i; j < activeRunnerCount - 1; j++) {
        activeRunners[j] = activeRunners[j + 1];
      }
      activeRunnerCount--;
      activeRunners[activeRunnerCount] = -1;
      Serial.print("[ACTIVE_REMOVE] Source relay activeRunnerList updated: Runner ");
      Serial.println(runnerId);
      return;
    }
  }
}

void addPendingAdd(int runnerId) {
  if (pendingAddCount >= MAX_PENDING_UPDATES) {
    return;
  }
  pendingAdd[pendingAddCount++] = runnerId;
}

void addPendingRemove(int runnerId) {
  if (pendingRemoveCount >= MAX_PENDING_UPDATES) {
    return;
  }
  pendingRemove[pendingRemoveCount++] = runnerId;
}

String buildSchedulePacket() {
  String msg = "SCHEDULE,";
  msg += String(cycleId);
  msg += ",";
  msg += String(RELAY_ID);
  msg += ",";
  msg += String(currentSectionId);
  msg += ",";
  msg += String(currentChannelId);
  msg += ",";
  msg += String(SLOT_DURATION_MS);
  msg += ",";
  msg += String(reserveSlotCount);
  msg += ",";
  msg += String(activeRunnerCount);
  for (int i = 0; i < activeRunnerCount; i++) {
    msg += ",";
    msg += String(activeRunners[i]);
  }
  return msg;
}

void handleRunnerStatus(String msg, int rssi, float snr) {
  int packetCycleId = getField(msg, 1).toInt();
  int runnerId = getField(msg, 2).toInt();
  int targetRelayId = getField(msg, 3).toInt();
  String lat = getField(msg, 4);
  String lng = getField(msg, 5);
  int pace = getField(msg, 6).toInt();
  int battery = getField(msg, 7).toInt();
  unsigned long runnerSeq = (unsigned long)getField(msg, 8).toInt();
  int gpsValid = getField(msg, 9).toInt();
  int handoverRequest = getField(msg, 10).toInt();
  unsigned long requestTime = (unsigned long)getField(msg, 11).toInt();

  if (packetCycleId != (int)cycleId) {
    Serial.println("[RUNNER_STATUS] Dropped: cycle mismatch");
    return;
  }
  if (targetRelayId != RELAY_ID) {
    Serial.println("[RUNNER_STATUS] Dropped: target relay mismatch");
    return;
  }
  if (!containsActiveRunner(runnerId)) {
    Serial.println("[RUNNER_STATUS] Dropped: runner is not in activeRunnerList");
    return;
  }

  Serial.println("[RUNNER_STATUS] Received Runner Packet");
  Serial.print("  runner_id=");
  Serial.println(runnerId);
  Serial.print("  handover_request=");
  Serial.println(handoverRequest);

  addRunnerDataToBuffer(packetCycleId, runnerId, lat, lng, pace, battery,
                        runnerSeq, gpsValid, rssi, snr);

  if (handoverRequest == 1) {
    Serial.print("[HANDOVER] Handover request received from Runner ");
    Serial.println(runnerId);
    addHandoverRequest(runnerId, requestTime);
  }
}

void handleHandoverJoin(String msg) {
  int packetCycleId = getField(msg, 1).toInt();
  int runnerId = getField(msg, 2).toInt();
  int sourceRelayId = getField(msg, 3).toInt();
  int targetRelayId = getField(msg, 4).toInt();

  bool targetMatchesThisRelay = (targetRelayId == RELAY_ID);
  if (DEMO_SCENARIO == 2 && RELAY_ID == 2 && DEMO_VIRTUAL_TARGET_GROUP &&
      isVirtualTargetRelayId(targetRelayId)) {
    targetMatchesThisRelay = true;
  }

  if (!targetMatchesThisRelay) {
    return;
  }

  Serial.println("[HANDOVER_JOIN] received");
  Serial.print("  cycle_id=");
  Serial.println(packetCycleId);
  Serial.print("  runner_id=");
  Serial.println(runnerId);
  Serial.print("  source_relay_id=");
  Serial.println(sourceRelayId);
  Serial.print("  target_relay_id=");
  Serial.println(targetRelayId);
  addPendingAdd(runnerId);
}

void handleEmergency(String msg, int rssi, float snr) {
  if (!isEmergencyRelay()) {
    return;
  }

  int emergencyId = getField(msg, 1).toInt();
  int runnerId = getField(msg, 2).toInt();
  String lat = getField(msg, 3);
  String lng = getField(msg, 4);
  int battery = getField(msg, 5).toInt();
  String timestamp = getField(msg, 6);
  int gpsValid = getField(msg, 7).toInt();

  Serial.println("[EMERGENCY] Emergency packet received. Forwarding to Emergency Gateway.");

  String forward = "EMERGENCY_FORWARD,";
  forward += String(emergencyId);
  forward += ",";
  forward += String(RELAY_ID);
  forward += ",";
  forward += String(runnerId);
  forward += ",";
  forward += lat;
  forward += ",";
  forward += lng;
  forward += ",";
  forward += String(battery);
  forward += ",";
  forward += timestamp;
  forward += ",";
  forward += String(gpsValid);
  forward += ",";
  forward += String(rssi);
  forward += ",";
  forward += String(snr, 2);
  sendLoRaMessage(forward);
}

void addRunnerDataToBuffer(int cycle, int runnerId, String lat, String lng,
                           int pace, int battery, unsigned long runnerSeq,
                           int gpsValid, int rssi, float snr) {
  if (bufferCount >= MAX_BUFFER_SIZE) {
    Serial.println("[BUFFER][WARN] Buffer full");
    return;
  }

  RunnerData &data = relayBuffer[bufferCount++];
  data.cycleId = cycle;
  data.runnerId = runnerId;
  data.lat = lat;
  data.lng = lng;
  data.pace = pace;
  data.battery = battery;
  data.seq = runnerSeq;
  data.gpsValid = gpsValid;
  data.rssi = rssi;
  data.snr = snr;

  Serial.print("[BUFFER] Stored runner packet. count=");
  Serial.println(bufferCount);
}

void addHandoverRequest(int runnerId, unsigned long requestTime) {
  if (handoverRequestCount >= MAX_HANDOVER_REQUESTS) {
    return;
  }
  handoverRequests[handoverRequestCount].valid = true;
  handoverRequests[handoverRequestCount].runnerId = runnerId;
  handoverRequests[handoverRequestCount].requestTime = requestTime;
  handoverRequestCount++;
}

void processHandoverRequests() {
  if (handoverRequestCount == 0) {
    Serial.println("[HANDOVER] No handover requests");
    return;
  }

  if (handoverRequestCount > 1) {
    Serial.println("[HANDOVER] Multiple handover requests detected");
  }

  for (int i = 0; i < handoverRequestCount - 1; i++) {
    for (int j = i + 1; j < handoverRequestCount; j++) {
      if (handoverRequests[j].requestTime < handoverRequests[i].requestTime) {
        HandoverRequest tmp = handoverRequests[i];
        handoverRequests[i] = handoverRequests[j];
        handoverRequests[j] = tmp;
      }
    }
  }

  for (int i = 0; i < handoverRequestCount; i++) {
    int runnerId = handoverRequests[i].runnerId;
    if (i < TARGET_RELAY_COUNT) {
      int nextRelayId = getNextHandoverRelayId();
      int nextChannelId = getNextHandoverChannelId(nextRelayId);
      int reserveSlotId = DEMO_VIRTUAL_TARGET_GROUP ? (i + 1) : RELAY_ID;
      approvedRunnerId = runnerId;
      Serial.print("[HANDOVER] Approved runner ");
      Serial.print(runnerId);
      Serial.print(" -> virtual target relay ");
      Serial.print(nextRelayId);
      Serial.print(", channel ");
      Serial.print(nextChannelId);
      Serial.print(", reserve_slot ");
      Serial.println(reserveSlotId);
      prepareAckForRunner(runnerId, nextRelayId, nextChannelId, reserveSlotId);
      addPendingRemove(runnerId);
    } else {
      Serial.print("[HANDOVER] Retry runner ");
      Serial.println(runnerId);
      prepareRetryForRunner(runnerId);
    }
  }
}

int getNextHandoverRelayId() {
  int target = nextHandoverRelayPointer;
  nextHandoverRelayPointer++;
  if (nextHandoverRelayPointer >= TARGET_RELAY_START_ID + TARGET_RELAY_COUNT) {
    nextHandoverRelayPointer = TARGET_RELAY_START_ID;
  }
  return target;
}

int getNextHandoverChannelId(int nextRelayId) {
  if (DEMO_SCENARIO == 2 && DEMO_VIRTUAL_TARGET_GROUP) {
    return 7;
  }
  return nextRelayId;
}

bool isVirtualTargetRelayId(int relayId) {
  return relayId >= TARGET_RELAY_START_ID &&
         relayId < TARGET_RELAY_START_ID + TARGET_RELAY_COUNT;
}

void prepareAckForRunner(int runnerId, int nextRelayId, int nextChannelId,
                         int reserveSlotId) {
  if (responsePacketCount >= MAX_HANDOVER_REQUESTS) {
    return;
  }

  int validFromCycle = cycleId + 1;
  String msg = "HANDOVER_ACK,";
  msg += String(cycleId);
  msg += ",";
  msg += String(runnerId);
  msg += ",";
  msg += String(RELAY_ID);
  msg += ",";
  msg += String(nextRelayId);
  msg += ",";
  msg += String(nextChannelId);
  msg += ",";
  msg += String(reserveSlotId);
  msg += ",";
  msg += String(validFromCycle);

  responsePackets[responsePacketCount].valid = true;
  responsePackets[responsePacketCount].message = msg;
  responsePacketCount++;
}

void prepareRetryForRunner(int runnerId) {
  if (responsePacketCount >= MAX_HANDOVER_REQUESTS) {
    return;
  }

  String msg = "HANDOVER_RETRY,";
  msg += String(cycleId);
  msg += ",";
  msg += String(runnerId);
  msg += ",";
  msg += String(cycleId + 1);

  responsePackets[responsePacketCount].valid = true;
  responsePackets[responsePacketCount].message = msg;
  responsePacketCount++;
}

void sendNextHandoverResponseIfNeeded() {
  if (responseCursor >= responsePacketCount) {
    return;
  }
  if ((long)(millis() - nextResponseTime) < 0) {
    return;
  }

  String msg = responsePackets[responseCursor].message;
  if (startsWithPacket(msg, "HANDOVER_ACK")) {
    Serial.println("[HANDOVER_ACK] sent");
  } else if (startsWithPacket(msg, "HANDOVER_RETRY")) {
    Serial.println("[HANDOVER_RETRY] sent");
  }
  sendLoRaMessage(msg);
  responseCursor++;
  nextResponseTime = millis() + RESPONSE_GAP_MS;
}

void applyPendingActiveListUpdates() {
  for (int i = 0; i < pendingRemoveCount; i++) {
    removeActiveRunner(pendingRemove[i]);
  }
  for (int i = 0; i < pendingAddCount; i++) {
    addActiveRunner(pendingAdd[i]);
  }
  printActiveRunnerList("Updated Active Runner List");
}

void forwardBufferedPacketsToGateway() {
  String start = "FORWARD_START,";
  start += String(cycleId);
  start += ",";
  start += String(RELAY_ID);
  start += ",";
  start += String(bufferCount);
  sendLoRaMessage(start);

  for (int i = 0; i < bufferCount; i++) {
    RunnerData &data = relayBuffer[i];
    String msg = "FORWARD_DATA,";
    msg += String(cycleId);
    msg += ",";
    msg += String(RELAY_ID);
    msg += ",";
    msg += String(data.runnerId);
    msg += ",";
    msg += data.lat;
    msg += ",";
    msg += data.lng;
    msg += ",";
    msg += String(data.pace);
    msg += ",";
    msg += String(data.battery);
    msg += ",";
    msg += String(data.seq);
    msg += ",";
    msg += String(data.gpsValid);
    msg += ",";
    msg += String(data.rssi);
    msg += ",";
    msg += String(data.snr, 2);
    sendLoRaMessage(msg);
    delay(120);
  }

  String done = "FORWARD_DONE,";
  done += String(cycleId);
  done += ",";
  done += String(RELAY_ID);
  done += ",";
  done += String(bufferCount);
  sendLoRaMessage(done);

  Serial.print("[FORWARD] Forwarded Packet Count=");
  Serial.println(bufferCount);
  showOLED("Relay " + String(RELAY_ID),
           "Cycle " + String(cycleId),
           "Forward " + String(bufferCount),
           "Ch " + String(currentChannelId));
}

void changeState(RelayState nextState) {
  if (currentState == nextState) {
    return;
  }
  Serial.print("[STATE] ");
  Serial.print(stateName(currentState));
  Serial.print(" -> ");
  Serial.println(stateName(nextState));
  currentState = nextState;
  stateStartTime = millis();
}

const char *stateName(RelayState state) {
  switch (state) {
    case BUILD_SLOT_TABLE:
      return "BUILD_SLOT_TABLE";
    case SEND_SCHEDULE:
      return "SEND_SCHEDULE";
    case LISTEN_RESERVE_SLOTS:
      return "LISTEN_RESERVE_SLOTS";
    case LISTEN_REGULAR_SLOTS:
      return "LISTEN_REGULAR_SLOTS";
    case PROCESS_HANDOVER:
      return "PROCESS_HANDOVER";
    case SEND_HANDOVER_RESPONSE:
      return "SEND_HANDOVER_RESPONSE";
    case UPDATE_ACTIVE_RUNNER_LIST:
      return "UPDATE_ACTIVE_RUNNER_LIST";
    case FORWARD_TO_GATEWAY:
      return "FORWARD_TO_GATEWAY";
    case CYCLE_DONE:
      return "CYCLE_DONE";
    case EMERGENCY_LISTEN:
      return "EMERGENCY_LISTEN";
  }
  return "UNKNOWN";
}
