#include <SPI.h>
#include <RadioLib.h>
#include <TinyGPSPlus.h>
#include <Wire.h>
#include <XPowersLib.h>

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
#define GPS_BAUD 9600 // Setting a rate to communicate with the GPS module. 

#define RUNNER_ID 1  // Set a unique ID for this runner.
#define LORA_FREQUENCY 915.0
#define LORA_SYNC_WORD 0x33
#define LORA_TX_POWER 17
#define LORA_SPREADING_FACTOR 7
#define LORA_BANDWIDTH 125.0
#define LORA_CODING_RATE_DENOMINATOR 5
#define LORA_PREAMBLE_LENGTH 8

#define TOTAL_RUNNERS 3
#define GROUP_COVERAGE_M 3000.0f 
#define RELAY_INTERVAL_M 1500.0f
#define RELAYS_PER_GROUP 2
#define RELAY_MAX_COUNT 2
#define RELAY_BEACON_SLOT_MS 500UL
#define BEACON_COLLECTION_MS \
 ((unsigned long)RELAYS_PER_GROUP * RELAY_BEACON_SLOT_MS)
#define PHASE_GUARD_MS 500UL
#define CURRENT_RELAY_TIMEOUT_MS 30000UL
#define MAX_RELAY_CANDIDATES RELAYS_PER_GROUP
#define EMERGENCY_TX_TOTAL_COUNT 3
#define EMERGENCY_RETRY_INTERVAL_MS 800UL
#define EMERGENCY_BACKOFF_MAX_MS 200UL
#define EMERGENCY_BROADCAST_RELAY_ID 0

// for testing without actual Relay/BEACON, set DUMMY_BEACON_MODE to true to generate fake BEACON packets periodically.
#define DUMMY_BEACON_MODE false
#define DUMMY_BEACON_INTERVAL_MS 8000UL
#define DUMMY_RELAY_ID 1
#define DUMMY_CURRENT_COUNT 3
#define DUMMY_MAX_COUNT RELAY_MAX_COUNT
#define DUMMY_RUNNER_SLOT_MS 2000

int dummyCycleId = 1;
unsigned long lastDummyBeaconTime = 0;

// If there are no GPS fix, use this dummy location(Seoul Station)
#define DUMMY_LAT 37.5558
#define DUMMY_LNG 126.9720

// Setting for SOS button on IO38, active LOW(connected to GND when pressed)
#define SOS_BUTTON_PIN 38
#define SOS_ACTIVE_LEVEL LOW

// I2C pins for AXP2101 PMU
#define I2C_SDA 21
#define I2C_SCL 22
#ifndef AXP2101_SLAVE_ADDRESS
#define AXP2101_SLAVE_ADDRESS 0x34
#endif

TinyGPSPlus gps;
HardwareSerial GPSserial(1);

XPowersAXP2101 PMU;
bool pmuOk = false;

enum RunnerState{
  WAIT_BEACON,
  SELECT_RELAY,
  WAIT_MY_SLOT,
  SEND_RUNNER_STATUS,
  CYCLE_DONE
};

struct RelayCandidate{
  bool valid;
  int cycleId;
  int relayId;
  int currentCount;
  int maxCount;
  int runnerSlotMs;
  int rssi;
  float snr;
  unsigned long lastSeen;
};

RunnerState currentState = WAIT_BEACON;
RelayCandidate relayCandidates[MAX_RELAY_CANDIDATES];

unsigned long firstBeaconTime = 0;
unsigned long sendTime = 0;
int activeCycleId = -1;
int selectedCandidateIndex = -1;
int selectedRelayId = -1;
int selectedCurrentCount = 0;
int selectedMaxCount = RELAY_MAX_COUNT;
int selectedRunnerSlotMs = 0;
int selectedRelayRssi = 0;
float selectedRelaySnr = 0.0;
unsigned long selectedRelayLastSeen = 0;

double lastLat = DUMMY_LAT;
double lastLng = DUMMY_LNG;
bool gpsValid = false;
unsigned long lastValidGpsTime = 0;

double startLat = DUMMY_LAT;
double startLng = DUMMY_LNG;
bool startPositionSet = false;

double prevLat = DUMMY_LAT;
double prevLng = DUMMY_LNG;
unsigned long prevGpsMillis = 0;
bool prevPositionSet = false;

