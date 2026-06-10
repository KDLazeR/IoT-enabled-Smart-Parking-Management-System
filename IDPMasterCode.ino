#include <WiFi.h>
#include <FirebaseESP32.h>
#include <Wire.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include <LiquidCrystal_I2C.h>
#include "Adafruit_VL53L0X.h"
#include <Adafruit_NeoPixel.h>
#include <U8g2lib.h>

// ::: WIFI & FIREBASE CREDENTIALS :::
#define WIFI_SSID "Kanchana’s iPhone"     
#define WIFI_PASSWORD "jjk001888"                
#define API_KEY "AIzaSyAQ7aiMWt4rP33b9bt0CmzYb-0N6EFlBmw"
#define DATABASE_URL "smart-parking-system-69023-default-rtdb.asia-southeast1.firebasedatabase.app"

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// ::: PIN DEFINITIONS :::
#define ENTRY_SS 5
#define ENTRY_RST 16
#define EXIT_SS 4
#define EXIT_RST 17
#define ENTRY_SERVO 25
#define EXIT_SERVO 26
#define LED_PIN 13
#define LED_COUNT 3
#define TCAADDR 0x70

// ::: OBJECT CREATION :::
MFRC522 rfidEntry(ENTRY_SS, ENTRY_RST);
MFRC522 rfidExit(EXIT_SS, EXIT_RST);
Servo entryGate;
Servo exitGate;
LiquidCrystal_I2C lcd(0x27, 16, 2);
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

Adafruit_VL53L0X sensor1 = Adafruit_VL53L0X();
Adafruit_VL53L0X sensor2 = Adafruit_VL53L0X();
Adafruit_VL53L0X sensor3 = Adafruit_VL53L0X();

// ::: TIMERS :::
unsigned long entryGateTimer = 0;
bool isEntryGateOpen = false;
bool isEntryDenied = false; 

unsigned long exitGateTimer = 0;
int exitGateState = 0; 
String exitingSlot = ""; 

// ::: SLOT STATE MACHINE :::
enum SlotState { FREE, RESERVED, EXPECTING_CAR, OCCUPIED };
SlotState slotStates[3] = {FREE, FREE, FREE};
String assignedUsers[3] = {"", "", ""};
unsigned long reserveTimers[3] = {0, 0, 0};

unsigned long lastFbPoll = 0;
int currentPollSlot = 0;

struct ParkSession {
  String uid;
  String slotName; 
  unsigned long startTime;
  bool isActive;
};
ParkSession sessions[10];

void tcaselect(uint8_t i) {
  if (i > 7) return;
  Wire.beginTransmission(TCAADDR);
  Wire.write(1 << i);
  Wire.endTransmission();
  delayMicroseconds(50); 
}

String getMappedID(String uid) {
  if (uid == "6B416C00") return "REG-001";
  if (uid == "73F77B89") return "REG-002";
  if (uid == "4B30B373") return "RES-001";
  if (uid == "0984A948") return "RES-002";
  return "UNKNOWN";
}

String getRawUID(MFRC522 &reader) {
  String uidString = "";
  for (byte i = 0; i < reader.uid.size; i++) {
    if (reader.uid.uidByte[i] < 0x10) uidString += "0";
    uidString += String(reader.uid.uidByte[i], HEX);
  }
  uidString.toUpperCase();
  return uidString;
}

