/*
 * ============================================================
 *  Digital Safe System - ESP32-S3 (Real Hardware)
 *  25CSCI11C - Introduction to Embedded Systems
 *  British University in Egypt - Group A-14
 * ============================================================
 *  Team: Zeina Alaaeldin (254588), Zainab Sabit (257156),
 *        Mahmoud Samir (257678), Ziad Youssef (257745)
 * ============================================================
 *  OLED:     1.5" SH1107 128x128 I2C (SDA=8, SCL=9)
 *  RFID:     MFRC522 SPI (SS=10, RST=14, SCK=12, MOSI=11, MISO=13)
 *  Servo:    GPIO 18
 *  Red LED:  GPIO 15 (via 220 ohm)
 *  Green LED:GPIO 16 (via 220 ohm)
 *  Buzzer:   GPIO 17
 *  Keypad:   Rows=1,2,42,41  Cols=40,39,38,37
 *  Bluetooth:ESP32-S3 built-in BLE
 * ============================================================
 */

#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <ESP32Servo.h>
#include <Keypad.h>
#include <EEPROM.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ─── Pins ────────────────────────────────────────────────────
#define RFID_SS   10
#define RFID_RST  14
#define OLED_SDA  8
#define OLED_SCL  9
#define SERVO_PIN 18
#define RED_LED   15
#define GREEN_LED 16
#define BUZZER    17

// ─── OLED (1.5" SH1107 128x128) ─────────────────────────────
Adafruit_SH1107 display(128, 128, &Wire, -1);

// ─── RFID ────────────────────────────────────────────────────
MFRC522 rfid(RFID_SS, RFID_RST);

// ─── Servo ───────────────────────────────────────────────────
Servo lockServo;

// ─── Keypad ──────────────────────────────────────────────────
const byte ROWS = 4, COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {1, 2, 42, 41};
byte colPins[COLS] = {40, 39, 38, 37};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// ─── BLE ─────────────────────────────────────────────────────
#define SERVICE_UUID  "12345678-1234-1234-1234-123456789abc"
#define TX_UUID       "abcd1234-ab12-cd34-ef56-123456789abc"
#define RX_UUID       "abcd5678-ab12-cd34-ef56-123456789abc"
BLEServer* pServer = NULL;
BLECharacteristic* pTxChar = NULL;
bool bleConnected = false;
String bleCommand = "";
bool newBleCmd = false;

// ─── EEPROM ──────────────────────────────────────────────────
#define EEPROM_SIZE  64
#define EEPROM_FLAG  0
#define EEPROM_PASS  1
#define EEPROM_RFID  10

// ─── State ───────────────────────────────────────────────────
String password    = "1234";
String masterCode  = "0000";
String input       = "";
String rfidUID     = "";
bool   isOpen      = false;
bool   lockedOut   = false;
int    fails       = 0;
bool   changingPass = false;
bool   oldPassOk   = false;
bool   regRFID     = false;

// ═════════════════════════════════════════════════════════════
//                     BLE CALLBACKS
// ═════════════════════════════════════════════════════════════
class ServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer* s)    { bleConnected = true;  }
  void onDisconnect(BLEServer* s) { bleConnected = false; s->startAdvertising(); }
};

class RxCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) {
    String v = c->getValue().c_str();
    v.trim();
    if (v.length() > 0) { bleCommand = v; newBleCmd = true; }
  }
};

// ═════════════════════════════════════════════════════════════
//                       HELPERS
// ═════════════════════════════════════════════════════════════
void beep(int ms) { digitalWrite(BUZZER, HIGH); delay(ms); digitalWrite(BUZZER, LOW); }
void successBeep() { beep(100); delay(80); beep(100); }
void errorBeep() { beep(150); delay(100); beep(150); delay(100); beep(150); }

void bleSend(String msg) {
  if (bleConnected && pTxChar) {
    pTxChar->setValue(msg.c_str());
    pTxChar->notify();
  }
}

String stars() {
  String s = "";
  for (unsigned int i = 0; i < input.length(); i++) s += "*";
  return s;
}

// ─── EEPROM ──────────────────────────────────────────────────
void savePassword() {
  EEPROM.write(EEPROM_FLAG, 0xAA);
  for (int i = 0; i < 4; i++) EEPROM.write(EEPROM_PASS + i, password[i]);
  EEPROM.commit();
}

void loadPassword() {
  if (EEPROM.read(EEPROM_FLAG) == 0xAA) {
    password = "";
    for (int i = 0; i < 4; i++) password += (char)EEPROM.read(EEPROM_PASS + i);
  }
}

