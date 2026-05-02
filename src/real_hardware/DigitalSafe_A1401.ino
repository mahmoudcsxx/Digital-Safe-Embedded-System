/*
 * ============================================================
 *  Digital Safe System (A1401) - ESP32-S3
 * ============================================================
 *  Master Code  : A3C9  (press D on locked screen — hidden)
 *  Factory Reset: 7D5B  (Master Menu → C → enter code)
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

// ─────────────────────────────────────────────────────────────
// Pins
// ─────────────────────────────────────────────────────────────
#define RFID_SS   10
#define RFID_RST  14
#define OLED_SDA  8
#define OLED_SCL  9
#define SERVO_PIN 18
#define RED_LED   15
#define GREEN_LED 16
#define BUZZER    17

// ─────────────────────────────────────────────────────────────
// BLE UUIDs
// ─────────────────────────────────────────────────────────────
#define SERVICE_UUID  "12345678-1234-1234-1234-123456789abc"
#define TX_UUID       "abcd1234-ab12-cd34-ef56-123456789abc"
#define RX_UUID       "abcd5678-ab12-cd34-ef56-123456789abc"

// ─────────────────────────────────────────────────────────────
// EEPROM
// ─────────────────────────────────────────────────────────────
#define EEPROM_SIZE       128
#define EEPROM_PASS_FLAG  0
#define EEPROM_PASS       1
#define EEPROM_RFID_FLAG  10
#define EEPROM_RFID_LEN   11
#define EEPROM_RFID_DATA  12

// ─────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────
const int PASSWORD_LENGTH        = 4;
const int MAX_FAILS              = 4;
const unsigned long BASE_LOCKOUT_MS  = 30000UL;
const unsigned long RFID_COOLDOWN_MS = 1000UL;

// Continuous rotation servo: 90=stop, 0=lock dir, 180=unlock dir
// SERVO_MOVE_MS halved from 300→150 so it travels ~90° not ~180°
const int SERVO_STOP    = 90;
const int SERVO_LOCK    = 0;    // swap with SERVO_UNLOCK if reversed
const int SERVO_UNLOCK  = 180;  // swap with SERVO_LOCK if reversed
const int SERVO_MOVE_MS = 150;  // 150ms ≈ 90° travel

// ─────────────────────────────────────────────────────────────
// Objects
// ─────────────────────────────────────────────────────────────
Adafruit_SH1107 display(128, 128, &Wire, -1);
MFRC522 rfid(RFID_SS, RFID_RST);
Servo lockServo;

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

BLEServer*         pServer = NULL;
BLECharacteristic* pTxChar = NULL;

// ─────────────────────────────────────────────────────────────
// Enums
// ─────────────────────────────────────────────────────────────
enum AppState {
  STATE_SETUP_PASSWORD,
  STATE_LOCKED,
  STATE_ENTER_PASSWORD,
  STATE_CHANGE_OLD,
  STATE_CHANGE_NEW,
  STATE_MASTER_CODE,
  STATE_MASTER_MENU,
  STATE_REGISTER_RFID,
  STATE_FACTORY_RESET_CODE,   // hidden factory reset entry
  STATE_OPEN,
  STATE_LOCKOUT
};

enum InputMode {
  MODE_KEYPAD,
  MODE_CHAR
};

// ─────────────────────────────────────────────────────────────
// Global State
// ─────────────────────────────────────────────────────────────
AppState  state     = STATE_LOCKED;
InputMode inputMode = MODE_KEYPAD;

String password   = "";
String masterCode = "A3C9";   // hidden — not shown on any screen
String input      = "";
String rfidUID    = "";
String bleCommand = "";

bool passwordConfigured   = false;
bool rfidConfigured       = false;
bool bleConnected         = false;
bool newBleCmd            = false;
bool starWaitingForHash   = false;
bool pendingRfidAfterAuth = false;  // set when B pressed while locked

int  fails     = 0;
int  lockCycle = 0;
unsigned long lockoutUntil     = 0;
unsigned long lastLockoutDraw  = 0;
unsigned long lastRfidActionAt = 0;

// ─────────────────────────────────────────────────────────────
// BLE Callbacks
// ─────────────────────────────────────────────────────────────
class ServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer* s) override    { bleConnected = true; }
  void onDisconnect(BLEServer* s) override { bleConnected = false; s->startAdvertising(); }
};

class RxCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    String value = c->getValue().c_str();
    value.trim();
    if (value.length() > 0) { bleCommand = value; newBleCmd = true; }
  }
};

// ─────────────────────────────────────────────────────────────
// Forward Declarations
// ─────────────────────────────────────────────────────────────
void bleSend(const String& msg);
void beep(int ms);
void successBeep();
void errorBeep();
void longErrorBeep();
void inputFeedback();
void clearInput();
bool isInputKey(char key);
char mapCharMode(char key);
unsigned long lockDurationForCycle(int cycleIndex);
unsigned long currentLockDurationMs();
unsigned long nextLockDurationMs();
void savePassword();
void loadPassword();
void clearRFIDStorage();
void saveRFID();
void loadRFID();
void factoryResetSafe();
String readUID();
void showText(String l1, String l2 = "", String l3 = "");
void drawLockIcon(int x, int y, bool locked);
void showWelcomeScreen();
void showLockedScreen();
void showOpenScreen();
void showPasswordEntry(const String& title);
void showLockoutScreen(unsigned long remainingSec);
void showMasterMenu();
void animSplash();
void animLoadingBar();
void animChecking(const String& msg = "Checking");
void animUnlock();
void animLock();
void stopServo();
void moveServoFor(int value, int durationMs);
void lockSafe();
void unlockSafe();
void flashWrongFeedback();
void startLockout();
void onSuccessfulAccess();
void onFailedAccess();
void verifyPasswordCandidate(const String& candidate);
void updateLockoutCountdown();
void appendInputChar(char key, const String& title);
void startFreshRfidRegistration();
void handlePasswordSetupConfirm();
void handlePasswordChangeConfirm();
void confirmMasterCode();
void confirmFactoryReset();
void processEntryStateKey(char key, const String& title, void (*onConfirm)());
void handleBleCommand();
void handleRfidRegistration();
void handleRfidScan();
void handleLockedKey(char key);
void handleMasterMenuKey(char key);
void handleKeypad();

// ─────────────────────────────────────────────────────────────
// Generic Helpers
// ─────────────────────────────────────────────────────────────
void bleSend(const String& msg) {
  if (bleConnected && pTxChar) { pTxChar->setValue(msg.c_str()); pTxChar->notify(); }
}

void beep(int ms) { digitalWrite(BUZZER, HIGH); delay(ms); digitalWrite(BUZZER, LOW); }
void successBeep()   { beep(100); delay(70); beep(100); }
void errorBeep()     { beep(220); delay(80); beep(220); }
void longErrorBeep() { beep(700); }

void inputFeedback() {
  digitalWrite(GREEN_LED, HIGH); beep(35); delay(30); digitalWrite(GREEN_LED, LOW);
  if (state != STATE_OPEN) digitalWrite(RED_LED, HIGH);
}

void clearInput() { input = ""; inputMode = MODE_KEYPAD; starWaitingForHash = false; }

bool isInputKey(char key) {
  return (key >= '0' && key <= '9') || key == 'A' || key == 'B' || key == 'C' || key == 'D';
}

// ─────────────────────────────────────────────────────────────
// CHAR Mode Map
// 1→A  2→B  3→C  4→D  5→E  6→F  7→G  8→H  9→I  0→J
// A→K  B→L  C→M  D→N
// ─────────────────────────────────────────────────────────────
char mapCharMode(char key) {
  switch (key) {
    case '1': return 'A'; case '2': return 'B'; case '3': return 'C';
    case '4': return 'D'; case '5': return 'E'; case '6': return 'F';
    case '7': return 'G'; case '8': return 'H'; case '9': return 'I';
    case '0': return 'J'; case 'A': return 'K'; case 'B': return 'L';
    case 'C': return 'M'; case 'D': return 'N'; default:  return key;
  }
}

unsigned long lockDurationForCycle(int cycleIndex) {
  switch (cycleIndex % 4) {
    case 0: return  30000UL;
    case 1: return  60000UL;
    case 2: return 120000UL;
    case 3: return 240000UL;
  }
  return 30000UL;
}
unsigned long currentLockDurationMs() { return lockDurationForCycle(lockCycle); }
unsigned long nextLockDurationMs()    { return lockDurationForCycle((lockCycle + 1) % 4); }

// ─────────────────────────────────────────────────────────────
// EEPROM
// ─────────────────────────────────────────────────────────────
void savePassword() {
  EEPROM.write(EEPROM_PASS_FLAG, 0xAA);
  for (int i = 0; i < PASSWORD_LENGTH; i++) {
    char ch = (i < (int)password.length()) ? password[i] : '0';
    EEPROM.write(EEPROM_PASS + i, ch);
  }
  EEPROM.commit(); passwordConfigured = true;
}

void loadPassword() {
  if (EEPROM.read(EEPROM_PASS_FLAG) == 0xAA) {
    password = ""; bool valid = true;
    for (int i = 0; i < PASSWORD_LENGTH; i++) {
      char ch = (char)EEPROM.read(EEPROM_PASS + i);
      if (ch == 0 || ch == 255) valid = false;
      password += ch;
    }
    passwordConfigured = valid && password.length() == PASSWORD_LENGTH;
    if (!passwordConfigured) password = "";
  } else { password = ""; passwordConfigured = false; }
}

void clearRFIDStorage() {
  EEPROM.write(EEPROM_RFID_FLAG, 0x00); EEPROM.write(EEPROM_RFID_LEN, 0);
  for (int i = 0; i < 16; i++) EEPROM.write(EEPROM_RFID_DATA + i, 0);
  EEPROM.commit(); rfidUID = ""; rfidConfigured = false;
}

void saveRFID() {
  byte len = (byte)rfidUID.length();
  EEPROM.write(EEPROM_RFID_FLAG, 0x55); EEPROM.write(EEPROM_RFID_LEN, len);
  for (int i = 0; i < 16; i++)
    EEPROM.write(EEPROM_RFID_DATA + i, (i < len) ? rfidUID[i] : 0);
  EEPROM.commit(); rfidConfigured = (len > 0);
}

void loadRFID() {
  if (EEPROM.read(EEPROM_RFID_FLAG) == 0x55) {
    byte len = EEPROM.read(EEPROM_RFID_LEN);
    if (len > 0 && len < 17) {
      rfidUID = "";
      for (int i = 0; i < len; i++) rfidUID += (char)EEPROM.read(EEPROM_RFID_DATA + i);
      rfidConfigured = true; return;
    }
  }
  rfidUID = ""; rfidConfigured = false;
}

void factoryResetSafe() {
  for (int i = 0; i < EEPROM_SIZE; i++) EEPROM.write(i, 0);
  EEPROM.commit();

  password = ""; rfidUID = "";
  passwordConfigured = false; rfidConfigured = false;
  fails = 0; lockCycle = 0; lockoutUntil = 0;
  lastLockoutDraw = 0; lastRfidActionAt = 0;
  pendingRfidAfterAuth = false; clearInput();

  showText("Factory Reset", "EEPROM cleared", "Safe is brand new");
  successBeep(); delay(1400);
  state = STATE_SETUP_PASSWORD; showPasswordEntry("SET NEW PASS");
}

// ─────────────────────────────────────────────────────────────
// RFID Reader
// ─────────────────────────────────────────────────────────────
String readUID() {
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase(); return uid;
}

// ─────────────────────────────────────────────────────────────
// Display / UI
// ─────────────────────────────────────────────────────────────
void showText(String l1, String l2, String l3) {
  display.clearDisplay();
  display.drawRoundRect(2, 2, 124, 124, 6, SH110X_WHITE);
  display.setTextColor(SH110X_WHITE); display.setTextSize(1);
  display.setCursor(0, 10); display.println(l1);
  display.setCursor(0, 35); display.println(l2);
  if (l3.length() > 0) { display.setCursor(0, 60); display.println(l3); }
  display.display();
}

void drawLockIcon(int x, int y, bool locked) {
  display.fillRoundRect(x, y+25, 40, 35, 5, SH110X_WHITE);
  if (locked) {
    display.drawRoundRect(x+8,  y,   24, 28, 10, SH110X_WHITE);
    display.drawRoundRect(x+10, y+2, 20, 26,  8, SH110X_WHITE);
    display.fillRect(x+9, y+22, 22, 10, SH110X_BLACK);
  } else {
    display.drawRoundRect(x+14, y-5, 24, 28, 10, SH110X_WHITE);
    display.drawRoundRect(x+16, y-3, 20, 26,  8, SH110X_WHITE);
    display.fillRect(x+15, y+18, 22, 10, SH110X_BLACK);
  }
  display.fillCircle(x+20, y+38, 4, SH110X_BLACK);
  display.fillRect(x+18, y+42, 5, 7, SH110X_BLACK);
}

void showWelcomeScreen() {
  display.clearDisplay();
  display.drawRoundRect(2, 2, 124, 124, 6, SH110X_WHITE);
  drawLockIcon(44, 12, true);
  display.setTextColor(SH110X_WHITE); display.setTextSize(1);
  display.setCursor(25, 78);  display.println("WELCOME USER");
  display.setCursor(18, 94);  display.println("DIGITAL SAFE SYS");
  display.setCursor(14, 108); display.println("Keypad | RFID | BLE");
  display.display();
}

void showLockedScreen() {
  display.clearDisplay();
  display.drawRoundRect(2, 2, 124, 124, 6, SH110X_WHITE);
  drawLockIcon(44, 8, true);
  display.setTextSize(1); display.setTextColor(SH110X_WHITE);
  display.setCursor(28, 72);  display.println("SAFE LOCKED");
  display.setCursor(18, 86);  display.println("Enter password");
  display.setCursor(10, 100); display.println("A:Change Pass");
  display.setCursor(10, 114); display.println("B:New RFID (auth)");
  // D (master) intentionally hidden
  display.display();
}

void showOpenScreen() {
  display.clearDisplay();
  display.drawRoundRect(2, 2, 124, 124, 6, SH110X_WHITE);
  drawLockIcon(44, 8, false);
  display.setTextSize(2); display.setTextColor(SH110X_WHITE);
  display.setCursor(24, 74); display.println("OPEN");
  display.setTextSize(1);
  display.setCursor(18, 100); display.println("# or RFID closes");
  display.setCursor(30, 112); display.println("safe now");
  display.display();
}

void showPasswordEntry(const String& title) {
  display.clearDisplay();
  display.drawRoundRect(2, 2, 124, 124, 6, SH110X_WHITE);
  display.setTextColor(SH110X_WHITE); display.setTextSize(1);
  display.setCursor(12, 12); display.println(title);

  int startX = 12, boxY = 36, boxW = 22, boxH = 28, gap = 8;
  for (int i = 0; i < PASSWORD_LENGTH; i++) {
    int x = startX + i * (boxW + gap);
    display.drawRoundRect(x, boxY, boxW, boxH, 4, SH110X_WHITE);
    if (i < (int)input.length()) {
      display.setTextSize(2); display.setCursor(x+6, boxY+7); display.print("*");
    }
  }
  display.setTextSize(1);
  display.setCursor(12, 78);  display.print("Mode: "); display.println(inputMode == MODE_CHAR ? "CHAR" : "KEYPAD");
  display.setCursor(12, 94);  display.println("A:abc  B:123");
  display.setCursor(12, 108); display.println("#:OK  *:Clear/Back");
  display.display();
}

void showLockoutScreen(unsigned long remainingSec) {
  display.clearDisplay();
  display.drawRoundRect(2, 2, 124, 124, 6, SH110X_WHITE);
  display.setTextSize(2); display.setTextColor(SH110X_WHITE);
  display.setCursor(18, 20); display.println("LOCKED");
  display.setTextSize(1);
  display.setCursor(20, 55); display.println("Too many wrong tries");
  display.setCursor(16, 72); display.print("Wait: "); display.print(remainingSec); display.println(" sec");
  display.setCursor(8,  92); display.print("Next lock: "); display.print(nextLockDurationMs()/1000UL); display.println("s");
  display.setCursor(8, 108); display.println("All input blocked");
  display.display();
}

void showMasterMenu() {
  display.clearDisplay();
  display.drawRoundRect(2, 2, 124, 124, 6, SH110X_WHITE);
  display.setTextColor(SH110X_WHITE); display.setTextSize(1);
  display.setCursor(16, 16); display.println("MASTER MENU");
  display.setCursor(10, 42); display.println("A: Reset Password");
  display.setCursor(10, 58); display.println("B: Reset RFID");
  display.setCursor(10, 74); display.println("#: Open Safe");
  display.setCursor(10, 90); display.println("*: Cancel");
  // C (factory reset) intentionally hidden
  display.display();
}

// ─────────────────────────────────────────────────────────────
// Animations
// ─────────────────────────────────────────────────────────────
void animSplash() {
  display.clearDisplay();
  display.drawRoundRect(4, 4, 120, 120, 8, SH110X_WHITE);
  display.drawRoundRect(6, 6, 116, 116, 6, SH110X_WHITE);
  display.display(); delay(250);

  display.setTextSize(2); display.setTextColor(SH110X_WHITE);
  const char* w1 = "DIGITAL";
  for (int i = 0; i < 7; i++) { display.setCursor(16+i*14, 25); display.print(w1[i]); display.display(); delay(90); }
  const char* w2 = "SAFE";
  for (int i = 0; i < 4; i++) { display.setCursor(30+i*14, 50); display.print(w2[i]); display.display(); delay(90); }

  display.setTextSize(1);
  display.setCursor(20, 82); display.println("Welcome user");
  display.setCursor(18, 96); display.println("System starting...");
  display.display(); delay(1000);
}

void animLoadingBar() {
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
  display.setCursor(12, 30); display.println("INITIALIZING...");
  display.drawRoundRect(14, 55, 100, 16, 4, SH110X_WHITE); display.display();
  for (int i = 0; i <= 96; i += 4) {
    display.fillRoundRect(16, 57, i, 12, 3, SH110X_WHITE);
    display.fillRect(32, 82, 60, 12, SH110X_BLACK);
    display.setCursor(42, 82); display.print((i*100)/96); display.print("%");
    display.display(); delay(22);
  }
  delay(400);
}

void animChecking(const String& msg) {
  for (int i = 0; i < 3; i++) {
    display.clearDisplay();
    display.drawRoundRect(2, 2, 124, 124, 6, SH110X_WHITE);
    drawLockIcon(44, 10, true);
    display.setTextSize(1); display.setTextColor(SH110X_WHITE);
    display.setCursor(28, 80); display.print(msg);
    for (int j = 0; j <= i; j++) display.print(".");
    display.drawRoundRect(18, 102, 92, 10, 3, SH110X_WHITE);
    display.fillRoundRect(20, 104, (i+1)*28, 6, 2, SH110X_WHITE);
    display.display(); delay(220);
  }
}

void animUnlock() {
  for (int offset = 0; offset <= 12; offset += 2) {
    display.clearDisplay();
    display.drawRoundRect(2, 2, 124, 124, 6, SH110X_WHITE);
    display.fillRoundRect(44, 45, 40, 35, 5, SH110X_WHITE);
    display.drawRoundRect(52+offset/2, 20-offset, 24, 28, 10, SH110X_WHITE);
    display.drawRoundRect(54+offset/2, 22-offset, 20, 26,  8, SH110X_WHITE);
    display.fillRect(53+offset/2, 42-offset, 22, 10, SH110X_BLACK);
    display.fillCircle(64, 58, 4, SH110X_BLACK);
    display.fillRect(62, 62, 5, 7, SH110X_BLACK);
    display.setTextSize(1); display.setTextColor(SH110X_WHITE);
    display.setCursor(26, 92); display.println("OPENING...");
    display.display(); delay(70);
  }
}

void animLock() {
  for (int offset = 12; offset >= 0; offset -= 2) {
    display.clearDisplay();
    display.drawRoundRect(2, 2, 124, 124, 6, SH110X_WHITE);
    display.fillRoundRect(44, 45, 40, 35, 5, SH110X_WHITE);
    display.drawRoundRect(52+offset/2, 20-offset, 24, 28, 10, SH110X_WHITE);
    display.drawRoundRect(54+offset/2, 22-offset, 20, 26,  8, SH110X_WHITE);
    display.fillRect(53+offset/2, 42-offset, 22, 10, SH110X_BLACK);
    display.fillCircle(64, 58, 4, SH110X_BLACK);
    display.fillRect(62, 62, 5, 7, SH110X_BLACK);
    display.setTextSize(1); display.setTextColor(SH110X_WHITE);
    display.setCursor(30, 92); display.println("LOCKING...");
    display.display(); delay(70);
  }
}

// ─────────────────────────────────────────────────────────────
// Servo  (continuous rotation — unchanged from original)
// ─────────────────────────────────────────────────────────────
void stopServo() {
  lockServo.write(SERVO_STOP);
}

void moveServoFor(int value, int durationMs) {
  lockServo.write(value);
  delay(durationMs);
  lockServo.write(SERVO_STOP);
}

void lockSafe() {
  state = STATE_LOCKED;
  moveServoFor(SERVO_LOCK, SERVO_MOVE_MS);
  digitalWrite(RED_LED,   HIGH);
  digitalWrite(GREEN_LED, LOW);
  animLock();
  beep(120);
  showLockedScreen();
  bleSend("STATUS:LOCKED");
}

void unlockSafe() {
  state = STATE_OPEN;
  moveServoFor(SERVO_UNLOCK, SERVO_MOVE_MS);
  digitalWrite(RED_LED,   LOW);
  digitalWrite(GREEN_LED, HIGH);
  animUnlock();
  successBeep();
  showText("Access Granted", "Opening...", "Press # or RFID");
  delay(700);
  showOpenScreen();
  bleSend("STATUS:UNLOCKED");
}

// ─────────────────────────────────────────────────────────────
// Security
// ─────────────────────────────────────────────────────────────
void flashWrongFeedback() {
  digitalWrite(RED_LED, HIGH); digitalWrite(GREEN_LED, LOW);
  longErrorBeep();
  display.clearDisplay();
  display.drawRoundRect(2, 2, 124, 124, 6, SH110X_WHITE);
  display.setTextSize(2); display.setTextColor(SH110X_WHITE);
  display.setCursor(18, 40); display.println("WRONG");
  display.setTextSize(1); display.setCursor(18, 70); display.println("Access denied");
  display.display(); delay(900);
  digitalWrite(RED_LED, HIGH);
}

void startLockout() {
  clearInput(); pendingRfidAfterAuth = false;
  state = STATE_LOCKOUT; fails = 0;
  unsigned long duration = currentLockDurationMs();
  lockoutUntil = millis() + duration;
  lockCycle    = (lockCycle + 1) % 4;
  bleSend("STATUS:LOCKED_OUT");
  showLockoutScreen(duration / 1000UL);
  for (int i = 0; i < 3; i++) {
    digitalWrite(RED_LED, HIGH); beep(180); delay(120);
    digitalWrite(RED_LED, LOW);  delay(120);
  }
  digitalWrite(RED_LED, HIGH);
}

void onSuccessfulAccess() {
  fails = 0; clearInput();

  // If B was pressed (register RFID), go there instead of unlocking
  if (pendingRfidAfterAuth) {
    pendingRfidAfterAuth = false;
    state = STATE_REGISTER_RFID;
    showText("Verified!", "Scan new RFID now");
    return;
  }

  unlockSafe();
}

void onFailedAccess() {
  fails++; flashWrongFeedback();
  if (fails >= MAX_FAILS) {
    startLockout();
  } else {
    display.clearDisplay();
    display.drawRoundRect(2, 2, 124, 124, 6, SH110X_WHITE);
    display.setTextSize(2); display.setTextColor(SH110X_WHITE);
    display.setCursor(18, 34); display.println("DENIED");
    display.setTextSize(1); display.setCursor(15, 66);
    display.print("Attempts left: "); display.println(MAX_FAILS - fails);
    display.display(); delay(1000);
    if (pendingRfidAfterAuth) pendingRfidAfterAuth = false;
    state = STATE_LOCKED; showLockedScreen();
  }
}

void verifyPasswordCandidate(const String& candidate) {
  animChecking();
  if (candidate == password || candidate == masterCode) onSuccessfulAccess();
  else                                                  onFailedAccess();
}

void updateLockoutCountdown() {
  if (state != STATE_LOCKOUT) return;
  unsigned long now = millis();
  if (now >= lockoutUntil) {
    clearInput(); state = STATE_LOCKED; lastLockoutDraw = 0;
    showText("Lockout ended", "Try again"); delay(700); showLockedScreen(); return;
  }
  if (now - lastLockoutDraw >= 500) {
    showLockoutScreen((lockoutUntil - now + 999UL) / 1000UL);
    lastLockoutDraw = now;
  }
}

// ─────────────────────────────────────────────────────────────
// Password / Input Logic
// ─────────────────────────────────────────────────────────────
void appendInputChar(char key, const String& title) {
  if (input.length() >= PASSWORD_LENGTH) return;
  starWaitingForHash = false;
  input += (inputMode == MODE_CHAR) ? mapCharMode(key) : key;
  showPasswordEntry(title);
}

void startFreshRfidRegistration() {
  clearInput(); state = STATE_REGISTER_RFID; showText("Password Saved", "Scan RFID now");
}

void handlePasswordSetupConfirm() {
  if (input.length() != PASSWORD_LENGTH) {
    errorBeep(); showText("Password must be", "exactly 4 chars"); delay(900); showPasswordEntry("SET NEW PASS"); return;
  }
  password = input; savePassword(); successBeep(); clearInput();
  if (!rfidConfigured) startFreshRfidRegistration();
  else { showText("Password Saved", "System ready"); delay(1000); state = STATE_LOCKED; showLockedScreen(); }
}

void handlePasswordChangeConfirm() {
  if (state == STATE_CHANGE_OLD) {
    animChecking("Verifying");
    if (input == password) { clearInput(); state = STATE_CHANGE_NEW; showPasswordEntry("NEW PASSWORD:"); }
    else { errorBeep(); clearInput(); state = STATE_LOCKED; showText("Wrong current", "password"); delay(1000); showLockedScreen(); }
    return;
  }
  if (input.length() != PASSWORD_LENGTH) {
    errorBeep(); showText("New password must", "be 4 chars"); delay(1000); showPasswordEntry("NEW PASSWORD:"); return;
  }
  password = input; savePassword(); successBeep(); clearInput();
  state = STATE_LOCKED; showText("Password", "Changed!"); delay(1000); showLockedScreen();
}

void confirmMasterCode() {
  animChecking("Verifying");
  if (input == masterCode) { clearInput(); state = STATE_MASTER_MENU; showMasterMenu(); }
  else { errorBeep(); clearInput(); state = STATE_LOCKED; showText("Wrong master", "code"); delay(900); showLockedScreen(); }
}

void confirmFactoryReset() {
  animChecking("Verifying");
  if (input == "7D5B") { clearInput(); factoryResetSafe(); }
  else { errorBeep(); clearInput(); state = STATE_MASTER_MENU; showText("Wrong reset code", "Returning..."); delay(900); showMasterMenu(); }
}

void processEntryStateKey(char key, const String& title, void (*onConfirm)()) {
  if (key == 'A') { inputMode = MODE_CHAR;   starWaitingForHash = false; showPasswordEntry(title); return; }
  if (key == 'B') { inputMode = MODE_KEYPAD; starWaitingForHash = false; showPasswordEntry(title); return; }
  if (key == '*') {
    if (input.length() > 0) { clearInput(); showPasswordEntry(title); }
    else {
      clearInput(); pendingRfidAfterAuth = false;
      if (state == STATE_SETUP_PASSWORD) showPasswordEntry(title);
      else { state = STATE_LOCKED; showLockedScreen(); }
    }
    return;
  }
  if (key == '#')      { onConfirm(); return; }
  if (isInputKey(key)) { appendInputChar(key, title); return; }
}

// ─────────────────────────────────────────────────────────────
// BLE
// ─────────────────────────────────────────────────────────────
void handleBleCommand() {
  if (!newBleCmd) return;
  newBleCmd = false;
  String cmd = bleCommand; String cmdUpper = cmd; cmdUpper.toUpperCase();

  if (state == STATE_LOCKOUT) { bleSend("RESULT:System locked out"); return; }

  if (cmdUpper.startsWith("PASS:")) {
    String p = cmd.substring(5); p.trim(); animChecking();
    if (p == password || p == masterCode) { bleSend("RESULT:Access granted"); onSuccessfulAccess(); }
    else                                  { bleSend("RESULT:Wrong password");  onFailedAccess(); }
  } else if (cmdUpper == "LOCK") {
    if (state == STATE_OPEN) { lockSafe(); bleSend("RESULT:Locked"); } else bleSend("RESULT:Already locked");
  } else if (cmdUpper == "STATUS") {
    if      (state == STATE_OPEN)    bleSend("STATUS:UNLOCKED");
    else if (state == STATE_LOCKOUT) bleSend("STATUS:LOCKED_OUT");
    else                             bleSend("STATUS:LOCKED");
  } else if (cmdUpper.startsWith("CHPASS:")) {
    String p = cmd.substring(7); p.trim();
    if (state != STATE_OPEN) { bleSend("RESULT:Unlock safe first"); return; }
    if (p.length() == PASSWORD_LENGTH) {
      password = p; savePassword(); bleSend("RESULT:Password updated");
      showText("Password changed", "via Bluetooth"); delay(700); showOpenScreen();
    } else bleSend("RESULT:Password must be 4 chars");
  } else if (cmdUpper == "RESETRFID") {
    if (state != STATE_OPEN) { bleSend("RESULT:Unlock safe first"); return; }
    clearRFIDStorage(); state = STATE_REGISTER_RFID;
    bleSend("RESULT:Scan new RFID"); showText("RFID cleared", "Scan new card");
  }
}

// ─────────────────────────────────────────────────────────────
// RFID
// ─────────────────────────────────────────────────────────────
void handleRfidRegistration() {
  if (state != STATE_REGISTER_RFID) return;
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;
  rfidUID = readUID(); saveRFID(); successBeep();
  showText("RFID Registered", rfidUID); delay(1300);
  rfid.PICC_HaltA(); rfid.PCD_StopCrypto1();
  state = STATE_LOCKED; showLockedScreen();
}

void handleRfidScan() {
  if (state == STATE_REGISTER_RFID || state == STATE_LOCKOUT) return;
  if (!rfidConfigured) return;
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

  String uid = readUID(); rfid.PICC_HaltA(); rfid.PCD_StopCrypto1();
  unsigned long now = millis();
  if (now - lastRfidActionAt < RFID_COOLDOWN_MS) return;

  inputFeedback(); animChecking("Reading RFID");

  if (uid == rfidUID) {
    lastRfidActionAt = now; clearInput();
    if (state == STATE_OPEN) { showText("RFID Accepted", "Closing..."); delay(500); lockSafe(); }
    else                     { showText("RFID Accepted", "Opening..."); delay(500); onSuccessfulAccess(); }
    return;
  }
  showText("RFID Denied", "Unknown card"); delay(600); onFailedAccess();
}

// ─────────────────────────────────────────────────────────────
// Keypad
// ─────────────────────────────────────────────────────────────
void handleLockedKey(char key) {
  // A → change password
  if (key == 'A') { clearInput(); state = STATE_CHANGE_OLD; showPasswordEntry("CURRENT PASS:"); return; }

  // B → register new RFID, requires password verification first
  if (key == 'B') {
    pendingRfidAfterAuth = true; clearInput();
    state = STATE_ENTER_PASSWORD; showPasswordEntry("VERIFY PASSWORD:"); return;
  }

  // D → master code (hidden from screen)
  if (key == 'D') { clearInput(); state = STATE_MASTER_CODE; showPasswordEntry("MASTER CODE:"); return; }

  if (key == '*') {
    clearInput(); inputMode = MODE_KEYPAD; starWaitingForHash = false;
    state = STATE_ENTER_PASSWORD; showPasswordEntry("ENTER PASSWORD:"); return;
  }

  if (isInputKey(key)) { clearInput(); state = STATE_ENTER_PASSWORD; appendInputChar(key, "ENTER PASSWORD:"); }
}

void handleMasterMenuKey(char key) {
  if (key == '*') { clearInput(); state = STATE_LOCKED; showLockedScreen(); return; }
  if (key == '#') { onSuccessfulAccess(); return; }
  if (key == 'A') {
    clearInput(); password = ""; passwordConfigured = false;
    EEPROM.write(EEPROM_PASS_FLAG, 0x00); EEPROM.commit();
    state = STATE_SETUP_PASSWORD; showPasswordEntry("SET NEW PASS"); return;
  }
  if (key == 'B') {
    clearRFIDStorage(); clearInput();
    state = STATE_REGISTER_RFID; showText("Register new RFID", "Scan card now..."); return;
  }
  // C → factory reset (hidden, requires code 7D5B)
  if (key == 'C') { clearInput(); state = STATE_FACTORY_RESET_CODE; showPasswordEntry("RESET CODE:"); return; }
}

void handleKeypad() {
  char key = keypad.getKey();
  if (!key) return;
  inputFeedback();

  if (state == STATE_OPEN)    { if (key == '#') lockSafe(); return; }
  if (state == STATE_LOCKOUT) { return; }

  switch (state) {
    case STATE_SETUP_PASSWORD:
      processEntryStateKey(key, "SET NEW PASS", handlePasswordSetupConfirm); break;

    case STATE_ENTER_PASSWORD:
      if (key == '#') { String c = input; clearInput(); verifyPasswordCandidate(c); }
      else processEntryStateKey(key, "ENTER PASSWORD:", [](){}); break;

    case STATE_CHANGE_OLD:
    case STATE_CHANGE_NEW:
      processEntryStateKey(key, state == STATE_CHANGE_OLD ? "CURRENT PASS:" : "NEW PASSWORD:", handlePasswordChangeConfirm); break;

    case STATE_MASTER_CODE:
      processEntryStateKey(key, "MASTER CODE:", confirmMasterCode); break;

    case STATE_MASTER_MENU:
      handleMasterMenuKey(key); break;

    case STATE_FACTORY_RESET_CODE:
      processEntryStateKey(key, "RESET CODE:", confirmFactoryReset); break;

    case STATE_REGISTER_RFID:
      if (key == '*') {
        clearInput();
        if (!passwordConfigured) { state = STATE_SETUP_PASSWORD; showPasswordEntry("SET NEW PASS"); }
        else                     { state = STATE_LOCKED; showLockedScreen(); }
      }
      break;

    case STATE_LOCKED:
      handleLockedKey(key); break;

    default: break;
  }
}

// ─────────────────────────────────────────────────────────────
// Setup / Loop
// ─────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  EEPROM.begin(EEPROM_SIZE);
  loadPassword();
  loadRFID();

  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(0x3C, true)) {
    Serial.println("[ERROR] OLED init failed");
    while (1) {}
  }
  display.setRotation(0);

  SPI.begin(12, 13, 11, RFID_SS);
  rfid.PCD_Init();

  // Servo — continuous rotation, stays attached, stop on boot
  lockServo.setPeriodHertz(50);
  lockServo.attach(SERVO_PIN, 500, 2400);
  stopServo();

  pinMode(RED_LED,   OUTPUT);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(BUZZER,    OUTPUT);
  digitalWrite(RED_LED,   HIGH);
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(BUZZER,    LOW);

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

  animSplash();
  animLoadingBar();

  showWelcomeScreen();
  successBeep();
  delay(1200);

  if (!passwordConfigured) {
    state = STATE_SETUP_PASSWORD; showPasswordEntry("SET NEW PASS");
  } else if (!rfidConfigured) {
    state = STATE_REGISTER_RFID; showText("No RFID found", "Scan card now");
  } else {
    state = STATE_LOCKED; showLockedScreen();
  }
}

void loop() {
  handleBleCommand();
  updateLockoutCountdown();
  handleRfidRegistration();
  handleRfidScan();
  handleKeypad();
}
