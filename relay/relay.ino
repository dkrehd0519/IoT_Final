#include <SPI.h>
#include <Wire.h>
#include <LoRa.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// TTGO LoRa32-OLED 보드 버전에 따라 핀 수정이 필요할 수 있다.
#define LORA_SS 18
#define LORA_RST 14
#define LORA_DIO0 26

// TTGO LoRa32-OLED 보드 버전에 따라 I2C/OLED 설정 수정이 필요할 수 있다.
#define OLED_SDA 4
#define OLED_SCL 15
#define OLED_ADDRESS 0x3C
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1

// Relay1은 1, Relay2는 2로 설정한다.
#define RELAY_ID 1

// Runner 없이 Gateway-Relay 통신을 시험할 때 true로 설정한다.
#define TEST_DUMMY_MODE false
#define DUMMY_GATEWAY_RECEIVE_MODE true

#define LORA_FREQUENCY 915E6
#define LORA_SYNC_WORD 0x33
#define LORA_TX_POWER 20
#define LORA_SPREADING_FACTOR 7
#define LORA_BANDWIDTH 125E3
#define LORA_CODING_RATE_DENOMINATOR 5
#define LORA_PREAMBLE_LENGTH 8

#define RELAY_BEACON_SLOT_MS 500UL
#define PHASE_GUARD_MS 500UL
#define SEND_NOW_TIMEOUT_MS 10000UL
#define MAX_BUFFER_SIZE 10
#define MAX_RUNNERS 20
#define FORWARD_INTERVAL_MS 400UL

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
bool oledReady = false;

enum RelayState {
  WAIT_SCHEDULE,
  SEND_BEACON,
  COLLECT_RUNNER_DATA,
  FORWARD_TO_GATEWAY,
  WAIT_SEND_NOW,
  CYCLE_DONE
};

struct RunnerData {
  int cycleId;
  int relayId;
  int runnerId;
  String lat;
  String lng;
  int pace;
  int battery;
  char status;
  int seq;
  int rssi;
  float snr;
};

RelayState currentState = WAIT_SCHEDULE;
RunnerData relayBuffer[MAX_BUFFER_SIZE];

int bufferCount = 0;
int activeCycleId = -1;
int relayCount = 0;
int runnerCount = 0;
int runnerSlotMs = 0;

unsigned long scheduleReceivedTime = 0;
unsigned long beaconSendTime = 0;
unsigned long runnerPhaseEndTime = 0;
unsigned long stateStartTime = 0;

// Gateway timeout 등의 이유로 SEND_NOW가 Runner phase 중 먼저 온 경우 기억한다.
bool pendingSendNow = false;

void showOLED(String line1, String line2 = "", String line3 = "", String line4 = "");
void sendLoRaMessage(String msg);
bool receiveLoRaMessage(String &msg, int &rssi, float &snr);
bool startsWithPacket(String msg, String type);
String getField(String msg, int index);
void parseSchedule(String msg);
void sendBeaconPacket();
bool isDuplicateRunnerPacket(int cycleId, int runnerId, int seq);
void addRunnerDataToBuffer(int cycleId, int relayId, int runnerId,
                           String lat, String lng, int pace, int battery,
                           char status, int seq, int rssi, float snr);
void handleRunnerPacket(String msg, int rssi, float snr);
void forwardBufferedPacketsToGateway();
void sendDonePacket();
void handleSendNowPacket(String msg);
void addDummyRunnerData();
void clearRelayBuffer();
void changeState(RelayState nextState);
const char *stateName(RelayState state);
void updateOLED();
bool emergencyForwarded[MAX_RUNNERS + 1] = {false};
void sendEmergencyPacketToGateway(int cycleId, int runnerId,
                                  String lat, String lng,
                                  int pace, int battery,
                                  char status, int seq,
                                  int rssi, float snr);

