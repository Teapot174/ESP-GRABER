#include <Arduino.h>
#include <EEPROM.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include <GyverButton.h>
#include <RCSwitch.h>
#include "config.h"
#include "interface.h"

// OLED
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Кнопки
GButton btn_up(BTN_UP, HIGH_PULL, NORM_OPEN);
GButton btn_down(BTN_DOWN, HIGH_PULL, NORM_OPEN);
GButton btn_ok(BTN_OK, HIGH_PULL, NORM_OPEN);
GButton btn_back(BTN_BACK, HIGH_PULL, NORM_OPEN);

// CC1101
#define DEFAULT_RF_FREQUENCY 433.92 // MHz
float frequency = DEFAULT_RF_FREQUENCY;
const float frequencies[] = {315.0, 433.92, 868.0, 915.0};
const int numFrequencies = 4;
int freqIndex = 1; // Дефолт 433.92 MHz

// Список частот для аналайзера
const float subghz_frequency_list[] = {315.0, 433.92, 868.0, 915.0};
const int subghz_frequency_count = sizeof(subghz_frequency_list) / sizeof(subghz_frequency_list[0]);
int current_scan_index = 0;
const int rssi_threshold = -85; // Порог RSSI

// RCSwitch
RCSwitch rcswitch = RCSwitch();

#define MAX_DATA_LOG 512
#define MAX_KEY_COUNT 20 // Ключи
#define RSSI_THRESHOLD -85
#define MAX_TRIES 5
#define EEPROM_SIZE 2048 // Место под ключи

volatile bool recieved = false;
volatile int keyRawLog[MAX_DATA_LOG];
volatile byte logLen;
volatile bool sleepOn = false;

enum emKeys { kUnknown, kP12bt, k12bt, k24bt, k64bt, kKeeLoq, kANmotors64, kPrinceton, kRcSwitch, kStarLine, kCAME, kNICE, kHOLTEK };
enum emMenuState { menuLogo, menuMain, menuReceive, menuTransmit, menuAnalyzer, menuJammer } menuState = menuLogo;

struct tpKeyData {
  byte keyID[9];
  int zero[2];
  int one[2];
  int prePulse[2];
  int startPause[2];
  int midlePause[2];
  byte prePulseLenth;
  byte codeLenth;
  byte firstDataIdx;
  emKeys type;
  float frequency;
  int te;
  char rawData[16];
  int bitLength;
  char preset[8];
};

byte maxKeyCount = MAX_KEY_COUNT;
byte EEPROM_key_count;
byte EEPROM_key_index = 0;
unsigned long stTimer = 0;
unsigned long scanTimer = 0;
byte menuIndex = 0;
bool awaitingDeleteConfirmation = false;
bool validKeyReceived = false;
bool readRAW = true;
bool autoSave = false;
int signals = 0;
uint64_t lastSavedKey = 0;
tpKeyData keyData1;
float detected_frequency = 0.0;
float last_detected_frequency = 0.0;
bool display_updated = false;
bool isJamming = false;

String getTypeName(emKeys tp);
void OLED_printKey(tpKeyData* kd, byte msgType = 0, bool isSending = false);
void OLED_printError(String st, bool err = true);
byte indxKeyInROM(tpKeyData* kd);
bool EEPROM_AddKey(tpKeyData* kd);
void EEPROM_get_key(byte EEPROM_key_index1, tpKeyData* kd);
void sendSynthKey(tpKeyData* kd);
void deleteCurrentKey();
void OLED_printWaitingSignal();
void myDelayMcs(unsigned long dl);
void sendSynthBit(int bt[2]);
void setupCC1101();
void read_rcswitch(tpKeyData* kd);
void read_raw(tpKeyData* kd);
void restoreReceiveMode();
void OLED_printAnalyzer(bool signalReceived = false, float detectedFreq = 0.0);
void OLED_printJammer();
void startJamming();
void stopJamming();