// FAST DETECTION ToF Sensor Logic (With updated physical routing)
void handleToFSensors(unsigned long currentMillis) {
  for (int i = 0; i < 3; i++) {
    int muxPort = 0;
    
    // Physical hardware routing map
    if (i == 0) muxPort = 2;      // Slot A1 reads Sensor 3 (Port 2)
    else if (i == 1) muxPort = 1; // Slot A2 reads Sensor 2 (Port 1)
    else if (i == 2) muxPort = 0; // Slot A3 reads Sensor 1 (Port 0)

    tcaselect(muxPort);
    
    VL53L0X_RangingMeasurementData_t measure;
    memset(&measure, 0, sizeof(VL53L0X_RangingMeasurementData_t));
    measure.RangeStatus = 4; 
    measure.RangeMilliMeter = 8190; 
    
    if (muxPort == 0) sensor1.rangingTest(&measure, false);
    else if (muxPort == 1) sensor2.rangingTest(&measure, false);
    else if (muxPort == 2) sensor3.rangingTest(&measure, false);

    bool obstacleDetected = (measure.RangeStatus != 4 && measure.RangeMilliMeter < 900 && measure.RangeMilliMeter > 10);
    String slotStr = "A" + String(i + 1);

    // 1. CAR PARKS
    if (obstacleDetected) {
      if (slotStates[i] == FREE || slotStates[i] == EXPECTING_CAR) {
        slotStates[i] = OCCUPIED;
        strip.setPixelColor(i, strip.Color(255, 0, 0)); // RED
        strip.show();
        Firebase.setString(fbdo, "/slots/" + slotStr, "Occupied");
      }
    }
    
    // 2. CAR LEAVES PHYSICAL SLOT
    else if (!obstacleDetected) {
      if (slotStates[i] == OCCUPIED) {
        slotStates[i] = FREE;
        assignedUsers[i] = ""; 
        strip.setPixelColor(i, strip.Color(0, 255, 0)); // GREEN
        strip.show();
        Firebase.setString(fbdo, "/slots/" + slotStr, "Free");
      }
    }

    // 3. RESERVATION TIMEOUT (15s rule)
    if (slotStates[i] == RESERVED) {
      if (currentMillis - reserveTimers[i] >= 15000) {
        slotStates[i] = FREE;
        assignedUsers[i] = ""; 
        strip.setPixelColor(i, strip.Color(0, 255, 0)); // GREEN
        strip.show();
        Firebase.setString(fbdo, "/slots/" + slotStr, "Free");
        Firebase.setString(fbdo, "/slot_names/" + slotStr, " "); 
      }
    }
  }
}

void pollReservations(unsigned long currentMillis) {
  if (currentMillis - lastFbPoll >= 1000) {
    lastFbPoll = currentMillis;
    String slotStr = "A" + String(currentPollSlot + 1);
    
    if (slotStates[currentPollSlot] == FREE) {
      if (Firebase.getString(fbdo, "/slots/" + slotStr)) {
        if (fbdo.stringData() == "Reserved") {
          slotStates[currentPollSlot] = RESERVED;
          reserveTimers[currentPollSlot] = currentMillis; 
          
          Firebase.getString(fbdo, "/slot_names/" + slotStr);
          assignedUsers[currentPollSlot] = fbdo.stringData();
          
          strip.setPixelColor(currentPollSlot, strip.Color(0, 0, 255)); // BLUE
          strip.show();
        }
      }
    }
    currentPollSlot = (currentPollSlot + 1) % 3;
  }
}