void saveRFID() {
  EEPROM.write(EEPROM_RFID, rfidUID.length());
  for (unsigned int i = 0; i < rfidUID.length() && i < 14; i++)
    EEPROM.write(EEPROM_RFID + 1 + i, rfidUID[i]);
  EEPROM.commit();
}

void loadRFID() {
  byte len = EEPROM.read(EEPROM_RFID);
  if (len > 0 && len < 15) {
    rfidUID = "";
    for (int i = 0; i < len; i++) rfidUID += (char)EEPROM.read(EEPROM_RFID + 1 + i);
  }
}

// ─── RFID ────────────────────────────────────────────────────
String readUID() {
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  return uid;
}

// ═════════════════════════════════════════════════════════════
//                    DISPLAY FUNCTIONS
// ═════════════════════════════════════════════════════════════
void showText(String l1, String l2 = "", String l3 = "") {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 10);  display.println(l1);
  display.setCursor(0, 35);  display.println(l2);
  if (l3.length() > 0) { display.setCursor(0, 60); display.println(l3); }
  display.display();
}

void drawLockIcon(int x, int y, bool locked) {
  // Lock body
  display.fillRoundRect(x, y + 25, 40, 35, 5, SH110X_WHITE);
  // Shackle
  if (locked) {
    display.drawRoundRect(x + 8, y, 24, 28, 10, SH110X_WHITE);
    display.drawRoundRect(x + 10, y + 2, 20, 26, 8, SH110X_WHITE);
    display.fillRect(x + 9, y + 22, 22, 10, SH110X_BLACK);
  } else {
    display.drawRoundRect(x + 14, y - 5, 24, 28, 10, SH110X_WHITE);
    display.drawRoundRect(x + 16, y - 3, 20, 26, 8, SH110X_WHITE);
    display.fillRect(x + 15, y + 18, 22, 10, SH110X_BLACK);
  }
  // Keyhole
  display.fillCircle(x + 20, y + 38, 4, SH110X_BLACK);
  display.fillRect(x + 18, y + 42, 5, 7, SH110X_BLACK);
}

void showLocked() {
  display.clearDisplay();
  display.drawRoundRect(2, 2, 124, 124, 6, SH110X_WHITE);

  drawLockIcon(44, 10, true);

  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(25, 75);  display.println("SAFE  LOCKED");
  display.setCursor(15, 90);  display.println("0-9: Enter pass");
  display.setCursor(15, 100); display.println("A:ChgPass B:RFID");
  display.setCursor(15, 110); display.println("D:Master code");
  display.display();
}

void showUnlocked() {
  display.clearDisplay();
  display.drawRoundRect(2, 2, 124, 124, 6, SH110X_WHITE);

  drawLockIcon(44, 10, false);

  display.setTextSize(2);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(25, 78);
  display.println("OPEN");

  display.setTextSize(1);
  display.setCursor(20, 105);
  display.println("Press # to CLOSE");
  display.display();
}

void showPasswordEntry(String title, String masked) {
  display.clearDisplay();
  display.drawRoundRect(2, 2, 124, 124, 6, SH110X_WHITE);

  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(15, 20);
  display.println(title);

  // Draw input boxes
  for (int i = 0; i < 4; i++) {
    int bx = 20 + i * 25;
    display.drawRoundRect(bx, 45, 20, 28, 4, SH110X_WHITE);
    if (i < (int)masked.length()) {
      display.setTextSize(2);
      display.setCursor(bx + 5, 49);
      display.print("*");
    }
  }

  display.setTextSize(1);
  display.setCursor(15, 85);
  display.println("# to confirm");
  display.setCursor(15, 100);
  display.println("* to cancel");
  display.display();
}

// ─── Animations ──────────────────────────────────────────────
void animSplash() {
  display.clearDisplay();
  display.drawRoundRect(4, 4, 120, 120, 8, SH110X_WHITE);
  display.drawRoundRect(6, 6, 116, 116, 6, SH110X_WHITE);
  display.display();
  delay(300);

  // Type "DIGITAL"
  display.setTextSize(2);
  display.setTextColor(SH110X_WHITE);
  const char* w1 = "DIGITAL";
  for (int i = 0; i < 7; i++) {
    display.setCursor(16 + i * 14, 25);
    display.print(w1[i]);
    display.display();
    delay(100);
  }

  // Type "SAFE"
  const char* w2 = "SAFE";
  for (int i = 0; i < 4; i++) {
    display.setCursor(30 + i * 14, 50);
    display.print(w2[i]);
    display.display();
    delay(100);
  }

  delay(300);
  display.setTextSize(1);
  display.setCursor(20, 80);
  display.println("Group A-14");
  display.setCursor(8, 95);
  display.println("BUE | ES Project");
  display.display();
  delay(1500);
}

