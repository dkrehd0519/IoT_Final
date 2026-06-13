#include <SPI.h>
#include <RadioLib.h>
#include <TinyGPSPlus.h>
#include <Wire.h>
#include <XPowersLib.h>

#define LORA_CS    18
#define LORA_DIO1  26
#define LORA_RST   23

SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST);

#define GPS_RX 34
#define GPS_TX 12
#define GPS_BAUD 9600 // Setting a rate to communicate with the GPS module. 

#define RUNNER_ID 1  // Set a unique ID for this runner.
#define LORA_FREQUENCY 923.0
#define LORA_SYNC_WORD 0x33
#define LORA_TX_POWER 17
#define LORA_SPREADING_FACTOR 7
#define LORA_BANDWIDTH 125.0
#define LORA_CODING_RATE_DENOMINATOR 5
#define LORA_PREAMBLE_LENGTH 8

#define BEACON_COLLECTION_MS 1500UL
#define PHASE_GUARD_MS 300UL
#define MAX_RELAY_CANDIDATES 2

// for testing without actual Relay/BEACON, set DUMMY_BEACON_MODE to true to generate fake BEACON packets periodically.
#define DUMMY_BEACON_MODE true
#define DUMMY_BEACON_INTERVAL_MS 8000UL
#define DUMMY_RELAY_ID 1
#define DUMMY_RUNNER_COUNT 3
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

bool emergencyMode = false;
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
int calculatePace();
int readBatteryPercent();
char getRunnerStatus();
String getTimestamp();
double distanceMeters(double lat1, double lon1, double lat2, double lon2); // calculate distance in meters between two GPS coordinates
void handleBeaconPacket(String msg, int rssi, float snr); // store received BEACON packet as RelayCandidate if valid
int selectBestRelay(); // based on RSSI and SNR
void sendRunnerStatus();
void resetBeaconCandidates(); // when a new cycle starts, clear previous candidates and reset related variables
void changeState(RunnerState nextState); 
const char *stateName(RunnerState state); // return the current state
void injectDummyBeaconIfNeeded(); // for testing without actual Relay/BEACON, generate fake BEACON packets periodically

