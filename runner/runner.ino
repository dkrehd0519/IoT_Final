#include <SPI.h>
#include <RadioLib.h>
#include <TinyGPSPlus.h>
#include <Wire.h>
#include <XPowersLib.h>

// 기존 통신 테스트를 통과한 T-Beam SX1262 환경 설정은 유지한다.
#define LORA_CS    18
#define LORA_DIO1  33
#define LORA_RST   23
#define LORA_BUSY 32
#define LORA_SCK 5
#define LORA_MISO 19
#define LORA_MOSI 27

SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);

#define GPS_RX 34
#define GPS_TX 12
#define GPS_BAUD 9600

// 데모 실행 시 Runner 장치마다 RUNNER_ID만 1, 2, 3으로 변경한다.
#define RUNNER_ID 1
#define DEMO_SCENARIO 1
#define TEST_MODE true

#define LORA_FREQUENCY 915.0
#define LORA_SYNC_WORD 0x33
#define LORA_TX_POWER 17
#define LORA_SPREADING_FACTOR 7
#define LORA_BANDWIDTH 125.0
#define LORA_CODING_RATE_DENOMINATOR 5
#define LORA_PREAMBLE_LENGTH 8

// 기존에 검증한 실제 주파수 테이블은 유지하고, 데모 channel id만 논리적으로 매핑한다.
const float CHANNEL_FREQ_MHZ[] = {
  915.0,
  916.0,
  917.0,
  918.0,
  919.0,
};

#define TOTAL_RUNNERS 3
#define DEMO_SLOT_DURATION_MS 900UL
#define PHASE_GUARD_MS 250UL
#define RESPONSE_WINDOW_MS 5000UL
#define EMERGENCY_TX_TOTAL_COUNT 3
#define EMERGENCY_RETRY_INTERVAL_MS 650UL
#define EMERGENCY_BACKOFF_MAX_MS 150UL

#define DUMMY_LAT 37.5558
#define DUMMY_LNG 126.9720

#define SOS_BUTTON_PIN 38
#define SOS_ACTIVE_LEVEL LOW

#define I2C_SDA 21
#define I2C_SCL 22
#ifndef AXP2101_SLAVE_ADDRESS
#define AXP2101_SLAVE_ADDRESS 0x34
#endif

TinyGPSPlus gps;
HardwareSerial GPSserial(1);

XPowersAXP2101 PMU;
bool pmuOk = false;

enum RunnerState {
  WAIT_SCHEDULE,
  WAIT_MY_SLOT,
  SEND_STATUS,
  WAIT_HANDOVER_RESPONSE,
  WAIT_VALID_CYCLE,
  SEND_HANDOVER_JOIN,
  NORMAL
};

RunnerState currentState = WAIT_SCHEDULE;

volatile bool receivedFlag = false;

int currentChannelId = 1;
int currentRelayId = 1;
int activeCycleId = -1;
int assignedSlotIndex = -1;
int slotDurationMs = DEMO_SLOT_DURATION_MS;
int reserveSlotCount = 0;
unsigned long sendTime = 0;
unsigned long responseDeadline = 0;

int nextRelayId = -1;
int nextChannelId = -1;
int reserveSlotId = -1;
int validFromCycle = -1;
int sourceRelayIdForJoin = -1;
bool handoverPending = false;
bool retryHandoverNextCycle = false;

double lastLat = DUMMY_LAT;
double lastLng = DUMMY_LNG;
bool gpsValid = false;
unsigned long lastValidGpsTime = 0;
double prevLat = DUMMY_LAT;
double prevLng = DUMMY_LNG;
unsigned long prevGpsMillis = 0;
bool prevPositionSet = false;
double totalDistanceM = 0.0;
unsigned long runStartMillis = 0;
int avgPaceSecPerKm = 0;

unsigned long seq = 1;
unsigned long emergencySeq = 0;
bool emergencyPending = false;
int emergencyTxCount = 0;
unsigned long nextEmergencyTxTime = 0;
bool lastButtonState = !SOS_ACTIVE_LEVEL;
int savedChannelBeforeEmergency = 1;

