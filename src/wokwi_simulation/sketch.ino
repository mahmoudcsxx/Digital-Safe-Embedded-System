/*
 * ============================================================
 *  Digital Safe System (A1401) - ESP32-S3 - WOKWI VERSION
 * ============================================================
 *  BLE replaced with Serial Monitor commands.
 *  OLED: board-grove-oled-sh1107 (SH1107 128x128)
 *  All other logic identical to real hardware version.
 *
 *  Serial cmds: PASS:1234  LOCK  STATUS  CHPASS:5678
 *  Default password: 1234  |  Master code: 0000
 * ============================================================
 */

#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <ESP32Servo.h>
#include <Keypad.h>

#define RFID_SS   10
#define RFID_RST  14
#define OLED_SDA  8
#define OLED_SCL  9
#define SERVO_PIN 18
#define RED_LED   15
#define GREEN_LED 16
#define BUZZER    17

const int PASSWORD_LENGTH = 4;
const int MAX_FAILS = 4;
const unsigned long BASE_LOCKOUT_MS = 30000UL;
const int SERVO_STOP = 90, SERVO_LOCK = 0, SERVO_UNLOCK = 180, SERVO_MOVE_MS = 300;

Adafruit_SH1107 display(128, 128, &Wire, -1);
MFRC522 rfid(RFID_SS, RFID_RST);
Servo lockServo;

const byte ROWS = 4, COLS = 4;
char keys[ROWS][COLS] = {{'1','2','3','A'},{'4','5','6','B'},{'7','8','9','C'},{'*','0','#','D'}};
byte rowPins[ROWS] = {1, 2, 42, 41};
byte colPins[COLS] = {40, 39, 38, 37};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

enum AppState { STATE_SETUP_PASSWORD, STATE_LOCKED, STATE_ENTER_PASSWORD, STATE_CHANGE_OLD, STATE_CHANGE_NEW, STATE_MASTER_CODE, STATE_REGISTER_RFID, STATE_OPEN, STATE_LOCKOUT };
enum InputMode { MODE_KEYPAD, MODE_CHAR };

AppState state = STATE_LOCKED;
InputMode inputMode = MODE_KEYPAD;
String password = "1234", masterCode = "0000", input = "", rfidUID = "";
bool passwordConfigured = false, starWaitingForHash = false;
int fails = 0, lockCycle = 0;
unsigned long lockoutUntil = 0, lastLockoutDraw = 0;

void beep(int ms) { digitalWrite(BUZZER, HIGH); delay(ms); digitalWrite(BUZZER, LOW); }
void successBeep() { beep(100); delay(70); beep(100); }
void errorBeep() { beep(220); delay(80); beep(220); }
void longErrorBeep() { beep(700); }
void inputFeedback() { digitalWrite(GREEN_LED, HIGH); beep(35); delay(30); digitalWrite(GREEN_LED, LOW); if (state != STATE_OPEN) digitalWrite(RED_LED, HIGH); }
String maskInput(const String& v) { String s=""; for(unsigned int i=0;i<v.length();i++) s+="*"; return s; }
void clearInput() { input=""; inputMode=MODE_KEYPAD; starWaitingForHash=false; }
bool isInputKey(char k) { return (k>='0'&&k<='9')||k=='A'||k=='B'||k=='C'||k=='D'; }
char mapCharMode(char k) { switch(k){ case '1':return 'A';case '2':return 'B';case '3':return 'C';case '4':return 'D';case '5':return 'E';case '6':return 'F';case '7':return 'G';case '8':return 'H';case '9':return 'I';case '0':return 'J';case 'A':return 'K';case 'B':return 'L';case 'C':return 'M';case 'D':return 'N';default:return k;} }
unsigned long currentLockDurationMs() { unsigned long d=BASE_LOCKOUT_MS; for(int i=0;i<lockCycle;i++) d*=2UL; return d; }
String readUID() { String u=""; for(byte i=0;i<rfid.uid.size;i++){if(rfid.uid.uidByte[i]<0x10)u+="0";u+=String(rfid.uid.uidByte[i],HEX);} u.toUpperCase(); return u; }

void showText(String l1, String l2="", String l3="") { display.clearDisplay(); display.setTextColor(SH110X_WHITE); display.setTextSize(1); display.setCursor(0,10); display.println(l1); display.setCursor(0,35); display.println(l2); if(l3.length()>0){display.setCursor(0,60);display.println(l3);} display.display(); }