void setup() {
  Serial.begin(115200);

  // Снова кнопки
  btn_up.setDebounce(50);
  btn_up.setTimeout(500);
  btn_up.setClickTimeout(300);
  btn_up.setStepTimeout(200);
  btn_down.setDebounce(50);
  btn_down.setTimeout(500);
  btn_down.setClickTimeout(300);
  btn_down.setStepTimeout(200);
  btn_ok.setDebounce(50);
  btn_ok.setTimeout(500);
  btn_ok.setClickTimeout(300);
  btn_ok.setStepTimeout(200);
  btn_back.setDebounce(50);
  btn_back.setTimeout(500);
  btn_back.setClickTimeout(300);
  btn_back.setStepTimeout(200);
  btn_up.setTickMode(AUTO);
  btn_down.setTickMode(AUTO);

  // Инициализация OLED
  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 allocation failed"));
    for (;;);
  }
  OLED_printLogo(display);

  // Инициализация CC1101
  setupCC1101();

  // Инициализация RCSwitch
  rcswitch.enableReceive(CC1101_GDO0);

  // Инициализация EEPROM
  EEPROM.begin(EEPROM_SIZE);
  byte read_count = EEPROM.read(0);
  EEPROM_key_count = (read_count <= MAX_KEY_COUNT) ? read_count : 0;
  EEPROM_key_index = EEPROM.read(1);
  if (EEPROM_key_count > 0 && EEPROM_key_index <= EEPROM_key_count && EEPROM_key_index > 0) {
    EEPROM_get_key(EEPROM_key_index, &keyData1);
  } else {
    EEPROM_key_count = 0;
    EEPROM_key_index = 0;
    EEPROM.write(0, 0);
    EEPROM.write(1, 0);
    EEPROM.commit();
    memset(&keyData1, 0, sizeof(tpKeyData));
  }

  // Ждём кнопку для выхода с лого
  while (!(btn_up.isClick() || btn_down.isClick() || btn_ok.isClick() || btn_back.isClick())) {
    btn_up.tick();
    btn_down.tick();
    btn_ok.tick();
    btn_back.tick();
  }
  menuState = menuMain;
  OLED_printMenu(display, menuIndex);
}