bool scenario3EmergencyTriggered = false;
unsigned long bootMillis = 0;

void setLoRaFlag(void) {
  receivedFlag = true;
}

void sendLoRaMessage(String msg);
bool receiveLoRaMessage(String &msg, int &rssi, float &snr);
bool startsWithPacket(String msg, String type);
String getField(String msg, int index);
void updateGPS();
void updateSOSButton();
void handleEmergencyIfNeeded();
void triggerEmergencyEvent();
void sendEmergencyPacket();
int calculatePace();
int readBatteryPercent();
String getTimestamp();
double distanceMeters(double lat1, double lon1, double lat2, double lon2);
float getFrequencyByChannel(int channelId);
bool switchRunnerChannel(int channelId);
int getInitialChannelForRunner();
int getInitialRelayForRunner();
bool scheduleContainsRunner(String msg, int &slotIndex);
bool shouldRequestHandover();
unsigned long getHandoverRequestTime();
void handleSchedule(String msg);
void sendRunnerStatus();
void handleHandoverAck(String msg);
void handleHandoverRetry(String msg);
void sendHandoverJoin();
void changeState(RunnerState nextState);
const char *stateName(RunnerState state);

void setup() {
  Serial.begin(115200);
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
  delay(1000);
  bootMillis = millis();

  Serial.println();
  Serial.println("========================================");
  Serial.println(" TTGO T-Beam SX1262 Marathon Runner Demo");
  Serial.println("========================================");
  Serial.print("[CONFIG] RUNNER_ID=");
  Serial.println(RUNNER_ID);
  Serial.print("[CONFIG] DEMO_SCENARIO=");
  Serial.println(DEMO_SCENARIO);
  Serial.println("[CONFIG] RadioLib/SX1262 pin and LoRa settings kept from tested code");

  pinMode(SOS_BUTTON_PIN, INPUT);
  GPSserial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX, GPS_TX);
  Wire.begin(I2C_SDA, I2C_SCL);
  pmuOk = PMU.begin(Wire, AXP2101_SLAVE_ADDRESS, I2C_SDA, I2C_SCL);

  int state = radio.begin(
      LORA_FREQUENCY,
      LORA_BANDWIDTH,
      LORA_SPREADING_FACTOR,
      LORA_CODING_RATE_DENOMINATOR,
      LORA_SYNC_WORD,
      LORA_TX_POWER,
      LORA_PREAMBLE_LENGTH);

  if (state != RADIOLIB_ERR_NONE) {
    Serial.print("[ERROR] RadioLib SX1262 initialization failed, code=");
    Serial.println(state);
    while (true) {
      delay(1000);
    }
  }

  radio.setCRC(true);
  radio.setDio1Action(setLoRaFlag);

  currentChannelId = getInitialChannelForRunner();
  currentRelayId = getInitialRelayForRunner();
  switchRunnerChannel(currentChannelId);

  state = radio.startReceive();
  if (state == RADIOLIB_ERR_NONE) {
    Serial.println("[LoRa] Receive mode started");
  }

  Serial.print("[DEMO] Initial relay=");
  Serial.print(currentRelayId);
  Serial.print(", channel=");
  Serial.println(currentChannelId);
  changeState(WAIT_SCHEDULE);
}