double totalDistanceM = 0.0;
int currentPaceSecPerKm = 0;
unsigned long runStartMillis = 0;
int avgPaceSecPerKm = 0;


unsigned long seq = 1;

// interrupt flag for LoRa reception
volatile bool receivedFlag = false;

bool emergencyPending = false;
int emergencyTxCount = 0;
unsigned long nextEmergencyTxTime = 0;
unsigned long emergencySeq = 0;
bool lastButtonState = !SOS_ACTIVE_LEVEL;

void setLoRaFlag(void){
  receivedFlag = true;
}

void sendLoRaMessage(String msg);
bool receiveLoRaMessage(String &msg, int &rssi, float &snr);
bool startsWithPacket(String msg, String type); // Parsing string packet type, e.g., "BEACON"
String getField(String msg, int index); // extract field from CSV string
void updateGPS();
void updateSOSButton();
void handleEmergencyImmediate();
void sendEmergencyDataNow(int targetRelayId, unsigned long eventSeq);
int calculatePace();
int readBatteryPercent();
char getRunnerStatus();
String getTimestamp();
double distanceMeters(double lat1, double lon1, double lat2, double lon2); // calculate distance in meters between two GPS coordinates
int getRelayGroupIndex(float totalDistanceM);
int getGroupStartRelayId(float totalDistanceM);
int getGroupEndRelayId(float totalDistanceM);
bool isRelayInCurrentGroup(int relayId, float totalDistanceM);
void handleBeacon(String msg, int rssi, float snr);
bool canUseRelayCandidate(RelayCandidate c, bool isCurrentRelay);
float getRelayScore(RelayCandidate c);
int selectBestRelay();
bool shouldKeepCurrentRelay();
void updateSelectedRelay();
void sendHandoverPacket(int oldRelayId, int newRelayId);
void sendRunnerStatus();
void resetBeaconCandidates(); // when a new cycle starts, clear previous candidates and reset related variables
void changeState(RunnerState nextState); 
const char *stateName(RunnerState state); // return the current state
void injectDummyBeaconIfNeeded(); // for testing without actual Relay/BEACON, generate fake BEACON packets periodically

void setup(){
    Serial.begin(115200);
    SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
    delay(1000);

    Serial.println();
    Serial.println("========================================");
    Serial.println(" TTGO T-Beam SX1262 Marathon Runner Node");
    Serial.println("========================================");
    Serial.print("[CONFIG] RUNNER_ID: ");
    Serial.println(RUNNER_ID);

    // setup SOS button pin
    pinMode(SOS_BUTTON_PIN, INPUT);
    Serial.println("[SOS] IO38 button configured as INPUT");

    GPSserial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX, GPS_TX);
    Serial.print("[GPS] UART initialized, RX=");
    Serial.print(GPS_RX);
    Serial.print(", TX=");
    Serial.print(GPS_TX);
    Serial.print(", baud=");
    Serial.println(GPS_BAUD);

    // Coonnect to PMU over I2C 
    // Check if it's working. If not, we'll just use dummy battery level.
    Wire.begin(I2C_SDA, I2C_SCL);
    pmuOk = PMU.begin(Wire, AXP2101_SLAVE_ADDRESS, I2C_SDA, I2C_SCL);
    if(pmuOk){
        Serial.println("[PMU] PMU detected");
    }else{
        Serial.println("[PMU] PMU not detected. Use dummy battery=78");
    }

    // Initialize LoRa radio(SX1262)
    Serial.println("[LoRa] Initializing SX1262 with RadioLib...");
    int state = radio.begin(
        LORA_FREQUENCY,
        LORA_BANDWIDTH,
        LORA_SPREADING_FACTOR,
        LORA_CODING_RATE_DENOMINATOR,
        LORA_SYNC_WORD,
        LORA_TX_POWER,
        LORA_PREAMBLE_LENGTH
    );

    if(state == RADIOLIB_ERR_NONE){
        Serial.println("[LoRa] Initialization complete");
    }else{
        Serial.print("[ERROR] RadioLib SX1262 initialization failed, code=");
        Serial.println(state);
        Serial.println("[HINT] Check SX1262 pin map: CS, DIO1, RST, BUSY.");
        while(true){
        delay(1000);
        }
    }

    radio.setCRC(true);

    // Set the DIO1 interrupt handler to set the receivedFlag when a packet is received
    radio.setDio1Action(setLoRaFlag);

    // Start in receive mode to listen for BEACON packets from Relays
    state = radio.startReceive();
    if(state == RADIOLIB_ERR_NONE){
        Serial.println("[LoRa] Receive mode started");
    }else{
        Serial.print("[ERROR] startReceive failed, code=");
        Serial.println(state);
    }

    Serial.println("[LoRa] 915 MHz, SF7, BW125 kHz, CR4/5, SyncWord 0x33, CRC ON");

    // Initialize beacon candidates and change to WAIT_BEACON state
    resetBeaconCandidates();
    changeState(WAIT_BEACON);
}