void loop() {
  btn_up.tick();
  btn_down.tick();
  btn_ok.tick();
  btn_back.tick();

  // Serial
  if (Serial.available()) {
    char echo = Serial.read();
    if (echo == 'e') {
      display.clearDisplay();
      display.setTextSize(1);
      display.setCursor(0, 0);
      display.print(F("EEPROM cleared success!"));
      display.display();
      EEPROM.write(0, 0);
      EEPROM.write(1, 0);
      EEPROM.commit();
      EEPROM_key_count = 0;
      EEPROM_key_index = 0;
      memset(&keyData1, 0, sizeof(tpKeyData));
      stTimer = millis();
    } else if (echo == 't' && menuState == menuTransmit && EEPROM_key_count > 0) {
      sendSynthKey(&keyData1);
      restoreReceiveMode();
      stTimer = millis();
    }
  }

  // Menu
  if (menuState == menuLogo) {
    if (btn_up.isClick() || btn_down.isClick() || btn_ok.isClick() || btn_back.isClick()) {
      menuState = menuMain;
      OLED_printMenu(display, menuIndex);
    }
  } else if (menuState == menuMain) {
    if (btn_up.isClick()) {
      menuIndex = (menuIndex == 0) ? 3 : menuIndex - 1;
      OLED_printMenu(display, menuIndex);
    }
    if (btn_down.isClick()) {
      menuIndex = (menuIndex == 3) ? 0 : menuIndex + 1;
      OLED_printMenu(display, menuIndex);
    }
    if (btn_ok.isClick()) {
      if (menuIndex == 0) menuState = menuReceive;
      else if (menuIndex == 1) menuState = menuTransmit;
      else if (menuIndex == 2) menuState = menuAnalyzer;
      else if (menuIndex == 3) menuState = menuJammer;
      if (menuState == menuReceive) {
        setupCC1101();
        rcswitch.disableReceive();
        rcswitch.enableReceive(CC1101_GDO0);
        validKeyReceived = false;
        signals = 0;
        memset(&keyData1, 0, sizeof(tpKeyData));
        lastSavedKey = 0;
        rcswitch.resetAvailable();
        OLED_printWaitingSignal();
      } else if (menuState == menuTransmit) {
        if (EEPROM_key_count > 0) {
          EEPROM_get_key(EEPROM_key_index, &keyData1);
        } else {
          memset(&keyData1, 0, sizeof(tpKeyData));
        }
        OLED_printKey(&keyData1);
      } else if (menuState == menuAnalyzer) {
        setupCC1101();
        rcswitch.disableReceive();
        current_scan_index = 0;
        detected_frequency = 0.0;
        last_detected_frequency = 0.0;
        display_updated = false;
        scanTimer = millis();
        OLED_printAnalyzer();
      } else if (menuState == menuJammer) {
        isJamming = false;
        OLED_printJammer();
      }
      stTimer = millis();
    }
    if (btn_back.isClick()) {
      menuState = menuLogo;
      OLED_printLogo(display);
    }
  } else {
    // EEPROM clear
    if (btn_up.isHold() && btn_down.isHold()) {
      display.clearDisplay();
      display.setTextSize(1);
      display.setCursor(0, 0);
      display.print(F("EEPROM cleared success!"));
      display.display();
      EEPROM.write(0, 0);
      EEPROM.write(1, 0);
      EEPROM.commit();
      EEPROM_key_count = 0;
      EEPROM_key_index = 0;
      memset(&keyData1, 0, sizeof(tpKeyData));
      stTimer = millis();
    }

    // Подтверждение удаления
    if (awaitingDeleteConfirmation) {
      if (btn_ok.isClick()) {
        deleteCurrentKey();
        awaitingDeleteConfirmation = false;
        if (menuState == menuTransmit) {
          OLED_printKey(&keyData1);
        } else if (menuState == menuReceive) {
          OLED_printWaitingSignal();
        } else {
          menuState = menuMain;
          OLED_printMenu(display, menuIndex);
        }
        stTimer = millis();
      } else if (btn_back.isClick() || btn_up.isClick() || btn_down.isClick()) {
        awaitingDeleteConfirmation = false;
        if (menuState == menuTransmit) {
          OLED_printKey(&keyData1);
        } else if (menuState == menuReceive) {
          OLED_printWaitingSignal();
        } else if (menuState == menuAnalyzer) {
          OLED_printAnalyzer(last_detected_frequency != 0.0, last_detected_frequency);
        } else if (menuState == menuJammer) {
          OLED_printJammer();
        } else {
          menuState = menuMain;
          OLED_printMenu(display, menuIndex);
        }
        stTimer = millis();
      }
      return;
    }

    // Сцены
    if (menuState == menuReceive) {
      // Переключение частоты
      if (btn_up.isClick()) {
        freqIndex = (freqIndex + 1) % numFrequencies;
        frequency = frequencies[freqIndex];
        keyData1.frequency = frequency;
        setupCC1101();
        OLED_printWaitingSignal();
        stTimer = millis();
      }
      if (btn_down.isClick()) {
        freqIndex = (freqIndex - 1 + numFrequencies) % numFrequencies;
        frequency = frequencies[freqIndex];
        keyData1.frequency = frequency;
        setupCC1101();
        OLED_printWaitingSignal();
        stTimer = millis();
      }
      if (btn_ok.isHolded() && validKeyReceived) {
        Serial.print(F("Attempting to save key: ID="));
        for (byte i = 0; i < (keyData1.codeLenth + 7) >> 3; i++) {
          Serial.print(keyData1.keyID[i], HEX);
        }
        Serial.println();
        if (EEPROM_AddKey(&keyData1)) {
          display.clearDisplay();
          display.drawBitmap(16, 6, image_DolphinSaved_bits, 92, 58, 1);
          display.setTextColor(1);
          display.setTextWrap(false);
          display.setCursor(6, 16);
          display.print("Saved");
          display.display();
          Serial.println(F("Key saved successfully"));
          delay(1000);
          validKeyReceived = false;
          signals = 0;
          memset(&keyData1, 0, sizeof(tpKeyData));
          lastSavedKey = 0;
          rcswitch.resetAvailable();
        } else {
          OLED_printError(F("Key not saved"), true);
          Serial.println(F("Failed to save key"));
        }
        OLED_printWaitingSignal();
        stTimer = millis();
      }
      if (rcswitch.available()) {
        Serial.println(F("Signal received"));
        if (!readRAW) {
          read_rcswitch(&keyData1);
        } else {
          read_raw(&keyData1);
        }
        if (validKeyReceived && autoSave && (lastSavedKey != keyData1.keyID[0] || keyData1.keyID[0] == 0)) {
          Serial.print(F("Auto-saving key: ID="));
          for (byte i = 0; i < (keyData1.codeLenth + 7) >> 3; i++) {
            Serial.print(keyData1.keyID[i], HEX);
          }
          Serial.println();
          if (EEPROM_AddKey(&keyData1)) {
            lastSavedKey = keyData1.keyID[0];
            Serial.println(F("Key auto-saved successfully"));
            validKeyReceived = false;
            signals = 0;
            memset(&keyData1, 0, sizeof(tpKeyData));
            rcswitch.resetAvailable();
          } else {
            Serial.println(F("Auto-save failed"));
          }
        }
        stTimer = millis();
      }
    } else if (menuState == menuTransmit) {
      if (btn_up.isClick() && (EEPROM_key_count > 0)) {
        EEPROM_key_index = (EEPROM_key_index == EEPROM_key_count) ? 1 : EEPROM_key_index + 1;
        EEPROM_get_key(EEPROM_key_index, &keyData1);
        OLED_printKey(&keyData1);
        stTimer = millis();
      }
      if (btn_down.isClick() && (EEPROM_key_count > 0)) {
        EEPROM_key_index = (EEPROM_key_index == 1) ? EEPROM_key_count : EEPROM_key_index - 1;
        EEPROM_get_key(EEPROM_key_index, &keyData1);
        OLED_printKey(&keyData1);
        stTimer = millis();
      }
      if (btn_ok.isClick()) {
        if (EEPROM_key_count > 0) {
          sendSynthKey(&keyData1);
          restoreReceiveMode();
        } else {
          OLED_printError(F("No keys to send"), true);
          delay(1000);
          OLED_printKey(&keyData1);
        }
        stTimer = millis();
      }
    } else if (menuState == menuAnalyzer) {
      if (millis() - scanTimer >= 250) {
        int rssi = ELECHOUSE_cc1101.getRssi();
        if (rssi >= rssi_threshold) {
          detected_frequency = subghz_frequency_list[current_scan_index];
          if (detected_frequency != last_detected_frequency) {
            last_detected_frequency = detected_frequency;
            Serial.print(F("Signal detected at "));
            Serial.print(detected_frequency);
            Serial.println(F(" MHz"));
            OLED_printAnalyzer(true, last_detected_frequency);
            display_updated = true;
          }
        } else {
          // Сигнал отсутствует, продолжаем скан
          detected_frequency = 0.0;
        }
        // Следующая частота
        current_scan_index = (current_scan_index + 1) % subghz_frequency_count;
        ELECHOUSE_cc1101.setMHZ(subghz_frequency_list[current_scan_index]);
        ELECHOUSE_cc1101.SetRx();
        delayMicroseconds(3500); // Стабилизация приемника
        scanTimer = millis();
      }
    } else if (menuState == menuJammer) {
      if (btn_up.isClick()) {
        freqIndex = (freqIndex + 1) % numFrequencies;
        frequency = frequencies[freqIndex];
        if (isJamming) {
          stopJamming();
          startJamming();
        }
        OLED_printJammer();
        stTimer = millis();
      }
      if (btn_down.isClick()) {
        freqIndex = (freqIndex - 1 + numFrequencies) % numFrequencies;
        frequency = frequencies[freqIndex];
        if (isJamming) {
          stopJamming();
          startJamming();
        }
        OLED_printJammer();
        stTimer = millis();
      }
      if (btn_ok.isClick()) {
        if (!isJamming) {
          startJamming();
          isJamming = true;
        } else {
          stopJamming();
          isJamming = false;
        }
        OLED_printJammer();
        stTimer = millis();
      }
    }

    if (btn_back.isClick()) {
      if (menuState == menuJammer && isJamming) {
        stopJamming();
        isJamming = false;
      }
      menuState = menuMain;
      restoreReceiveMode();
      OLED_printMenu(display, menuIndex);
      stTimer = millis();
    }
    if (btn_back.isHold() && EEPROM_key_count > 0 && !awaitingDeleteConfirmation && menuState == menuTransmit) {
      display.clearDisplay();
      display.drawBitmap(83, 22, image_WarningDolphinFlip_bits, 45, 42, 1);
      display.drawBitmap(77, 2, image_file_delete_bin_bits, 13, 16, 1);
      display.setTextColor(1);
      display.setTextWrap(false);
      display.setCursor(7, 7);
      display.print("Press OK to ");
      display.display();
      awaitingDeleteConfirmation = true;
      stTimer = millis();
    }
  }
}