void loop() {
  updateGPS();
  updateSOSButton();

  if (TEST_MODE && DEMO_SCENARIO == 3 && RUNNER_ID == 1 &&
      !scenario3EmergencyTriggered &&
      (unsigned long)(millis() - bootMillis) > 12000UL) {
    scenario3EmergencyTriggered = true;
    Serial.println("[TEST] Scenario 3 emergency trigger fired");
    triggerEmergencyEvent();
  }

  handleEmergencyIfNeeded();

  String message;
  int rssi = 0;
  float snr = 0.0;
  if (receiveLoRaMessage(message, rssi, snr)) {
    if (startsWithPacket(message, "SCHEDULE")) {
      handleSchedule(message);
    } else if (startsWithPacket(message, "HANDOVER_ACK")) {
      handleHandoverAck(message);
    } else if (startsWithPacket(message, "HANDOVER_RETRY")) {
      handleHandoverRetry(message);
    } else {
      Serial.println("[RX] Ignored packet for Runner");
    }
  }

  switch (currentState) {
    case WAIT_SCHEDULE:
      break;

    case WAIT_MY_SLOT:
      if ((long)(millis() - sendTime) >= 0) {
        changeState(SEND_STATUS);
      }
      break;

    case SEND_STATUS:
      sendRunnerStatus();
      if (shouldRequestHandover()) {
        responseDeadline = millis() + RESPONSE_WINDOW_MS;
        changeState(WAIT_HANDOVER_RESPONSE);
      } else {
        changeState(NORMAL);
      }
      break;

    case WAIT_HANDOVER_RESPONSE:
      if ((long)(millis() - responseDeadline) >= 0) {
        Serial.println("[HANDOVER] Response timeout. Will retry on next assigned schedule.");
        retryHandoverNextCycle = true;
        changeState(WAIT_SCHEDULE);
      }
      break;

    case WAIT_VALID_CYCLE:
      break;

    case SEND_HANDOVER_JOIN:
      if ((long)(millis() - sendTime) >= 0) {
        sendHandoverJoin();
        currentRelayId = nextRelayId;
        currentChannelId = nextChannelId;
        handoverPending = false;
        retryHandoverNextCycle = false;
        changeState(WAIT_SCHEDULE);
      }
      break;

    case NORMAL:
      changeState(WAIT_SCHEDULE);
      break;
  }
}

void sendLoRaMessage(String msg) {
  Serial.print("[TX] ");
  Serial.println(msg);
  radio.standby();
  int state = radio.transmit(msg);
  if (state == RADIOLIB_ERR_NONE) {
    Serial.println("[TX] Success");
  } else {
    Serial.print("[TX][ERROR] RadioLib transmit failed, code=");
    Serial.println(state);
  }
  receivedFlag = false;
  radio.startReceive();
}

bool receiveLoRaMessage(String &msg, int &rssi, float &snr) {
  if (!receivedFlag) {
    return false;
  }

  receivedFlag = false;
  int state = radio.readData(msg);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.print("[RX][ERROR] readData failed, code=");
    Serial.println(state);
    radio.startReceive();
    return false;
  }

  msg.trim();
  rssi = (int)radio.getRSSI();
  snr = radio.getSNR();

  Serial.print("[RX] ");
  Serial.print(msg);
  Serial.print(" | RSSI=");
  Serial.print(rssi);
  Serial.print(" dBm, SNR=");
  Serial.print(snr, 2);
  Serial.println(" dB");

  radio.startReceive();
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

  if (!gps.location.isValid()) {
    if (TEST_MODE) {
      lastLat = DUMMY_LAT + (RUNNER_ID * 0.0001);
      lastLng = DUMMY_LNG + (RUNNER_ID * 0.0001);
      gpsValid = false;
    }
    return;
  }

  double newLat = gps.location.lat();
  double newLng = gps.location.lng();
  unsigned long nowMs = millis();

  lastLat = newLat;
  lastLng = newLng;
  gpsValid = true;
  lastValidGpsTime = nowMs;

  if (runStartMillis == 0) {
    runStartMillis = nowMs;
  }

  if (!prevPositionSet) {
    prevLat = newLat;
    prevLng = newLng;
    prevGpsMillis = nowMs;
    prevPositionSet = true;
    return;
  }

  double movedM = distanceMeters(prevLat, prevLng, newLat, newLng);
  unsigned long dtMs = nowMs - prevGpsMillis;
  if (dtMs == 0) {
    return;
  }

  double speedMps = movedM / (dtMs / 1000.0);
  if (movedM >= 1.0 && movedM <= 50.0 && speedMps >= 0.1 && speedMps <= 8.0) {
    totalDistanceM += movedM;
    double totalKm = totalDistanceM / 1000.0;
    double elapsedRunSec = (nowMs - runStartMillis) / 1000.0;
    if (totalKm > 0.01) {
      avgPaceSecPerKm = (int)(elapsedRunSec / totalKm);
    }
    prevLat = newLat;
    prevLng = newLng;
    prevGpsMillis = nowMs;
  }
}