void animLoadingBar() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(15, 30);
  display.println("INITIALIZING...");
  display.drawRoundRect(14, 55, 100, 16, 4, SH110X_WHITE);
  display.display();

  for (int i = 0; i <= 96; i += 3) {
    display.fillRoundRect(16, 57, i, 12, 3, SH110X_WHITE);
    display.fillRect(30, 80, 70, 15, SH110X_BLACK);
    display.setCursor(45, 82);
    display.print((i * 100) / 96);
    display.print("%");
    display.display();
    delay(25);
  }

  display.setCursor(40, 100);
  display.println("READY!");
  display.display();
  delay(800);
}

void animUnlock() {
  // Shackle opening animation
  for (int offset = 0; offset <= 12; offset += 2) {
    display.clearDisplay();
    display.drawRoundRect(2, 2, 124, 124, 6, SH110X_WHITE);

    // Lock body
    display.fillRoundRect(44, 45, 40, 35, 5, SH110X_WHITE);
    // Moving shackle
    display.drawRoundRect(52 + offset / 2, 20 - offset, 24, 28, 10, SH110X_WHITE);
    display.drawRoundRect(54 + offset / 2, 22 - offset, 20, 26, 8, SH110X_WHITE);
    display.fillRect(53 + offset / 2, 42 - offset, 22, 10, SH110X_BLACK);
    // Keyhole
    display.fillCircle(64, 58, 4, SH110X_BLACK);
    display.fillRect(62, 62, 5, 7, SH110X_BLACK);

    display.setTextSize(1);
    display.setTextColor(SH110X_WHITE);
    display.setCursor(25, 90);
    display.println("UNLOCKING...");
    display.display();
    delay(70);
  }

  // Flash
  display.invertDisplay(true);  delay(100);
  display.invertDisplay(false); delay(100);
  display.invertDisplay(true);  delay(80);
  display.invertDisplay(false);

  // ACCESS GRANTED
  display.clearDisplay();
  display.drawRoundRect(2, 2, 124, 124, 6, SH110X_WHITE);
  display.setTextSize(2);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(12, 40);
  display.println("ACCESS");
  display.setCursor(8, 65);
  display.println("GRANTED");
  display.display();
  delay(1000);
}

void animLock() {
  // Shackle closing animation
  for (int offset = 12; offset >= 0; offset -= 2) {
    display.clearDisplay();
    display.drawRoundRect(2, 2, 124, 124, 6, SH110X_WHITE);

    display.fillRoundRect(44, 45, 40, 35, 5, SH110X_WHITE);
    display.drawRoundRect(52 + offset / 2, 20 - offset, 24, 28, 10, SH110X_WHITE);
    display.drawRoundRect(54 + offset / 2, 22 - offset, 20, 26, 8, SH110X_WHITE);
    display.fillRect(53 + offset / 2, 42 - offset, 22, 10, SH110X_BLACK);
    display.fillCircle(64, 58, 4, SH110X_BLACK);
    display.fillRect(62, 62, 5, 7, SH110X_BLACK);

    display.setTextSize(1);
    display.setTextColor(SH110X_WHITE);
    display.setCursor(30, 90);
    display.println("LOCKING...");
    display.display();
    delay(70);
  }
  delay(300);
}

void animAlarm() {
  for (int i = 0; i < 10; i++) {
    display.clearDisplay();
    display.setTextSize(2);
    display.setTextColor(SH110X_WHITE);
    display.setCursor(10, 30);
    display.println("ALARM!");
    display.setTextSize(1);
    display.setCursor(10, 60);
    display.println("INTRUDER ALERT");
    display.display();

    digitalWrite(RED_LED, HIGH);
    digitalWrite(BUZZER, HIGH);
    delay(200);

    display.invertDisplay(true);
    digitalWrite(RED_LED, LOW);
    digitalWrite(BUZZER, LOW);
    delay(200);
    display.invertDisplay(false);
  }
  digitalWrite(RED_LED, HIGH);
}