void setupCC1101() {
  ELECHOUSE_cc1101.setSpiPin(CC1101_SCK, CC1101_MISO, CC1101_MOSI, CC1101_CS);
  ELECHOUSE_cc1101.setGDO0(CC1101_GDO0);
  ELECHOUSE_cc1101.Init();
  ELECHOUSE_cc1101.setModulation(2);
  ELECHOUSE_cc1101.setMHZ(frequency);
  ELECHOUSE_cc1101.setRxBW(270.0);
  ELECHOUSE_cc1101.setDeviation(0);
  ELECHOUSE_cc1101.setPA(12);
  ELECHOUSE_cc1101.SetRx();
}

void restoreReceiveMode() {
  ELECHOUSE_cc1101.Init();
  ELECHOUSE_cc1101.setModulation(2);
  ELECHOUSE_cc1101.setMHZ(frequency); // Последняя выбранная частота
  ELECHOUSE_cc1101.setRxBW(270.0);
  ELECHOUSE_cc1101.setDeviation(0);
  ELECHOUSE_cc1101.setPA(12);
  ELECHOUSE_cc1101.SetRx();
  pinMode(CC1101_GDO0, INPUT);
  rcswitch.disableReceive();
  rcswitch.enableReceive(CC1101_GDO0);
  recieved = false;
}