void updateSOSButton() {
  int currentButtonState = digitalRead(SOS_BUTTON_PIN);
  if (currentButtonState == SOS_ACTIVE_LEVEL && lastButtonState != SOS_ACTIVE_LEVEL) {
    triggerEmergencyEvent();
  }
  lastButtonState = currentButtonState;
}

void triggerEmergencyEvent() {
  if (emergencyPending) {
    return;
  }

  emergencyPending = true;
  emergencyTxCount = 0;
  emergencySeq++;
  savedChannelBeforeEmergency = currentChannelId;
  nextEmergencyTxTime = millis() + random(0, EMERGENCY_BACKOFF_MAX_MS);
  Serial.print("[EMERGENCY] Triggered. saved_channel=");
  Serial.println(savedChannelBeforeEmergency);
}

void handleEmergencyIfNeeded() {
  if (!emergencyPending) {
    return;
  }

  if (emergencyTxCount >= EMERGENCY_TX_TOTAL_COUNT) {
    emergencyPending = false;
    switchRunnerChannel(savedChannelBeforeEmergency);
    Serial.println("[EMERGENCY] Burst complete. Returned to normal channel.");
    return;
  }

  if ((long)(millis() - nextEmergencyTxTime) < 0) {
    return;
  }

  switchRunnerChannel(13);
  sendEmergencyPacket();
  emergencyTxCount++;
  nextEmergencyTxTime = millis() + EMERGENCY_RETRY_INTERVAL_MS +
                        random(0, EMERGENCY_BACKOFF_MAX_MS);
}

void sendEmergencyPacket() {
  String message = "EMERGENCY,";
  message += String(emergencySeq);
  message += ",";
  message += String(RUNNER_ID);
  message += ",";
  message += String(lastLat, 6);
  message += ",";
  message += String(lastLng, 6);
  message += ",";
  message += String(readBatteryPercent());
  message += ",";
  message += getTimestamp();
  message += ",";
  message += gpsValid ? "1" : "0";

  Serial.print("[EMERGENCY][TX] count=");
  Serial.print(emergencyTxCount + 1);
  Serial.print("/");
  Serial.println(EMERGENCY_TX_TOTAL_COUNT);
  sendLoRaMessage(message);
}

int calculatePace() {
  if (avgPaceSecPerKm > 0) {
    return avgPaceSecPerKm;
  }
  return 340 + RUNNER_ID * 5;
}

int readBatteryPercent() {
  if (pmuOk && PMU.isBatteryConnect()) {
    return PMU.getBatteryPercent();
  }
  return 78;
}

String getTimestamp() {
  if (gps.time.isValid()) {
    char buffer[7];
    sprintf(buffer, "%02d%02d%02d", gps.time.hour(), gps.time.minute(), gps.time.second());
    return String(buffer);
  }

  unsigned long seconds = millis() / 1000;
  char buffer[7];
  sprintf(buffer, "%02d%02d%02d",
          (int)((seconds / 3600) % 24),
          (int)((seconds / 60) % 60),
          (int)(seconds % 60));
  return String(buffer);
}

double distanceMeters(double lat1, double lon1, double lat2, double lon2) {
  const double R = 6371000.0;
  double phi1 = radians(lat1);
  double phi2 = radians(lat2);
  double dphi = radians(lat2 - lat1);
  double dlambda = radians(lon2 - lon1);
  double a = sin(dphi / 2) * sin(dphi / 2) +
             cos(phi1) * cos(phi2) *
             sin(dlambda / 2) * sin(dlambda / 2);
  double c = 2 * atan2(sqrt(a), sqrt(1 - a));
  return R * c;
}

float getFrequencyByChannel(int channelId) {
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
  return CHANNEL_FREQ_MHZ[index];
}

