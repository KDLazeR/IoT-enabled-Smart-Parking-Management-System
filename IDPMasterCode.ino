#include <WiFi.h>
#include <FirebaseESP32.h>
#include <Wire.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include <LiquidCrystal_I2C.h>
#include "Adafruit_VL53L0X.h"
#include <Adafruit_NeoPixel.h>

// ::: WIFI & FIREBASE CREDENTIALS :::
#define WIFI_SSID "Kanchana’s iPhone"     
#define WIFI_PASSWORD "jjk001888"                
#define API_KEY "AIzaSyAQ7aiMWt4rP33b9bt0CmzYb-0N6EFlBmw"
#define DATABASE_URL "smart-parking-system-69023-default-rtdb.asia-southeast1.firebasedatabase.app"

// Firebase Objects
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

// Initialize 3 ToF Sensors
Adafruit_VL53L0X sensor1 = Adafruit_VL53L0X();
Adafruit_VL53L0X sensor2 = Adafruit_VL53L0X();
Adafruit_VL53L0X sensor3 = Adafruit_VL53L0X();

// ::: GLOBAL VARIABLES :::
unsigned long entryGateTimer = 0;
bool isEntryGateOpen = false;
bool isEntryDenied = false; // Tracks if the "Lot Full" message is showing

unsigned long exitGateTimer = 0;
bool isExitGateOpen = false;

// Track states of all 3 slots
bool isSlot1Occupied = false; 
bool isSlot2Occupied = false; 
bool isSlot3Occupied = false; 

struct ParkSession {
  String uid;
  unsigned long startTime;
  bool isActive;
};
ParkSession sessions[10];

// Multiplexer switch
void tcaselect(uint8_t i) {
  if (i > 7) return;
  Wire.beginTransmission(TCAADDR);
  Wire.write(1 << i);
  Wire.endTransmission();
}

// Convert RFID byte array to String
String getUID(MFRC522 &reader) {
  String uidString = "";
  for (byte i = 0; i < reader.uid.size; i++) {
    if (reader.uid.uidByte[i] < 0x10) uidString += "0";
    uidString += String(reader.uid.uidByte[i], HEX);
  }
  uidString.toUpperCase();
  return uidString;
}

// Helper Function for Checking Slots
void checkSlot(uint8_t tcaPort, Adafruit_VL53L0X &sensor, bool &isOccupied, String slotName, int ledIndex) {
  tcaselect(tcaPort);
  VL53L0X_RangingMeasurementData_t measure;
  sensor.rangingTest(&measure, false);

  bool currentlyOccupied = (measure.RangeStatus != 4 && measure.RangeMilliMeter < 900);

  if (currentlyOccupied != isOccupied) {
    isOccupied = currentlyOccupied;
    
    if (isOccupied) {
      strip.setPixelColor(ledIndex, strip.Color(255, 0, 0)); // Red
      Firebase.setString(fbdo, "/slots/" + slotName, "Occupied");
      Serial.println("Slot " + slotName + " is OCCUPIED");
    } else {
      strip.setPixelColor(ledIndex, strip.Color(0, 255, 0)); // Green
      Firebase.setString(fbdo, "/slots/" + slotName, "Free");
      Serial.println("Slot " + slotName + " is FREE");
    }
    strip.show();
  }
}