void read_rcswitch(tpKeyData* kd) {
  uint64_t decoded = rcswitch.getReceivedValue();
  if (decoded) {
    Serial.println(F("RcSwitch signal captured"));
    signals++;
    kd->frequency = frequency;
    kd->keyID[0] = decoded & 0xFF;
    kd->keyID[1] = (decoded >> 8) & 0xFF;
    kd->keyID[2] = (decoded >> 16) & 0xFF;
    kd->keyID[3] = (decoded >> 24) & 0xFF;
    kd->type = (rcswitch.getReceivedProtocol() == 1 && rcswitch.getReceivedBitlength() == 24) ? kPrinceton : kRcSwitch;
    if (rcswitch.getReceivedBitlength() <= 40) {
      if (rcswitch.getReceivedProtocol() == 11) {
        kd->type = kCAME;
      }
    }
    kd->te = rcswitch.getReceivedDelay();
    kd->bitLength = rcswitch.getReceivedBitlength();
    kd->codeLenth = kd->bitLength;
    strncpy(kd->preset, String(rcswitch.getReceivedProtocol()).c_str(), sizeof(kd->preset) - 1);
    kd->preset[sizeof(kd->preset) - 1] = '\0';
    kd->rawData[0] = '\0';
    unsigned int* raw = rcswitch.getReceivedRawdata();
    String rawStr = "";
    for (int i = 0; i < kd->bitLength * 2 && i < 15; i++) {
      if (i > 0) rawStr += " ";
      int sign = (i % 2 == 0) ? 1 : -1;
      rawStr += String(sign * (int)raw[i]);
    }
    strncpy(kd->rawData, rawStr.c_str(), sizeof(kd->rawData) - 1);
    kd->rawData[sizeof(kd->rawData) - 1] = '\0';
    validKeyReceived = true;
    byte idx = indxKeyInROM(kd);
    OLED_printKey(kd, idx == 0 ? 1 : 3);
  }
  rcswitch.resetAvailable();
}

void read_raw(tpKeyData* kd) {
  delay(400);
  unsigned int* raw = rcswitch.getReceivedRawdata();
  uint64_t decoded = rcswitch.getReceivedValue();
  String data = "";
  int transitions = 0;
  for (transitions = 0; transitions < MAX_DATA_LOG; transitions++) {
    if (raw[transitions] == 0) break;
    if (transitions > 0) data += " ";
    int sign = (transitions % 2 == 0) ? 1 : -1;
    data += String(sign * (int)raw[transitions]);
  }
  if (transitions > 20) {
    Serial.println(F("Raw signal captured"));
    signals++;
    kd->frequency = frequency;
    if (data.length() >= sizeof(kd->rawData)) {
      data = data.substring(0, sizeof(kd->rawData) - 1);
    }
    strncpy(kd->rawData, data.c_str(), sizeof(kd->rawData) - 1);
    kd->rawData[sizeof(kd->rawData) - 1] = '\0';
    kd->type = kUnknown;
    kd->te = 0;
    kd->bitLength = 0;
    strncpy(kd->preset, "0", sizeof(kd->preset) - 1);
    kd->preset[sizeof(kd->preset) - 1] = '\0';
    kd->codeLenth = transitions;
    if (decoded) {
      kd->keyID[0] = decoded & 0xFF;
      kd->keyID[1] = (decoded >> 8) & 0xFF;
      kd->keyID[2] = (decoded >> 16) & 0xFF;
      kd->keyID[3] = (decoded >> 24) & 0xFF;
      kd->type = (rcswitch.getReceivedProtocol() == 1 && rcswitch.getReceivedBitlength() == 24) ? kPrinceton : kRcSwitch;
      if (rcswitch.getReceivedBitlength() <= 40 && rcswitch.getReceivedProtocol() == 11) {
        kd->type = kCAME;
      }
      kd->te = rcswitch.getReceivedDelay();
      kd->bitLength = rcswitch.getReceivedBitlength();
      kd->codeLenth = kd->bitLength;
      strncpy(kd->preset, String(rcswitch.getReceivedProtocol()).c_str(), sizeof(kd->preset) - 1);
      kd->preset[sizeof(kd->preset) - 1] = '\0';
    } else {
      if (transitions >= 129 && transitions <= 132) {
        kd->type = kStarLine;
      } else if (transitions >= 133 && transitions <= 137) {
        kd->type = kKeeLoq;
      } else if (transitions >= 40 && transitions <= 60) {
        kd->type = kCAME;
      }
    }
    validKeyReceived = true;
    byte idx = indxKeyInROM(kd);
    OLED_printKey(kd, idx == 0 ? 1 : 3);
  }
  rcswitch.resetAvailable();
}

void OLED_printWaitingSignal() {
  display.clearDisplay();
  display.drawBitmap(0, 4, image_RFIDDolphinReceive_bits, 97, 61, 1);
  display.drawBitmap(79, 8, image_Layer_4_bits, 23, 17, 0);
  display.setTextColor(1);
  display.setTextWrap(false);
  display.setCursor(65, 38);
  display.print("Waiting");
  display.setCursor(65, 47);
  display.print("signal...");
  // Выбранная частота
  display.setCursor(72, 13);
  display.print(String(frequency, 2) + "MHz");
  display.display();
}