bool switchRunnerChannel(int channelId) {
  float freqMHz = getFrequencyByChannel(channelId);
  Serial.print("[CHANNEL] Switch logical channel ");
  Serial.print(channelId);
  Serial.print(" -> ");
  Serial.print(freqMHz, 2);
  Serial.println(" MHz");

  radio.standby();
  int state = radio.setFrequency(freqMHz);
  if (state == RADIOLIB_ERR_NONE) {
    currentChannelId = channelId;
    radio.startReceive();
    return true;
  }

  Serial.print("[CHANNEL][ERROR] setFrequency failed, code=");
  Serial.println(state);
  radio.startReceive();
  return false;
}

int getInitialChannelForRunner() {
  if (DEMO_SCENARIO == 1) {
    return (RUNNER_ID == 3) ? 2 : 1;
  }
  if (DEMO_SCENARIO == 2) {
    return 1;
  }
  return 1;
}

int getInitialRelayForRunner() {
  if (DEMO_SCENARIO == 1) {
    return (RUNNER_ID == 3) ? 2 : 1;
  }
  return 1;
}

bool scheduleContainsRunner(String msg, int &slotIndex) {
  int activeCount = getField(msg, 7).toInt();
  slotIndex = -1;
  for (int i = 0; i < activeCount; i++) {
    int runnerId = getField(msg, 8 + i).toInt();
    if (runnerId == RUNNER_ID) {
      slotIndex = i;
      return true;
    }
  }
  return false;
}

bool shouldRequestHandover() {
  if (DEMO_SCENARIO != 2) {
    return false;
  }

  if (handoverPending) {
    return false;
  }

  if (currentRelayId != 1) {
    return false;
  }

  if (RUNNER_ID == 1) {
    return true;
  }

  if (RUNNER_ID == 2) {
    return retryHandoverNextCycle || activeCycleId >= 2;
  }

  return false;
}

unsigned long getHandoverRequestTime() {
  if (DEMO_SCENARIO == 2 && RUNNER_ID == 1) {
    return millis();
  }
  if (DEMO_SCENARIO == 2 && RUNNER_ID == 2) {
    return millis() + 100UL;
  }
  return 0;
}

void handleSchedule(String msg) {
  if (getField(msg, 0) != "SCHEDULE") {
    return;
  }

  int cycleId = getField(msg, 1).toInt();
  int relayId = getField(msg, 2).toInt();
  int channelId = getField(msg, 4).toInt();
  int parsedSlotMs = getField(msg, 5).toInt();
  int parsedReserveSlots = getField(msg, 6).toInt();

  if (channelId != currentChannelId) {
    Serial.println("[SCHEDULE] Ignored: schedule is for another logical channel");
    return;
  }

  if (handoverPending && relayId == nextRelayId && cycleId >= validFromCycle) {
    activeCycleId = cycleId;
    currentRelayId = relayId;
    slotDurationMs = parsedSlotMs;
    reserveSlotCount = parsedReserveSlots;
    sendTime = millis() + PHASE_GUARD_MS +
               (unsigned long)(reserveSlotId - 1) * (unsigned long)slotDurationMs;
    Serial.print("[HANDOVER] Target schedule received. reserve_slot=");
    Serial.print(reserveSlotId);
    Serial.print(", join at millis=");
    Serial.println(sendTime);
    changeState(SEND_HANDOVER_JOIN);
    return;
  }

  int slotIndex = -1;
  if (!scheduleContainsRunner(msg, slotIndex)) {
    Serial.print("[SCHEDULE] Runner ");
    Serial.print(RUNNER_ID);
    Serial.println(" not in activeRunnerList. Ignored.");
    return;
  }

  activeCycleId = cycleId;
  currentRelayId = relayId;
  slotDurationMs = parsedSlotMs;
  reserveSlotCount = parsedReserveSlots;
  assignedSlotIndex = slotIndex;
  sendTime = millis() + PHASE_GUARD_MS +
             (unsigned long)reserveSlotCount * (unsigned long)slotDurationMs +
             (unsigned long)assignedSlotIndex * (unsigned long)slotDurationMs;

  Serial.println("[SCHEDULE] Assigned regular slot");
  Serial.print("  cycle_id=");
  Serial.println(activeCycleId);
  Serial.print("  relay_id=");
  Serial.println(currentRelayId);
  Serial.print("  slot_index=");
  Serial.println(assignedSlotIndex);
  Serial.print("  send_at_millis=");
  Serial.println(sendTime);
  changeState(WAIT_MY_SLOT);
}