// ─── Lock / Unlock ───────────────────────────────────────────
void lockSafe() {
  isOpen = false;
  lockServo.write(0);
  digitalWrite(RED_LED, HIGH);
  digitalWrite(GREEN_LED, LOW);
  animLock();
  beep(200);
  showLocked();
  bleSend("STATUS:LOCKED");
  Serial.println("[LOCKED]");
}

void unlockSafe() {
  isOpen = true;
  lockServo.write(90);
  digitalWrite(RED_LED, LOW);
  digitalWrite(GREEN_LED, HIGH);
  animUnlock();
  successBeep();
  showUnlocked();
  bleSend("STATUS:UNLOCKED");
  Serial.println("[UNLOCKED]");
}

// ═════════════════════════════════════════════════════════════
//                         SETUP
// ═════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  Serial.println("=== Digital Safe - ESP32-S3 ===");
  Serial.println("Group A-14 | BUE 2026\n");

  // EEPROM
  EEPROM.begin(EEPROM_SIZE);
  loadPassword();
  loadRFID();

  // I2C + OLED
  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(0x3C, true)) {
    Serial.println("[ERROR] OLED fail!");
    while(1);
  }
  display.setRotation(0);
  Serial.println("[OK] OLED 1.5\" SH1107 128x128");

  // SPI + RFID
  SPI.begin(12, 13, 11, RFID_SS);
  rfid.PCD_Init();
  Serial.println("[OK] RFID MFRC522");

  // Servo
  lockServo.attach(SERVO_PIN);
  lockServo.write(0);
  Serial.println("[OK] Servo");

  // GPIO
  pinMode(RED_LED, OUTPUT);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(BUZZER, OUTPUT);
  digitalWrite(RED_LED, HIGH);
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(BUZZER, LOW);

  // BLE
  BLEDevice::init("DigitalSafe_A14");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCB());
  BLEService* svc = pServer->createService(SERVICE_UUID);
  pTxChar = svc->createCharacteristic(TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  pTxChar->addDescriptor(new BLE2902());
  BLECharacteristic* pRx = svc->createCharacteristic(RX_UUID, BLECharacteristic::PROPERTY_WRITE);
  pRx->setCallbacks(new RxCB());
  svc->start();
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(true);
  adv->start();
  Serial.println("[OK] BLE: DigitalSafe_A14");

  // Startup animations
  animSplash();
  animLoadingBar();

  showLocked();
  Serial.println("\nReady! Password: " + password);
}