void dummyGatewayReceive(String msg);
void dummyGatewayHandleDone(String msg);

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("========================================");
  Serial.println(" TTGO LoRa32 Marathon Relay Node");
  Serial.println("========================================");
  Serial.print("[CONFIG] RELAY_ID: ");
  Serial.println(RELAY_ID);
  Serial.print("[CONFIG] TEST_DUMMY_MODE: ");
  Serial.println(TEST_DUMMY_MODE ? "true" : "false");

  if (RELAY_ID != 1 && RELAY_ID != 2) {
    Serial.println("[ERROR] RELAY_ID must be 1 or 2.");
    while (true) {
      delay(1000);
    }
  }

  Wire.begin(OLED_SDA, OLED_SCL);
  oledReady = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS);

  if (oledReady) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    showOLED("Relay Boot", "Relay ID: " + String(RELAY_ID),
             "OLED ready", "LoRa init...");
    Serial.println("[OLED] Initialization complete");
  } else {
    Serial.println("[OLED][WARN] Initialization failed. Relay continues without OLED.");
  }

  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(LORA_FREQUENCY)) {
    Serial.println("[ERROR] LoRa initialization failed. Check wiring and board pins.");
    showOLED("LoRa ERROR", "Relay: " + String(RELAY_ID),
             "Check pins", "Relay stopped");

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

  clearRelayBuffer();
  currentState = WAIT_SCHEDULE;
  stateStartTime = millis();
  updateOLED();
}