void drawLockIcon(int x, int y, bool locked) {
  display.fillRoundRect(x,y+25,40,35,5,SH110X_WHITE);
  if(locked){display.drawRoundRect(x+8,y,24,28,10,SH110X_WHITE);display.drawRoundRect(x+10,y+2,20,26,8,SH110X_WHITE);display.fillRect(x+9,y+22,22,10,SH110X_BLACK);}
  else{display.drawRoundRect(x+14,y-5,24,28,10,SH110X_WHITE);display.drawRoundRect(x+16,y-3,20,26,8,SH110X_WHITE);display.fillRect(x+15,y+18,22,10,SH110X_BLACK);}
  display.fillCircle(x+20,y+38,4,SH110X_BLACK); display.fillRect(x+18,y+42,5,7,SH110X_BLACK);
}

void showWelcomeScreen() { display.clearDisplay(); display.drawRoundRect(2,2,124,124,6,SH110X_WHITE); drawLockIcon(44,12,true); display.setTextColor(SH110X_WHITE); display.setTextSize(1); display.setCursor(25,78); display.println("WELCOME USER"); display.setCursor(18,94); display.println("DIGITAL SAFE SYS"); display.setCursor(14,108); display.println("Keypad | RFID | BLE"); display.display(); }
void showLockedScreen() { display.clearDisplay(); display.drawRoundRect(2,2,124,124,6,SH110X_WHITE); drawLockIcon(44,10,true); display.setTextSize(1); display.setTextColor(SH110X_WHITE); display.setCursor(26,75); display.println("SAFE LOCKED"); display.setCursor(18,89); display.println("Enter Password"); display.setCursor(10,103); display.println("A:Change   B:RFID"); display.setCursor(10,115); display.println("D:Master"); display.display(); }
void showOpenScreen() { display.clearDisplay(); display.drawRoundRect(2,2,124,124,6,SH110X_WHITE); drawLockIcon(44,10,false); display.setTextSize(2); display.setTextColor(SH110X_WHITE); display.setCursor(24,78); display.println("OPEN"); display.setTextSize(1); display.setCursor(18,105); display.println("Press # to CLOSE"); display.display(); }

void showPasswordEntry(const String& title) {
  display.clearDisplay(); display.drawRoundRect(2,2,124,124,6,SH110X_WHITE);
  display.setTextSize(1); display.setTextColor(SH110X_WHITE); display.setCursor(10,16); display.println(title);
  String masked=maskInput(input);
  for(int i=0;i<PASSWORD_LENGTH;i++){int bx=10+i*28;display.drawRoundRect(bx,42,22,30,4,SH110X_WHITE);if(i<(int)masked.length()){display.setTextSize(2);display.setCursor(bx+6,50);display.print("*");}}
  display.setTextSize(1); display.setCursor(8,86); display.print("Mode: "); display.println(inputMode==MODE_CHAR?"CHAR":"KEYPAD");
  display.setCursor(8,98); display.println("*:Char mode  *#:Keypad"); display.setCursor(8,110); display.println("#:OK    * cancel/next"); display.display();
}

void showLockoutScreen(unsigned long sec) { display.clearDisplay(); display.drawRoundRect(2,2,124,124,6,SH110X_WHITE); display.setTextSize(2); display.setTextColor(SH110X_WHITE); display.setCursor(18,20); display.println("LOCKED"); display.setTextSize(1); display.setCursor(20,55); display.println("Too many wrong tries"); display.setCursor(16,72); display.print("Wait: "); display.print(sec); display.println(" sec"); display.setCursor(8,92); display.print("Next lock: "); display.print(currentLockDurationMs()/1000UL); display.println("s"); display.setCursor(8,108); display.println("Master/RFID blocked too"); display.display(); }