void injectDummyBeaconIfNeeded(){

    if(!DUMMY_BEACON_MODE){
        return;
    }

    if(currentState != WAIT_BEACON){
        return;
    }

    if((unsigned long)(millis()- lastDummyBeaconTime)< DUMMY_BEACON_INTERVAL_MS){
        return;
    }

    lastDummyBeaconTime = millis();

    int cycleId = dummyCycleId;

    // Relay 1 beacon
    String dummyBeacon1 = "BEACON,";
    dummyBeacon1 += String(cycleId);
    dummyBeacon1 += ",";
    dummyBeacon1 += String(1);   // relay_id = 1
    dummyBeacon1 += ",";
    dummyBeacon1 += String(DUMMY_CURRENT_COUNT);
    dummyBeacon1 += ",";
    dummyBeacon1 += String(DUMMY_MAX_COUNT);
    dummyBeacon1 += ",";
    dummyBeacon1 += String(DUMMY_RUNNER_SLOT_MS);

    int fakeRssi1 = -70;
    float fakeSnr1 = 8.5;

    // Relay 2 beacon
    String dummyBeacon2 = "BEACON,";
    dummyBeacon2 += String(cycleId);
    dummyBeacon2 += ",";
    dummyBeacon2 += String(2);   // relay_id = 2
    dummyBeacon2 += ",";
    dummyBeacon2 += String(DUMMY_CURRENT_COUNT + 20);
    dummyBeacon2 += ",";
    dummyBeacon2 += String(DUMMY_MAX_COUNT);
    dummyBeacon2 += ",";
    dummyBeacon2 += String(DUMMY_RUNNER_SLOT_MS);

    int fakeRssi2 = -45;
    float fakeSnr2 = 6.0;

    Serial.println();
    Serial.println("[DUMMY] Inject 2 dummy BEACONs for relay selection test");

    Serial.print("[DUMMY] ");
    Serial.print(dummyBeacon1);
    Serial.print(" RSSI=");
    Serial.print(fakeRssi1);
    Serial.print(", SNR=");
    Serial.println(fakeSnr1);

    handleBeacon(dummyBeacon1, fakeRssi1, fakeSnr1);

    Serial.print("[DUMMY] ");
    Serial.print(dummyBeacon2);
    Serial.print(" RSSI=");
    Serial.print(fakeRssi2);
    Serial.print(", SNR=");
    Serial.println(fakeSnr2);

    handleBeacon(dummyBeacon2, fakeRssi2, fakeSnr2);

    dummyCycleId++;
}