void deleteCurrentKey() {
  if (EEPROM_key_count == 0) {
    Serial.println(F("No keys to delete"));
    return;
  }

  if (EEPROM_key_index < 1 || EEPROM_key_index > EEPROM_key_count) {
    EEPROM_key_index = EEPROM_key_count;
  }

  Serial.print(F("Deleting key at index "));
  Serial.println(EEPROM_key_index);

  // Сдвигаем все последующие ключи
  for (byte i = EEPROM_key_index; i < EEPROM_key_count; i++) {
    tpKeyData nextKey;
    int nextAddress = (i + 1) * sizeof(tpKeyData) + 2;
    int currentAddress = i * sizeof(tpKeyData) + 2;

    // Следующий ключ
    for (byte j = 0; j < sizeof(tpKeyData); j++) {
      ((byte*)&nextKey)[j] = EEPROM.read(nextAddress + j);
    }

    for (byte j = 0; j < sizeof(tpKeyData); j++) {
      EEPROM.write(currentAddress + j, ((byte*)&nextKey)[j]);
    }
  }

  int lastAddress = EEPROM_key_count * sizeof(tpKeyData) + 2;
  for (byte j = 0; j < sizeof(tpKeyData); j++) {
    EEPROM.write(lastAddress + j, 0);
  }

  EEPROM_key_count--;
  EEPROM.write(0, EEPROM_key_count);

  if (EEPROM_key_count > 0) {
    if (EEPROM_key_index > EEPROM_key_count) {
      EEPROM_key_index = EEPROM_key_count;
    }
  } else {
    EEPROM_key_index = 0;
  }
  EEPROM.write(1, EEPROM_key_index);
  EEPROM.commit();

  // Текущий ключ
  if (EEPROM_key_count > 0) {
    EEPROM_get_key(EEPROM_key_index, &keyData1);
    Serial.print(F("Key deleted, now at index: "));
    Serial.println(EEPROM_key_index);
  } else {
    EEPROM_key_index = 0;
    memset(&keyData1, 0, sizeof(tpKeyData));
    Serial.println(F("All keys deleted"));
  }

  // Подтверждение удаления
  display.clearDisplay();
  display.drawBitmap(5, 2, image_DolphinMafia_bits, 119, 62, 1);
  display.setTextColor(1);
  display.setTextWrap(false);
  display.setCursor(84, 15);
  display.print("Deleted");
  display.display();
  delay(1000);
}

void OLED_printKey(tpKeyData* kd, byte msgType, bool isSending) {
  String st;
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(1);
  display.setTextWrap(false);

  // Сигналы
  switch (msgType) {
    case 0:
      st = " Signal " + String(EEPROM_key_index) + "/" + String(EEPROM_key_count) + " in ESP";
      break;
    case 1:
      st = " Hold OK to save";
      break;
    case 3:
      st = " Signal " + String(indxKeyInROM(kd));
      break;
  }
  display.setCursor(0, 0);
  display.print(st);

  // Информация ключа
  st = "";
  for (byte i = 0; i < (kd->codeLenth + 7) >> 3; i++) st += String(kd->keyID[i], HEX) + ":";
  st.remove(st.length() - 1);
  
  display.setCursor(0, 12);
  display.print("Code: " + st);
  
  st = "Type: " + getTypeName(kd->type);
  display.setCursor(0, 24);
  display.print(st);
  
  st = "Freq: " + String(kd->frequency) + " MHz";
  display.setCursor(0, 36);
  display.print(st);
  
  if (kd->bitLength > 0) {
    st = "Bits: " + String(kd->bitLength);
    display.setCursor(0, 48);
    display.print(st);
  }

  // "Sending..." и битмап
  if (isSending) {
    display.setCursor(67, 54);
    display.print("Sending...");
    display.drawBitmap(109, 40, image_satellite_dish_bits, 15, 16, 1);
  }
  display.display();
}

void OLED_printError(String st, bool err) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(err ? F("Error!") : F("OK"));
  display.setCursor(0, 12);
  display.print(st);
  display.display();
}

byte indxKeyInROM(tpKeyData* kd) {
  if (!kd || kd->codeLenth == 0) return 0;
  bool eq = true;
  byte* buf = (byte*)kd;
  for (byte j = 1; j <= EEPROM_key_count; j++) {
    eq = true;
    byte i = (kd->type == kKeeLoq || kd->type == kANmotors64) ? 4 : 0;
    for (; i < (kd->codeLenth + 7) >> 3; i++) {
      byte eepromByte = EEPROM.read(i + j * sizeof(tpKeyData) + 2);
      if (buf[i] != eepromByte) {
        eq = false;
        break;
      }
    }
    if (eq) {
      Serial.print(F("Key already exists at index "));
      Serial.println(j);
      return j;
    }
  }
  return 0;
}