void setup() {
  Serial.begin(115200);
  Wire.begin();
  SPI.begin();

  // Connect to WiFi
  Serial.print("Connecting to WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  Serial.println("\nWiFi Connected!");

  // Initialize Firebase
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  config.signer.test_mode = true; 
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // Fix for Dual RFID Conflict
  pinMode(ENTRY_SS, OUTPUT);
  pinMode(EXIT_SS, OUTPUT);
  digitalWrite(ENTRY_SS, HIGH); 
  digitalWrite(EXIT_SS, HIGH);  

  rfidEntry.PCD_Init();
  rfidExit.PCD_Init();

  // Initialize Servos
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  entryGate.setPeriodHertz(50);
  exitGate.setPeriodHertz(50);
  entryGate.attach(ENTRY_SERVO, 500, 2400);
  exitGate.attach(EXIT_SERVO, 500, 2400);
  entryGate.write(0); 
  exitGate.write(0);  

  // Initialize LCD
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print(" SMART PARKING  ");
  lcd.setCursor(0, 1);
  lcd.print(" PLEASE SCAN IN ");

  // Initialize NeoPixels (Start all 3 as Green)
  strip.begin();
  strip.setBrightness(50);
  strip.setPixelColor(0, strip.Color(0, 255, 0)); // A1
  strip.setPixelColor(1, strip.Color(0, 255, 0)); // A2
  strip.setPixelColor(2, strip.Color(0, 255, 0)); // A3
  strip.show();

  // Initialize 3 ToF Sensors on multiplexer ports 0, 1, 2
  tcaselect(0); if (!sensor1.begin()) Serial.println("ToF A1 failed.");
  tcaselect(1); if (!sensor2.begin()) Serial.println("ToF A2 failed.");
  tcaselect(2); if (!sensor3.begin()) Serial.println("ToF A3 failed.");
}

void loop() {
  unsigned long currentMillis = millis();

  // ==========================================
  // 1. CHECK ALL 3 SLOTS
  // ==========================================
  checkSlot(0, sensor1, isSlot1Occupied, "A1", 0);
  checkSlot(1, sensor2, isSlot2Occupied, "A2", 1);
  checkSlot(2, sensor3, isSlot3Occupied, "A3", 2);

  // ==========================================
  // 2. ENTRY GATE LOGIC
  // ==========================================
  if (rfidEntry.PICC_IsNewCardPresent() && rfidEntry.PICC_ReadCardSerial()) {
    
    // Check if the lot is completely full
    bool isLotFull = (isSlot1Occupied && isSlot2Occupied && isSlot3Occupied);

    if (isLotFull) {
      Serial.println("Entry Denied: Parking Lot Full");
      
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("  PARKING FULL  ");
      lcd.setCursor(0, 1);
      lcd.print("   NO ENTRY!    ");
      
      isEntryDenied = true;
      entryGateTimer = currentMillis; 
      
      rfidEntry.PICC_HaltA(); 
      
    } else {
      // NORMAL ENTRY (Lot is not full)
      String scannedUID = getUID(rfidEntry);
      
      for (int i = 0; i < 10; i++) {
        if (!sessions[i].isActive) {
          sessions[i].uid = scannedUID;
          sessions[i].startTime = currentMillis;
          sessions[i].isActive = true;
          break;
        }
      }

      // Dynamic slot assignment display
      String assigned = "A1";
      if (!isSlot1Occupied) assigned = "A1";
      else if (!isSlot2Occupied) assigned = "A2";
      else if (!isSlot3Occupied) assigned = "A3";

      Firebase.setString(fbdo, "/users/" + scannedUID + "/status", "Parked");
      
      entryGate.write(45);
      isEntryGateOpen = true;
      entryGateTimer = currentMillis; 

      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Welcome!");
      lcd.setCursor(0, 1);
      lcd.print("Slot: " + assigned);

      rfidEntry.PICC_HaltA(); 
    }
  }

  // Timer Check: Reset gate and LCD for normal entry (after 7 seconds)
  if (isEntryGateOpen && (currentMillis - entryGateTimer >= 7000)) {
    entryGate.write(0); 
    isEntryGateOpen = false;
    
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(" SMART PARKING  ");
    lcd.setCursor(0, 1);
    lcd.print(" PLEASE SCAN IN ");
  } 
  // Timer Check: Reset LCD if entry was denied (after 3 seconds)
  else if (isEntryDenied && (currentMillis - entryGateTimer >= 3000)) {
    isEntryDenied = false;
    
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(" SMART PARKING  ");
    lcd.setCursor(0, 1);
    lcd.print(" PLEASE SCAN IN ");
  }

  // ==========================================
  // 3. EXIT GATE & FEE LOGIC
  // ==========================================
  if (rfidExit.PICC_IsNewCardPresent() && rfidExit.PICC_ReadCardSerial()) {
    String scannedUID = getUID(rfidExit);
    
    bool sessionFound = false;
    for (int i = 0; i < 10; i++) {
      if (sessions[i].isActive && sessions[i].uid == scannedUID) {
        sessions[i].isActive = false; 
        
        float totalTimeSeconds = (currentMillis - sessions[i].startTime) / 1000.0;
        float totalFee = totalTimeSeconds * 5.0; 
        
        Firebase.setString(fbdo, "/users/" + scannedUID + "/status", "Left");
        Firebase.setFloat(fbdo, "/users/" + scannedUID + "/last_duration", totalTimeSeconds);
        Firebase.setFloat(fbdo, "/users/" + scannedUID + "/last_fee", totalFee);
        
        sessionFound = true;
        break;
      }
    }

    if (sessionFound) {
      exitGate.write(45);
      isExitGateOpen = true;
      exitGateTimer = currentMillis; 
    }

    rfidExit.PICC_HaltA(); 
  }

  if (isExitGateOpen && (currentMillis - exitGateTimer >= 7000)) {
    exitGate.write(0); 
    isExitGateOpen = false;
  }
}