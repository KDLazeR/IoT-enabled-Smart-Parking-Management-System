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

// Initialize OLED with SH1106 driver (perfect for 1.3" displays)
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// Initialize 3 ToF Sensors
Adafruit_VL53L0X sensor1 = Adafruit_VL53L0X();
Adafruit_VL53L0X sensor2 = Adafruit_VL53L0X();
Adafruit_VL53L0X sensor3 = Adafruit_VL53L0X();

// ::: GLOBAL VARIABLES :::
unsigned long entryGateTimer = 0;
bool isEntryGateOpen = false;
bool isEntryDenied = false; 

// Exit Gate State Machine Variables
unsigned long exitGateTimer = 0;
int exitGateState = 0; // 0 = Idle, 1 = Showing Receipt (Waiting 7s to open), 2 = Gate Open (Waiting 7s to close)

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
    
    // 1. INSTANT PHYSICAL FEEDBACK 
    if (isOccupied) {
      strip.setPixelColor(ledIndex, strip.Color(255, 0, 0)); // Red
    } else {
      strip.setPixelColor(ledIndex, strip.Color(0, 255, 0)); // Green
    }
    strip.show(); 
    
    // 2. CLOUD UPDATE
    if (isOccupied) {
      Firebase.setString(fbdo, "/slots/" + slotName, "Occupied");
      Serial.println("Slot " + slotName + " is OCCUPIED");
    } else {
      Firebase.setString(fbdo, "/slots/" + slotName, "Free");
      Firebase.setString(fbdo, "/slot_names/" + slotName, ""); 
      Serial.println("Slot " + slotName + " is FREE");
    }
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

  // Initialize OLED (Clean initial layout)
  u8g2.begin();
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf); 
  u8g2.drawStr(0, 10, "IoT+ Smart Parking");
  u8g2.drawStr(0, 20, "Management System");
  u8g2.drawHLine(0, 23, 128); // Underline
  u8g2.drawStr(20, 48, "Ready for Exit");
  u8g2.sendBuffer();

  // Initialize NeoPixels (Start all 3 as Green)
  strip.begin();
  strip.setBrightness(50);
  strip.setPixelColor(0, strip.Color(0, 255, 0)); 
  strip.setPixelColor(1, strip.Color(0, 255, 0)); 
  strip.setPixelColor(2, strip.Color(0, 255, 0)); 
  strip.show();

  // Initialize 3 ToF Sensors
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

      // Upload Status and link the RFID to the assigned slot
      Firebase.setString(fbdo, "/users/" + scannedUID + "/status", "Parked");
      Firebase.setString(fbdo, "/slot_names/" + assigned, scannedUID);
      
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

  // Timer Check for Entry Gate
  if (isEntryGateOpen && (currentMillis - entryGateTimer >= 7000)) {
    entryGate.write(0); 
    isEntryGateOpen = false;
    
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(" SMART PARKING  ");
    lcd.setCursor(0, 1);
    lcd.print(" PLEASE SCAN IN ");
  } 
  else if (isEntryDenied && (currentMillis - entryGateTimer >= 3000)) {
    isEntryDenied = false;
    
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(" SMART PARKING  ");
    lcd.setCursor(0, 1);
    lcd.print(" PLEASE SCAN IN ");
  }

  // ==========================================
  // 3. EXIT GATE & FEE LOGIC (STATE MACHINE)
  // ==========================================
  
  // Accept scan only if system is currently Idle (State 0)
  if (exitGateState == 0 && rfidExit.PICC_IsNewCardPresent() && rfidExit.PICC_ReadCardSerial()) {
    String scannedUID = getUID(rfidExit);
    
    bool sessionFound = false;
    for (int i = 0; i < 10; i++) {
      if (sessions[i].isActive && sessions[i].uid == scannedUID) {
        sessions[i].isActive = false; 
        
        float totalTimeSeconds = (currentMillis - sessions[i].startTime) / 1000.0;
        float totalFee = totalTimeSeconds * 5.0; 
        
        // Push final receipt to Firebase
        Firebase.setString(fbdo, "/users/" + scannedUID + "/status", "Left");
        Firebase.setFloat(fbdo, "/users/" + scannedUID + "/last_duration", totalTimeSeconds);
        Firebase.setFloat(fbdo, "/users/" + scannedUID + "/last_fee", totalFee);

        // Display formatted layout on 1.3" OLED
        u8g2.clearBuffer();          
        u8g2.setFont(u8g2_font_6x10_tf); 
        
        // Underlined main title
        u8g2.drawStr(0, 10, "IoT+ Smart Parking");
        u8g2.drawStr(0, 20, "Management System");
        u8g2.drawHLine(0, 23, 128); 
        
        // Clean duration metrics
        String durText = "Duration: " + String((int)totalTimeSeconds) + "s";
        u8g2.drawStr(0, 40, durText.c_str());

        // Prominent Fee Display
        u8g2.setFont(u8g2_font_ncenB12_tr); 
        String feeText = "Rs. " + String((int)totalFee);
        u8g2.drawStr(0, 60, feeText.c_str());
        
        u8g2.sendBuffer(); 
        
        sessionFound = true;
        break;
      }
    }

    if (sessionFound) {
      // Transition to State 1: Displaying receipt details, waiting 7 seconds to open servo
      exitGateState = 1; 
      exitGateTimer = currentMillis; 
    }

    rfidExit.PICC_HaltA(); 
  }

  // State 1 to 2: 7 seconds have run out, open the exit gate servo
  if (exitGateState == 1 && (currentMillis - exitGateTimer >= 7000)) {
    exitGate.write(45); 
    exitGateState = 2; // Transition to State 2 (Servo open)
    exitGateTimer = currentMillis; // Reset timer loop tracking for final closure
  }
  
  // State 2 to 0: Main open interval finishes, safely drop servo and return to idle
  else if (exitGateState == 2 && (currentMillis - exitGateTimer >= 7000)) {
    exitGate.write(0); 
    exitGateState = 0; 
    
    // Clear display back to default welcome screen layout
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf); 
    u8g2.drawStr(0, 10, "IoT+ Smart Parking");
    u8g2.drawStr(0, 20, "Management System");
    u8g2.drawHLine(0, 23, 128);
    u8g2.drawStr(20, 48, "Ready for Exit");
    u8g2.sendBuffer();
  }
}