bool EEPROM_AddKey(tpKeyData* kd) {
  if (!kd) {
    Serial.println(F("Invalid key data: NULL pointer"));
    return false;
  }
  if (kd->codeLenth == 0) {
    Serial.println(F("Invalid key data: codeLenth is zero"));
    return false;
  }
  if (kd->frequency == 0.0) {
    Serial.println(F("Invalid key data: frequency is zero"));
    return false;
  }
  byte indx = indxKeyInROM(kd);
  if (indx != 0) {
    EEPROM_key_index = indx;
    EEPROM.write(1, EEPROM_key_index);
    EEPROM.commit();
    Serial.println(F("Key already exists, updated index"));
    return false;
  }
  if (EEPROM_key_count >= maxKeyCount) {
    Serial.println(F("Max key count reached"));
    return false;
  }
  EEPROM_key_count++;
  EEPROM_key_index = EEPROM_key_count;
  int address = EEPROM_key_index * sizeof(tpKeyData) + 2;
  if (address + sizeof(tpKeyData) > EEPROM_SIZE) {
    EEPROM_key_count--;
    Serial.println(F("EEPROM overflow"));
    return false;
  }
  for (byte i = 0; i < sizeof(tpKeyData); i++) {
    EEPROM.write(address + i, ((byte*)kd)[i]);
  }
  EEPROM.write(0, EEPROM_key_count);
  EEPROM.write(1, EEPROM_key_index);
  EEPROM.commit();
  Serial.print(F("Key saved at index "));
  Serial.println(EEPROM_key_index);
  return true;
}

void EEPROM_get_key(byte EEPROM_key_index1, tpKeyData* kd) {
  int address = EEPROM_key_index1 * sizeof(tpKeyData) + 2;
  if (address + sizeof(tpKeyData) > EEPROM_SIZE) {
    Serial.println(F("Invalid EEPROM address for key read"));
    return;
  }
  for (byte i = 0; i < sizeof(tpKeyData); i++) {
    ((byte*)kd)[i] = EEPROM.read(address + i);
  }
}

String getTypeName(emKeys tp) {
  switch (tp) {
    case kUnknown: return F("Unknown");
    case kP12bt: return F("Pre 12bit");
    case k12bt: return F("12bit");
    case k24bt: return F("24bit");
    case k64bt: return F("64bit");
    case kKeeLoq: return F("KeeLoq");
    case kANmotors64: return F("ANmotors");
    case kPrinceton: return F("Princeton");
    case kRcSwitch: return F("RcSwitch");
    case kStarLine: return F("StarLine");
    case kCAME: return F("CAME");
    case kNICE: return F("NICE");
    case kHOLTEK: return F("HOLTEK");
  }
  return "";
}

void myDelayMcs(unsigned long dl) {
  if (dl > 16000) {
    delay(dl / 1000);
  } else {
    delayMicroseconds(dl);
  }
}

