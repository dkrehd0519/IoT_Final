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

#define LORA_FREQUENCY 915E6
#define LORA_SYNC_WORD 0x33
#define LORA_TX_POWER 20
#define LORA_SPREADING_FACTOR 7
#define LORA_BANDWIDTH 125E3
#define LORA_CODING_RATE_DENOMINATOR 5
#define LORA_PREAMBLE_LENGTH 8

#define RELAY_COUNT 2
#define RUNNER_COUNT 3
#define RUNNER_SLOT_MS 150
#define MAX_RUNNERS RUNNER_COUNT

#define RELAY1_ID 1
#define RELAY2_ID 2

// Runner 3명 기준:
// Runner phase 종료 약 4.5초 + 최대 3개 forwarding 약 1.2초를 고려한다.
#define RELAY1_TIMEOUT_MS 8000UL
#define RELAY2_TIMEOUT_MS 5000UL
#define CYCLE_GUARD_MS 1000UL

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
bool oledReady = false;

enum GatewayState {
  START_CYCLE,
  WAIT_RELAY1_DATA,
  SEND_RELAY2_START,
  WAIT_RELAY2_DATA,
  CYCLE_DONE
};

GatewayState currentState = START_CYCLE;

unsigned long cycleId = 1;
unsigned long stateStartTime = 0;

int relay1PacketCount = 0;
int relay2PacketCount = 0;
int relay1DoneCount = -1;
int relay2DoneCount = -1;
bool relay1TimedOut = false;
bool relay2TimedOut = false;
int lastGatewayEmergencySeq[MAX_RUNNERS + 1];

void showOLED(String line1, String line2 = "", String line3 = "", String line4 = "");
void sendLoRaMessage(String msg);
bool receiveLoRaMessage(String &msg, int &rssi, float &snr);
bool startsWithPacket(String msg, String type);
String getField(String msg, int index);
void sendSchedulePacket();
void sendRelay2StartPacket();
void handleForwardPacket(String msg, int rssi, float snr);
void handleDonePacket(String msg);
void changeState(GatewayState nextState);
const char *stateName(GatewayState state);
void updateWaitingOLED();
void handleEmergencyPacket(String msg, int rssi, float snr);

void setup(){
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("========================================");
  Serial.println(" TTGO LoRa32 Marathon Gateway Node");
  Serial.println("========================================");

  // 보드 버전에 따라 OLED_SDA/OLED_SCL 핀 수정이 필요할 수 있다.
  Wire.begin(OLED_SDA, OLED_SCL);
  oledReady = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS);

  if(oledReady){
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    showOLED("Gateway Boot", "OLED ready", "LoRa init...");
    Serial.println("[OLED] Initialization complete");
  } else {
    Serial.println("[OLED][WARN] Initialization failed. Gateway continues without OLED.");
  }

  // 보드 버전에 따라 LoRa 핀 수정이 필요할 수 있다.
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if(!LoRa.begin(LORA_FREQUENCY)){
    Serial.println("[ERROR] LoRa initialization failed. Check wiring and board pins.");
    showOLED("LoRa ERROR", "Check pins", "Gateway stopped");

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

  for(int i = 0; i <= MAX_RUNNERS; i++){
    lastGatewayEmergencySeq[i] = -1;
  }

  currentState = START_CYCLE;
  stateStartTime = millis();
}

