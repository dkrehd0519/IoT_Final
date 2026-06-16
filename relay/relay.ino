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
#define TEST_DUMMY_MODE true
#define DUMMY_GATEWAY_RECEIVE_MODE true
#define DUMMY_RELAY_CYCLE_INTERVAL_MS 3000UL
#define DUMMY_RUNNER_SLOT_MS 150

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
#define TOTAL_RUNNERS 3
#define GROUP_COVERAGE_M 10000.0f
#define RELAY_INTERVAL_M 500.0f
#define RELAYS_PER_GROUP 2
#define RELAY_MAX_COUNT 2
#define ACTIVE_TIMEOUT_MS 30000UL
#define MAX_ACTIVE_RUNNERS 300
#define MAX_BUFFER_SIZE MAX_ACTIVE_RUNNERS
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

struct ActiveRunner {
  bool active;
  int runnerId;
  unsigned long lastSeen;
};

RelayState currentState = WAIT_SCHEDULE;
RunnerData relayBuffer[MAX_BUFFER_SIZE];
ActiveRunner activeRunners[MAX_ACTIVE_RUNNERS];

int bufferCount = 0;
int activeCycleId = -1;
int relayCount = 0;
int runnerCount = 0;
int runnerSlotMs = 0;

unsigned long scheduleReceivedTime = 0;
unsigned long beaconSendTime = 0;
unsigned long runnerPhaseEndTime = 0;
unsigned long stateStartTime = 0;
unsigned long lastDummyRelayCycleTime = 0;

int dummyRelayCycleId = 1;

// Gateway timeout 등의 이유로 SEND_NOW가 Runner phase 중 먼저 온 경우 기억한다.
bool pendingSendNow = false;

void showOLED(String line1, String line2 = "", String line3 = "", String line4 = "");
void sendLoRaMessage(String msg);
bool receiveLoRaMessage(String &msg, int &rssi, float &snr);
bool startsWithPacket(String msg, String type);
String getField(String msg, int index);
void parseSchedule(String msg);
void sendBeacon();
bool isDuplicateRunnerPacket(int cycleId, int runnerId, int seq);
void addRunnerDataToBuffer(int cycleId, int relayId, int runnerId,
                           String lat, String lng, int pace, int battery,
                           char status, int seq, int rssi, float snr);
void initActiveRunners();
void upsertActiveRunner(int runnerId); // Runner를 active list에 추가하거나 lastSeen 갱신
void removeActiveRunner(int runnerId);
void cleanupActiveRunners(); // timeout된 Runner를 active list에서 제거
int getCurrentCount();
void handleRunnerData(String msg, int rssi, float snr);
void handleHandoverPacket(String msg); // HANDOVER 패킷 처리 및 기존 relay에서 runner 제거
bool isEmergencyDataPacket(String msg);
void handleReceivedPacket(String msg, int rssi, float snr); // 수신 패킷 종류에 따라 처리 함수로 분기
void forwardBufferedPacketsToGateway();
void sendDonePacket();
void handleSendNowPacket(String msg);
void injectDummyRelayCycleIfNeeded();
void clearRelayBuffer();
void changeState(RelayState nextState);
const char *stateName(RelayState state);
void updateOLED();
int lastEmergencySeq[TOTAL_RUNNERS + 1];
void sendEmergencyPacketToGateway(int cycleId, int runnerId,
                                  String lat, String lng,
                                  int pace, int battery,
                                  char status, int seq,
                                  int rssi, float snr);

void dummyGatewayReceive(String msg);
void dummyGatewayHandleDone(String msg);