void sendSynthKey(tpKeyData* kd) {
  recieved = true;
  OLED_printKey(kd, 0, true);

  Serial.print(F("Transmitting key, protocol: "));
  Serial.print(getTypeName(kd->type));
  Serial.print(F(", ID: "));
  for (byte i = 0; i < (kd->codeLenth + 7) >> 3; i++) {
    Serial.print(kd->keyID[i], HEX);
  }
  Serial.print(F(", Freq: "));
  Serial.print(kd->frequency);
  Serial.println(F(" MHz"));

  ELECHOUSE_cc1101.setModulation(2);
  ELECHOUSE_cc1101.setMHZ(kd->frequency);
  ELECHOUSE_cc1101.setRxBW(270.0);
  ELECHOUSE_cc1101.setPA(12);
  ELECHOUSE_cc1101.SetTx();
  pinMode(CC1101_GDO0, OUTPUT);

  if (kd->type == kRcSwitch || kd->type == kPrinceton || kd->type == kCAME || kd->type == kNICE || kd->type == kHOLTEK) {
    uint64_t data = 0;
    for (int i = 0; i < (kd->bitLength + 7) / 8; i++) {
      data |= ((uint64_t)kd->keyID[i] << (i * 8));
    }
    int protocol = atoi(kd->preset);
    if (kd->type == kCAME || kd->type == kNICE || kd->type == kHOLTEK) {
      protocol = 11;
      kd->te = 270;
    } else if (kd->type == kPrinceton) {
      protocol = 1;
      kd->te = 350;
    }
    rcswitch.enableTransmit(CC1101_GDO0);
    rcswitch.setProtocol(protocol);
    if (kd->te) rcswitch.setPulseLength(kd->te);
    rcswitch.setRepeatTransmit(10);
    rcswitch.send(data, kd->bitLength);
    rcswitch.disableTransmit();
  } else {
    String data = String(kd->rawData);
    int buff_size = 0;
    int index = 0;
    while (index >= 0) {
      index = data.indexOf(' ', index + 1);
      buff_size++;
    }
    int* transmittimings = (int*)calloc(sizeof(int), buff_size + 1);
    int startIndex = 0;
    index = 0;
    for (size_t i = 0; i < buff_size; i++) {
      index = data.indexOf(' ', startIndex);
      if (index == -1) {
        transmittimings[i] = data.substring(startIndex).toInt();
      } else {
        transmittimings[i] = data.substring(startIndex, index).toInt();
      }
      startIndex = index + 1;
    }
    transmittimings[buff_size] = 0;

    for (int nRepeat = 0; nRepeat < 2; nRepeat++) {
      unsigned int currenttiming = 0;
      bool currentlogiclevel = true;
      while (transmittimings[currenttiming]) {
        if (transmittimings[currenttiming] >= 0) {
          currentlogiclevel = true;
        } else {
          currentlogiclevel = false;
          transmittimings[currenttiming] = (-1) * transmittimings[currenttiming];
        }
        digitalWrite(CC1101_GDO0, currentlogiclevel ? HIGH : LOW);
        myDelayMcs(transmittimings[currenttiming]);
        currenttiming++;
      }
      digitalWrite(CC1101_GDO0, LOW);
    }
    free(transmittimings);
  }

  ELECHOUSE_cc1101.SetRx();
  recieved = false;

  display.clearDisplay();
  display.setTextColor(1);
  display.setTextWrap(false);
  display.setCursor(15, 47);
  display.print("Successfully!");
  display.drawBitmap(15, 12, image_Connected_bits, 62, 31, 1);
  display.display();
  delay(1000);
  OLED_printKey(kd);
}

void sendSynthBit(int bt[2]) {
  static uint8_t dataOn[1] = {0xFF};
  static uint8_t dataOff[1] = {0x00};
  if (bt[0] == 0) return;
  for (byte i = 0; i < 2; i++) {
    if (bt[i] > 0) {
      ELECHOUSE_cc1101.SendData(dataOn, 1);
      myDelayMcs(bt[i]);
    } else {
      ELECHOUSE_cc1101.SendData(dataOff, 1);
      myDelayMcs(-bt[i]);
    }
  }
}

void OLED_printAnalyzer(bool signalReceived, float detectedFreq) {
  display.clearDisplay();
  display.drawBitmap(0, 7, image_Dolphin_MHz_bits, 108, 57, 1);
  display.drawBitmap(100, 6, image_MHz_1_bits, 25, 11, 1);
  display.setTextColor(1);
  display.setTextWrap(false);
  display.setCursor(62, 10);
  if (signalReceived || detectedFreq != 0.0) {
    char freq_str[7];
    snprintf(freq_str, sizeof(freq_str), "%06.2f", detectedFreq);
    display.print(freq_str);
  } else {
    display.print("000.00");
  }
  display.display();
}

void OLED_printJammer() {
  display.clearDisplay();
  display.drawBitmap(0, 3, image_Dolphin_Send_bits, 97, 61, 1);
  display.setTextColor(1);
  display.setTextWrap(false);
  display.setCursor(78, 12);
  display.print(String(frequency, 2));
  display.setCursor(65, 42);
  if (isJamming) {
    display.print("Jamming...");
  } else {
    display.print("Press OK");
  }
  if (!isJamming) {
    display.setCursor(65, 51);
    display.print("to start");
  }
  display.display();
}

void startJamming() {
  Serial.println(F("Starting jammer"));
  ELECHOUSE_cc1101.Init();
  ELECHOUSE_cc1101.setModulation(0);
  ELECHOUSE_cc1101.setMHZ(frequency);
  ELECHOUSE_cc1101.setPA(12); // Мощность
  ELECHOUSE_cc1101.setDeviation(0);
  ELECHOUSE_cc1101.setRxBW(270.0);
  
  ELECHOUSE_cc1101.SetTx();
  ELECHOUSE_cc1101.SpiWriteReg(0x3E, 0xFF);
  ELECHOUSE_cc1101.SpiWriteReg(0x35, 0x60);
}

void stopJamming() {
  Serial.println(F("Stopping jammer"));
  ELECHOUSE_cc1101.SpiWriteReg(0x35, 0x00);
  ELECHOUSE_cc1101.SetRx();
  restoreReceiveMode();
}