void setup(){
    Serial.begin(115200);
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

    Serial.println("[LoRa] 923 MHz, SF7, BW125 kHz, CR4/5, SyncWord 0x33, CRC ON");

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

    if((unsigned long)(millis() - lastDummyBeaconTime) < DUMMY_BEACON_INTERVAL_MS){
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
    dummyBeacon1 += String(DUMMY_RUNNER_COUNT);
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
    dummyBeacon2 += String(DUMMY_RUNNER_COUNT);
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

    handleBeaconPacket(dummyBeacon1, fakeRssi1, fakeSnr1);

    Serial.print("[DUMMY] ");
    Serial.print(dummyBeacon2);
    Serial.print(" RSSI=");
    Serial.print(fakeRssi2);
    Serial.print(", SNR=");
    Serial.println(fakeSnr2);

    handleBeaconPacket(dummyBeacon2, fakeRssi2, fakeSnr2);

    dummyCycleId++;
}

void loop(){
    // Always read GPS data to keep location updated, even when not sending status.
    //so that the latest position is available when it's time to send.
    updateGPS();
    // updateSOSButton();

    // testing function to simulate receiving BEACON packets without actual Relay hardware.
    injectDummyBeaconIfNeeded();

    // Check the beacon only when in WAIT_BEACON or SELECT_RELAY state.
    if(currentState == WAIT_BEACON || currentState == SELECT_RELAY){
        String message;
        int rssi = 0;
        float snr = 0.0;

        if(receiveLoRaMessage(message, rssi, snr)){
            // If it's a BEACON packet, process it to collect Relay candidates. Otherwise, ignore.
            if(startsWithPacket(message, "BEACON")){
                handleBeaconPacket(message, rssi, snr);
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
                selectedCandidateIndex = selectBestRelay();
                
                // If tehre are no candidates, skip to the end of the cycle and wait for the next BEACON.
                if(selectedCandidateIndex < 0){
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
                Serial.print(" selected by RSSI/SNR, RSSI=");
                Serial.print(selected.rssi);
                Serial.print(" dBm, SNR=");
                Serial.print(selected.snr, 2);
                Serial.println(" dB");

                // Checking the validity of the selected candidate's parameters.
                if(RUNNER_ID < 1){
                    Serial.println("[ERROR] RUNNER_ID must start at 1. Transmission skipped.");
                    changeState(CYCLE_DONE);
                    break;
                }

                if(selectedRunnerCount <= 0 || selectedRunnerSlotMs <= 0){
                    Serial.println("[ERROR] Invalid runner_count or runner_slot_ms. Transmission skipped.");
                    changeState(CYCLE_DONE);
                    break;
                }

                if(RUNNER_ID > selectedRunnerCount){
                    Serial.print("[WARN] RUNNER_ID ");
                    Serial.print(RUNNER_ID);
                    Serial.print(" exceeds runner_count ");
                    Serial.println(selectedRunnerCount);
                    Serial.println("[WARN] No assigned slot; transmission skipped to avoid collision.");
                    changeState(CYCLE_DONE);
                    break;
                }
                
                // Calculating the slot
                unsigned long slotIndex =(unsigned long)(RUNNER_ID - 1);
                unsigned long mySlotDelay = slotIndex *(unsigned long)selectedRunnerSlotMs;

                // The time to send the RUNNER status. (the first BEACON time + collection period + guard time + my slot delay)
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
    if(!gps.location.isValid()) return;

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

    if(dtMs == 0) return;

    double elapsedSec = dtMs / 1000.0;
    double speedMps = movedM / elapsedSec;
    
    if(movedM < 1.0) return;  // ignore small movements 
    if(movedM > 50.0) return; // ignore large jumps
    if(speedMps < 0.1) return; // ignore slow speed 
    if(speedMps > 8.0) return; // ignore fast speed

    totalDistanceM += movedM; // accumulate total distance
    currentPaceSecPerKm = (int)(1000.0 / speedMps); // pace between last two points

    // for average pace
    double totalKm = totalDistanceM / 1000.0;
    double elapsedRunSec = (nowMs - runStartMillis) / 1000.0;

    if(totalKm > 0.01){
        avgPaceSecPerKm = (int)(elapsedRunSec / totalKm);
    }

    prevLat = newLat;
    prevLng = newLng;
    prevGpsMillis = nowMs;
}

// return the average pace (send to Relay)
int calculatePace(){
  return avgPaceSecPerKm;
}

int readBatteryPercent(){
  if(pmuOk && PMU.isBatteryConnect()){
    return PMU.getBatteryPercent();
  }

  return 78;
}

// Check the SOS button state and update the emergency mode accordingly.
void updateSOSButton() {
  int currentButtonState = digitalRead(SOS_BUTTON_PIN);

  // detect the moment when the button is just pressed
  if(currentButtonState == SOS_ACTIVE_LEVEL && lastButtonState != SOS_ACTIVE_LEVEL){
    // emergencyMode = true; // for mode1, 2

    // for mode 3
    emergencyMode = !emergencyMode; 
    if(emergencyMode){
      Serial.println("[SOS] Emergency activated");
    }else{
      Serial.println("[SOS] Emergency cancelled");
    }

    Serial.println("[SOS] Emergency activated");
  }

  lastButtonState = currentButtonState;
}

char getRunnerStatus(){
  // for mode 1
  // int buttonValue = digitalRead(SOS_BUTTON_PIN);
  // if(buttonValue == SOS_ACTIVE_LEVEL){
  //   return 'E';
  // }

  // for mode 2
  // if(emergencyMode){
  //   emergencyMode = false; 
  //   return 'E';
  // }

  // for mode 3
  if(emergencyMode){
    emergencyMode = false; 
    return 'E';
  }

  return 'M';

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

void handleBeaconPacket(String msg, int rssi, float snr){
  // BEACON,cycle_id,relay_id,runner_count,runner_slot_ms
  String cycleField = getField(msg, 1);
  String relayField = getField(msg, 2);
  String countField = getField(msg, 3);
  String slotField = getField(msg, 4);

  if(getField(msg, 0)!= "BEACON" ||
      cycleField.length()== 0 ||
      relayField.length()== 0 ||
      countField.length()== 0 ||
      slotField.length()== 0 ||
      getField(msg, 5).length()!= 0){
    Serial.println("[BEACON][ERROR] Invalid CSV format");
    return;
  }

  int cycleId = cycleField.toInt();
  int relayId = relayField.toInt();
  int runnerCount = countField.toInt();
  int runnerSlotMs = slotField.toInt();

  if(cycleId < 0 || relayId <= 0 || runnerCount <= 0 || runnerSlotMs <= 0){
    Serial.println("[BEACON][ERROR] Invalid field value");
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
      existing.runnerCount = runnerCount;
      existing.runnerSlotMs = runnerSlotMs;
      existing.rssi = rssi;
      existing.snr = snr;

      Serial.print("[BEACON] Duplicate updated with stronger signal: relay ");
      Serial.println(relayId);
    }else{
      Serial.print("[BEACON] Duplicate kept existing stronger signal: relay ");
      Serial.println(relayId);
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
  Serial.print(" ms, RSSI=");
  Serial.print(rssi);
  Serial.print(", SNR=");
  Serial.println(snr, 2);
}

int selectBestRelay(){
  int bestIndex = -1;

  for(int i = 0; i < MAX_RELAY_CANDIDATES; i++){
    if(!relayCandidates[i].valid ||
        relayCandidates[i].cycleId != activeCycleId){
      continue;
    }

    if(bestIndex < 0 ||
        relayCandidates[i].rssi > relayCandidates[bestIndex].rssi ||
       (relayCandidates[i].rssi == relayCandidates[bestIndex].rssi &&
         relayCandidates[i].snr > relayCandidates[bestIndex].snr)){
      bestIndex = i;
    }
  }

  return bestIndex;
}

void sendRunnerStatus(){
  if(selectedCandidateIndex < 0 || selectedRelayId <= 0){
    Serial.println("[TX][ERROR] No selected Relay. Transmission cancelled.");
    return;
  }

  int pace = calculatePace();
  int battery = readBatteryPercent();
  // char status = getRunnerStatus();

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
  // message += String(status);
  // message += ",";
  message += String(seq);
  message += ",";
  message += gpsValid ? "1" : "0";

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
  // Serial.print(", status=");
  // Serial.print(status);

  Serial.println();

  sendLoRaMessage(message);
  seq++;
}

void resetBeaconCandidates(){
  for(int i = 0; i < MAX_RELAY_CANDIDATES; i++){
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