void loop(){
    // Always read GPS data to keep location updated, even when not sending status.
    //so that the latest position is available when it's time to send.
    updateGPS();
    updateSOSButton();
    handleEmergencyImmediate();

    // testing function to simulate receiving BEACON packets without actual Relay hardware.
    // injectDummyBeaconIfNeeded();

    // Check the beacon only when in WAIT_BEACON or SELECT_RELAY state.
    if(currentState == WAIT_BEACON || currentState == SELECT_RELAY){
        String message;
        int rssi = 0;
        float snr = 0.0;

        if(receiveLoRaMessage(message, rssi, snr)){
            // If it's a BEACON packet, process it to collect Relay candidates. Otherwise, ignore.
            if(startsWithPacket(message, "BEACON")){
                handleBeacon(message, rssi, snr);
            }else{
                Serial.println("[RX] Ignored: packet type is not BEACON");
            }
        }
    }

    // Process per state logic
    switch(currentState){

        case WAIT_BEACON:
            break;

        case SELECT_RELAY:
            /*
            Wait for the BEACON_COLLECTION_MS after receiving the first BEACON to collect candidates,
            then select the best Relay based on RSSI/SNR.
            */ 
            if((unsigned long)(millis()- firstBeaconTime)>= BEACON_COLLECTION_MS){
                updateSelectedRelay();
                
                // If tehre are no candidates, skip to the end of the cycle and wait for the next BEACON.
                if(selectedRelayId <= 0){
                    Serial.println("[SELECT] No valid BEACON. Skip this cycle.");
                    changeState(CYCLE_DONE);
                    break;
                }

                // Checking the validity of the selected candidate's parameters.
                if(RUNNER_ID < 1){
                    Serial.println("[ERROR] RUNNER_ID must start at 1. Transmission skipped.");
                    changeState(CYCLE_DONE);
                    break;
                }

                if(selectedMaxCount <= 0 || selectedRunnerSlotMs <= 0){
                    Serial.println("[ERROR] Invalid max_count or runner_slot_ms. Transmission skipped.");
                    changeState(CYCLE_DONE);
                    break;
                }

                unsigned long slotIndex =
                   (unsigned long)((RUNNER_ID - 1)% selectedMaxCount);
                unsigned long mySlotDelay = slotIndex *(unsigned long)selectedRunnerSlotMs;

                // The time to send the RUNNER status.(the first BEACON time + collection period + guard time + my slot delay)
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
            updateGPS();

            // Check if it's time to send the RUNNER status.
            if((long)(millis()- sendTime)>= 0){
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

void sendLoRaMessage(String msg){
    Serial.print("[TX] ");
    Serial.println(msg);

    radio.standby();

    // Transmit the message. The transmit function will automatically switch to TX mode, send the message.
    int state = radio.transmit(msg);

    // Check if the transmission was successful
    if(state == RADIOLIB_ERR_NONE){
        Serial.println("[TX] Success");
    }else{
        Serial.print("[TX][ERROR] RadioLib transmit failed, code=");
        Serial.println(state);
    }

    receivedFlag = false;

    // After transmitting, start receiving again to listen for the next BEACON or other packets.
    state = radio.startReceive();
    if(state != RADIOLIB_ERR_NONE){
        Serial.print("[RX][ERROR] Restart receive failed, code=");
        Serial.println(state);
    }
}

bool receiveLoRaMessage(String &msg, int &rssi, float &snr){

    // Check if the receivedFlag is set by the DIO1 interrupt, which indicates a packet has been received.
    if(!receivedFlag){
        return false;
    }

    receivedFlag = false;

    // Read the received packet.
    int state = radio.readData(msg);
    // Check if reading was successful
    if(state != RADIOLIB_ERR_NONE){
        Serial.print("[RX][ERROR] readData failed, code=");
        Serial.println(state);
        // Even if reading failed, we should restart receive mode to keep listening for the next packets.
        radio.startReceive();
        return false;
    }

    msg.trim();

    rssi =(int)radio.getRSSI();
    snr = radio.getSNR();

    Serial.print("[RX] ");
    Serial.print(msg);
    Serial.print(" | RSSI=");
    Serial.print(rssi);
    Serial.print(" dBm, SNR=");
    Serial.print(snr, 2);
    Serial.println(" dB");

    // After processing the received packet, start receiving again to listen for the next one.
    radio.startReceive();
    return true;
}

// Check if the received message starts with the expected packet type, e.g., "BEACON".
bool startsWithPacket(String msg, String type){
  msg.trim();
  type.trim();

  return msg == type || msg.startsWith(type + ",");
}

// Extract the field from a CSV.
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

void updateGPS(){
    while(GPSserial.available()> 0){
        gps.encode(GPSserial.read());
    }

    // if the GPS location is not valid, return without updating the position.
    if(!gps.location.isValid())return;

    // Update the last known valid GPS position and time.
    double newLat = gps.location.lat();
    double newLng = gps.location.lng();
    unsigned long nowMs = millis();

    lastLat = newLat;
    lastLng = newLng;
    gpsValid = true;
    lastValidGpsTime = nowMs;

    // If the start position is not set yet, set it to the first valid GPS location.
    if(!startPositionSet){
        startLat = newLat;
        startLng = newLng;
        startPositionSet = true;
        runStartMillis = nowMs;
        Serial.println("[GPS] Start position set");
    }

    // store the previous position and time to calculate distance and pace. 
    if(!prevPositionSet){
        prevLat = newLat;
        prevLng = newLng;
        prevGpsMillis = nowMs;
        prevPositionSet = true;
        return;
    }

    // Calculate the distance moved since the last GPS update and the time elapsed.
    double movedM = distanceMeters(prevLat, prevLng, newLat, newLng);
    unsigned long dtMs = nowMs - prevGpsMillis;

    if(dtMs == 0)return;

    double elapsedSec = dtMs / 1000.0;
    double speedMps = movedM / elapsedSec;
    
    if(movedM < 1.0)return;  // ignore small movements
    if(movedM > 50.0)return; // ignore large jumps
    if(speedMps < 0.1)return; // ignore slow speed
    if(speedMps > 8.0)return; // ignore fast speed

    totalDistanceM += movedM; // accumulate total distance
    currentPaceSecPerKm =(int)(1000.0 / speedMps); // pace between last two points

    // for average pace
    double totalKm = totalDistanceM / 1000.0;
    double elapsedRunSec =(nowMs - runStartMillis)/ 1000.0;

    if(totalKm > 0.01){
        avgPaceSecPerKm =(int)(elapsedRunSec / totalKm);
    }

    prevLat = newLat;
    prevLng = newLng;
    prevGpsMillis = nowMs;
}

// return the average pace(send to Relay)
int calculatePace(){
  return avgPaceSecPerKm;
}

int readBatteryPercent(){
  if(pmuOk && PMU.isBatteryConnect()){
    return PMU.getBatteryPercent();
  }

  return 78;
}

// Detect only the first pressed edge and create one emergency event.
void updateSOSButton(){
  int currentButtonState = digitalRead(SOS_BUTTON_PIN);

  if(currentButtonState == SOS_ACTIVE_LEVEL && lastButtonState != SOS_ACTIVE_LEVEL){
    if(!emergencyPending){
      emergencyPending = true;
      emergencyTxCount = 0;
      emergencySeq++;
      nextEmergencyTxTime = millis() + random(0, EMERGENCY_BACKOFF_MAX_MS);
      Serial.println("[SOS] Emergency one-shot event triggered");
    }
  }

  lastButtonState = currentButtonState;
}

char getRunnerStatus(){
  return 'M';
}

void handleEmergencyImmediate(){
  if(!emergencyPending){
    return;
  }

  if(emergencyTxCount >= EMERGENCY_TX_TOTAL_COUNT){
    emergencyPending = false;
    Serial.println("[EMERGENCY] Emergency burst finished");
    return;
  }

  if((long)(millis() - nextEmergencyTxTime) < 0){
    return;
  }

  int targetRelayId = selectedRelayId;
  if(targetRelayId <= 0){
    targetRelayId = EMERGENCY_BROADCAST_RELAY_ID;
  }

  sendEmergencyDataNow(targetRelayId, emergencySeq);
  emergencyTxCount++;
  nextEmergencyTxTime =
      millis() + EMERGENCY_RETRY_INTERVAL_MS +
      random(0, EMERGENCY_BACKOFF_MAX_MS);
}

void sendEmergencyDataNow(int targetRelayId, unsigned long eventSeq){
  int pace = calculatePace();
  int battery = readBatteryPercent();

  String message = "DATA,";
  message += String(activeCycleId);
  message += ",";
  message += String(RUNNER_ID);
  message += ",";
  message += String(targetRelayId);
  message += ",";
  message += String(lastLat, 6);
  message += ",";
  message += String(lastLng, 6);
  message += ",";
  message += String(pace);
  message += ",";
  message += String(battery);
  message += ",";
  message += String(eventSeq);
  message += ",";
  message += gpsValid ? "1" : "0";
  message += ",E";

  Serial.print("[EMERGENCY][IMMEDIATE_TX] count=");
  Serial.print(emergencyTxCount + 1);
  Serial.print("/");
  Serial.print(EMERGENCY_TX_TOTAL_COUNT);
  Serial.print(", ");
  Serial.println(message);

  sendLoRaMessage(message);
}

String getTimestamp(){
  if(gps.time.isValid()){
    char buffer[7];
    sprintf(buffer, "%02d%02d%02d",
            gps.time.hour(),
            gps.time.minute(),
            gps.time.second());
    return String(buffer);
  }

  unsigned long seconds = millis()/ 1000;

  int h =(seconds / 3600)% 24;
  int m =(seconds / 60)% 60;
  int s = seconds % 60;

  char buffer[7];
  sprintf(buffer, "%02d%02d%02d", h, m, s);

  return String(buffer);
}

double distanceMeters(double lat1, double lon1, double lat2, double lon2){
  const double R = 6371000.0;

  double phi1 = radians(lat1);
  double phi2 = radians(lat2);
  double dphi = radians(lat2 - lat1);
  double dlambda = radians(lon2 - lon1);

  double a = sin(dphi / 2)* sin(dphi / 2)+
             cos(phi1)* cos(phi2)*
             sin(dlambda / 2)* sin(dlambda / 2);

  double c = 2 * atan2(sqrt(a), sqrt(1 - a));

  return R * c;
}

// set a Groupt Index based on the total distance.
int getRelayGroupIndex(float distanceM){
  /*
  0~10km   → groupIndex 0
  10~20km  → groupIndex 1
  20~30km  → groupIndex 2
  */
  if(distanceM < 0.0f)return 0;

  return(int)(distanceM / GROUP_COVERAGE_M);
}

// Get the start Relay ID of the current group based on the total distance.
int getGroupStartRelayId(float distanceM){
  return getRelayGroupIndex(distanceM)* RELAYS_PER_GROUP + 1;
}

// Get the end Relay ID of the current group based on the total distance.
int getGroupEndRelayId(float distanceM){
  return getGroupStartRelayId(distanceM)+ RELAYS_PER_GROUP - 1;
}

// Check if the given Relay ID belongs to the current group
bool isRelayInCurrentGroup(int relayId, float distanceM){
  return relayId >= getGroupStartRelayId(distanceM)&&
         relayId <= getGroupEndRelayId(distanceM);
}

void handleBeacon(String msg, int rssi, float snr){
  // BEACON,cycle_id,relay_id,currentCount,maxCount,runnerSlotMs
  String cycleField = getField(msg, 1);
  String relayField = getField(msg, 2);
  String currentCountField = getField(msg, 3);
  String maxCountField = getField(msg, 4);
  String slotField = getField(msg, 5);

  if(getField(msg, 0)!= "BEACON" ||
      cycleField.length()== 0 ||
      relayField.length()== 0 ||
      currentCountField.length()== 0 ||
      maxCountField.length()== 0 ||
      slotField.length()== 0 ||
      getField(msg, 6).length()!= 0){
    Serial.println("[BEACON][ERROR] Invalid CSV format");
    return;
  }

  int cycleId = cycleField.toInt();
  int relayId = relayField.toInt();
  int currentCount = currentCountField.toInt();
  int maxCount = maxCountField.toInt();
  int runnerSlotMs = slotField.toInt();

  if(cycleId < 0 || relayId <= 0 || currentCount < 0 ||
      maxCount <= 0 || runnerSlotMs <= 0){
    Serial.println("[BEACON][ERROR] Invalid field value");
    return;
  }

  // 이 부분 다시 체크
  if(!isRelayInCurrentGroup(relayId, totalDistanceM)){
    Serial.print("[BEACON] Ignored relay ");
    Serial.print(relayId);
    Serial.print(": current group is ");
    Serial.println(getRelayGroupIndex(totalDistanceM)+ 1);
    return;
  }

  // a new cycle starts
  if(currentState == WAIT_BEACON){
    resetBeaconCandidates();
    activeCycleId = cycleId;
    firstBeaconTime = millis();

    Serial.print("[BEACON] Collection started for cycle ");
    Serial.println(activeCycleId);

    changeState(SELECT_RELAY);
  }

  // If the state is nor WAIT_BEACON, then ignore the BEACON
  if(currentState != SELECT_RELAY){
    Serial.println("[BEACON] Ignored: current cycle is no longer collecting candidates");
    return;
  }

  // If the cycleId does not match the activeCycleId, ignore the BEACON
  if(cycleId != activeCycleId){
    Serial.print("[BEACON] Ignored: cycle ");
    Serial.print(cycleId);
    Serial.print(" does not match active cycle ");
    Serial.println(activeCycleId);
    return;
  }

  int candidateIndex = -1;
  int emptyIndex = -1;

  for(int i = 0; i < MAX_RELAY_CANDIDATES; i++){
    if(relayCandidates[i].valid &&
        relayCandidates[i].cycleId == cycleId &&
        relayCandidates[i].relayId == relayId){
      candidateIndex = i;
      break;
    }

    if(!relayCandidates[i].valid && emptyIndex < 0){
      emptyIndex = i;
    }
  }

  // Update the existing candidate to good performance if the same relayId is found.
  // If the same relayId is received, then update the candidate only when the new signal is better than the previous.
  if(candidateIndex >= 0){
    RelayCandidate &existing = relayCandidates[candidateIndex];

    if(rssi > existing.rssi ||(rssi == existing.rssi && snr > existing.snr)){
      existing.rssi = rssi;
      existing.snr = snr;
    }
    existing.currentCount = currentCount;
    existing.maxCount = maxCount;
    existing.runnerSlotMs = runnerSlotMs;
    existing.lastSeen = millis();

    if(relayId == selectedRelayId){
      selectedRelayLastSeen = existing.lastSeen;
    }

    return;
  }

  if(emptyIndex < 0){
    Serial.print("[BEACON] Candidate list full; relay ");
    Serial.print(relayId);
    Serial.println(" ignored");
    return;
  }

  RelayCandidate &candidate = relayCandidates[emptyIndex];
  candidate.valid = true;
  candidate.cycleId = cycleId;
  candidate.relayId = relayId;
  candidate.currentCount = currentCount;
  candidate.maxCount = maxCount;
  candidate.runnerSlotMs = runnerSlotMs;
  candidate.rssi = rssi;
  candidate.snr = snr;
  candidate.lastSeen = millis();

  if(relayId == selectedRelayId){
    selectedRelayLastSeen = candidate.lastSeen;
  }

  Serial.print("[BEACON] relay=");
  Serial.print(relayId);
  Serial.print(", count=");
  Serial.print(currentCount);
  Serial.print("/");
  Serial.print(maxCount);
  Serial.print(", RSSI=");
  Serial.print(rssi);
  Serial.print(", SNR=");
  Serial.println(snr, 2);
}

bool canUseRelayCandidate(RelayCandidate candidate, bool isCurrentRelay){
  if(!candidate.valid)return false;
  if(candidate.cycleId != activeCycleId)return false;
  if(!isRelayInCurrentGroup(candidate.relayId, totalDistanceM))return false;
  if(candidate.maxCount <= 0)return false;

  if(isCurrentRelay)return candidate.currentCount <= candidate.maxCount;

  return candidate.currentCount < candidate.maxCount;
}

// A scoring function to evaluate the Relay candidates based on RSSI, SNR, and load ratio.
float getRelayScore(RelayCandidate candidate){
  float loadRatio =(float)candidate.currentCount /(float)candidate.maxCount;
  return candidate.rssi * 1.0f + candidate.snr * 2.0f - loadRatio * 20.0f;
}

int selectBestRelay(){
  int bestIndex = -1;

  for(int i = 0; i < MAX_RELAY_CANDIDATES; i++){
    bool isCurrentRelay = relayCandidates[i].relayId == selectedRelayId;
    if(!canUseRelayCandidate(relayCandidates[i], isCurrentRelay)){
      continue;
    }

    if(bestIndex < 0 || getRelayScore(relayCandidates[i])> getRelayScore(relayCandidates[bestIndex])){
      bestIndex = i;
    }
  }

  return bestIndex;
}

bool shouldKeepCurrentRelay(){
  if(selectedRelayId <= 0 || !isRelayInCurrentGroup(selectedRelayId, totalDistanceM)){
    return false;
  }

  for(int i = 0; i < MAX_RELAY_CANDIDATES; i++){
    if(relayCandidates[i].valid && relayCandidates[i].relayId == selectedRelayId){
      return canUseRelayCandidate(relayCandidates[i], true);
    }
  }

  return selectedRelayLastSeen > 0 &&(unsigned long)(millis()- selectedRelayLastSeen)< CURRENT_RELAY_TIMEOUT_MS;
}

void updateSelectedRelay(){
  if(shouldKeepCurrentRelay()){
    for(int i = 0; i < MAX_RELAY_CANDIDATES; i++){
      if(relayCandidates[i].valid &&
         relayCandidates[i].relayId == selectedRelayId){
        selectedCandidateIndex = i;
        selectedCurrentCount = relayCandidates[i].currentCount;
        selectedMaxCount = relayCandidates[i].maxCount;
        selectedRunnerSlotMs = relayCandidates[i].runnerSlotMs;
        selectedRelayRssi = relayCandidates[i].rssi;
        selectedRelaySnr = relayCandidates[i].snr;
        break;
      }
    }
    Serial.print("[RELAY_SELECT] Keep relay ");
    Serial.println(selectedRelayId);
    return;
  }

  int oldRelayId = selectedRelayId;
  int bestIndex = selectBestRelay();
  if(bestIndex < 0){
    selectedCandidateIndex = -1;
    selectedRelayId = -1;
    return;
  }

  RelayCandidate &selected = relayCandidates[bestIndex];
  if(oldRelayId > 0 && oldRelayId != selected.relayId){
    sendHandoverPacket(oldRelayId, selected.relayId);
  }

  selectedCandidateIndex = bestIndex;
  selectedRelayId = selected.relayId;
  selectedCurrentCount = selected.currentCount;
  selectedMaxCount = selected.maxCount;
  selectedRunnerSlotMs = selected.runnerSlotMs;
  selectedRelayRssi = selected.rssi;
  selectedRelaySnr = selected.snr;
  selectedRelayLastSeen = selected.lastSeen;

  Serial.print("[RELAY_SELECT] Selected relay ");
  Serial.print(selectedRelayId);
  Serial.print(", score=");
  Serial.println(getRelayScore(selected), 2);
}

void sendHandoverPacket(int oldRelayId, int newRelayId){
  String message = "HANDOVER,";
  message += String(activeCycleId);
  message += ",";
  message += String(RUNNER_ID);
  message += ",";
  message += String(oldRelayId);
  message += ",";
  message += String(newRelayId);

  Serial.print("[SEND_HANDOVER] ");
  Serial.println(message);
  sendLoRaMessage(message);
}

void sendRunnerStatus(){
  if(selectedRelayId <= 0){
    Serial.println("[TX][ERROR] No selected Relay. Transmission cancelled.");
    return;
  }

  int pace = calculatePace();
  int battery = readBatteryPercent();
  char status = getRunnerStatus();

  // DATA,cycle_id,runner_id,selectedRelayId,lat,lng,pace,battery,seq,gps_valid,status
  String message = "DATA,";
  message += String(activeCycleId);
  message += ",";
  message += String(RUNNER_ID);
  message += ",";
  message += String(selectedRelayId);
  message += ",";
  message += String(lastLat, 6);
  message += ",";
  message += String(lastLng, 6);
  message += ",";
  message += String(pace);
  message += ",";
  message += String(battery);
  message += ",";
  message += String(seq);
  message += ",";
  message += gpsValid ? "1" : "0";
  message += ",";
  message += String(status);

  Serial.print("[GPS] valid=");
  Serial.print(gpsValid ? 1 : 0);
  Serial.print(", lat=");
  Serial.print(lastLat, 5);
  Serial.print(", lng=");
  Serial.print(lastLng, 5);
  Serial.print(", distance=");
  Serial.print(totalDistanceM);
  Serial.print(" m, selected_relay=");
  Serial.print(selectedRelayId);
  Serial.print(", pace=");
  Serial.print(pace);
  Serial.print(", battery=");
  Serial.print(battery);
  Serial.print(", status=");
  Serial.print(status);
  Serial.print(", relay_rssi=");
  Serial.print(selectedRelayRssi);
  Serial.print(", relay_snr=");
  Serial.print(selectedRelaySnr, 2);

  Serial.println();

  sendLoRaMessage(message);
  seq++;
}

void resetBeaconCandidates(){
  for(int i = 0; i < MAX_RELAY_CANDIDATES; i++){
    relayCandidates[i].cycleId = -1;
    relayCandidates[i].relayId = -1;
    relayCandidates[i].currentCount = 0;
    relayCandidates[i].maxCount = 0;
    relayCandidates[i].runnerSlotMs = 0;
    relayCandidates[i].rssi = -999;
    relayCandidates[i].snr = -999.0;
    relayCandidates[i].lastSeen = 0;
    relayCandidates[i].valid = false;
  }

  firstBeaconTime = 0;
  sendTime = 0;
  activeCycleId = -1;
  selectedCandidateIndex = -1;
}

void changeState(RunnerState nextState){
  if(currentState != nextState){
    Serial.print("[STATE] ");
    Serial.print(stateName(currentState));
    Serial.print(" -> ");
    Serial.println(stateName(nextState));
  }

  currentState = nextState;
}

const char *stateName(RunnerState state){
  switch(state){
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
