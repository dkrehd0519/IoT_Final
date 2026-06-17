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

// 데모 실행 시 Gateway 장치마다 아래 값을 변경한다.
#define DEMO_SCENARIO 1
#define GATEWAY_ID 1
#define CHANNEL_ID 1
#define SHOW_RAW_PACKETS false
#define SHOW_STATE_LOG false
#define SHOW_DEBUG_LOG false

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

#define MAX_FORWARD_DATA 8
#define FORWARD_TIMEOUT_MS 6000UL

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
bool oledReady = false;

enum GatewayState {
  WAIT_FORWARD_START,
  RECEIVE_FORWARD_DATA,
  WAIT_FORWARD_DONE,
  PRINT_OR_SEND_TO_SERVER
};

struct ForwardData {
  int cycleId;
  int relayId;
  int runnerId;
  String lat;
  String lng;
  int pace;
  int battery;
  unsigned long seq;
  int gpsValid;
  int runnerRssi;
  float runnerSnr;
};

GatewayState currentState = WAIT_FORWARD_START;

ForwardData forwardBuffer[MAX_FORWARD_DATA];
int forwardCount = 0;
int expectedForwardCount = -1;
int currentCycleId = -1;
int currentRelayId = -1;
unsigned long stateStartTime = 0;
int currentChannelId = CHANNEL_ID;

void showOLED(String line1, String line2 = "", String line3 = "", String line4 = "");
bool receiveLoRaMessage(String &msg, int &rssi, float &snr);
bool startsWithPacket(String msg, String type);
String getField(String msg, int index);
long getFrequencyByChannel(int channelId);
int getEffectiveChannelId();
bool isEmergencyGateway();
void handleForwardStart(String msg);
void handleForwardData(String msg, int relayRssi, float relaySnr);
void handleForwardDone(String msg);
void handleEmergencyForward(String msg);
void printAndSendCycleData();
void sendToServerStub(ForwardData data);
void changeState(GatewayState nextState);
const char *stateName(GatewayState state);

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("========================================");
  Serial.println(" 마라톤 Gateway 노드 시작");
  Serial.println("========================================");
  Serial.print("데모 시나리오: ");
  Serial.println(DEMO_SCENARIO);
  Serial.print("Gateway ID: ");
  Serial.println(GATEWAY_ID);
  Serial.println("통신 설정: 기존 테스트 완료 설정 유지");

  Wire.begin(OLED_SDA, OLED_SCL);
  oledReady = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS);
  if (oledReady) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
  }

  currentChannelId = getEffectiveChannelId();

  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  long freq = getFrequencyByChannel(currentChannelId);
  Serial.print("사용 Channel: ");
  Serial.print(currentChannelId);
  Serial.print(" / 주파수=");
  Serial.println(freq);

  if (!LoRa.begin(freq)) {
    Serial.println("오류: LoRa 초기화 실패. 배선과 핀 설정을 확인하세요.");
    showOLED("LoRa ERROR", "Gateway " + String(GATEWAY_ID), "Check pins");
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

  showOLED("Gateway " + String(GATEWAY_ID),
           "Scenario " + String(DEMO_SCENARIO),
           "Channel " + String(currentChannelId),
           isEmergencyGateway() ? "Emergency" : "Normal");
  changeState(WAIT_FORWARD_START);
}

void loop() {
  String message;
  int rssi = 0;
  float snr = 0.0;

  if (receiveLoRaMessage(message, rssi, snr)) {
    if (startsWithPacket(message, "FORWARD_START")) {
      handleForwardStart(message);
    } else if (startsWithPacket(message, "FORWARD_DATA")) {
      handleForwardData(message, rssi, snr);
    } else if (startsWithPacket(message, "FORWARD_DONE")) {
      handleForwardDone(message);
    } else if (startsWithPacket(message, "EMERGENCY_FORWARD")) {
      handleEmergencyForward(message);
    } else {
      if (SHOW_DEBUG_LOG) {
        Serial.println("무시: Gateway 대상 packet이 아님");
      }
    }
  }

  if ((currentState == RECEIVE_FORWARD_DATA || currentState == WAIT_FORWARD_DONE) &&
      (unsigned long)(millis() - stateStartTime) >= FORWARD_TIMEOUT_MS) {
    Serial.println("Gateway 수신 시간 초과: 다음 전달 대기");
    changeState(WAIT_FORWARD_START);
  }

  if (currentState == PRINT_OR_SEND_TO_SERVER) {
    printAndSendCycleData();
    changeState(WAIT_FORWARD_START);
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

  if (SHOW_RAW_PACKETS) {
    Serial.print("수신 packet: ");
    Serial.print(msg);
    Serial.print(" | Gateway RSSI=");
    Serial.print(rssi);
    Serial.print(" dBm, SNR=");
    Serial.print(snr, 2);
    Serial.println(" dB");
  }
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
    return (GATEWAY_ID == 2) ? 2 : 1;
  }
  if (DEMO_SCENARIO == 2) {
    return (GATEWAY_ID == 2) ? 7 : 1;
  }
  if (DEMO_SCENARIO == 3) {
    return (GATEWAY_ID == 2) ? 13 : 1;
  }
  return CHANNEL_ID;
}