void animSplash() { display.clearDisplay(); display.drawRoundRect(4,4,120,120,8,SH110X_WHITE); display.drawRoundRect(6,6,116,116,6,SH110X_WHITE); display.display(); delay(250); display.setTextSize(2); display.setTextColor(SH110X_WHITE); const char*w1="DIGITAL"; for(int i=0;i<7;i++){display.setCursor(16+i*14,25);display.print(w1[i]);display.display();delay(90);} const char*w2="SAFE"; for(int i=0;i<4;i++){display.setCursor(30+i*14,50);display.print(w2[i]);display.display();delay(90);} display.setTextSize(1); display.setCursor(20,82); display.println("Welcome user"); display.setCursor(18,96); display.println("System starting..."); display.display(); delay(1000); }
void animLoadingBar() { display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE); display.setCursor(12,30); display.println("INITIALIZING..."); display.drawRoundRect(14,55,100,16,4,SH110X_WHITE); display.display(); for(int i=0;i<=96;i+=4){display.fillRoundRect(16,57,i,12,3,SH110X_WHITE);display.fillRect(32,82,60,12,SH110X_BLACK);display.setCursor(42,82);display.print((i*100)/96);display.print("%");display.display();delay(22);} delay(400); }
void animChecking(const String& msg="Checking") { for(int i=0;i<3;i++){display.clearDisplay();display.drawRoundRect(2,2,124,124,6,SH110X_WHITE);drawLockIcon(44,10,true);display.setTextSize(1);display.setTextColor(SH110X_WHITE);display.setCursor(28,80);display.print(msg);for(int j=0;j<=i;j++)display.print(".");display.drawRoundRect(18,102,92,10,3,SH110X_WHITE);display.fillRoundRect(20,104,(i+1)*28,6,2,SH110X_WHITE);display.display();delay(220);} }
void animUnlock() { for(int o=0;o<=12;o+=2){display.clearDisplay();display.drawRoundRect(2,2,124,124,6,SH110X_WHITE);display.fillRoundRect(44,45,40,35,5,SH110X_WHITE);display.drawRoundRect(52+o/2,20-o,24,28,10,SH110X_WHITE);display.drawRoundRect(54+o/2,22-o,20,26,8,SH110X_WHITE);display.fillRect(53+o/2,42-o,22,10,SH110X_BLACK);display.fillCircle(64,58,4,SH110X_BLACK);display.fillRect(62,62,5,7,SH110X_BLACK);display.setTextSize(1);display.setTextColor(SH110X_WHITE);display.setCursor(26,92);display.println("OPENING...");display.display();delay(70);} }
void animLock() { for(int o=12;o>=0;o-=2){display.clearDisplay();display.drawRoundRect(2,2,124,124,6,SH110X_WHITE);display.fillRoundRect(44,45,40,35,5,SH110X_WHITE);display.drawRoundRect(52+o/2,20-o,24,28,10,SH110X_WHITE);display.drawRoundRect(54+o/2,22-o,20,26,8,SH110X_WHITE);display.fillRect(53+o/2,42-o,22,10,SH110X_BLACK);display.fillCircle(64,58,4,SH110X_BLACK);display.fillRect(62,62,5,7,SH110X_BLACK);display.setTextSize(1);display.setTextColor(SH110X_WHITE);display.setCursor(30,92);display.println("LOCKING...");display.display();delay(70);} }

void stopServo(){lockServo.write(SERVO_STOP);}
void moveServoFor(int v,int ms){lockServo.write(v);delay(ms);lockServo.write(SERVO_STOP);}

void lockSafe() { state=STATE_LOCKED; moveServoFor(SERVO_LOCK,SERVO_MOVE_MS); digitalWrite(RED_LED,HIGH); digitalWrite(GREEN_LED,LOW); animLock(); beep(120); showLockedScreen(); Serial.println("[STATUS] LOCKED"); }
void unlockSafe() { state=STATE_OPEN; moveServoFor(SERVO_UNLOCK,SERVO_MOVE_MS); digitalWrite(RED_LED,LOW); digitalWrite(GREEN_LED,HIGH); animUnlock(); successBeep(); showText("Access Granted","Opening...","Press # to close"); delay(700); showOpenScreen(); Serial.println("[STATUS] UNLOCKED"); }

void flashWrongFeedback() { digitalWrite(RED_LED,HIGH); digitalWrite(GREEN_LED,LOW); longErrorBeep(); display.clearDisplay(); display.drawRoundRect(2,2,124,124,6,SH110X_WHITE); display.setTextSize(2); display.setTextColor(SH110X_WHITE); display.setCursor(18,40); display.println("WRONG"); display.setTextSize(1); display.setCursor(18,70); display.println("Password / RFID denied"); display.display(); delay(900); digitalWrite(RED_LED,HIGH); }