void loop(){
  // Relay 수신 단계에서만 LoRa packet을 처리한다.
  if(currentState == WAIT_RELAY1_DATA || currentState == WAIT_RELAY2_DATA){
    String message;
    int rssi = 0;
    float snr = 0.0;

    if(receiveLoRaMessage(message, rssi, snr)){
      if(startsWithPacket(message, "EMERGENCY")){
        handleEmergencyPacket(message, rssi, snr);
      } else if(startsWithPacket(message, "FORWARD")){
        handleForwardPacket(message, rssi, snr);
      } else if(startsWithPacket(message, "DONE")){
        handleDonePacket(message);
      } else {
        Serial.println("[RX] Ignored: expected EMERGENCY, FORWARD or DONE packet");
      }
    }
  }

  switch(currentState){
    case START_CYCLE:
      relay1PacketCount = 0;
      relay2PacketCount = 0;
      relay1DoneCount = -1;
      relay2DoneCount = -1;
      relay1TimedOut = false;
      relay2TimedOut = false;

      Serial.println();
      Serial.print("[CYCLE] Starting cycle ");
      Serial.println(cycleId);
      showOLED("START_CYCLE", "Cycle: " + String(cycleId), "Sending schedule", "R1:0  R2:0");

      sendSchedulePacket();
      changeState(WAIT_RELAY1_DATA);
      break;

    case WAIT_RELAY1_DATA:
      if((unsigned long)(millis()- stateStartTime)>= RELAY1_TIMEOUT_MS){
        relay1TimedOut = true;
        Serial.print("[TIMEOUT] Relay1 DONE not received within ");
        Serial.print(RELAY1_TIMEOUT_MS);
        Serial.println(" ms. Moving to Relay2.");
        changeState(SEND_RELAY2_START);
      }
      break;

    case SEND_RELAY2_START:
      sendRelay2StartPacket();
      changeState(WAIT_RELAY2_DATA);
      break;

    case WAIT_RELAY2_DATA:
      if((unsigned long)(millis()- stateStartTime)>= RELAY2_TIMEOUT_MS){
        relay2TimedOut = true;
        Serial.print("[TIMEOUT] Relay2 DONE not received within ");
        Serial.print(RELAY2_TIMEOUT_MS);
        Serial.println(" ms. Ending cycle.");
        changeState(CYCLE_DONE);
      }
      break;

    case CYCLE_DONE:
      if((unsigned long)(millis()- stateStartTime)>= CYCLE_GUARD_MS){
        cycleId++;
        changeState(START_CYCLE);
      }
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
  // 송신 전 standby로 전환하고, 송신 완료 후 다시 수신 모드로 복귀한다.
  LoRa.idle();
  LoRa.beginPacket();
  LoRa.print(msg);

  if(LoRa.endPacket()== 1){
    Serial.print("[TX] ");
    Serial.println(msg);
  } else {
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
  Serial.print(" | Gateway RSSI=");
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

void sendSchedulePacket(){
  // SCHEDULE,cycle_id,relay_count,runner_count,runner_slot_ms
  String message = "SCHEDULE,";
  message += String(cycleId);
  message += ",";
  message += String(RELAY_COUNT);
  message += ",";
  message += String(RUNNER_COUNT);
  message += ",";
  message += String(RUNNER_SLOT_MS);

  sendLoRaMessage(message);
}

void sendRelay2StartPacket(){
  // SEND_NOW,cycle_id,target_relay_id
  String message = "SEND_NOW,";
  message += String(cycleId);
  message += ",";
  message += String(RELAY2_ID);

  Serial.println("[CONTROL] Requesting Relay2 forwarding");
  showOLED("SEND_RELAY2", "Cycle: " + String(cycleId),
           "R1 packets: " + String(relay1PacketCount), "Sending SEND_NOW");
  sendLoRaMessage(message);
}

void handleForwardPacket(String msg, int rssi, float snr){
  // FORWARD,cycle_id,relay_id,runner_id,lat,lng,pace,battery,seq,rssi,snr
  if(getField(msg, 0)!= "FORWARD" ||
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
    Serial.println("[FORWARD][ERROR] Invalid CSV format");
    return;
  }

  unsigned long packetCycleId =(unsigned long)getField(msg, 1).toInt();
  int relayId = getField(msg, 2).toInt();
  int runnerId = getField(msg, 3).toInt();

  if(packetCycleId != cycleId){
    Serial.print("[FORWARD] Ignored: packet cycle ");
    Serial.print(packetCycleId);
    Serial.print(" does not match active cycle ");
    Serial.println(cycleId);
    return;
  }

  int expectedRelayId =
     (currentState == WAIT_RELAY1_DATA)? RELAY1_ID : RELAY2_ID;
  if(relayId != expectedRelayId){
    Serial.print("[FORWARD] Ignored: expected relay ");
    Serial.print(expectedRelayId);
    Serial.print(", received relay ");
    Serial.println(relayId);
    return;
  }

  if(relayId == RELAY1_ID){
    relay1PacketCount++;
  } else if(relayId == RELAY2_ID){
    relay2PacketCount++;
  }

  Serial.println("[FORWARD] Parsed runner data");
  Serial.print("  cycle_id: ");
  Serial.println(packetCycleId);
  Serial.print("  relay_id: ");
  Serial.println(relayId);
  Serial.print("  runner_id: ");
  Serial.println(runnerId);
  Serial.print("  lat/lng: ");
  Serial.print(getField(msg, 4));
  Serial.print(", ");
  Serial.println(getField(msg, 5));
  Serial.print("  pace/battery/seq: ");
  Serial.print(getField(msg, 6));
  Serial.print(" / ");
  Serial.print(getField(msg, 7));
  Serial.print(" / ");
  Serial.println(getField(msg, 8));
  Serial.print("  Runner-to-Relay RSSI/SNR: ");
  Serial.print(getField(msg, 9));
  Serial.print(" dBm / ");
  Serial.print(getField(msg, 10));
  Serial.println(" dB");
  Serial.print("  Relay-to-Gateway RSSI/SNR: ");
  Serial.print(rssi);
  Serial.print(" dBm / ");
  Serial.print(snr, 2);
  Serial.println(" dB");

  // TODO: 이 지점에서 server HTTP 또는 MQTT publish 함수를 호출할 수 있다.
  updateWaitingOLED();
}

void handleDonePacket(String msg){
  // DONE,cycle_id,relay_id,count
  if(getField(msg, 0)!= "DONE" ||
      getField(msg, 1).length()== 0 ||
      getField(msg, 2).length()== 0 ||
      getField(msg, 3).length()== 0 ||
      getField(msg, 4).length()!= 0){
    Serial.println("[DONE][ERROR] Invalid CSV format");
    return;
  }

  unsigned long packetCycleId =(unsigned long)getField(msg, 1).toInt();
  int relayId = getField(msg, 2).toInt();
  int count = getField(msg, 3).toInt();

  if(packetCycleId != cycleId || count < 0){
    Serial.println("[DONE] Ignored: invalid cycle_id or count");
    return;
  }

  if(currentState == WAIT_RELAY1_DATA && relayId == RELAY1_ID){
    relay1DoneCount = count;
    Serial.print("[DONE] Relay1 complete. Reported count=");
    Serial.print(relay1DoneCount);
    Serial.print(", received FORWARD count=");
    Serial.println(relay1PacketCount);

    if(relay1DoneCount != relay1PacketCount){
      Serial.println("[DONE][WARN] Relay1 reported count differs from received count");
    }

    changeState(SEND_RELAY2_START);
    return;
  }

  if(currentState == WAIT_RELAY2_DATA && relayId == RELAY2_ID){
    relay2DoneCount = count;
    Serial.print("[DONE] Relay2 complete. Reported count=");
    Serial.print(relay2DoneCount);
    Serial.print(", received FORWARD count=");
    Serial.println(relay2PacketCount);

    if(relay2DoneCount != relay2PacketCount){
      Serial.println("[DONE][WARN] Relay2 reported count differs from received count");
    }

    changeState(CYCLE_DONE);
    return;
  }

  Serial.print("[DONE] Ignored: unexpected relay ");
  Serial.print(relayId);
  Serial.print(" while state is ");
  Serial.println(stateName(currentState));
}

void changeState(GatewayState nextState){
  if(currentState != nextState){
    Serial.print("[STATE] ");
    Serial.print(stateName(currentState));
    Serial.print(" -> ");
    Serial.println(stateName(nextState));
  }

  currentState = nextState;
  stateStartTime = millis();

  if(nextState == WAIT_RELAY1_DATA || nextState == WAIT_RELAY2_DATA){
    updateWaitingOLED();
  } else if(nextState == CYCLE_DONE){
    Serial.println("----------------------------------------");
    Serial.print("[RESULT] Cycle ");
    Serial.println(cycleId);
    Serial.print("[RESULT] Relay1 FORWARD=");
    Serial.print(relay1PacketCount);
    Serial.print(", DONE count=");
    Serial.print(relay1DoneCount);
    Serial.print(", timeout=");
    Serial.println(relay1TimedOut ? "YES" : "NO");
    Serial.print("[RESULT] Relay2 FORWARD=");
    Serial.print(relay2PacketCount);
    Serial.print(", DONE count=");
    Serial.print(relay2DoneCount);
    Serial.print(", timeout=");
    Serial.println(relay2TimedOut ? "YES" : "NO");
    Serial.println("----------------------------------------");

    String relay1Result = "R1:" + String(relay1PacketCount);
    relay1Result += relay1TimedOut ? " TIMEOUT" : " DONE";
    String relay2Result = "R2:" + String(relay2PacketCount);
    relay2Result += relay2TimedOut ? " TIMEOUT" : " DONE";

    showOLED("CYCLE_DONE", "Cycle: " + String(cycleId),
             relay1Result, relay2Result);
  }
}

void handleEmergencyPacket(String msg, int rssi, float snr){
  // EMERGENCY,cycle_id,relay_id,runner_id,lat,lng,pace,battery,status,seq,runner_rssi,runner_snr

  if(getField(msg, 0) != "EMERGENCY" ||
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
     getField(msg, 12).length() != 0){
    Serial.println("[EMERGENCY][ERROR] Invalid CSV format");
    return;
  }

  unsigned long packetCycleId = (unsigned long)getField(msg, 1).toInt();
  int relayId = getField(msg, 2).toInt();
  int runnerId = getField(msg, 3).toInt();
  int seq = getField(msg, 9).toInt();

  if(runnerId > 0 && runnerId <= MAX_RUNNERS){
    if(lastGatewayEmergencySeq[runnerId] == seq){
      Serial.print("[EMERGENCY][DUPLICATE] Already displayed. runner=");
      Serial.print(runnerId);
      Serial.print(", seq=");
      Serial.println(seq);
      return;
    }

    lastGatewayEmergencySeq[runnerId] = seq;
  }

  Serial.println();
  Serial.println("🚨 [EMERGENCY RECEIVED]");
  Serial.print("  cycle_id: ");
  Serial.println(packetCycleId);
  Serial.print("  relay_id: ");
  Serial.println(relayId);
  Serial.print("  runner_id: ");
  Serial.println(runnerId);

  Serial.print("  lat/lng: ");
  Serial.print(getField(msg, 4));
  Serial.print(", ");
  Serial.println(getField(msg, 5));

  Serial.print("  pace/battery/status/seq: ");
  Serial.print(getField(msg, 6));
  Serial.print(" / ");
  Serial.print(getField(msg, 7));
  Serial.print(" / ");
  Serial.print(getField(msg, 8));
  Serial.print(" / ");
  Serial.println(getField(msg, 9));

  Serial.print("  Runner-to-Relay RSSI/SNR: ");
  Serial.print(getField(msg, 10));
  Serial.print(" dBm / ");
  Serial.print(getField(msg, 11));
  Serial.println(" dB");

  Serial.print("  Relay-to-Gateway RSSI/SNR: ");
  Serial.print(rssi);
  Serial.print(" dBm / ");
  Serial.print(snr, 2);
  Serial.println(" dB");

  showOLED("!!! EMERGENCY !!!",
           "Runner: " + String(runnerId),
           "Relay: " + String(relayId),
           getField(msg, 4) + "," + getField(msg, 5));
}

const char *stateName(GatewayState state){
  switch(state){
    case START_CYCLE:
      return "START_CYCLE";
    case WAIT_RELAY1_DATA:
      return "WAIT_RELAY1_DATA";
    case SEND_RELAY2_START:
      return "SEND_RELAY2_START";
    case WAIT_RELAY2_DATA:
      return "WAIT_RELAY2_DATA";
    case CYCLE_DONE:
      return "CYCLE_DONE";
    default:
      return "UNKNOWN";
  }
}

void updateWaitingOLED(){
  String stateLine =
     (currentState == WAIT_RELAY1_DATA)? "WAIT RELAY1" : "WAIT RELAY2";
  showOLED(stateLine, "Cycle: " + String(cycleId),
           "R1 packets: " + String(relay1PacketCount),
           "R2 packets: " + String(relay2PacketCount));
}