bool isEmergencyGateway() {
  return DEMO_SCENARIO == 3 && GATEWAY_ID == 2;
}

void handleForwardStart(String msg) {
  currentCycleId = getField(msg, 1).toInt();
  currentRelayId = getField(msg, 2).toInt();
  expectedForwardCount = getField(msg, 3).toInt();
  forwardCount = 0;

  Serial.println();
  Serial.println();
  Serial.println("========== Gateway 수신 시작 ==========");
  Serial.print("Cycle: ");
  Serial.println(currentCycleId);
  Serial.print("Relay: ");
  Serial.println(currentRelayId);
  Serial.print("예상 Runner 수: ");
  Serial.println(expectedForwardCount);

  showOLED("Forward Start",
           "Relay " + String(currentRelayId),
           "Cycle " + String(currentCycleId),
           "Count " + String(expectedForwardCount));
  changeState(RECEIVE_FORWARD_DATA);
}

void handleForwardData(String msg, int relayRssi, float relaySnr) {
  if (forwardCount >= MAX_FORWARD_DATA) {
    Serial.println("경고: Gateway buffer가 가득 찼습니다.");
    return;
  }

  int packetCycleId = getField(msg, 1).toInt();
  int relayId = getField(msg, 2).toInt();
  if (packetCycleId != currentCycleId || relayId != currentRelayId) {
    if (SHOW_DEBUG_LOG) Serial.println("무시: cycle 또는 relay 불일치");
    return;
  }

  ForwardData &data = forwardBuffer[forwardCount++];
  data.cycleId = packetCycleId;
  data.relayId = relayId;
  data.runnerId = getField(msg, 3).toInt();
  data.lat = getField(msg, 4);
  data.lng = getField(msg, 5);
  data.pace = getField(msg, 6).toInt();
  data.battery = getField(msg, 7).toInt();
  data.seq = (unsigned long)getField(msg, 8).toInt();
  data.gpsValid = getField(msg, 9).toInt();
  data.runnerRssi = getField(msg, 10).toInt();
  data.runnerSnr = getField(msg, 11).toFloat();

  Serial.println("Runner 데이터 수신");
  Serial.print("  Relay ID: ");
  Serial.println(data.relayId);
  Serial.print("  Runner ID: ");
  Serial.println(data.runnerId);
  Serial.print("  위치: ");
  Serial.print(data.lat);
  Serial.print(", ");
  Serial.println(data.lng);
  Serial.print("  Pace / Battery / Seq: ");
  Serial.print(data.pace);
  Serial.print(" / ");
  Serial.print(data.battery);
  Serial.print(" / ");
  Serial.println(data.seq);
  Serial.print("  Runner->Relay RSSI/SNR: ");
  Serial.print(data.runnerRssi);
  Serial.print(" / ");
  Serial.println(data.runnerSnr, 2);
  Serial.print("  Relay->Gateway RSSI/SNR: ");
  Serial.print(relayRssi);
  Serial.print(" / ");
  Serial.println(relaySnr, 2);

  changeState(WAIT_FORWARD_DONE);
}