void sendRunnerStatus() {
  bool requestHandover = shouldRequestHandover();
  unsigned long requestTime = requestHandover ? getHandoverRequestTime() : 0;

  String message = "RUNNER_STATUS,";
  message += String(activeCycleId);
  message += ",";
  message += String(RUNNER_ID);
  message += ",";
  message += String(currentRelayId);
  message += ",";
  message += String(lastLat, 6);
  message += ",";
  message += String(lastLng, 6);
  message += ",";
  message += String(calculatePace());
  message += ",";
  message += String(readBatteryPercent());
  message += ",";
  message += String(seq++);
  message += ",";
  message += gpsValid ? "1" : "0";
  message += ",";
  message += requestHandover ? "1" : "0";
  message += ",";
  message += String(requestTime);

  Serial.print("[RUNNER_STATUS] slot_index=");
  Serial.print(assignedSlotIndex);
  Serial.print(", handover_request=");
  Serial.println(requestHandover ? "1" : "0");
  sendLoRaMessage(message);
}

void handleHandoverAck(String msg) {
  int cycleId = getField(msg, 1).toInt();
  int runnerId = getField(msg, 2).toInt();
  if (runnerId != RUNNER_ID) {
    return;
  }

  sourceRelayIdForJoin = getField(msg, 3).toInt();
  nextRelayId = getField(msg, 4).toInt();
  nextChannelId = getField(msg, 5).toInt();
  reserveSlotId = getField(msg, 6).toInt();
  validFromCycle = getField(msg, 7).toInt();
  handoverPending = true;
  retryHandoverNextCycle = false;

  Serial.println("[HANDOVER_ACK] Accepted");
  Serial.print("  cycle_id=");
  Serial.println(cycleId);
  Serial.print("  next_relay_id=");
  Serial.println(nextRelayId);
  Serial.print("  next_channel=");
  Serial.println(nextChannelId);
  Serial.print("  reserve_slot_id=");
  Serial.println(reserveSlotId);
  Serial.print("  valid_from_cycle=");
  Serial.println(validFromCycle);

  switchRunnerChannel(nextChannelId);
  changeState(WAIT_VALID_CYCLE);
}

void handleHandoverRetry(String msg) {
  int runnerId = getField(msg, 2).toInt();
  if (runnerId != RUNNER_ID) {
    return;
  }

  int retryCycle = getField(msg, 3).toInt();
  retryHandoverNextCycle = true;
  Serial.print("[HANDOVER_RETRY] retry_cycle=");
  Serial.println(retryCycle);
  changeState(WAIT_SCHEDULE);
}

void sendHandoverJoin() {
  String message = "HANDOVER_JOIN,";
  message += String(activeCycleId);
  message += ",";
  message += String(RUNNER_ID);
  message += ",";
  message += String(sourceRelayIdForJoin);
  message += ",";
  message += String(nextRelayId);
  message += ",";
  message += String(seq++);

  Serial.print("[HANDOVER_JOIN] reserve_slot=");
  Serial.println(reserveSlotId);
  sendLoRaMessage(message);
}

void changeState(RunnerState nextState) {
  if (currentState == nextState) {
    return;
  }
  Serial.print("[STATE] ");
  Serial.print(stateName(currentState));
  Serial.print(" -> ");
  Serial.println(stateName(nextState));
  currentState = nextState;
}

const char *stateName(RunnerState state) {
  switch (state) {
    case WAIT_SCHEDULE:
      return "WAIT_SCHEDULE";
    case WAIT_MY_SLOT:
      return "WAIT_MY_SLOT";
    case SEND_STATUS:
      return "SEND_STATUS";
    case WAIT_HANDOVER_RESPONSE:
      return "WAIT_HANDOVER_RESPONSE";
    case WAIT_VALID_CYCLE:
      return "WAIT_VALID_CYCLE";
    case SEND_HANDOVER_JOIN:
      return "SEND_HANDOVER_JOIN";
    case NORMAL:
      return "NORMAL";
  }
  return "UNKNOWN";
}