// ═════════════════════════════════════════════════════════════
//                       MAIN LOOP
// ═════════════════════════════════════════════════════════════
void loop() {

  // ─── BLE ───────────────────────────────────────────────────
  if (newBleCmd) {
    newBleCmd = false;
    String cmd = bleCommand;
    if (cmd.startsWith("PASS:")) {
      String p = cmd.substring(5);
      if (p == password || p == masterCode) {
        fails = 0; lockedOut = false;
        unlockSafe();
        bleSend("RESULT:Access granted");
      } else {
        fails++; errorBeep();
        bleSend("RESULT:Wrong password");
        if (fails >= 3) {
          lockedOut = true;
          showText("!! LOCKED !!", "BT wrong pass", "Use master/RFID");
          animAlarm();
        }
      }
    } else if (cmd == "LOCK") {
      if (isOpen) { lockSafe(); bleSend("RESULT:Locked"); }
      else bleSend("RESULT:Already locked");
    } else if (cmd == "STATUS") {
      bleSend(isOpen ? "STATUS:UNLOCKED" : "STATUS:LOCKED");
    }
  }

  // ─── Serial debug ─────────────────────────────────────────
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.startsWith("PASS:")) {
      String p = cmd.substring(5);
      if (p == password || p == masterCode) { fails = 0; lockedOut = false; unlockSafe(); }
      else { fails++; errorBeep(); if (fails >= 3) { lockedOut = true; showText("!! LOCKED !!","",""); animAlarm(); } }
    } else if (cmd == "LOCK" && isOpen) { lockSafe(); }
    else if (cmd == "STATUS") { Serial.println(isOpen ? "UNLOCKED" : "LOCKED"); }
  }

  // ─── RFID (locked) ────────────────────────────────────────
  if (!isOpen && !regRFID) {
    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
      String uid = readUID();
      Serial.println("[RFID] " + uid);
      if (rfidUID.length() > 0 && uid == rfidUID) {
        fails = 0; lockedOut = false;
        showText("RFID OK!", "Welcome!"); delay(800);
        unlockSafe();
      } else {
        errorBeep();
        showText("RFID Denied!", "Unknown card"); delay(1500);
        lockedOut ? showText("!! LOCKED !!","Too many tries","Use master/RFID") : showLocked();
      }
      rfid.PICC_HaltA(); rfid.PCD_StopCrypto1();
    }
  }

  // ─── RFID registration ────────────────────────────────────
  if (regRFID) {
    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
      rfidUID = readUID();
      saveRFID();
      successBeep();
      showText("RFID Registered!", rfidUID);
      Serial.println("[RFID] Saved: " + rfidUID);
      delay(2000);
      rfid.PICC_HaltA(); rfid.PCD_StopCrypto1();
      regRFID = false;
      showLocked();
    }
  }

  // ─── Keypad ───────────────────────────────────────────────
  char key = keypad.getKey();
  if (!key) return;
  beep(50);

  // Safe is open
  if (isOpen) { if (key == '#') lockSafe(); return; }

  // RFID reg mode
  if (regRFID) { if (key == '*') { regRFID = false; showLocked(); } return; }

  // Changing password
  if (changingPass) {
    if (key >= '0' && key <= '9' && input.length() < 4) {
      input += key;
      showPasswordEntry(oldPassOk ? "NEW PASSWORD:" : "CURRENT PASS:", stars());
    } else if (key == '#') {
      if (!oldPassOk) {
        if (input == password) {
          oldPassOk = true; input = "";
          showText("Verified!", "Enter new pass:");
        } else {
          errorBeep(); changingPass = false; oldPassOk = false; input = "";
          showText("Wrong!", ""); delay(1500); showLocked();
        }
      } else if (input.length() == 4) {
        password = input; savePassword();
        successBeep(); changingPass = false; oldPassOk = false; input = "";
        showText("Password", "Changed!"); delay(1500); showLocked();
      }
    } else if (key == '*') { changingPass = false; oldPassOk = false; input = ""; showLocked(); }
    return;
  }

  // Locked out
  if (lockedOut) {
    if (key >= '0' && key <= '9' && input.length() < 4) {
      input += key;
      showPasswordEntry("MASTER CODE:", stars());
    } else if (key == '#') {
      if (input == masterCode) {
        fails = 0; lockedOut = false; input = "";
        successBeep(); showText("Master Reset!",""); delay(1500); unlockSafe();
      } else {
        input = ""; errorBeep();
        showText("WRONG","Try again"); delay(1500);
        showText("!! LOCKED !!","Too many tries","Use master/RFID");
      }
    } else if (key == '*') { input = ""; showText("!! LOCKED !!","Too many tries","Use master/RFID"); }
    return;
  }

  // Normal locked — enter password
  if (key >= '0' && key <= '9') {
    input = String(key);
    showPasswordEntry("ENTER PASSWORD:", "*");
    while (true) {
      char k = keypad.getKey();
      if (!k) { delay(10); continue; }
      beep(50);
      if (k >= '0' && k <= '9' && input.length() < 4) {
        input += k;
        showPasswordEntry("ENTER PASSWORD:", stars());
      }
      else if (k == '#') {
        if (input == password || input == masterCode) { fails = 0; input = ""; unlockSafe(); }
        else {
          fails++; input = ""; errorBeep();
          if (fails >= 3) {
            lockedOut = true;
            showText("!! LOCKED !!","Too many tries","Use master/RFID");
            animAlarm();
          } else {
            showText("WRONG!","Attempts left:", String(3 - fails));
            delay(1500); showLocked();
          }
        }
        break;
      } else if (k == '*') { input = ""; showLocked(); break; }
    }
  }
  else if (key == 'A') { changingPass = true; oldPassOk = false; input = ""; showPasswordEntry("CURRENT PASS:",""); }
  else if (key == 'B') { regRFID = true; showText("Register RFID","Scan card now..."); }
  else if (key == 'D') {
    input = "";
    showPasswordEntry("MASTER CODE:", "");
    while (true) {
      char k = keypad.getKey();
      if (!k) { delay(10); continue; }
      beep(50);
      if (k >= '0' && k <= '9' && input.length() < 4) { input += k; showPasswordEntry("MASTER CODE:", stars()); }
      else if (k == '#') {
        if (input == masterCode) { fails = 0; lockedOut = false; input = ""; successBeep(); showText("Master OK!",""); delay(1500); unlockSafe(); }
        else { input = ""; errorBeep(); showText("WRONG",""); delay(1500); showLocked(); }
        break;
      } else if (k == '*') { input = ""; showLocked(); break; }
    }
  }
}