void setup() {
  Serial.begin(115200);
  Wire.begin();
  SPI.begin();
  Wire.setClock(400000); 

  Serial.print("Connecting to WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) { delay(500); }
  
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  config.signer.test_mode = true; 
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // NUKE GHOST DATA ON STARTUP
  Serial.println("Wiping ghost data from Firebase...");
  Firebase.setString(fbdo, "/slot_names/A1", " ");
  Firebase.setString(fbdo, "/slot_names/A2", " ");
  Firebase.setString(fbdo, "/slot_names/A3", " ");

  pinMode(ENTRY_SS, OUTPUT); pinMode(EXIT_SS, OUTPUT);
  digitalWrite(ENTRY_SS, HIGH); digitalWrite(EXIT_SS, HIGH);  
  rfidEntry.PCD_Init(); rfidExit.PCD_Init();

  ESP32PWM::allocateTimer(0); ESP32PWM::allocateTimer(1);
  entryGate.setPeriodHertz(50); exitGate.setPeriodHertz(50);
  entryGate.attach(ENTRY_SERVO, 500, 2400); entryGate.write(0); 
  exitGate.attach(EXIT_SERVO, 500, 2400); exitGate.write(0);  

  lcd.init(); lcd.backlight();
  lcd.setCursor(0, 0); lcd.print(" SMART PARKING  ");
  lcd.setCursor(0, 1); lcd.print(" PLEASE SCAN IN ");

  u8g2.begin(); u8g2.clearBuffer(); u8g2.setFont(u8g2_font_6x10_tf); 
  u8g2.drawStr(0, 10, "IoT+ Smart Parking");
  u8g2.drawStr(0, 20, "Management System");
  u8g2.drawHLine(0, 23, 128); u8g2.drawStr(20, 48, "Ready for Exit");
  u8g2.sendBuffer();

  strip.begin(); strip.setBrightness(50);
  strip.setPixelColor(0, strip.Color(0, 255, 0)); 
  strip.setPixelColor(1, strip.Color(0, 255, 0)); 
  strip.setPixelColor(2, strip.Color(0, 255, 0)); 
  strip.show();

  tcaselect(0); sensor1.begin(); sensor1.setMeasurementTimingBudgetMicroSeconds(20000);
  tcaselect(1); sensor2.begin(); sensor2.setMeasurementTimingBudgetMicroSeconds(20000);
  tcaselect(2); sensor3.begin(); sensor3.setMeasurementTimingBudgetMicroSeconds(20000);
}

void loop() {
  unsigned long currentMillis = millis();

  handleToFSensors(currentMillis);
  pollReservations(currentMillis);

  // ==========================================
  // ENTRY GATE LOGIC
  // ==========================================
  if (rfidEntry.PICC_IsNewCardPresent() && rfidEntry.PICC_ReadCardSerial()) {
    String rawUID = getRawUID(rfidEntry);
    String mappedID = getMappedID(rawUID);
    
    if (mappedID == "UNKNOWN") {
      lcd.clear(); lcd.setCursor(0, 0); lcd.print("UNAUTHORIZED");
      isEntryDenied = true; entryGateTimer = currentMillis;
    } 
    else {
      bool alreadyInside = false;
      for (int i=0; i<3; i++) {
        if (assignedUsers[i] == mappedID && slotStates[i] != RESERVED) {
          alreadyInside = true; break;
        }
      }

      if (alreadyInside) {
        lcd.clear(); lcd.setCursor(0, 0); lcd.print("ALREADY INSIDE");
        isEntryDenied = true; entryGateTimer = currentMillis;
      }
      else {
        String assignedSlotStr = "";
        int assignedIndex = -1;
        bool isResCard = mappedID.startsWith("RES");

        if (isResCard) {
          for (int i=0; i<3; i++) {
            if (slotStates[i] == RESERVED && assignedUsers[i] == mappedID) {
              assignedIndex = i; 
              slotStates[i] = EXPECTING_CAR; 
              break;
            }
          }
        }

        if (!isResCard) {
          for (int i=0; i<3; i++) {
            if (slotStates[i] == FREE) {
              assignedIndex = i; 
              slotStates[i] = EXPECTING_CAR;
              assignedUsers[i] = mappedID;
              break;
            }
          }
        }

        if (assignedIndex == -1) {
          lcd.clear(); lcd.setCursor(0, 0); 
          if (isResCard) lcd.print("NO RESERVATION");
          else lcd.print("  PARKING FULL  ");
          isEntryDenied = true; entryGateTimer = currentMillis;
        } 
        else {
          assignedSlotStr = "A" + String(assignedIndex + 1);

          for (int i = 0; i < 10; i++) {
            if (!sessions[i].isActive) {
              sessions[i].uid = mappedID;
              sessions[i].slotName = assignedSlotStr; 
              sessions[i].startTime = currentMillis;
              sessions[i].isActive = true;
              break;
            }
          }

          Firebase.setString(fbdo, "/users/" + mappedID + "/status", "Parked");
          Firebase.setString(fbdo, "/slots/" + assignedSlotStr, "Expecting");
          Firebase.setString(fbdo, "/slot_names/" + assignedSlotStr, mappedID); 
          
          entryGate.write(45);
          isEntryGateOpen = true;
          entryGateTimer = currentMillis; 

          lcd.clear(); lcd.setCursor(0, 0); lcd.print("Welcome " + mappedID);
          lcd.setCursor(0, 1); lcd.print("Go to Slot: " + assignedSlotStr);
        }
      }
    }
    rfidEntry.PICC_HaltA(); 
  }

  if (isEntryGateOpen && (currentMillis - entryGateTimer >= 7000)) {
    entryGate.write(0); isEntryGateOpen = false;
    lcd.clear(); lcd.setCursor(0, 0); lcd.print(" SMART PARKING  ");
    lcd.setCursor(0, 1); lcd.print(" PLEASE SCAN IN ");
  } else if (isEntryDenied && (currentMillis - entryGateTimer >= 3000)) {
    isEntryDenied = false;
    lcd.clear(); lcd.setCursor(0, 0); lcd.print(" SMART PARKING  ");
    lcd.setCursor(0, 1); lcd.print(" PLEASE SCAN IN ");
  }

  // ==========================================
  // EXIT GATE LOGIC
  // ==========================================
  if (exitGateState == 0 && rfidExit.PICC_IsNewCardPresent() && rfidExit.PICC_ReadCardSerial()) {
    String rawUID = getRawUID(rfidExit);
    String mappedID = getMappedID(rawUID);
    
    bool sessionFound = false;
    for (int i = 0; i < 10; i++) {
      if (sessions[i].isActive && sessions[i].uid == mappedID) {
        sessions[i].isActive = false; 
        
        exitingSlot = sessions[i].slotName; 
        
        float rawTimeSeconds = (currentMillis - sessions[i].startTime) / 1000.0;
        int roundedSeconds = round(rawTimeSeconds); 
        
        int durationFee = roundedSeconds * 5; 
        int resFee = mappedID.startsWith("RES") ? 10 : 0;
        int totalFee = durationFee + resFee;
        
        Firebase.setString(fbdo, "/users/" + mappedID + "/status", "Left");
        Firebase.setInt(fbdo, "/users/" + mappedID + "/last_duration", roundedSeconds);
        Firebase.setInt(fbdo, "/users/" + mappedID + "/duration_fee", durationFee);
        Firebase.setInt(fbdo, "/users/" + mappedID + "/res_fee", resFee);
        Firebase.setInt(fbdo, "/users/" + mappedID + "/last_fee", totalFee);

        u8g2.clearBuffer();          
        u8g2.setFont(u8g2_font_6x10_tf); 
        u8g2.drawStr(0, 10, "IoT+ Smart Parking");
        u8g2.drawStr(0, 20, "Management System");
        u8g2.drawHLine(0, 23, 128); 
        
        String durText = "Duration: " + String(roundedSeconds) + "s";
        u8g2.drawStr(0, 38, durText.c_str());

        u8g2.setFont(u8g2_font_ncenB14_tr); 
        String feeText = "Fee: Rs." + String(totalFee);
        u8g2.drawStr(0, 62, feeText.c_str());
        u8g2.sendBuffer(); 
        
        sessionFound = true; break;
      }
    }

    if (sessionFound) {
      exitGateState = 1; 
      exitGateTimer = currentMillis; 
    }
    rfidExit.PICC_HaltA(); 
  }

  if (exitGateState == 1 && (currentMillis - exitGateTimer >= 7000)) {
    exitGate.write(45); 
    exitGateState = 2; 
    exitGateTimer = currentMillis; 
  }
  
  // EXIT GATE CLOSES AND WIPES ID
  else if (exitGateState == 2 && (currentMillis - exitGateTimer >= 7000)) {
    exitGate.write(0); 
    exitGateState = 0; 
    
    if (exitingSlot != "") {
      Firebase.setString(fbdo, "/slot_names/" + exitingSlot, " "); 
      
      int slotIndex = exitingSlot.charAt(1) - '1'; 
      if (slotIndex >= 0 && slotIndex <= 2) {
        if (slotStates[slotIndex] == EXPECTING_CAR) {
           slotStates[slotIndex] = FREE;
           strip.setPixelColor(slotIndex, strip.Color(0, 255, 0));
           strip.show();
           Firebase.setString(fbdo, "/slots/" + exitingSlot, "Free");
        }
      }
      exitingSlot = ""; 
    }

    u8g2.clearBuffer(); u8g2.setFont(u8g2_font_6x10_tf); 
    u8g2.drawStr(0, 10, "IoT+ Smart Parking");
    u8g2.drawStr(0, 20, "Management System");
    u8g2.drawHLine(0, 23, 128); u8g2.drawStr(20, 48, "Ready for Exit");
    u8g2.sendBuffer();
  }
}