void handleForwardDone(String msg) {
  int packetCycleId = getField(msg, 1).toInt();
  int relayId = getField(msg, 2).toInt();
  int reportedCount = getField(msg, 3).toInt();

  if (packetCycleId != currentCycleId || relayId != currentRelayId) {
    if (SHOW_DEBUG_LOG) Serial.println("무시: cycle 또는 relay 불일치");
    return;
  }

  Serial.println("Gateway 수신 완료");
  Serial.print("  Relay 보고 개수: ");
  Serial.println(reportedCount);
  Serial.print("  Gateway 수신 개수: ");
  Serial.println(forwardCount);
  if (reportedCount != forwardCount) {
    Serial.println("경고: Relay 보고 개수와 Gateway 수신 개수가 다릅니다.");
  }

  changeState(PRINT_OR_SEND_TO_SERVER);
}

void handleEmergencyForward(String msg) {
  if (!isEmergencyGateway()) {
    return;
  }

  int emergencyId = getField(msg, 1).toInt();
  int relayId = getField(msg, 2).toInt();
  int runnerId = getField(msg, 3).toInt();
  String lat = getField(msg, 4);
  String lng = getField(msg, 5);
  int battery = getField(msg, 6).toInt();
  String timestamp = getField(msg, 7);
  int gpsValid = getField(msg, 8).toInt();
  int rssi = getField(msg, 9).toInt();
  float snr = getField(msg, 10).toFloat();

  Serial.println();
  Serial.println("========== 긴급 상황 발생 ==========");
  Serial.print("긴급 ID: ");
  Serial.println(emergencyId);
  Serial.print("Runner ID: ");
  Serial.println(runnerId);
  Serial.print("Relay ID: ");
  Serial.println(relayId);
  Serial.print("위도: ");
  Serial.println(lat);
  Serial.print("경도: ");
  Serial.println(lng);
  Serial.print("배터리: ");
  Serial.println(battery);
  Serial.print("시간: ");
  Serial.println(timestamp);
  Serial.print("GPS 유효: ");
  Serial.println(gpsValid);
  Serial.print("RSSI: ");
  Serial.println(rssi);
  Serial.print("SNR: ");
  Serial.println(snr, 2);
  Serial.println("===================================");

  showOLED("!!! EMERGENCY !!!",
           "Runner " + String(runnerId),
           "Relay " + String(relayId),
           "Batt " + String(battery));
}

void printAndSendCycleData() {
  Serial.println();
  Serial.println("========== Cycle 수신 요약 ==========");
  Serial.print("Cycle: ");
  Serial.println(currentCycleId);
  Serial.print("Relay: ");
  Serial.println(currentRelayId);
  Serial.print("수신 Runner 수: ");
  Serial.println(forwardCount);

  for (int i = 0; i < forwardCount; i++) {
    ForwardData &data = forwardBuffer[i];
    Serial.print("  Runner ");
    Serial.print(data.runnerId);
    Serial.print(" -> 위도=");
    Serial.print(data.lat);
    Serial.print(", 경도=");
    Serial.print(data.lng);
    Serial.print(", Pace=");
    Serial.print(data.pace);
    Serial.print(", 배터리=");
    Serial.println(data.battery);
    sendToServerStub(data);
  }

  showOLED("Gateway " + String(GATEWAY_ID),
           "Relay " + String(currentRelayId),
           "Received " + String(forwardCount),
           "Ch " + String(currentChannelId));
}

void sendToServerStub(ForwardData data) {
  if (SHOW_DEBUG_LOG) {
    Serial.print("서버 전송 준비: relay=");
    Serial.print(data.relayId);
    Serial.print(", runner=");
    Serial.println(data.runnerId);
  }
}

void changeState(GatewayState nextState) {
  if (currentState == nextState) {
    return;
  }
  if (SHOW_STATE_LOG) {
    Serial.print("상태 변경: ");
    Serial.print(stateName(currentState));
    Serial.print(" -> ");
    Serial.println(stateName(nextState));
  }
  currentState = nextState;
  stateStartTime = millis();
}

const char *stateName(GatewayState state) {
  switch (state) {
    case WAIT_FORWARD_START:
      return "WAIT_FORWARD_START";
    case RECEIVE_FORWARD_DATA:
      return "RECEIVE_FORWARD_DATA";
    case WAIT_FORWARD_DONE:
      return "WAIT_FORWARD_DONE";
    case PRINT_OR_SEND_TO_SERVER:
      return "PRINT_OR_SEND_TO_SERVER";
  }
  return "UNKNOWN";
}