void loop() {
  // 현재 상태에 필요한 packet만 처리한다.
  if (currentState == WAIT_SCHEDULE ||
      currentState == COLLECT_RUNNER_DATA ||
      currentState == WAIT_SEND_NOW) {
    String message;
    int rssi = 0;
    float snr = 0.0;

    if (receiveLoRaMessage(message, rssi, snr)) {
      if (currentState == WAIT_SCHEDULE &&
          startsWithPacket(message, "SCHEDULE")) {
        parseSchedule(message);
      } else if (currentState == COLLECT_RUNNER_DATA &&
                 startsWithPacket(message, "RUNNER")) {
        handleRunnerPacket(message, rssi, snr);
      } else if ((currentState == COLLECT_RUNNER_DATA ||
                  currentState == WAIT_SEND_NOW) &&
                 startsWithPacket(message, "SEND_NOW")) {
        handleSendNowPacket(message);
      } else {
        Serial.print("[RX] Ignored in state ");
        Serial.println(stateName(currentState));
      }
    }
  }

  switch (currentState) {
    case WAIT_SCHEDULE:
      break;

    case SEND_BEACON:
      if ((long)(millis() - beaconSendTime) >= 0) {
        sendBeaconPacket();
        changeState(COLLECT_RUNNER_DATA);
      }
      break;

    case COLLECT_RUNNER_DATA:
      if ((long)(millis() - runnerPhaseEndTime) >= 0) {
        Serial.println("[RUNNER] Runner collection phase complete");

        if (TEST_DUMMY_MODE) {
          addDummyRunnerData();
        }

        if (RELAY_ID == 1) {
          changeState(FORWARD_TO_GATEWAY);
        } else if (pendingSendNow) {
          Serial.println("[CONTROL] Previously received SEND_NOW is valid");
          changeState(FORWARD_TO_GATEWAY);
        } else {
          changeState(WAIT_SEND_NOW);
        }
      }
      break;

    case FORWARD_TO_GATEWAY:
      forwardBufferedPacketsToGateway();
      sendDonePacket();
      changeState(CYCLE_DONE);
      break;

    case WAIT_SEND_NOW:
      if ((unsigned long)(millis() - stateStartTime) >= SEND_NOW_TIMEOUT_MS) {
        Serial.print("[TIMEOUT] SEND_NOW not received within ");
        Serial.print(SEND_NOW_TIMEOUT_MS);
        Serial.println(" ms. Abandoning this cycle.");
        changeState(CYCLE_DONE);
      }
      break;

    case CYCLE_DONE:
      Serial.print("[CYCLE] Cycle ");
      Serial.print(activeCycleId);
      Serial.println(" complete. Waiting for next SCHEDULE.");
      activeCycleId = -1;
      relayCount = 0;
      runnerCount = 0;
      runnerSlotMs = 0;
      pendingSendNow = false;
      changeState(WAIT_SCHEDULE);
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

    if (DUMMY_GATEWAY_RECEIVE_MODE) {
      dummyGatewayReceive(msg);
    }

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

void parseSchedule(String msg) {
  // SCHEDULE,cycle_id,relay_count,runner_count,runner_slot_ms
  String cycleField = getField(msg, 1);
  String relayCountField = getField(msg, 2);
  String runnerCountField = getField(msg, 3);
  String runnerSlotField = getField(msg, 4);

  if (getField(msg, 0) != "SCHEDULE" ||
      cycleField.length() == 0 ||
      relayCountField.length() == 0 ||
      runnerCountField.length() == 0 ||
      runnerSlotField.length() == 0 ||
      getField(msg, 5).length() != 0) {
    Serial.println("[SCHEDULE][ERROR] Invalid CSV format");
    return;
  }

  int parsedCycleId = cycleField.toInt();
  int parsedRelayCount = relayCountField.toInt();
  int parsedRunnerCount = runnerCountField.toInt();
  int parsedRunnerSlotMs = runnerSlotField.toInt();

  if (parsedCycleId < 0 ||
      parsedRelayCount <= 0 ||
      RELAY_ID > parsedRelayCount ||
      parsedRunnerCount <= 0 ||
      parsedRunnerSlotMs <= 0) {
    Serial.println("[SCHEDULE][ERROR] Invalid field value");
    return;
  }

  clearRelayBuffer();
  activeCycleId = parsedCycleId;
  relayCount = parsedRelayCount;
  runnerCount = parsedRunnerCount;
  runnerSlotMs = parsedRunnerSlotMs;
  pendingSendNow = false;
  scheduleReceivedTime = millis();

  // Relay ID가 1부터 시작하므로 Relay1은 slot 0, Relay2는 slot 1이다.
  beaconSendTime =
      scheduleReceivedTime + (unsigned long)(RELAY_ID - 1) * RELAY_BEACON_SLOT_MS;

  // 모든 Relay beacon slot과 guard 이후 Runner uplink phase가 시작된다.
  unsigned long beaconPhaseMs =
      (unsigned long)relayCount * RELAY_BEACON_SLOT_MS;
  unsigned long runnerPhaseMs =
      (unsigned long)runnerCount * (unsigned long)runnerSlotMs;
  runnerPhaseEndTime =
      scheduleReceivedTime + beaconPhaseMs + PHASE_GUARD_MS + runnerPhaseMs;

  Serial.print("[SCHEDULE] cycle=");
  Serial.print(activeCycleId);
  Serial.print(", relay_count=");
  Serial.print(relayCount);
  Serial.print(", runner_count=");
  Serial.print(runnerCount);
  Serial.print(", runner_slot_ms=");
  Serial.println(runnerSlotMs);
  Serial.print("[TIMING] Beacon send at millis=");
  Serial.print(beaconSendTime);
  Serial.print(", Runner phase ends at millis=");
  Serial.println(runnerPhaseEndTime);

  changeState(SEND_BEACON);
}

void sendBeaconPacket() {
  // BEACON,cycle_id,relay_id,runner_count,runner_slot_ms
  String message = "BEACON,";
  message += String(activeCycleId);
  message += ",";
  message += String(RELAY_ID);
  message += ",";
  message += String(runnerCount);
  message += ",";
  message += String(runnerSlotMs);

  Serial.print("[BEACON] Sending in relay slot ");
  Serial.println(RELAY_ID - 1);
  sendLoRaMessage(message);
}

bool isDuplicateRunnerPacket(int cycleId, int runnerId, int seq) {
  for (int i = 0; i < bufferCount; i++) {
    if (relayBuffer[i].cycleId == cycleId &&
        relayBuffer[i].runnerId == runnerId &&
        relayBuffer[i].seq == seq) {
      return true;
    }
  }

  return false;
}

void addRunnerDataToBuffer(int cycleId, int relayId, int runnerId,
                           String lat, String lng, int pace, int battery,
                           char status,int seq, int rssi, float snr) {
  if (bufferCount >= MAX_BUFFER_SIZE) {
    Serial.println("[BUFFER][WARN] Buffer full. Runner packet dropped.");
    return;
  }

  if (isDuplicateRunnerPacket(cycleId, runnerId, seq)) {
    Serial.print("[BUFFER] Duplicate dropped: runner=");
    Serial.print(runnerId);
    Serial.print(", seq=");
    Serial.println(seq);
    return;
  }

  RunnerData &data = relayBuffer[bufferCount];
  data.cycleId = cycleId;
  data.relayId = relayId;
  data.runnerId = runnerId;
  data.lat = lat;
  data.lng = lng;
  data.pace = pace;
  data.battery = battery;
  data.status = status;
  data.seq = seq;
  data.rssi = rssi;
  data.snr = snr;
  bufferCount++;

  Serial.print("[BUFFER] Added runner ");
  Serial.print(runnerId);
  Serial.print(", seq=");
  Serial.print(seq);
  Serial.print(", count=");
  Serial.print(bufferCount);
  Serial.print("/");
  Serial.println(MAX_BUFFER_SIZE);
  updateOLED();
}

void handleRunnerPacket(String msg, int rssi, float snr) {
  // 기본 형식:
  // RUNNER,cycle_id,runner_id,target_relay_id,lat,lng,pace,battery,seq
  // Runner 구현의 gps_valid가 붙은 10-field 형식도 허용한다.
  if (getField(msg, 0) != "RUNNER" ||
      getField(msg, 1).length() == 0 ||
      getField(msg, 2).length() == 0 ||
      getField(msg, 3).length() == 0 ||
      getField(msg, 4).length() == 0 ||
      getField(msg, 5).length() == 0 ||
      getField(msg, 6).length() == 0 ||
      getField(msg, 7).length() == 0 ||
      getField(msg, 8).length() == 0 ||
      getField(msg, 9).length() == 0 ||
      getField(msg, 10).length() == 0 ||
      getField(msg, 11).length() == 0 ||
      getField(msg, 12).length() == 0 ||
      getField(msg, 13).length() != 0) {
    Serial.println("[RUNNER][ERROR] Invalid CSV format");
    return;
  }

  int packetCycleId = getField(msg, 1).toInt();
  int runnerId = getField(msg, 2).toInt();
  int targetRelayId = getField(msg, 3).toInt();
  String lat = getField(msg, 4);
  String lng = getField(msg, 5);
  int pace = getField(msg, 6).toInt();
  int battery = getField(msg, 7).toInt();
  String statusField = getField(msg, 8);
  char status = statusField.charAt(0);
  int seq = getField(msg, 9).toInt();
  int rssi = getField(msg, 10).toInt();
  float snr = getField(msg, 11).toFloat();
  String gpsValidField = getField(msg, 12);

  if (packetCycleId != activeCycleId) {
    Serial.print("[RUNNER] Dropped: cycle ");
    Serial.print(packetCycleId);
    Serial.print(" does not match active cycle ");
    Serial.println(activeCycleId);
    return;
  }

  if (targetRelayId != RELAY_ID) {
    Serial.print("[RUNNER] Dropped: target relay ");
    Serial.print(targetRelayId);
    Serial.print(" is not this relay ");
    Serial.println(RELAY_ID);
    return;
  }

  if (runnerId <= 0 || runnerId > MAX_RUNNERS || seq < 0) {
    Serial.println("[RUNNER][ERROR] Invalid runner_id or seq");
    return;
  }

  if (status != 'M' && status != 'E') {
    Serial.println("[RUNNER][ERROR] Invalid status");
    return;
  }

  if (gpsValidField.length() > 0) {
    Serial.print("[RUNNER] gps_valid=");
    Serial.println(gpsValidField);
  }

  if(status == 'E' && !emergencyForwarded[runnerId]){
    Serial.print("[EMERGENCY] First E from runner ");
    Serial.println(runnerId);

    sendEmergencyPacketToGateway(packetCycleId, runnerId, lat, lng,
                                 pace, battery, status, seq, rssi, snr);

    emergencyForwarded[runnerId] = true;

    return;
  }

  if (status == 'M') {
    emergencyForwarded[runnerId] = false;
  }

  addRunnerDataToBuffer(packetCycleId, RELAY_ID, runnerId, lat, lng,
                        pace, battery, status, seq, rssi, snr);
}

void sendEmergencyPacketToGateway(int cycleId, int runnerId,
                                  String lat, String lng,
                                  int pace, int battery,
                                  char status, int seq,
                                  int rssi, float snr) {
  String message = "EMERGENCY,";
  message += String(cycleId);
  message += ",";
  message += String(RELAY_ID);
  message += ",";
  message += String(runnerId);
  message += ",";
  message += lat;
  message += ",";
  message += lng;
  message += ",";
  message += String(pace);
  message += ",";
  message += String(battery);
  message += ",";
  message += String(status);
  message += ",";
  message += String(seq);
  message += ",";
  message += String(rssi);
  message += ",";
  message += String(snr, 2);

  Serial.print("[EMERGENCY][TX] ");
  Serial.println(message);

  sendLoRaMessage(message);
}

void forwardBufferedPacketsToGateway() {
  Serial.print("[FORWARD] Sending ");
  Serial.print(bufferCount);
  Serial.println(" buffered packet(s) to Gateway");
  updateOLED();

  for (int i = 0; i < bufferCount; i++) {
    RunnerData &data = relayBuffer[i];

    // FORWARD,cycle_id,relay_id,runner_id,lat,lng,pace,battery,seq,rssi,snr
    String message = "FORWARD,";
    message += String(data.cycleId);
    message += ",";
    message += String(RELAY_ID);
    message += ",";
    message += String(data.runnerId);
    message += ",";
    message += data.lat;
    message += ",";
    message += data.lng;
    message += ",";
    message += String(data.pace);
    message += ",";
    message += String(data.battery);
    message += ",";
    message += String(data.status);
    message += ",";
    message += String(data.seq);
    message += ",";
    message += String(data.rssi);
    message += ",";
    message += String(data.snr, 2);

    sendLoRaMessage(message);

    // 연속 송신으로 인한 Gateway 수신 누락 가능성을 줄인다.
    delay(FORWARD_INTERVAL_MS);
  }
}

void sendDonePacket() {
  // DONE,cycle_id,relay_id,count
  String message = "DONE,";
  message += String(activeCycleId);
  message += ",";
  message += String(RELAY_ID);
  message += ",";
  message += String(bufferCount);

  sendLoRaMessage(message);
}

void handleSendNowPacket(String msg) {
  // SEND_NOW,cycle_id,target_relay_id
  if (getField(msg, 0) != "SEND_NOW" ||
      getField(msg, 1).length() == 0 ||
      getField(msg, 2).length() == 0 ||
      getField(msg, 3).length() != 0) {
    Serial.println("[SEND_NOW][ERROR] Invalid CSV format");
    return;
  }

  int packetCycleId = getField(msg, 1).toInt();
  int targetRelayId = getField(msg, 2).toInt();

  if (packetCycleId != activeCycleId || targetRelayId != RELAY_ID) {
    Serial.println("[SEND_NOW] Ignored: cycle or target relay does not match");
    return;
  }

  if (RELAY_ID != 2) {
    Serial.println("[SEND_NOW] Ignored: Relay1 does not require SEND_NOW");
    return;
  }

  if (currentState == COLLECT_RUNNER_DATA) {
    pendingSendNow = true;
    Serial.println("[SEND_NOW] Received early; forwarding after Runner phase");
  } else if (currentState == WAIT_SEND_NOW) {
    Serial.println("[SEND_NOW] Valid command received");
    changeState(FORWARD_TO_GATEWAY);
  }
}

void addDummyRunnerData() {
  int dummyRunnerId;
  String dummyLat;
  String dummyLng;
  int dummyPace;
  int dummyBattery;
  int dummySeq = 1;

  if (RELAY_ID == 1) {
    dummyRunnerId = 1;
    dummyLat = "36.10321";
    dummyLng = "129.38712";
    dummyPace = 342;
    dummyBattery = 78;
  } else {
    dummyRunnerId = 2;
    dummyLat = "36.10390";
    dummyLng = "129.38800";
    dummyPace = 355;
    dummyBattery = 80;
  }

  if (isDuplicateRunnerPacket(activeCycleId, dummyRunnerId, dummySeq)) {
    Serial.println("[DUMMY] Matching dummy packet already exists; not added");
    return;
  }

  Serial.println("[DUMMY] Adding one test RunnerData item");
  addRunnerDataToBuffer(activeCycleId, RELAY_ID, dummyRunnerId,
                        dummyLat, dummyLng, dummyPace, dummyBattery,
                        'M', dummySeq, -70, 8.0);
}

void dummyGatewayReceive(String msg) {
  if (!DUMMY_GATEWAY_RECEIVE_MODE) {
    return;
  }

  Serial.println();
  Serial.println("========== [DUMMY GATEWAY RX] ==========");

  if (startsWithPacket(msg, "EMERGENCY")) {
    Serial.println("🚨🚨🚨 [DUMMY GATEWAY] EMERGENCY RECEIVED 🚨🚨🚨");
    Serial.print("[DUMMY GATEWAY][EMERGENCY] ");
    Serial.println(msg);

    int cycleId = getField(msg, 1).toInt();
    int relayId = getField(msg, 2).toInt();
    int runnerId = getField(msg, 3).toInt();
    String lat = getField(msg, 4);
    String lng = getField(msg, 5);
    int pace = getField(msg, 6).toInt();
    int battery = getField(msg, 7).toInt();
    String status = getField(msg, 8);
    int seq = getField(msg, 9).toInt();

    Serial.print("cycle=");
    Serial.print(cycleId);
    Serial.print(", relay=");
    Serial.print(relayId);
    Serial.print(", runner=");
    Serial.print(runnerId);
    Serial.print(", status=");
    Serial.print(status);
    Serial.print(", seq=");
    Serial.print(seq);
    Serial.print(", lat=");
    Serial.print(lat);
    Serial.print(", lng=");
    Serial.print(lng);
    Serial.print(", pace=");
    Serial.print(pace);
    Serial.print(", battery=");
    Serial.println(battery);
  }

  else if (startsWithPacket(msg, "FORWARD")) {
    Serial.println("[DUMMY GATEWAY] FORWARD RECEIVED");
    Serial.print("[DUMMY GATEWAY][FORWARD] ");
    Serial.println(msg);

    int cycleId = getField(msg, 1).toInt();
    int relayId = getField(msg, 2).toInt();
    int runnerId = getField(msg, 3).toInt();
    String lat = getField(msg, 4);
    String lng = getField(msg, 5);
    int pace = getField(msg, 6).toInt();
    int battery = getField(msg, 7).toInt();
    String status = getField(msg, 8);
    int seq = getField(msg, 9).toInt();

    Serial.print("cycle=");
    Serial.print(cycleId);
    Serial.print(", relay=");
    Serial.print(relayId);
    Serial.print(", runner=");
    Serial.print(runnerId);
    Serial.print(", status=");
    Serial.print(status);
    Serial.print(", seq=");
    Serial.print(seq);
    Serial.print(", lat=");
    Serial.print(lat);
    Serial.print(", lng=");
    Serial.print(lng);
    Serial.print(", pace=");
    Serial.print(pace);
    Serial.print(", battery=");
    Serial.println(battery);
  }

  else if (startsWithPacket(msg, "DONE")) {
    Serial.println("[DUMMY GATEWAY] DONE RECEIVED");
    Serial.print("[DUMMY GATEWAY][DONE] ");
    Serial.println(msg);

    dummyGatewayHandleDone(msg);
  }

  else if (startsWithPacket(msg, "BEACON")) {
    Serial.println("[DUMMY GATEWAY] BEACON observed");
    Serial.print("[DUMMY GATEWAY][BEACON] ");
    Serial.println(msg);
  }

  else {
    Serial.println("[DUMMY GATEWAY] UNKNOWN PACKET");
    Serial.println(msg);
  }

  Serial.println("========================================");
  Serial.println();
}

void dummyGatewayHandleDone(String msg) {
  // DONE,cycle_id,relay_id,count
  int cycleId = getField(msg, 1).toInt();
  int relayId = getField(msg, 2).toInt();
  int count = getField(msg, 3).toInt();

  Serial.print("[DUMMY GATEWAY] cycle=");
  Serial.print(cycleId);
  Serial.print(", relay=");
  Serial.print(relayId);
  Serial.print(", forwarded_count=");
  Serial.println(count);

  if (relayId == 1) {
    Serial.println("[DUMMY GATEWAY] Relay1 finished.");
    Serial.println("[DUMMY GATEWAY] In real gateway, SEND_NOW would be sent to Relay2.");
  }

  if (relayId == 2) {
    Serial.println("[DUMMY GATEWAY] Relay2 finished. Cycle complete.");
  }
}

void clearRelayBuffer() {
  for (int i = 0; i < MAX_BUFFER_SIZE; i++) {
    relayBuffer[i].cycleId = -1;
    relayBuffer[i].relayId = -1;
    relayBuffer[i].runnerId = -1;
    relayBuffer[i].lat = "";
    relayBuffer[i].lng = "";
    relayBuffer[i].pace = 0;
    relayBuffer[i].battery = 0;
    relayBuffer[i].status = 'M';
    relayBuffer[i].seq = -1;
    relayBuffer[i].rssi = -999;
    relayBuffer[i].snr = -999.0;
  }

  bufferCount = 0;
}

void changeState(RelayState nextState) {
  if (currentState != nextState) {
    Serial.print("[STATE] ");
    Serial.print(stateName(currentState));
    Serial.print(" -> ");
    Serial.println(stateName(nextState));
  }

  currentState = nextState;
  stateStartTime = millis();
  updateOLED();
}

const char *stateName(RelayState state) {
  switch (state) {
    case WAIT_SCHEDULE:
      return "WAIT_SCHEDULE";
    case SEND_BEACON:
      return "SEND_BEACON";
    case COLLECT_RUNNER_DATA:
      return "COLLECT_RUNNER";
    case FORWARD_TO_GATEWAY:
      return "FORWARD_GATEWAY";
    case WAIT_SEND_NOW:
      return "WAIT_SEND_NOW";
    case CYCLE_DONE:
      return "CYCLE_DONE";
    default:
      return "UNKNOWN";
  }
}

void updateOLED() {
  String cycleLine =
      (activeCycleId >= 0) ? "Cycle: " + String(activeCycleId) : "Cycle: -";
  showOLED("Relay " + String(RELAY_ID), String(stateName(currentState)),
           cycleLine, "Buffer: " + String(bufferCount));
}