void setup(){
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

  if(RELAY_ID <= 0){
    Serial.println("[ERROR] RELAY_ID must be greater than 0.");
    while(true){
      delay(1000);
    }
  }

  Wire.begin(OLED_SDA, OLED_SCL);
  oledReady = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS);

  if(oledReady){
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    showOLED("Relay Boot", "Relay ID: " + String(RELAY_ID),
             "OLED ready", "LoRa init...");
    Serial.println("[OLED] Initialization complete");
  }else{
    Serial.println("[OLED][WARN] Initialization failed. Relay continues without OLED.");
  }

  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if(!LoRa.begin(LORA_FREQUENCY)){
    Serial.println("[ERROR] LoRa initialization failed. Check wiring and board pins.");
    showOLED("LoRa ERROR", "Relay: " + String(RELAY_ID),
             "Check pins", "Relay stopped");

    while(true){
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
  initActiveRunners();
  for(int i = 0; i <= TOTAL_RUNNERS; i++){
    lastEmergencySeq[i] = -1;
  }
  lastDummyRelayCycleTime = millis()- DUMMY_RELAY_CYCLE_INTERVAL_MS;
  currentState = WAIT_SCHEDULE;
  stateStartTime = millis();
  updateOLED();
}

void loop(){
  cleanupActiveRunners();
  injectDummyRelayCycleIfNeeded();

  // 현재 상태에 필요한 packet만 처리한다.
  if(currentState == WAIT_SCHEDULE ||
      currentState == COLLECT_RUNNER_DATA ||
      currentState == WAIT_SEND_NOW){

    String message;
    int rssi = 0;
    float snr = 0.0;

    if(receiveLoRaMessage(message, rssi, snr)){
      handleReceivedPacket(message, rssi, snr);
    }
  }

  switch(currentState){
    case WAIT_SCHEDULE:
      break;

    case SEND_BEACON:
      if((long)(millis()- beaconSendTime)>= 0){
        sendBeacon();
        changeState(COLLECT_RUNNER_DATA);
      }
      break;

    case COLLECT_RUNNER_DATA:
      if((long)(millis()- runnerPhaseEndTime)>= 0){
        Serial.println("[RUNNER] Runner collection phase complete");

        if(RELAY_ID == 1){
          changeState(FORWARD_TO_GATEWAY);
        }else if(pendingSendNow){
          Serial.println("[CONTROL] Previously received SEND_NOW is valid");
          changeState(FORWARD_TO_GATEWAY);
        }else {
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
      if((unsigned long)(millis()- stateStartTime)>= SEND_NOW_TIMEOUT_MS){
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

void showOLED(String line1, String line2, String line3, String line4){
  if(!oledReady){
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

void sendLoRaMessage(String msg){
  LoRa.idle();
  LoRa.beginPacket();
  LoRa.print(msg);

  if(LoRa.endPacket()== 1){
    Serial.print("[TX] ");
    Serial.println(msg);

    if(DUMMY_GATEWAY_RECEIVE_MODE){
      dummyGatewayReceive(msg);
    }

  }else {
    Serial.println("[TX][ERROR] LoRa transmission failed");
  }

  LoRa.receive();
}

bool receiveLoRaMessage(String &msg, int &rssi, float &snr){
  int packetSize = LoRa.parsePacket();
  if(packetSize == 0){
    return false;
  }

  msg = "";
  while(LoRa.available()){
    msg +=(char)LoRa.read();
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

bool startsWithPacket(String msg, String type){
  msg.trim();
  type.trim();
  return msg == type || msg.startsWith(type + ",");
}

String getField(String msg, int index){
  if(index < 0){
    return "";
  }

  int fieldStart = 0;
  int currentIndex = 0;

  for(int i = 0; i <= msg.length(); i++){
    if(i == msg.length()|| msg.charAt(i)== ','){
      if(currentIndex == index){
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

void parseSchedule(String msg){
  // SCHEDULE,cycle_id,relay_count,runner_count,runner_slot_ms
  String cycleField = getField(msg, 1);
  String relayCountField = getField(msg, 2);
  String runnerCountField = getField(msg, 3);
  String runnerSlotField = getField(msg, 4);

  if(getField(msg, 0)!= "SCHEDULE" ||
      cycleField.length()== 0 ||
      relayCountField.length()== 0 ||
      runnerCountField.length()== 0 ||
      runnerSlotField.length()== 0 ||
      getField(msg, 5).length()!= 0){
    Serial.println("[SCHEDULE][ERROR] Invalid CSV format");
    return;
  }

  int parsedCycleId = cycleField.toInt();
  int parsedRelayCount = relayCountField.toInt();
  int parsedRunnerCount = runnerCountField.toInt();
  int parsedRunnerSlotMs = runnerSlotField.toInt();

  if(parsedCycleId < 0 ||
      parsedRelayCount <= 0 ||
      parsedRunnerCount <= 0 ||
      parsedRunnerSlotMs <= 0){
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

  // Relay ID가 커져도 Group 안에서는 0~19 beacon slot을 사용한다.
  unsigned long relaySlotIndex =
     (unsigned long)((RELAY_ID - 1)% RELAYS_PER_GROUP);
  beaconSendTime =
      scheduleReceivedTime + relaySlotIndex * RELAY_BEACON_SLOT_MS;

  // 모든 Relay beacon slot과 guard 이후 Runner uplink phase가 시작된다.
  unsigned long beaconPhaseMs =
     (unsigned long)RELAYS_PER_GROUP * RELAY_BEACON_SLOT_MS;
  unsigned long runnerPhaseMs =
     (unsigned long)RELAY_MAX_COUNT *(unsigned long)runnerSlotMs;
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

void sendBeacon(){
  // BEACON,cycle_id,relay_id,currentCount,maxCount,runnerSlotMs
  int currentCount = getCurrentCount();
  String message = "BEACON,";
  message += String(activeCycleId);
  message += ",";
  message += String(RELAY_ID);
  message += ",";
  message += String(currentCount);
  message += ",";
  message += String(RELAY_MAX_COUNT);
  message += ",";
  message += String(runnerSlotMs);

  Serial.print("[BEACON] relay=");
  Serial.print(RELAY_ID);
  Serial.print(", count=");
  Serial.print(currentCount);
  Serial.print("/");
  Serial.print(RELAY_MAX_COUNT);
  Serial.print(", slot=");
  Serial.println((RELAY_ID - 1)% RELAYS_PER_GROUP);
  sendLoRaMessage(message);
}

bool isDuplicateRunnerPacket(int cycleId, int runnerId, int seq){
  for(int i = 0; i < bufferCount; i++){
    if(relayBuffer[i].cycleId == cycleId &&
        relayBuffer[i].runnerId == runnerId &&
        relayBuffer[i].seq == seq){
      return true;
    }
  }

  return false;
}

void addRunnerDataToBuffer(int cycleId, int relayId, int runnerId,
                           String lat, String lng, int pace, int battery,
                           char status,int seq, int rssi, float snr){
  if(bufferCount >= MAX_BUFFER_SIZE){
    Serial.println("[BUFFER][WARN] Buffer full. Runner packet dropped.");
    return;
  }

  if(isDuplicateRunnerPacket(cycleId, runnerId, seq)){
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

void initActiveRunners(){
  for(int i = 0; i < MAX_ACTIVE_RUNNERS; i++){
    activeRunners[i].active = false;
    activeRunners[i].runnerId = -1;
    activeRunners[i].lastSeen = 0;
  }
}

// If the same runner come in update the lastSeen time. Otherwise, insert to the active runner list.
void upsertActiveRunner(int runnerId){
  int emptyIndex = -1;

  for(int i = 0; i < MAX_ACTIVE_RUNNERS; i++){
    if(activeRunners[i].active && activeRunners[i].runnerId == runnerId){
      activeRunners[i].lastSeen = millis();
      return;
    }

    if(!activeRunners[i].active && emptyIndex < 0){
      emptyIndex = i;
    }
  }

  if(emptyIndex < 0){
    Serial.println("[ACTIVE][WARN] Active runner list is full");
    return;
  }

  activeRunners[emptyIndex].active = true;
  activeRunners[emptyIndex].runnerId = runnerId;
  activeRunners[emptyIndex].lastSeen = millis();

  Serial.print("[ACTIVE_ADD] runner ");
  Serial.println(runnerId);
}

// when a relay receives a HandOver packet, it should remove the runner from its active list.
void removeActiveRunner(int runnerId){
  for(int i = 0; i < MAX_ACTIVE_RUNNERS; i++){
    if(activeRunners[i].active &&
        activeRunners[i].runnerId == runnerId){
      activeRunners[i].active = false;
      activeRunners[i].runnerId = -1;
      activeRunners[i].lastSeen = 0;

      Serial.print("[ACTIVE_REMOVE] runner ");
      Serial.println(runnerId);
      return;
    }
  }
}

// If a runner is not heard from for a certain time, it is removed from the active list considering it has finished
void cleanupActiveRunners(){
  unsigned long now = millis();
  for(int i = 0; i < MAX_ACTIVE_RUNNERS; i++){
    if(activeRunners[i].active &&
       (unsigned long)(now - activeRunners[i].lastSeen)>=
            ACTIVE_TIMEOUT_MS){
      int runnerId = activeRunners[i].runnerId;
      activeRunners[i].active = false;
      activeRunners[i].runnerId = -1;
      activeRunners[i].lastSeen = 0;
      Serial.print("[ACTIVE_TIMEOUT_REMOVE] runner ");
      Serial.println(runnerId);
    }
  }
}

int getCurrentCount(){
  int count = 0;

  for(int i = 0; i < MAX_ACTIVE_RUNNERS; i++){
    if(activeRunners[i].active){
      count++;
    }
  }

  return count;
}

void handleRunnerData(String msg, int rssi, float snr){
  // DATA,cycle_id,runner_id,selectedRelayId,lat,lng,pace,battery,seq,gps_valid,status
  if(getField(msg, 0)!= "DATA" ||
      getField(msg, 1).length()== 0 ||
      getField(msg, 2).length()== 0 ||
      getField(msg, 3).length()== 0 ||
      getField(msg, 4).length()== 0 ||
      getField(msg, 5).length()== 0 ||
      getField(msg, 6).length()== 0 ||
      getField(msg, 7).length()== 0 ||
      getField(msg, 8).length()== 0 ||
      getField(msg, 9).length()== 0 ||
      getField(msg, 10).length()== 0 ||
      getField(msg, 11).length()!= 0){
    Serial.println("[DATA][ERROR] Invalid CSV format");
    return;
  }

  int packetCycleId = getField(msg, 1).toInt();
  int runnerId = getField(msg, 2).toInt();
  int targetRelayId = getField(msg, 3).toInt();
  String lat = getField(msg, 4);
  String lng = getField(msg, 5);
  int pace = getField(msg, 6).toInt();
  int battery = getField(msg, 7).toInt();
  int seq = getField(msg, 8).toInt();
  String gpsValidField = getField(msg, 9);
  String statusField = getField(msg, 10);
  char status = statusField.charAt(0);

  if(packetCycleId != activeCycleId){
    Serial.print("[DATA] Dropped: cycle ");
    Serial.print(packetCycleId);
    Serial.print(" does not match active cycle ");
    Serial.println(activeCycleId);
    return;
  }

  if(runnerId <= 0 || runnerId > TOTAL_RUNNERS || seq < 0){
    Serial.println("[DATA][ERROR] Invalid runner_id or seq");
    return;
  }

  if(statusField.length()!= 1 ||
     (status != 'M' && status != 'E')){
    Serial.println("[DATA][ERROR] Invalid status");
    return;
  }

  bool isEmergency = (status == 'E');
  if(targetRelayId != RELAY_ID &&
     !(isEmergency && targetRelayId == 0)){
    Serial.print("[DATA] Dropped: selected relay ");
    Serial.print(targetRelayId);
    Serial.print(" is not this relay ");
    Serial.println(RELAY_ID);
    return;
  }

  if(gpsValidField.length()> 0){
    Serial.print("[DATA] gps_valid=");
    Serial.println(gpsValidField);
  }

  if(isEmergency){
    // LoRa 송신 중에는 수신할 수 없으므로 Runner가 같은 event를 3회 재전송한다.
    upsertActiveRunner(runnerId);

    if(lastEmergencySeq[runnerId] != seq){
      Serial.print("[EMERGENCY][PRIORITY] runner ");
      Serial.print(runnerId);
      Serial.print(", seq=");
      Serial.println(seq);

      sendEmergencyPacketToGateway(packetCycleId, runnerId, lat, lng,
                                   pace, battery, status, seq, rssi, snr);
      lastEmergencySeq[runnerId] = seq;
    }else{
      Serial.print("[EMERGENCY] Duplicate emergency retry ignored from runner ");
      Serial.println(runnerId);
    }

    return;
  }

  // 지금 RUNNER를 보낸 Runner가 이미 이 Relay의 active list에 등록되어 있는지 확인한다.
  bool alreadyActive = false;
  for(int i = 0; i < MAX_ACTIVE_RUNNERS; i++){
    if(activeRunners[i].active && activeRunners[i].runnerId == runnerId){
      alreadyActive = true;
      break;
    }
  }

  // 일반 DATA만 maxCount 제한을 적용한다.
  if(!alreadyActive && getCurrentCount() >= RELAY_MAX_COUNT){
    Serial.print("[DATA] Dropped: relay capacity reached ");
    Serial.print(getCurrentCount());
    Serial.print("/");
    Serial.println(RELAY_MAX_COUNT);
    return;
  }

  upsertActiveRunner(runnerId);
  Serial.print("[DATA_ACCEPT] runner=");
  Serial.print(runnerId);
  Serial.print(", currentCount=");
  Serial.print(getCurrentCount());
  Serial.print("/");
  Serial.println(RELAY_MAX_COUNT);

  addRunnerDataToBuffer(packetCycleId, RELAY_ID, runnerId, lat, lng,
                        pace, battery, status, seq, rssi, snr);
}

void handleHandoverPacket(String msg){
  // HANDOVER,cycle_id,runner_id,oldRelayId,newRelayId
  if(getField(msg, 0)!= "HANDOVER" ||
      getField(msg, 1).length()== 0 ||
      getField(msg, 2).length()== 0 ||
      getField(msg, 3).length()== 0 ||
      getField(msg, 4).length()== 0 ||
      getField(msg, 5).length()!= 0){
    Serial.println("[HANDOVER][ERROR] Invalid CSV format");
    return;
  }

  int packetCycleId = getField(msg, 1).toInt();
  int runnerId = getField(msg, 2).toInt();
  int oldRelayId = getField(msg, 3).toInt();
  int newRelayId = getField(msg, 4).toInt();

  if(packetCycleId != activeCycleId ||
      runnerId <= 0 || runnerId > TOTAL_RUNNERS ||
      oldRelayId <= 0 || newRelayId <= 0){
    Serial.println("[HANDOVER] Ignored invalid cycle or field value");
    return;
  }

  if(oldRelayId == RELAY_ID){
    removeActiveRunner(runnerId);
  }
}

bool isEmergencyDataPacket(String msg){
  return startsWithPacket(msg, "DATA") && getField(msg, 10) == "E";
}

void handleReceivedPacket(String msg, int rssi, float snr){
  if(isEmergencyDataPacket(msg)){
    handleRunnerData(msg, rssi, snr);
  }else if(currentState == WAIT_SCHEDULE &&
      startsWithPacket(msg, "SCHEDULE")){
    parseSchedule(msg);
  }else if(currentState == COLLECT_RUNNER_DATA &&
             startsWithPacket(msg, "DATA")){
    handleRunnerData(msg, rssi, snr);
  }else if(startsWithPacket(msg, "HANDOVER")){
    handleHandoverPacket(msg);
  }else if((currentState == COLLECT_RUNNER_DATA ||
              currentState == WAIT_SEND_NOW)&&
             startsWithPacket(msg, "SEND_NOW")){
    handleSendNowPacket(msg);
  }else {
    Serial.print("[RX] Ignored in state ");
    Serial.println(stateName(currentState));
  }
}

void sendEmergencyPacketToGateway(int cycleId, int runnerId,
                                  String lat, String lng,
                                  int pace, int battery,
                                  char status, int seq,
                                  int rssi, float snr){
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

void forwardBufferedPacketsToGateway(){
  Serial.print("[FORWARD] Sending ");
  Serial.print(bufferCount);
  Serial.println(" buffered packet(s)to Gateway");
  updateOLED();

  for(int i = 0; i < bufferCount; i++){
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

void sendDonePacket(){
  // DONE,cycle_id,relay_id,count
  String message = "DONE,";
  message += String(activeCycleId);
  message += ",";
  message += String(RELAY_ID);
  message += ",";
  message += String(bufferCount);

  sendLoRaMessage(message);
}

void handleSendNowPacket(String msg){
  // SEND_NOW,cycle_id,target_relay_id
  if(getField(msg, 0)!= "SEND_NOW" ||
      getField(msg, 1).length()== 0 ||
      getField(msg, 2).length()== 0 ||
      getField(msg, 3).length()!= 0){
    Serial.println("[SEND_NOW][ERROR] Invalid CSV format");
    return;
  }

  int packetCycleId = getField(msg, 1).toInt();
  int targetRelayId = getField(msg, 2).toInt();

  if(packetCycleId != activeCycleId || targetRelayId != RELAY_ID){
    Serial.println("[SEND_NOW] Ignored: cycle or target relay does not match");
    return;
  }

  if(RELAY_ID != 2){
    Serial.println("[SEND_NOW] Ignored: Relay1 does not require SEND_NOW");
    return;
  }

  if(currentState == COLLECT_RUNNER_DATA){
    pendingSendNow = true;
    Serial.println("[SEND_NOW] Received early; forwarding after Runner phase");
  }else if(currentState == WAIT_SEND_NOW){
    Serial.println("[SEND_NOW] Valid command received");
    changeState(FORWARD_TO_GATEWAY);
  }
}

void injectDummyRelayCycleIfNeeded(){
  if(!TEST_DUMMY_MODE){
    return;
  }

  if(currentState == WAIT_SCHEDULE){
    if((unsigned long)(millis()- lastDummyRelayCycleTime)<
        DUMMY_RELAY_CYCLE_INTERVAL_MS){
      return;
    }

    clearRelayBuffer();
    activeCycleId = dummyRelayCycleId++;
    relayCount = RELAYS_PER_GROUP;
    runnerCount = RELAY_MAX_COUNT;
    runnerSlotMs = DUMMY_RUNNER_SLOT_MS;
    pendingSendNow = false;
    scheduleReceivedTime = millis();
    beaconSendTime = millis();
    unsigned long beaconPhaseMs =
       (unsigned long)RELAYS_PER_GROUP * RELAY_BEACON_SLOT_MS;
    unsigned long runnerPhaseMs =
       (unsigned long)RELAY_MAX_COUNT *(unsigned long)runnerSlotMs;
    runnerPhaseEndTime =
        millis()+ beaconPhaseMs + PHASE_GUARD_MS + runnerPhaseMs;
    lastDummyRelayCycleTime = millis();

    Serial.println();
    Serial.println("[DUMMY] Starting LoRa end-to-end test cycle");
    Serial.print("[DUMMY] cycle=");
    Serial.print(activeCycleId);
    Serial.print(", relay=");
    Serial.println(RELAY_ID);
    Serial.print("[DUMMY] Waiting for real Runner DATA until millis=");
    Serial.println(runnerPhaseEndTime);

    changeState(SEND_BEACON);
  }
}

void dummyGatewayReceive(String msg){
  if(!DUMMY_GATEWAY_RECEIVE_MODE){
    return;
  }

  Serial.println();
  Serial.println("========== [DUMMY GATEWAY RX] ==========");

  if(startsWithPacket(msg, "EMERGENCY")){
    Serial.println("[DUMMY GATEWAY] EMERGENCY RECEIVED");
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

  else if(startsWithPacket(msg, "FORWARD")){
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
    int seq = getField(msg, 8).toInt();

    Serial.print("cycle=");
    Serial.print(cycleId);
    Serial.print(", relay=");
    Serial.print(relayId);
    Serial.print(", runner=");
    Serial.print(runnerId);
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

  else if(startsWithPacket(msg, "DONE")){
    Serial.println("[DUMMY GATEWAY] DONE RECEIVED");
    Serial.print("[DUMMY GATEWAY][DONE] ");
    Serial.println(msg);

    dummyGatewayHandleDone(msg);
  }

  else if(startsWithPacket(msg, "BEACON")){
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

void dummyGatewayHandleDone(String msg){
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

  if(relayId == 1){
    Serial.println("[DUMMY GATEWAY] Relay1 finished.");
    Serial.println("[DUMMY GATEWAY] In real gateway, SEND_NOW would be sent to Relay2.");
  }

  if(relayId == 2){
    Serial.println("[DUMMY GATEWAY] Relay2 finished. Cycle complete.");
  }
}

void clearRelayBuffer(){
  for(int i = 0; i < MAX_BUFFER_SIZE; i++){
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

void changeState(RelayState nextState){
  if(currentState != nextState){
    Serial.print("[STATE] ");
    Serial.print(stateName(currentState));
    Serial.print(" -> ");
    Serial.println(stateName(nextState));
  }

  currentState = nextState;
  stateStartTime = millis();
  updateOLED();
}

const char *stateName(RelayState state){
  switch(state){
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

void updateOLED(){
  String cycleLine =
     (activeCycleId >= 0)? "Cycle: " + String(activeCycleId): "Cycle: -";
  showOLED("Relay " + String(RELAY_ID), String(stateName(currentState)),
           cycleLine, "Buffer: " + String(bufferCount));
}