void startLockout() { clearInput(); state=STATE_LOCKOUT; fails=0; unsigned long d=currentLockDurationMs(); lockoutUntil=millis()+d; lockCycle++; Serial.println("[STATUS] LOCKED_OUT"); showLockoutScreen(d/1000UL); for(int i=0;i<3;i++){digitalWrite(RED_LED,HIGH);beep(180);delay(120);digitalWrite(RED_LED,LOW);delay(120);} digitalWrite(RED_LED,HIGH); }
void onSuccessfulAccess() { fails=0; lockCycle=0; clearInput(); unlockSafe(); }
void onFailedAccess() { fails++; flashWrongFeedback(); if(fails>=MAX_FAILS){startLockout();}else{display.clearDisplay();display.drawRoundRect(2,2,124,124,6,SH110X_WHITE);display.setTextSize(2);display.setTextColor(SH110X_WHITE);display.setCursor(18,34);display.println("DENIED");display.setTextSize(1);display.setCursor(15,66);display.print("Attempts left: ");display.println(MAX_FAILS-fails);display.display();delay(1000);state=STATE_LOCKED;showLockedScreen();} }
void verifyPasswordCandidate(const String& c) { animChecking(); if(c==password||c==masterCode) onSuccessfulAccess(); else onFailedAccess(); }
void updateLockoutCountdown() { if(state!=STATE_LOCKOUT)return; unsigned long n=millis(); if(n>=lockoutUntil){clearInput();state=STATE_LOCKED;lastLockoutDraw=0;showText("Lockout ended","Try again");delay(700);showLockedScreen();return;} if(n-lastLockoutDraw>=500){unsigned long r=(lockoutUntil-n+999UL)/1000UL;showLockoutScreen(r);lastLockoutDraw=n;} }

void appendInputChar(char k,const String& t) { if(input.length()>=(unsigned)PASSWORD_LENGTH)return; if(starWaitingForHash&&k=='#'){inputMode=MODE_KEYPAD;starWaitingForHash=false;showPasswordEntry(t);return;} starWaitingForHash=false; char v=(inputMode==MODE_CHAR)?mapCharMode(k):k; input+=v; showPasswordEntry(t); }
void handlePasswordSetupConfirm() { if((int)input.length()!=PASSWORD_LENGTH){errorBeep();showText("Password must be","exactly 4 chars");delay(900);showPasswordEntry("SET PASSWORD:");return;} password=input; passwordConfigured=true; successBeep(); showText("Password Saved","System ready"); delay(1000); clearInput(); state=STATE_LOCKED; showLockedScreen(); Serial.println("[INFO] Password set: "+password); }
void handlePasswordChangeConfirm() { if(state==STATE_CHANGE_OLD){animChecking("Verifying");if(input==password){clearInput();state=STATE_CHANGE_NEW;showPasswordEntry("NEW PASSWORD:");}else{errorBeep();clearInput();state=STATE_LOCKED;showText("Wrong current","password");delay(1000);showLockedScreen();}return;} if((int)input.length()!=PASSWORD_LENGTH){errorBeep();showText("New password must","be 4 chars");delay(1000);showPasswordEntry("NEW PASSWORD:");return;} password=input; successBeep(); clearInput(); state=STATE_LOCKED; showText("Password","Changed!"); Serial.println("[INFO] Password changed: "+password); delay(1000); showLockedScreen(); }
void confirmMasterCode() { animChecking("Verifying"); if(input==masterCode){onSuccessfulAccess();}else{errorBeep();clearInput();state=STATE_LOCKED;showText("Wrong master","code");delay(900);showLockedScreen();} }
void processEntryStateKey(char k,const String& t,void(*onConfirm)()) { if(k=='*'){if(input.length()==0){inputMode=MODE_CHAR;starWaitingForHash=true;showPasswordEntry(t);}else{clearInput();if(state==STATE_SETUP_PASSWORD)showPasswordEntry("SET PASSWORD:");else{state=STATE_LOCKED;showLockedScreen();}}return;} if(isInputKey(k)){appendInputChar(k,t);return;} if(k=='#'){onConfirm();} }

void handleSerial() { if(!Serial.available())return; String cmd=Serial.readStringUntil('\n'); cmd.trim(); if(cmd.length()==0)return; Serial.println("[SER] "+cmd); String cu=cmd; cu.toUpperCase(); if(state==STATE_LOCKOUT){Serial.println("[SER] Locked out");return;} if(cu.startsWith("PASS:")){String p=cmd.substring(5);p.trim();animChecking();if(p==password||p==masterCode){Serial.println("[SER] GRANTED");onSuccessfulAccess();}else{Serial.println("[SER] DENIED");onFailedAccess();}}else if(cu=="LOCK"){if(state==STATE_OPEN){lockSafe();Serial.println("[SER] Locked");}else Serial.println("[SER] Already locked");}else if(cu=="STATUS"){if(state==STATE_OPEN)Serial.println("[SER] UNLOCKED");else if(state==STATE_LOCKOUT)Serial.println("[SER] LOCKED_OUT");else Serial.println("[SER] LOCKED");}else if(cu.startsWith("CHPASS:")){String p=cmd.substring(7);p.trim();if((int)p.length()==PASSWORD_LENGTH){password=p;Serial.println("[SER] Pass updated: "+password);showText("Password changed","via Serial");delay(700);if(state!=STATE_OPEN)showLockedScreen();}else Serial.println("[SER] Must be 4 chars");}else Serial.println("[SER] Cmds: PASS:<code> LOCK STATUS CHPASS:<new>"); }

void handleRfidRegistration() { if(state!=STATE_REGISTER_RFID)return; if(!rfid.PICC_IsNewCardPresent()||!rfid.PICC_ReadCardSerial())return; rfidUID=readUID(); successBeep(); showText("RFID Registered",rfidUID); Serial.println("[RFID] Reg: "+rfidUID); delay(1300); rfid.PICC_HaltA(); rfid.PCD_StopCrypto1(); state=STATE_LOCKED; showLockedScreen(); }
void handleRfidScan() { if(state==STATE_OPEN||state==STATE_REGISTER_RFID||state==STATE_LOCKOUT)return; if(!rfid.PICC_IsNewCardPresent()||!rfid.PICC_ReadCardSerial())return; String uid=readUID(); inputFeedback(); animChecking("Reading RFID"); if(rfidUID.length()>0&&uid==rfidUID){showText("RFID Accepted","Opening...");delay(500);onSuccessfulAccess();}else{showText("RFID Denied","Unknown card");delay(600);onFailedAccess();} rfid.PICC_HaltA(); rfid.PCD_StopCrypto1(); }

void handleLockedKey(char k) { if(k=='A'){clearInput();state=STATE_CHANGE_OLD;showPasswordEntry("CURRENT PASS:");return;} if(k=='B'){clearInput();state=STATE_REGISTER_RFID;showText("Register RFID","Scan card now...");return;} if(k=='D'){clearInput();state=STATE_MASTER_CODE;showPasswordEntry("MASTER CODE:");return;} if(k=='*'){clearInput();inputMode=MODE_CHAR;starWaitingForHash=true;state=STATE_ENTER_PASSWORD;showPasswordEntry("ENTER PASSWORD:");return;} if(isInputKey(k)){clearInput();state=STATE_ENTER_PASSWORD;appendInputChar(k,"ENTER PASSWORD:");} }

void handleKeypad() { char k=keypad.getKey(); if(!k)return; inputFeedback(); if(state==STATE_OPEN){if(k=='#')lockSafe();return;} if(state==STATE_LOCKOUT)return; switch(state){ case STATE_SETUP_PASSWORD:processEntryStateKey(k,"SET PASSWORD:",handlePasswordSetupConfirm);break; case STATE_ENTER_PASSWORD:if(k=='#'){String c=input;clearInput();verifyPasswordCandidate(c);}else{processEntryStateKey(k,"ENTER PASSWORD:",[](){});}break; case STATE_CHANGE_OLD:case STATE_CHANGE_NEW:processEntryStateKey(k,state==STATE_CHANGE_OLD?"CURRENT PASS:":"NEW PASSWORD:",handlePasswordChangeConfirm);break; case STATE_MASTER_CODE:processEntryStateKey(k,"MASTER CODE:",confirmMasterCode);break; case STATE_REGISTER_RFID:if(k=='*'){clearInput();state=STATE_LOCKED;showLockedScreen();}break; case STATE_LOCKED:handleLockedKey(k);break; default:break;} }

void setup() {
  Serial.begin(115200);
  Serial.println("=== Digital Safe (A1401) - Wokwi ===");
  Wire.begin(OLED_SDA,OLED_SCL);
  if(!display.begin(0x3C,true)){Serial.println("[ERROR] OLED");while(1){}}
  display.setRotation(0);
  SPI.begin(12,13,11,RFID_SS);
  rfid.PCD_Init();
  lockServo.setPeriodHertz(50);
  lockServo.attach(SERVO_PIN,500,2400);
  stopServo();
  pinMode(RED_LED,OUTPUT); pinMode(GREEN_LED,OUTPUT); pinMode(BUZZER,OUTPUT);
  digitalWrite(RED_LED,HIGH); digitalWrite(GREEN_LED,LOW); digitalWrite(BUZZER,LOW);
  animSplash(); animLoadingBar(); showWelcomeScreen(); successBeep(); delay(1200);
  passwordConfigured=true; state=STATE_LOCKED; showLockedScreen();
  Serial.println("\nReady! Pass:1234 Master:0000");
  Serial.println("Cmds: PASS:1234 LOCK STATUS CHPASS:5678\n");
}

void loop() {
  handleSerial();
  updateLockoutCountdown();
  handleRfidRegistration();
  handleRfidScan();
  handleKeypad();
}
