#include <U8g2lib.h>
#include <Wire.h>
#include <EEPROM.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ArduinoOTA.h>
#include <ESP8266httpUpdate.h>
#include <ESP8266Ping.h>

// Флаг доступности дисплея
bool displayAvailable = false;

// Пины для Wemos ESP8266
#define RELAY_PIN    D5  // GPIO 14, Реле HW-482
#define BUTTON_SEL   D6  // GPIO 12, Кнопка выбора
#define LIMIT_SWITCH D7  // GPIO 13, Оптический Endstop (не используется)
#define START_SWITCH D0  // GPIO 16, Концевик для старта подачи
#define SDA_PIN      D2  // GPIO 4, I2C SDA
#define SCL_PIN      D1  // GPIO 5, I2C SCL

// Константы
const String VERSION = "1.1.1";
const String GITHUB_REPO_URL = "https://raw.githubusercontent.com/Ko1hozer/tempest_feeder_autoloader_airsoft/main/";  // Замените на ваш repo
const String GITHUB_FIRMWARE_URL = GITHUB_REPO_URL + "Autoloader.bin";
const String GITHUB_VERSION_URL = GITHUB_REPO_URL + "version.txt";

// Прототипы функций
void displayLogo();
void displayMenu();
void loadPresets();
void loadStats();
void loadCalibration();
void loadAPConfig();
void loadHomeWifiConfig();
void updateStats();
void loadBalls(int targetBalls);
void calibrateBalls();
void handleRoot();
void handleUpdate();
void handleReset();
void handleClear();
void handleCalibrate();
void handleTestRelay();
void handleOTA();
void handleGithubOTA();
void handleCheckUpdate();
void handleExit();
void handleExitWithSave();
void handleAPConfig();
void handleHomeWifiConfig();
void enterConfigMode();
void exitConfigMode();
void startOTA();
void saveAndRestart();

// OLED-дисплей
U8G2_SSD1306_128X64_NONAME_F_SW_I2C u8g2(U8G2_R0, SCL_PIN, SDA_PIN, U8X8_PIN_NONE);

// Wi-Fi настройки
char ssid[33] = "Autoloader";
char password[33] = "12345678";
char homeSsid[33] = "";
char homePassword[33] = "";

// Серверы
ESP8266WebServer server(80);
DNSServer dnsServer;
const byte DNS_PORT = 53;

// Пресеты
struct Preset {
  char name[21]; // До 10 кириллических символов + '\0'
  int balls;
};
Preset ballOptions[3];
const int numOptions = 3;
int selectedOption = 0;
bool lastButtonState = HIGH;
bool lastStartState = HIGH;
bool lastLimitState = LOW; // Инвертированный Endstop: LOW в покое

// Статистика
struct Stats {
  int counts[3]; // Для каждого пресета
};
Stats stats;

// Калибровка
struct Calibration {
  float ballsPerSecond; // Шары в секунду
};
Calibration calibration;
const float defaultBallsPerSecond = 14.0; // По умолчанию 14 шаров/сек
const int calibrationTime = 2000; // 2 секунды для калибровки

const int eepromAddrPresets = 0;
const int eepromAddrStats = 75; // После пресетов (3 * (21 + 4) = 75 байт)
const int eepromAddrCalibration = 150; // После статистики
const int eepromAddrAPConfig = 250; // Для SSID и пароля
const int eepromAddrHomeWifi = 316; // Для homeSsid и homePassword

// Переменные
int ballCount = 0;
bool isLoading = false;
bool isConfigMode = false;
bool isCalibrating = false;
bool isOTAMode = false;
bool isHomeWifiConnected = false;
unsigned long buttonPressStart = 0;
unsigned long calibrationStartTime = 0;
const unsigned long calibrationTimeout = 60000; // 60 секунд
bool allowNewLoad = true;

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("Setup started. Version: " + VERSION);
  
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(50000);
  Wire.setClockStretchLimit(1500);  // Улучшение стабильности I2C
  Serial.println("I2C initialized");
  
  if (u8g2.begin()) {
    displayAvailable = true;
    u8g2.enableUTF8Print();
    u8g2.setFont(u8g2_font_8x13_t_cyrillic);
    Serial.println("Display initialized");
    displayLogo();
  } else {
    Serial.println("Display initialization failed! Continuing without display.");
    displayAvailable = false;
  }
  
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BUTTON_SEL, INPUT_PULLUP);
  pinMode(LIMIT_SWITCH, INPUT_PULLUP);
  pinMode(START_SWITCH, INPUT_PULLUP);
  
  digitalWrite(RELAY_PIN, LOW);
  
  delay(200);
  lastStartState = digitalRead(START_SWITCH);
  Serial.print("Initial START_SWITCH state: ");
  Serial.println(lastStartState == HIGH ? "HIGH (open)" : "LOW (closed)");
  
  lastLimitState = digitalRead(LIMIT_SWITCH);
  Serial.print("Initial LIMIT_SWITCH state: ");
  Serial.println(lastLimitState == LOW ? "LOW (no ball)" : "HIGH (ball detected)");
  
  EEPROM.begin(512);
  loadPresets();
  loadStats();
  loadCalibration();
  loadAPConfig();
  loadHomeWifiConfig();
  Serial.println("EEPROM initialized");
  
  digitalWrite(RELAY_PIN, LOW);
  
  // По умолчанию: Только AP mode
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid, password);
  Serial.println("Default AP mode. IP: " + WiFi.softAPIP().toString());
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  server.begin();

  if (displayAvailable) {
    u8g2.clearBuffer();
    u8g2.setCursor(0, 20);
    u8g2.print("AP mode IP:");
    u8g2.setCursor(0, 40);
    u8g2.print("192.168.4.1");
    u8g2.sendBuffer();
  }
  
  // Обработчики сервера
  server.on("/", handleRoot);
  server.on("/update", HTTP_POST, handleUpdate);
  server.on("/reset", HTTP_POST, handleReset);
  server.on("/clear", HTTP_POST, handleClear);
  server.on("/calibrate", HTTP_POST, handleCalibrate);
  server.on("/testRelay", HTTP_POST, handleTestRelay);
  server.on("/ota", HTTP_POST, handleOTA);
  server.on("/githubota", HTTP_POST, handleGithubOTA);
  server.on("/checkupdate", HTTP_POST, handleCheckUpdate);
  server.on("/exit", HTTP_POST, handleExit);
  server.on("/exitWithSave", HTTP_POST, handleExitWithSave);
  server.on("/apconfig", HTTP_POST, handleAPConfig);
  server.on("/homewificonfig", HTTP_POST, handleHomeWifiConfig);
  server.onNotFound([]() {
    server.sendHeader("Location", "http://" + WiFi.softAPIP().toString());
    server.send(302);
  });
  
  displayMenu();
  Serial.println("Setup complete");
}

void loop() {
  ESP.wdtFeed();  // Watchdog для предотвращения зависаний
  
  if (isOTAMode) {
    ArduinoOTA.handle();
    if (millis() - calibrationStartTime > 300000) {  // Таймаут OTA 5 мин
      isOTAMode = false;
      displayMenu();
    }
    return;
  }

  // Обработка кнопки выбора
  bool currentButtonState = digitalRead(BUTTON_SEL);
  if (currentButtonState == LOW && lastButtonState == HIGH && !isLoading && !isCalibrating) {
    buttonPressStart = millis();
  }
  if (currentButtonState == LOW && buttonPressStart > 0 && millis() - buttonPressStart >= 3000) {
    if (!isConfigMode) {
      enterConfigMode();
      Serial.println("Entered config mode via long press");
    } else {
      saveAndRestart();
      Serial.println("Saving changes and restarting via long press");
    }
    buttonPressStart = 0;
  }
  if (currentButtonState == HIGH && buttonPressStart > 0 && millis() - buttonPressStart < 3000 && !isLoading && !isCalibrating && !isConfigMode) {
    if (millis() - buttonPressStart >= 200) {
      selectedOption = (selectedOption + 1) % numOptions;
      displayMenu();
      Serial.println("Button pressed, selectedOption: " + String(selectedOption));
    }
    buttonPressStart = 0;
  }
  lastButtonState = currentButtonState;

  if (isConfigMode) {
    dnsServer.processNextRequest();
    server.handleClient();
  }

  bool currentStartState = digitalRead(START_SWITCH);
  if (currentStartState != lastStartState) {
    Serial.print("START_SWITCH changed to: ");
    Serial.println(currentStartState == HIGH ? "HIGH (open)" : "LOW (closed)");
  }

  static unsigned long debounceStart = 0;
  static bool stableState = HIGH;
  static unsigned long stableLowTime = 0;
  if (currentStartState != stableState) {
    if (debounceStart == 0) {
      debounceStart = millis();
    }
    if (millis() - debounceStart >= 50) {
      stableState = currentStartState;
      debounceStart = 0;
      Serial.print("Stable state: ");
      Serial.println(stableState == HIGH ? "HIGH at " + String(millis()) : "LOW at " + String(millis()));
      if (stableState == LOW) {
        stableLowTime = millis();
      } else {
        stableLowTime = 0;
        allowNewLoad = true;
        digitalWrite(RELAY_PIN, LOW);
        Serial.println("Relay OFF: Start switch released at " + String(millis()));
      }
    }
  } else {
    debounceStart = 0;
    if (stableState == LOW && stableLowTime > 0 && millis() - stableLowTime >= 200) {
      Serial.println("Stable LOW confirmed at " + String(millis()));
      if (isCalibrating) {
        Serial.println("Calibration switch triggered");
        calibrateBalls();
        isCalibrating = false;
        calibrationStartTime = 0;
        displayMenu();
      } else if (ballCount == 0 && !isLoading && !isCalibrating && !isConfigMode && allowNewLoad) {
        Serial.println("Start switch triggered");
        isLoading = true;
        allowNewLoad = false;
        loadBalls(ballOptions[selectedOption].balls);
        isLoading = false;
        displayMenu();
      } else {
        Serial.println("Ignored: isLoading=" + String(isLoading) + ", isCalibrating=" + String(isCalibrating) + ", isConfigMode=" + String(isConfigMode) + ", allowNewLoad=" + String(allowNewLoad));
      }
    }
  }
  lastStartState = currentStartState;

  static unsigned long lastDisplayUpdate = 0;
  if (isCalibrating && calibrationStartTime > 0 && millis() - lastDisplayUpdate >= 1000) {
    if (displayAvailable) {
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_8x13_t_cyrillic);
      u8g2.setCursor(0, 10);
      u8g2.print("Калибровка");
      u8g2.setCursor(0, 30);
      u8g2.print("Зажми подаватель");
      u8g2.setCursor(0, 45);
      u8g2.print("Ост: ");
      u8g2.print((calibrationTimeout - (millis() - calibrationStartTime)) / 1000);
      u8g2.print(" сек");
      u8g2.sendBuffer();
    }
    Serial.println("Calibration countdown: " + String((calibrationTimeout - (millis() - calibrationStartTime)) / 1000) + " sec remaining");
    lastDisplayUpdate = millis();
  }

  if (isCalibrating && calibrationStartTime > 0 && millis() - calibrationStartTime >= calibrationTimeout) {
    isCalibrating = false;
    calibrationStartTime = 0;
    if (displayAvailable) {
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_8x13_t_cyrillic);
      u8g2.setCursor(0, 10);
      u8g2.print("Калибровка");
      u8g2.setCursor(0, 30);
      u8g2.print("Тайм-аут");
      u8g2.sendBuffer();
    }
    Serial.println("Calibration timed out");
    delay(2000);
    displayMenu();
  }
}

void displayLogo() {
  if (!displayAvailable) {
    Serial.println("Display not available, skipping logo display");
    delay(100);
    return;
  }
  for (int i = 0; i < 15; i++) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_unifont_t_cyrillic);
    u8g2.setCursor(0, 15);
    u8g2.print("TEMPEST FEEDER");
    u8g2.drawCircle(100, 40, 12, U8G2_DRAW_ALL);
    u8g2.drawLine(100, 28, 100, 52);
    u8g2.drawLine(88, 40, 112, 40);
    int bulletX = 40 + (i * (90 - 40) / 14);
    u8g2.drawBox(bulletX - 8, 37, 8, 6);
    u8g2.drawTriangle(bulletX, 37, bulletX, 43, bulletX + 6, 40);
    u8g2.sendBuffer();
    delay(2);
  }
  Serial.println("Logo displayed");
  delay(100);
}

void displayMenu() {
  if (isConfigMode || isCalibrating || isOTAMode) return;
  if (!displayAvailable) {
    Serial.println("Display not available, menu not shown. Selected: " + String(ballOptions[selectedOption].name) + ": " + String(ballOptions[selectedOption].balls));
    return;
  }
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_8x13_t_cyrillic);
  u8g2.setCursor(0, 15);
  u8g2.print("TEMPEST FEEDER");
  u8g2.setCursor(0, 35);
  u8g2.print(ballOptions[selectedOption].name);
  u8g2.setCursor(100, 35);
  u8g2.print(ballOptions[selectedOption].balls);
  u8g2.setCursor(0, 55);
  u8g2.print("Вставь магаз");
  u8g2.sendBuffer();
  Serial.println("Menu displayed: " + String(ballOptions[selectedOption].name) + ": " + String(ballOptions[selectedOption].balls));
}

void loadPresets() {
  Serial.println("loadPresets started");
  EEPROM.get(eepromAddrPresets, ballOptions);
  bool valid = true;
  for (int i = 0; i < numOptions; i++) {
    Serial.print("Preset ");
    Serial.print(i);
    Serial.print(": name=");
    Serial.print(ballOptions[i].name);
    Serial.print(", balls=");
    Serial.println(ballOptions[i].balls);
    
    int charCount = 0;
    bool hasNull = false;
    for (int j = 0; j < 20; j++) {
      if (ballOptions[i].name[j] == '\0') {
        hasNull = true;
        break;
      }
      if ((ballOptions[i].name[j] & 0xC0) != 0x80) charCount++;
    }
    if (!hasNull || charCount > 10 || ballOptions[i].balls < 10 || ballOptions[i].balls > 500) {
      valid = false;
      strcpy(ballOptions[i].name, i == 0 ? "МКа ув" : i == 1 ? "МКа" : "Пекаль ув");
      ballOptions[i].balls = i == 0 ? 160 : i == 1 ? 120 : i == 2 ? 46 : 20;
      Serial.println("Invalid preset " + String(i) + ", reset to default");
    }
  }
  if (!valid) {
    Serial.println("Writing corrected presets to EEPROM");
    EEPROM.put(eepromAddrPresets, ballOptions);
    if (EEPROM.commit()) {
      Serial.println("EEPROM commit successful");
    } else {
      Serial.println("EEPROM commit failed");
    }
  }
  Serial.println("loadPresets finished");
}

void loadStats() {
  Serial.println("loadStats started");
  EEPROM.get(eepromAddrStats, stats);
  bool valid = true;
  for (int i = 0; i < numOptions; i++) {
    Serial.print("Stats ");
    Serial.print(i);
    Serial.print(": count=");
    Serial.println(stats.counts[i]);
    if (stats.counts[i] < 0 || stats.counts[i] > 100000) {
      valid = false;
      break;
    }
  }
  if (!valid) {
    for (int i = 0; i < numOptions; i++) stats.counts[i] = 0;
    EEPROM.put(eepromAddrStats, stats);
    if (EEPROM.commit()) {
      Serial.println("Stats reset to 0 and saved to EEPROM");
    } else {
      Serial.println("EEPROM commit failed");
    }
  }
  Serial.println("loadStats finished");
}

void loadCalibration() {
  Serial.println("loadCalibration started");
  EEPROM.get(eepromAddrCalibration, calibration);
  Serial.print("Calibration: ballsPerSecond=");
  Serial.println(calibration.ballsPerSecond);
  if (calibration.ballsPerSecond <= 0 || calibration.ballsPerSecond > 50 || isnan(calibration.ballsPerSecond)) {
    calibration.ballsPerSecond = defaultBallsPerSecond;
    EEPROM.put(eepromAddrCalibration, calibration);
    if (EEPROM.commit()) {
      Serial.println("Invalid calibration, reset to default: " + String(defaultBallsPerSecond));
    } else {
      Serial.println("EEPROM commit failed");
    }
  }
  Serial.println("loadCalibration finished");
}

void loadAPConfig() {
  Serial.println("loadAPConfig started");
  char tempSSID[33];
  char tempPassword[33];
  EEPROM.get(eepromAddrAPConfig, tempSSID);
  EEPROM.get(eepromAddrAPConfig + 33, tempPassword);
  String savedSSID = String(tempSSID);
  String savedPassword = String(tempPassword);
  if (savedSSID.length() > 0 && savedSSID.length() <= 32 && savedPassword.length() >= 8 && savedPassword.length() <= 32) {
    strncpy(ssid, savedSSID.c_str(), 33);
    strncpy(password, savedPassword.c_str(), 33);
    Serial.println("Loaded AP settings from EEPROM: SSID=" + savedSSID + ", Password=" + savedPassword);
  } else {
    strncpy(ssid, "Autoloader", 33);
    strncpy(password, "12345678", 33);
    EEPROM.put(eepromAddrAPConfig, ssid);
    EEPROM.put(eepromAddrAPConfig + 33, password);
    if (EEPROM.commit()) {
      Serial.println("Invalid AP settings, saved defaults to EEPROM");
    } else {
      Serial.println("EEPROM commit failed");
    }
  }
  Serial.println("loadAPConfig finished");
}

void loadHomeWifiConfig() {
  Serial.println("loadHomeWifiConfig started");
  EEPROM.get(eepromAddrHomeWifi, homeSsid);
  EEPROM.get(eepromAddrHomeWifi + 33, homePassword);
  if (strlen(homeSsid) > 0 && strlen(homePassword) >= 8) {
    Serial.println("Loaded home WiFi: " + String(homeSsid));
  } else {
    strcpy(homeSsid, "");
    strcpy(homePassword, "");
  }
  Serial.println("loadHomeWifiConfig finished");
}

void updateStats() {
  if (ballCount > 0) {
    stats.counts[selectedOption] += ballCount;
    EEPROM.put(eepromAddrStats, stats);
    if (EEPROM.commit()) {
      Serial.println("Stats updated: count[" + String(selectedOption) + "] = " + String(stats.counts[selectedOption]));
    } else {
      Serial.println("EEPROM commit failed in updateStats");
    }
  } else {
    Serial.println("No balls loaded, stats not updated");
  }
}

void loadBalls(int targetBalls) {
  Serial.println("loadBalls started at " + String(millis()));
  if (displayAvailable) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_8x13_t_cyrillic);
    u8g2.setCursor(0, 10);
    u8g2.print("Загоняем ");
    u8g2.print(targetBalls);
    u8g2.print(" шаров");
    u8g2.sendBuffer();
  }
  Serial.println("Loading screen sent");

  ballCount = targetBalls;
  digitalWrite(RELAY_PIN, HIGH);
  Serial.println("Relay ON at " + String(millis()));

  unsigned long startTime = millis();
  unsigned long lastDisplayUpdate = millis();
  float feedTimeSeconds = (float)targetBalls / calibration.ballsPerSecond;
  unsigned long feedTimeMs = feedTimeSeconds * 1000;
  Serial.print("Calculated feed time: ");
  Serial.print(feedTimeMs);
  Serial.println(" ms");

  while (millis() - startTime < feedTimeMs) {
    ESP.wdtFeed();  // Предотвращение зависаний
    if (digitalRead(START_SWITCH) == HIGH) {
      Serial.println("START_SWITCH HIGH detected at " + String(millis()));
      digitalWrite(RELAY_PIN, LOW);
      Serial.println("Relay OFF at " + String(millis()));
      if (displayAvailable) {
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_8x13_t_cyrillic);
        u8g2.setCursor(0, 10);
        u8g2.print("Магазин вынут");
        u8g2.sendBuffer();
      }
      ballCount = 0;
      Serial.println("Magazine removed, loading stopped");
      delay(500);
      return;
    }
    if (displayAvailable && millis() - lastDisplayUpdate >= 1000) {
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_8x13_t_cyrillic);
      u8g2.setCursor(0, 10);
      u8g2.print("Загнали: ");
      int estimatedBalls = (millis() - startTime) * calibration.ballsPerSecond / 1000;
      u8g2.print(estimatedBalls);
      u8g2.print("/");
      u8g2.print(targetBalls);
      int progressWidth = (estimatedBalls * 80) / targetBalls;
      u8g2.drawFrame(24, 45, 80, 8);
      u8g2.drawBox(24, 45, progressWidth, 8);
      u8g2.sendBuffer();
      Serial.println("Progress updated: " + String(estimatedBalls) + "/" + String(targetBalls));
      lastDisplayUpdate = millis();
    }
    yield();
  }

  digitalWrite(RELAY_PIN, LOW);
  Serial.println("Relay OFF: Feed time completed at " + String(millis()));
  Serial.print("Loading complete. Total balls: ");
  Serial.println(ballCount);
  updateStats();
  ballCount = 0;
  lastStartState = HIGH;
  delay(200);
}

void calibrateBalls() {
  Serial.println("calibrateBalls started at " + String(millis()));
  if (displayAvailable) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_8x13_t_cyrillic);
    u8g2.setCursor(0, 10);
    u8g2.print("Калибровка...");
    u8g2.setCursor(0, 30);
    u8g2.print("Считай шары!");
    u8g2.sendBuffer();
  }
  Serial.println("Calibration screen sent");

  digitalWrite(RELAY_PIN, HIGH);
  Serial.println("Relay ON at " + String(millis()));
  unsigned long startTime = millis();
  while (millis() - startTime < calibrationTime) {
    ESP.wdtFeed();
    if (digitalRead(START_SWITCH) == HIGH) {
      digitalWrite(RELAY_PIN, LOW);
      if (displayAvailable) {
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_8x13_t_cyrillic);
        u8g2.setCursor(0, 10);
        u8g2.print("Магаз откинут");
        u8g2.sendBuffer();
      }
      Serial.println("Magazine removed during calibration");
      delay(500);
      return;
    }
    yield();
  }
  digitalWrite(RELAY_PIN, LOW);
  Serial.println("Relay OFF: Calibration complete at " + String(millis()));
  if (displayAvailable) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_8x13_t_cyrillic);
    u8g2.setCursor(20, 10);
    u8g2.print("Введи шары");
    u8g2.setCursor(10, 30);
    u8g2.print("в веб-интерфейсе");
    u8g2.sendBuffer();
  }
  Serial.println("Calibration finished, awaiting input in web interface");
}

void handleRoot() {
  Serial.println("handleRoot started. Free heap: " + String(ESP.getFreeHeap()));
  
  if (!isConfigMode) {
    Serial.println("Error: Not in config mode");
    server.send(500, "text/plain", "Device not in config mode");
    return;
  }

  int totalBalls = 0;
  for (int i = 0; i < numOptions; i++) {
    totalBalls += stats.counts[i];
  }

  String page = server.arg("page");
  if (page == "") page = "stats";

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");

  String html = "<!DOCTYPE html><html lang='ru'><head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>Автолоадер</title>";
  html += "<style>";
  html += "body { background: linear-gradient(135deg, #1a202c, #2d3748); color: #e2e8f0; font-family: Arial, sans-serif; margin: 0; min-height: 100vh; }";
  html += ".container { display: flex; flex-direction: column; min-height: 100vh; width: 100%; align-items: stretch; }";
  html += ".flex { display: flex; } .flex-col { flex-direction: column; } .justify-between { justify-content: space-between; } .items-center { align-items: center; }";
  html += ".p-2 { padding: 8px; } .p-3 { padding: 12px; } .p-4 { padding: 16px; } .px-4 { padding-left: 16px; padding-right: 16px; } .py-2 { padding-top: 8px; padding-bottom: 8px; } .py-3 { padding-top: 12px; padding-bottom: 12px; }";
  html += ".mt-2 { margin-top: 8px; } .mt-3 { margin-top: 12px; } .mt-6 { margin-top: 24px; } .mb-3 { margin-bottom: 12px; } .mb-4 { margin-bottom: 16px; } .mb-6 { margin-bottom: 24px; } .mb-8 { margin-bottom: 32px; } .mr-2 { margin-right: 8px; }";
  html += ".space-y-2 > :not([hidden]) ~ :not([hidden]) { margin-top: 8px; } .space-y-4 > :not([hidden]) ~ :not([hidden]) { margin-top: 16px; } .space-y-6 > :not([hidden]) ~ :not([hidden]) { margin-top: 24px; }";
  html += ".bg-gray-600 { background-color: #4a5568; } .bg-gray-700 { background-color: #2d3748; } .bg-blue-500 { background-color: #4299e1; } .bg-blue-600 { background-color: #3182ce; } .bg-blue-700 { background-color: #2b6cb0; }";
  html += ".bg-red-600 { background-color: #e53e3e; } .bg-red-700 { background-color: #c53030; } .bg-green-600 { background-color: #38a169; } .bg-green-700 { background-color: #2f855a; }";
  html += ".bg-purple-600 { background-color: #805ad5; } .bg-purple-700 { background-color: #6b46c1; } .bg-orange-600 { background-color: #dd6b20; } .bg-orange-700 { background-color: #c05621; } .bg-yellow-600 { background-color: #d69e2e; } .bg-yellow-700 { background-color: #b7791f; }";
  html += ".text-white { color: #fff; } .text-gray-200 { color: #edf2f7; } .text-gray-300 { color: #e2e8f0; } .text-blue-300 { color: #90cdf4; } .text-green-300 { color: #68d391; } .text-red-300 { color: #fc8181; }";
  html += ".text-2xl { font-size: 1.5rem; line-height: 2rem; } .text-4xl { font-size: 2.25rem; line-height: 2.5rem; }";
  html += ".font-bold { font-weight: 700; } .font-semibold { font-weight: 600; } .font-medium { font-weight: 500; }";
  html += ".rounded { border-radius: 0.25rem; } .rounded-lg { border-radius: 0.5rem; }";
  html += ".border { border-width: 1px; } .border-gray-600 { border-color: #4a5568; }";
  html += ".w-full { width: 100%; } .min-h-screen { min-height: 100vh; }";
  html += ".sidebar { width: 256px; height: 100vh; position: fixed; top: 0; left: 0; background: #2d3748; transform: translateX(-100%); transition: transform 0.3s ease-in-out; z-index: 1000; overflow: hidden; padding-top: 64px; }";
  html += ".sidebar.open { transform: translateX(0); }";
  html += ".sidebar-overlay { display: none; position: fixed; top: 0; left: 0; width: 100%; height: 100%; background: rgba(0,0,0,0.5); z-index: 999; }";
  html += ".sidebar-overlay.show { display: block; }";
  html += ".main-content { flex-grow: 1; padding: 16px; padding-top: 64px; width: 100%; box-sizing: border-box; }";
  html += ".card { background: #2d3748; border-radius: 0.5rem; box-shadow: 0 4px 6px rgba(0,0,0,0.2); padding: 1.5rem; margin-bottom: 2rem; transition: transform 0.2s; }";
  html += ".card:hover { transform: translateY(-5px); }";
  html += ".btn { transition: all 0.3s ease; transform: scale(1); } .btn:hover { transform: scale(1.05); }";
  html += ".tooltip { position: relative; } .tooltip .tooltip-text { visibility: hidden; width: 120px; background: #4a5568; color: #fff; text-align: center; border-radius: 6px; padding: 5px; position: absolute; z-index: 1; bottom: 125%; left: 50%; margin-left: -60px; opacity: 0; transition: opacity 0.3s; }";
  html += ".tooltip:hover .tooltip-text { visibility: visible; opacity: 1; }";
  html += "input, button { border: 1px solid #4a5568; padding: 8px; border-radius: 0.25rem; } input:focus, button:focus { outline: none; box-shadow: 0 0 0 2px #4299e1; }";
  html += "table { border-collapse: collapse; width: 100%; } th, td { border: 1px solid #4a5568; padding: 12px; }";
  html += "@media (min-width: 768px) { .container { flex-direction: row; min-height: 100vh; align-items: stretch; } .sidebar { position: static; transform: none; width: 256px; height: 100vh; flex-shrink: 0; padding: 16px; } .sidebar-overlay { display: none; } .hamburger-btn { display: none; } .main-content { padding: 16px; width: calc(100% - 256px); height: 100vh; overflow-y: auto; box-sizing: border-box; } }";
  html += "svg.icon { width: 16px; height: 16px; fill: currentColor; margin-right: 8px; vertical-align: middle; }";
  html += ".sidebar-nav-btn { display: flex; align-items: center; width: 100%; padding: 12px 16px; border-radius: 0.5rem; transition: all 0.3s ease; transform: scale(1); }";
  html += ".sidebar-nav-btn:hover { transform: scale(1.05); }";
  html += ".hamburger-btn { display: flex; flex-direction: column; justify-content: center; align-items: center; width: 40px; height: 40px; padding: 8px; border-radius: 0.5rem; transition: all 0.3s ease; transform: scale(1); background: #3182ce; position: fixed; top: 16px; left: 16px; z-index: 1001; }";
  html += ".hamburger-btn:hover { transform: scale(1.05); }";
  html += ".hamburger-btn span { width: 24px; height: 3px; background: #fff; margin: 2px 0; border-radius: 2px; }";
  html += ".footer { text-align: center; padding: 16px; color: #e2e8f0; font-size: 0.9rem; margin-top: auto; }";
  html += ".footer a { color: #90cdf4; text-decoration: none; }";
  html += ".footer a:hover { text-decoration: underline; }";
  html += ".text-4xl { font-size: 2.25rem; line-height: 2.5rem; } .text-2xl { font-size: 1.5rem; line-height: 2rem; }";
  html += ".break-words { word-break: break-word; hyphens: auto; }";
  html += "@media (max-width: 767px) { .text-4xl { font-size: 1.5rem; line-height: 2rem; } }";
  html += "</style>";
  html += "<script>";
  html += "function toggleSidebar() {";
  html += "  const sidebar = document.getElementById('sidebar');";
  html += "  const overlay = document.getElementById('overlay');";
  html += "  sidebar.classList.toggle('open');";
  html += "  overlay.classList.toggle('show');";
  html += "}";
  html += "function checkUpdate() { fetch('/checkupdate', {method: 'POST'}).then(res => res.text()).then(text => alert(text)); }";
  html += "function githubOTA() { fetch('/githubota', {method: 'POST'}).then(res => res.text()).then(text => alert(text)); }";
  html += "</script></head>";
  html += "<body>";
  html += "<div class='container'>";
  html += "<div id='overlay' class='sidebar-overlay' onclick='toggleSidebar()'></div>";
  html += "<div id='sidebar' class='sidebar'>";
  html += "<div class='flex justify-between items-center p-4'>";
  html += "<h2 class='text-2xl font-bold text-white'>Автолоадер</h2>";
  html += "<button class='hamburger-btn bg-blue-600 text-white md:hidden' onclick='toggleSidebar()'>";
  html += "<span></span><span></span><span></span>";
  html += "</button>";
  html += "</div>";
  html += "<nav class='flex flex-col space-y-2 p-4'>";
  html += String("<a href='/?page=stats' class='sidebar-nav-btn bg-blue-600 text-white") + (page == "stats" ? " bg-blue-700" : "") + "'>";
  html += "<svg class='icon' viewBox='0 0 24 24'><path d='M5 4v16h4V4H5zm6 4v12h4V8h-4zm6 4v8h4v-8h-4z'/></svg>Статистика</a>";
  html += String("<a href='/?page=settings' class='sidebar-nav-btn bg-blue-600 text-white") + (page == "settings" ? " bg-blue-700" : "") + "'>";
  html += "<svg class='icon' viewBox='0 0 24 24'><path d='M19.14 12.94c.04-.3.06-.61.06-.94s-.02-.64-.06-.94l2.03-1.58a.49.49 0 0 0 .12-.61l-1.92-3.32a.49.49 0 0 0-.59-.22l-2.39.96c-.5-.38-1.03-.7-1.62-.94l-.36-2.54a.49.49 0 0 0-.48-.41h-3.84a.49.49 0 0 0-.48.41l-.36 2.54c-.59.24-1.13-.56-1.62.94l-2.39-.96a.49.49 0 0 0-.59.22l-1.92 3.32a.49.49 0 0 0 .12.61l2.03 1.58c-.04.3-.06.61-.06.94s.02.64.06.94l-2.03 1.58a.49.49 0 0 0-.12.61l1.92 3.32a.49.49 0 0 0 .59.22l2.39-.96c.5.38 1.03.7 1.62.94l.36 2.54a.49.49 0 0 0 .48.41h3.84a.49.49 0 0 0 .48-.41l.36-2.54c.59-.24 1.13-.56 1.62-.94l2.39.96a.49.49 0 0 0 .59-.22l1.92-3.32a.49.49 0 0 0-.12-.61l-2.03-1.58zM12 15.6c-1.98 0-3.6-1.62-3.6-3.6s1.62-3.6 3.6-3.6 3.6 1.62 3.6 3.6-1.62 3.6-3.6 3.6z'/></svg>Настройки</a>";
  html += String("<a href='/?page=config' class='sidebar-nav-btn bg-blue-600 text-white") + (page == "config" ? " bg-blue-700" : "") + "'>";
  html += "<svg class='icon' viewBox='0 0 24 24'><path d='M17.65 6.35A7.95 7.95 0 0 0 12 4c-4.42 0-7.99 3.58-7.99 8s3.57 8 7.99 8c3.73 0 6.84-2.55 7.73-6h-2.08A5.99 5.99 0 0 1 12 18c-3.31 0-6-2.69-6-6s2.69-6 6-6c1.66 0 3.14.69 4.22 1.78L13 11h7V4l-2.35 2.35z'/></svg>Конфигурация</a>";
  html += "<form action='/exitWithSave' method='POST'>";
  html += "<button type='submit' class='sidebar-nav-btn bg-yellow-600 text-white hover:bg-yellow-700 tooltip'>";
  html += "<svg class='icon' viewBox='0 0 24 24'><path d='M17 3H7c-1.1 0-2 .9-2 2v14c0 1.1.9 2 2 2h10c1.1 0 2-.9 2-2V5c0-1.1-.9-2-2-2zm-5 16c-1.66 0-3-1.34-3-3s1.34-3 3-3 3 1.34 3 3-1.34 3-3 3zm3-10H9V7h6v2z'/></svg>Выход";
  html += "<span class='tooltip-text'>Сохранить и перезагрузить</span></button>";
  html += "</form>";
  html += "</nav>";
  html += "</div>";
  html += "<div class='main-content'>";
  html += "<button class='hamburger-btn bg-blue-600 text-white mb-4 md:hidden' onclick='toggleSidebar()'>";
  html += "<span></span><span></span><span></span>";
  html += "</button>";
  html += "<h1 class='text-4xl md:text-4xl font-extrabold text-center text-white mb-8 break-words'>TEMPEST FEEDER</h1>";
  if (server.arg("updated") == "true") {
    html += "<p class='text-green-300 mb-4'>Настройки успешно сохранены!</p>";
  } else if (server.arg("updated") == "false") {
    html += "<p class='text-red-300 mb-4'>Ошибка сохранения настроек!</p>";
  }
  server.sendContent(html);
  html = "";

  if (page == "stats") {
    html += "<div id='stats' class='card'>";
    html += "<h2 class='text-2xl font-semibold text-blue-300 mb-4'>";
    html += "<svg class='icon' viewBox='0 0 24 24'><path d='M5 4v16h4V4H5zm6 4v12h4V8h-4zm6 4v8h4v-8h-4z'/></svg>Статистика</h2>";
    html += "<table class='w-full border-collapse text-gray-200'>";
    html += "<thead><tr class='bg-gray-700'><th class='border border-gray-600 p-3'>Название</th><th class='border border-gray-600 p-3'>Шаров в магазе</th><th class='border border-gray-600 p-3'>Просрали</th></tr></thead>";
    html += "<tbody>";
    for (int i = 0; i < numOptions; i++) {
      html += "<tr><td class='border border-gray-600 p-3'>" + String(ballOptions[i].name) + "</td><td class='border border-gray-600 p-3'>" + String(ballOptions[i].balls) + "</td><td class='border border-gray-600 p-3'>" + String(stats.counts[i]) + "</td></tr>";
    }
    html += "</tbody>";
    html += "<tfoot><tr class='bg-gray-600'><td class='border border-gray-600 p-3 font-semibold' colspan='2'>Общее количество шаров:</td><td class='border border-gray-600 p-3'>" + String(totalBalls) + "</td></tr></tfoot>";
    html += "</table>";
    html += "<form action='/reset' method='POST' class='mt-6'>";
    html += "<button type='submit' class='btn bg-red-600 text-white px-6 py-3 rounded-lg hover:bg-red-700 w-full tooltip'>";
    html += "<svg class='icon' viewBox='0 0 24 24'><path d='M6 19c0 1.1.9 2 2 2h8c1.1 0 2-.9 2-2V7H6v12zM19 4h-3.5l-1-1h-5l-1 1H5v2h14V4z'/></svg>Сбросить статистику";
    html += "<span class='tooltip-text'>Обнулить статистику</span></button>";
    html += "</form>";
    html += "</div>";
  } else if (page == "settings") {
    html += "<div id='presets' class='card'>";
    html += "<h2 class='text-2xl font-semibold text-blue-300 mb-4'>";
    html += "<svg class='icon' viewBox='0 0 24 24'><path d='M19.14 12.94c.04-.3.06-.61.06-.94s-.02-.64-.06-.94l2.03-1.58a.49.49 0 0 0 .12-.61l-1.92-3.32a.49.49 0 0 0-.59-.22l-2.39.96c-.5-.38-1.03-.7-1.62-.94l-.36-2.54a.49.49 0 0 0-.48-.41h-3.84a.49.49 0 0 0-.48.41l-.36 2.54c-.59.24-1.13-.56-1.62.94l-2.39-.96a.49.49 0 0 0-.59.22l-1.92 3.32a.49.49 0 0 0 .12.61l2.03 1.58c-.04.3-.06.61-.06.94s.02.64.06.94l-2.03 1.58a.49.49 0 0 0-.12.61l1.92 3.32a.49.49 0 0 0 .59.22l2.39-.96c.5.38 1.03.7 1.62.94l.36 2.54a.49.49 0 0 0 .48.41h3.84a.49.49 0 0 0 .48-.41l.36-2.54c.59-.24 1.13-.56 1.62-.94l2.39.96a.49.49 0 0 0 .59-.22l1.92-3.32a.49.49 0 0 0-.12-.61l-2.03-1.58zM12 15.6c-1.98 0-3.6-1.62-3.6-3.6s1.62-3.6 3.6-3.6 3.6 1.62 3.6 3.6-1.62 3.6-3.6 3.6z'/></svg>Настройки пресетов</h2>";
    html += "<form action='/update' method='POST' class='space-y-6'>";
    for (int i = 0; i < numOptions; i++) {
      html += "<div class='flex flex-col'>";
      html += "<label class='font-medium text-gray-300'>Пресет №" + String(i + 1) + "</label>";
      html += "<input type='text' name='name" + String(i) + "' value='" + String(ballOptions[i].name) + "' maxlength='10' class='mt-2 p-3 bg-gray-700 border border-gray-600 rounded-lg text-white focus:outline-none focus:ring-2 focus:ring-blue-500' placeholder='Название (макс. 10 символов)'>";
      html += "<label class='font-medium text-gray-300 mt-3'>Шары:</label>";
      html += "<input type='number' name='balls" + String(i) + "' value='" + String(ballOptions[i].balls) + "' min='10' max='500' class='mt-2 p-3 bg-gray-700 border border-gray-600 rounded-lg text-white focus:outline-none focus:ring-2 focus:ring-blue-500' placeholder='10-500'>";
      html += "</div>";
    }
    html += "<button type='submit' class='btn bg-blue-600 text-white px-6 py-3 rounded-lg hover:bg-blue-700 w-full tooltip'>";
    html += "<svg class='icon' viewBox='0 0 24 24'><path d='M17 3H7c-1.1 0-2 .9-2 2v14c0 1.1.9 2 2 2h10c1.1 0 2-.9 2-2V5c0-1.1-.9-2-2-2zm-5 16c-1.66 0-3-1.34-3-3s1.34-3 3-3 3 1.34 3 3-1.34 3-3 3zm3-10H9V7h6v2z'/></svg>Сохранить";
    html += "<span class='tooltip-text'>Сохранить настройки пресетов</span></button>";
    html += "</form>";
    html += "</div>";
    html += "<div id='calibration' class='card'>";
    html += "<h2 class='text-2xl font-semibold text-blue-300 mb-4'>";
    html += "<svg class='icon' viewBox='0 0 24 24'><path d='M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm0 18c-4.41 0-8-3.59-8-8s3.59-8 8-8 8 3.59 8 8-3.59 8-8 8zm-1-13h2v6h-2zm0 8h2v2h-2z'/></svg>Калибровка</h2>";
    html += "<p class='mb-3 text-gray-300'>Текущая скорость: " + String(calibration.ballsPerSecond, 1) + " шаров/сек</p>";
    html += "<form action='/calibrate' method='POST' class='space-y-4'>";
    html += "<button type='submit' name='start' value='1' class='btn bg-green-600 text-white px-6 py-3 rounded-lg hover:bg-green-700 w-full tooltip'>";
    html += "<svg class='icon' viewBox='0 0 24 24'><path d='M8 5v14l11-7L8 5z'/></svg>Запустить калибровку";
    html += "<span class='tooltip-text'>Запустить процесс калибровки</span></button>";
    html += "<div class='flex flex-col'>";
    html += "<label class='font-medium text-gray-300'>Шары за " + String(calibrationTime / 1000) + " сек:</label>";
    html += "<input type='number' name='balls' min='1' max='100' class='mt-2 p-3 bg-gray-700 border border-gray-600 rounded-lg text-white focus:outline-none focus:ring-2 focus:ring-blue-500' placeholder='1-100'>";
    html += "</div>";
    html += "<button type='submit' class='btn bg-blue-600 text-white px-6 py-3 rounded-lg hover:bg-blue-700 w-full tooltip'>";
    html += "<svg class='icon' viewBox='0 0 24 24'><path d='M17 3H7c-1.1 0-2 .9-2 2v14c0 1.1.9 2 2 2h10c1.1 0 2-.9 2-2V5c0-1.1-.9-2-2-2zm-5 16c-1.66 0-3-1.34-3-3s1.34-3 3-3 3 1.34 3 3-1.34 3-3 3zm3-10H9V7h6v2z'/></svg>Сохранить калибровку";
    html += "<span class='tooltip-text'>Сохранить введенные данные</span></button>";
    html += "</form>";
    html += "</div>";
    html += "<div id='relay' class='card'>";
    html += "<h2 class='text-2xl font-semibold text-blue-300 mb-4'>";
    html += "<svg class='icon' viewBox='0 0 24 24'><path d='M14.76 3.76a5 5 0 0 0-7.07 0L3 8.45l4.71 4.71 4.69-4.69a5 5 0 0 0 0-7.07zM12 10.41L9.41 13 8 11.59l2.59-2.59L12 7.59l-1.41 1.41z'/></svg>Тест подачи</h2>";
    html += "<form action='/testRelay' method='POST'>";
    html += "<button type='submit' class='btn bg-purple-600 text-white px-6 py-3 rounded-lg hover:bg-purple-700 w-full tooltip'>";
    html += "<svg class='icon' viewBox='0 0 24 24'><path d='M14.76 3.76a5 5 0 0 0-7.07 0L3 8.45l4.71 4.71 4.69-4.69a5 5 0 0 0 0-7.07zM12 10.41L9.41 13 8 11.59l2.59-2.59L12 7.59l-1.41 1.41z'/></svg>Тест подачи";
    html += "<span class='tooltip-text'>Проверить работу подачи</span></button>";
    html += "</form>";
    html += "</div>";
  } else if (page == "config") {
    html += "<p>Статус интернета: ";
    html += isHomeWifiConnected ? "<span style='color:green'>Подключено</span>" : "<span style='color:red'>Нет</span>";
    html += "</p>";
    html += "<div id='apconfig' class='card'>";
    html += "<h2 class='text-2xl font-semibold text-blue-300 mb-4'>";
    html += "<svg class='icon' viewBox='0 0 24 24'><path d='M17.65 6.35A7.95 7.95 0 0 0 12 4c-4.42 0-7.99 3.58-7.99 8s3.57 8 7.99 8c3.73 0 6.84-2.55 7.73-6h-2.08A5.99 5.99 0 0 1 12 18c-3.31 0-6-2.69-6-6s2.69-6 6-6c1.66 0 3.14.69 4.22 1.78L13 11h7V4l-2.35 2.35z'/></svg>Настройки точки доступа</h2>";
    html += "<form action='/apconfig' method='POST' class='space-y-4'>";
    html += "<div class='flex flex-col'>";
    html += "<label class='font-medium text-gray-300'>SSID:</label>";
    html += "<input type='text' name='ssid' value='" + String(ssid) + "' maxlength='32' class='mt-2 p-3 bg-gray-700 border border-gray-600 rounded-lg text-white focus:outline-none focus:ring-2 focus:ring-blue-500' placeholder='Название сети'>";
    html += "</div>";
    html += "<div class='flex flex-col'>";
    html += "<label class='font-medium text-gray-300'>Пароль:</label>";
    html += "<input type='text' name='password' value='" + String(password) + "' maxlength='32' class='mt-2 p-3 bg-gray-700 border border-gray-600 rounded-lg text-white focus:outline-none focus:ring-2 focus:ring-blue-500' placeholder='Пароль (мин. 8 символов)'>";
    html += "</div>";
    html += "<button type='submit' class='btn bg-blue-600 text-white px-6 py-3 rounded-lg hover:bg-blue-700 w-full tooltip'>";
    html += "<svg class='icon' viewBox='0 0 24 24'><path d='M17 3H7c-1.1 0-2 .9-2 2v14c0 1.1.9 2 2 2h10c1.1 0 2-.9 2-2V5c0-1.1-.9-2-2-2zm-5 16c-1.66 0-3-1.34-3-3s1.34-3 3-3 3 1.34 3 3-1.34 3-3 3zm3-10H9V7h6v2z'/></svg>Сохранить";
    html += "<span class='tooltip-text'>Сохранить настройки Wi-Fi</span></button>";
    html += "</form>";
    html += "</div>";
    html += "<div id='homewifi' class='card'>";
    html += "<h2 class='text-2xl font-semibold text-blue-300 mb-4'>";
    html += "<svg class='icon' viewBox='0 0 24 24'><path d='M12 1c-3.86 0-7.66 1.58-10.39 4.31L3 6.71c2.31-2.31 5.39-3.58 8.69-3.58 3.31 0 6.38 1.27 8.69 3.58l1.41-1.41C19.66 2.58 15.86 1 12 1zm3.54 7.54c-1.07-.64-2.27-1-3.54-1s-2.47.36-3.54 1L12 12l3.54-3.46z'/></svg>Домашняя WiFi</h2>";
    html += "<form action='/homewificonfig' method='POST' class='space-y-4'>";
    html += "<div class='flex flex-col'>";
    html += "<label class='font-medium text-gray-300'>SSID:</label>";
    html += "<input type='text' name='homessid' value='" + String(homeSsid) + "' maxlength='32' class='mt-2 p-3 bg-gray-700 border border-gray-600 rounded-lg text-white focus:outline-none focus:ring-2 focus:ring-blue-500' placeholder='Название сети'>";
    html += "</div>";
    html += "<div class='flex flex-col'>";
    html += "<label class='font-medium text-gray-300'>Пароль:</label>";
    html += "<input type='text' name='homepassword' value='" + String(homePassword) + "' maxlength='32' class='mt-2 p-3 bg-gray-700 border border-gray-600 rounded-lg text-white focus:outline-none focus:ring-2 focus:ring-blue-500' placeholder='Пароль (мин. 8 символов)'>";
    html += "</div>";
    html += "<button type='submit' class='btn bg-blue-600 text-white px-6 py-3 rounded-lg hover:bg-blue-700 w-full tooltip'>";
    html += "<svg class='icon' viewBox='0 0 24 24'><path d='M17 3H7c-1.1 0-2 .9-2 2v14c0 1.1.9 2 2 2h10c1.1 0 2-.9 2-2V5c0-1.1-.9-2-2-2zm-5 16c-1.66 0-3-1.34-3-3s1.34-3 3-3 3 1.34 3 3-1.34 3-3 3zm3-10H9V7h6v2z'/></svg>Сохранить";
    html += "<span class='tooltip-text'>Сохранить настройки домашней Wi-Fi</span></button>";
    html += "</form>";
    html += "</div>";
    html += "<div id='update' class='card'>";
    html += "<h2 class='text-2xl font-semibold text-blue-300 mb-4'>";
    html += "<svg class='icon' viewBox='0 0 24 24'><path d='M12 4V1L8 5l4 4V6c3.31 0 6 2.69 6 6 0 1.01-.25 1.97-.68 2.82l1.46 1.46C19.54 15.03 20 13.57 20 12c0-4.41-3.59-8-8-8zm0 16c-3.31 0-6-2.69-6-6 0-1.01.25-1.97.68-2.82L5.22 9.72C4.46 10.97 4 11.43 4 12c0 4.41 3.59 8 8 8v3l4-4-4-4v3z'/></svg>Обновление прошивки</h2>";
    html += "<button onclick='checkUpdate()' class='btn bg-orange-600 text-white px-6 py-3 rounded-lg hover:bg-orange-700 w-full tooltip'>";
    html += "<svg class='icon' viewBox='0 0 24 24'><path d='M12 4V1L8 5l4 4V6c3.31 0 6 2.69 6 6 0 1.01-.25 1.97-.68 2.82l1.46 1.46C19.54 15.03 20 13.57 20 12c0-4.41-3.59-8-8-8zm0 16c-3.31 0-6-2.69-6-6 0-1.01.25-1.97.68-2.82L5.22 9.72C4.46 10.97 4 11.43 4 12c0 4.41 3.59 8 8 8v3l4-4-4-4v3z'/></svg>Проверить обновления";
    html += "<span class='tooltip-text'>Проверить наличие новой версии</span></button>";
    html += "<form action='/ota' method='POST' class='mt-4'>";
    html += "<button type='submit' class='btn bg-purple-600 text-white px-6 py-3 rounded-lg hover:bg-purple-700 w-full tooltip'>";
    html += "<svg class='icon' viewBox='0 0 24 24'><path d='M12 4V1L8 5l4 4V6c3.31 0 6 2.69 6 6 0 1.01-.25 1.97-.68 2.82l1.46 1.46C19.54 15.03 20 13.57 20 12c0-4.41-3.59-8-8-8zm0 16c-3.31 0-6-2.69-6-6 0-1.01.25-1.97.68-2.82L5.22 9.72C4.46 10.97 4 11.43 4 12c0 4.41 3.59 8 8 8v3l4-4-4-4v3z'/></svg>Ручное OTA";
    html += "<span class='tooltip-text'>Запустить OTA обновление</span></button>";
    html += "</form>";
    html += "<button onclick='githubOTA()' class='btn bg-purple-600 text-white px-6 py-3 rounded-lg hover:bg-purple-700 w-full mt-4 tooltip'>";
    html += "<svg class='icon' viewBox='0 0 24 24'><path d='M12 4V1L8 5l4 4V6c3.31 0 6 2.69 6 6 0 1.01-.25 1.97-.68 2.82l1.46 1.46C19.54 15.03 20 13.57 20 12c0-4.41-3.59-8-8-8zm0 16c-3.31 0-6-2.69-6-6 0-1.01.25-1.97.68-2.82L5.22 9.72C4.46 10.97 4 11.43 4 12c0 4.41 3.59 8 8 8v3l4-4-4-4v3z'/></svg>GitHub OTA";
    html += "<span class='tooltip-text'>Обновление с GitHub</span></button>";
    html += "</div>";
    html += "</div>";
    html += "</div>";
    html += "<div class='footer'>";
    html += "<p>Версия прошивки: " + String(VERSION) + " | <a href='https://github.com/Ko1hozer/tempest_feeder_autoloader_airsoft'>GitHub</a></p>";
    html += "</div>";
    html += "</div>";
    html += "</body></html>";
    server.sendContent(html);
    server.sendContent("");
    Serial.println("handleRoot finished");
  }
}

void handleUpdate() {
    Serial.println("handleUpdate started");
    bool updated = true;
    for (int i = 0; i < numOptions; i++) {
        String nameKey = "name" + String(i);
        String ballsKey = "balls" + String(i);
        if (server.hasArg(nameKey) && server.hasArg(ballsKey)) {
            String newName = server.arg(nameKey);
            int newBalls = server.arg(ballsKey).toInt();
            if (newName.length() > 0 && newName.length() <= 10 && newBalls >= 10 && newBalls <= 500) {
                strncpy(ballOptions[i].name, newName.c_str(), 21);
                ballOptions[i].name[20] = '\0';
                ballOptions[i].balls = newBalls;
                Serial.println("Updated preset " + String(i) + ": name=" + String(ballOptions[i].name) + ", balls=" + String(ballOptions[i].balls));
            } else {
                updated = false;
                Serial.println("Invalid input for preset " + String(i));
            }
        } else {
            updated = false;
            Serial.println("Missing arguments for preset " + String(i));
        }
    }
    if (updated) {
        EEPROM.put(eepromAddrPresets, ballOptions);
        if (EEPROM.commit()) {
            Serial.println("Presets saved to EEPROM");
        } else {
            Serial.println("EEPROM commit failed");
            updated = false;
        }
    }
    server.sendHeader("Location", "/?page=settings&updated=" + String(updated ? "true" : "false"));
    server.send(302);
    Serial.println("handleUpdate finished");
}

void handleReset() {
    Serial.println("handleReset started");
    for (int i = 0; i < numOptions; i++) {
        stats.counts[i] = 0;
    }
    EEPROM.put(eepromAddrStats, stats);
    if (EEPROM.commit()) {
        Serial.println("Stats reset and saved to EEPROM");
    } else {
        Serial.println("EEPROM commit failed");
    }
    server.sendHeader("Location", "/?page=stats");
    server.send(302);
    Serial.println("handleReset finished");
}

void handleClear() {
    Serial.println("handleClear started");
    for (int i = 0; i < numOptions; i++) {
        strcpy(ballOptions[i].name, i == 0 ? "МКа ув" : i == 1 ? "МКа" : "Пекаль ув");
        ballOptions[i].balls = i == 0 ? 160 : i == 1 ? 120 : i == 2 ? 46 : 20;
        stats.counts[i] = 0;
    }
    calibration.ballsPerSecond = defaultBallsPerSecond;
    EEPROM.put(eepromAddrPresets, ballOptions);
    EEPROM.put(eepromAddrStats, stats);
    EEPROM.put(eepromAddrCalibration, calibration);
    if (EEPROM.commit()) {
        Serial.println("All settings reset and saved to EEPROM");
    } else {
        Serial.println("EEPROM commit failed");
    }
    server.sendHeader("Location", "/?page=stats");
    server.send(302);
    Serial.println("handleClear finished");
}

void handleCalibrate() {
    Serial.println("handleCalibrate started");
    if (server.hasArg("start") && server.arg("start") == "1") {
        isCalibrating = true;
        calibrationStartTime = millis();
        server.sendHeader("Location", "/?page=settings");
        server.send(302);
        Serial.println("Calibration started");
    } else if (server.hasArg("balls")) {
        int balls = server.arg("balls").toInt();
        if (balls >= 1 && balls <= 100) {
            calibration.ballsPerSecond = (float)balls / (calibrationTime / 1000.0);
            EEPROM.put(eepromAddrCalibration, calibration);
            if (EEPROM.commit()) {
                Serial.println("Calibration saved: " + String(calibration.ballsPerSecond) + " balls/sec");
            } else {
                Serial.println("EEPROM commit failed");
            }
            isCalibrating = false;
            calibrationStartTime = 0;
            server.sendHeader("Location", "/?page=settings&updated=true");
            server.send(302);
        } else {
            server.sendHeader("Location", "/?page=settings&updated=false");
            server.send(302);
            Serial.println("Invalid calibration input");
        }
    } else {
        server.sendHeader("Location", "/?page=settings&updated=false");
        server.send(302);
        Serial.println("Missing calibration arguments");
    }
    if (displayAvailable) {
        displayMenu();
    }
    Serial.println("handleCalibrate finished");
}

void handleTestRelay() {
    Serial.println("handleTestRelay started");
    digitalWrite(RELAY_PIN, HIGH);
    delay(1000);
    digitalWrite(RELAY_PIN, LOW);
    server.sendHeader("Location", "/?page=settings");
    server.send(302);
    Serial.println("handleTestRelay finished");
}

void handleOTA() {
    Serial.println("handleOTA started");
    startOTA();
    server.sendHeader("Location", "/?page=config");
    server.send(302);
    Serial.println("handleOTA finished");
}

void handleGithubOTA() {
    Serial.println("handleGithubOTA started");
    if (!isHomeWifiConnected) {
        server.send(200, "text/plain", "Нет подключения к интернету");
        Serial.println("No internet connection for GitHub OTA");
        return;
    }
    WiFiClientSecure client;
    client.setInsecure();
    t_httpUpdate_return ret = ESPhttpUpdate.update(client, GITHUB_FIRMWARE_URL);
    switch (ret) {
        case HTTP_UPDATE_FAILED:
            server.send(200, "text/plain", "Ошибка обновления: " + String(ESPhttpUpdate.getLastErrorString()));
            Serial.println("Update failed: " + String(ESPhttpUpdate.getLastErrorString()));
            break;
        case HTTP_UPDATE_NO_UPDATES:
            server.send(200, "text/plain", "Обновления не найдены");
            Serial.println("No updates found");
            break;
        case HTTP_UPDATE_OK:
            server.send(200, "text/plain", "Обновление успешно, перезагрузка...");
            Serial.println("Update successful, restarting");
            ESP.restart();
            break;
    }
    Serial.println("handleGithubOTA finished");
}

void handleCheckUpdate() {
    Serial.println("handleCheckUpdate started");
    if (!isHomeWifiConnected) {
        server.send(200, "text/plain", "Нет подключения к интернету");
        Serial.println("No internet connection for update check");
        return;
    }
    WiFiClientSecure client;
    client.setInsecure();
    if (!client.connect(GITHUB_VERSION_URL, 443)) {
        server.send(200, "text/plain", "Не удалось подключиться к серверу");
        Serial.println("Failed to connect to version server");
        return;
    }
    client.print(String("GET ") + GITHUB_VERSION_URL + " HTTP/1.1\r\n" +
                 "Host: raw.githubusercontent.com\r\n" +
                 "User-Agent: ESP8266\r\n" +
                 "Connection: close\r\n\r\n");
    while (client.connected() && !client.available()) {
        delay(10);
    }
    String remoteVersion = "";
    while (client.available()) {
        String line = client.readStringUntil('\n');
        if (line.startsWith("HTTP/1.1 200")) {
            while (client.available()) {
                remoteVersion = client.readStringUntil('\n');
                remoteVersion.trim();
                if (remoteVersion.length() > 0) break;
            }
            break;
        }
    }
    client.stop();
    if (remoteVersion.length() == 0) {
        server.send(200, "text/plain", "Не удалось получить версию");
        Serial.println("Failed to retrieve version");
        return;
    }
    if (remoteVersion > VERSION) {
        server.send(200, "text/plain", "Доступна новая версия: " + remoteVersion);
        Serial.println("New version available: " + remoteVersion);
    } else {
        server.send(200, "text/plain", "У вас последняя версия: " + String(VERSION));
        Serial.println("Current version is up to date: " + String(VERSION));
    }
    Serial.println("handleCheckUpdate finished");
}

void handleExit() {
    Serial.println("handleExit started");
    exitConfigMode();
    server.sendHeader("Location", "/");
    server.send(302);
    Serial.println("handleExit finished");
}

void handleExitWithSave() {
    Serial.println("handleExitWithSave started");
    saveAndRestart();
    server.sendHeader("Location", "/");
    server.send(302);
    Serial.println("handleExitWithSave finished");
}

void handleAPConfig() {
    Serial.println("handleAPConfig started");
    bool updated = false;
    if (server.hasArg("ssid") && server.hasArg("password")) {
        String newSSID = server.arg("ssid");
        String newPassword = server.arg("password");
        if (newSSID.length() > 0 && newSSID.length() <= 32 && newPassword.length() >= 8 && newPassword.length() <= 32) {
            strncpy(ssid, newSSID.c_str(), 33);
            strncpy(password, newPassword.c_str(), 33);
            EEPROM.put(eepromAddrAPConfig, ssid);
            EEPROM.put(eepromAddrAPConfig + 33, password);
            if (EEPROM.commit()) {
                updated = true;
                Serial.println("AP settings saved: SSID=" + newSSID + ", Password=" + newPassword);
            } else {
                Serial.println("EEPROM commit failed");
            }
        } else {
            Serial.println("Invalid AP settings");
        }
    }
    server.sendHeader("Location", "/?page=config&updated=" + String(updated ? "true" : "false"));
    server.send(302);
    Serial.println("handleAPConfig finished");
}

void handleHomeWifiConfig() {
    Serial.println("handleHomeWifiConfig started");
    bool updated = false;
    if (server.hasArg("homessid") && server.hasArg("homepassword")) {
        String newHomeSsid = server.arg("homessid");
        String newHomePassword = server.arg("homepassword");
        if (newHomeSsid.length() <= 32 && newHomePassword.length() <= 32) {
            strncpy(homeSsid, newHomeSsid.c_str(), 33);
            strncpy(homePassword, newHomePassword.c_str(), 33);
            EEPROM.put(eepromAddrHomeWifi, homeSsid);
            EEPROM.put(eepromAddrHomeWifi + 33, homePassword);
            if (EEPROM.commit()) {
                updated = true;
                Serial.println("Home WiFi settings saved: SSID=" + newHomeSsid);
            } else {
                Serial.println("EEPROM commit failed");
            }
        } else {
            Serial.println("Invalid home WiFi settings");
        }
    }
    server.sendHeader("Location", "/?page=config&updated=" + String(updated ? "true" : "false"));
    server.send(302);
    Serial.println("handleHomeWifiConfig finished");
}

void enterConfigMode() {
    isConfigMode = true;
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(ssid, password);
    dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
    server.begin();
    Serial.println("Config mode: AP_STA enabled. AP IP: " + WiFi.softAPIP().toString());

    isHomeWifiConnected = false;
    if (strlen(homeSsid) > 0 && strlen(homePassword) > 0) {
        WiFi.begin(homeSsid, homePassword);
        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
            delay(500);
            Serial.print(".");
        }
        if (WiFi.status() == WL_CONNECTED) {
            isHomeWifiConnected = true;
            Serial.println("Connected to home WiFi. STA IP: " + WiFi.localIP().toString());
            if (Ping.ping(IPAddress(8, 8, 8, 8))) {
                Serial.println("Internet available");
            }
        } else {
            Serial.println("Failed to connect to home WiFi");
        }
    } else {
        Serial.println("No home WiFi settings");
    }

    if (displayAvailable) {
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_8x13_t_cyrillic);
        u8g2.setCursor(5, 10);
        u8g2.print("Режим настройки");
        u8g2.setCursor(0, 30);
        u8g2.print("AP: " + String(ssid));
        u8g2.setCursor(0, 45);
        if (isHomeWifiConnected) {
            u8g2.print("STA: Connected");
            u8g2.setCursor(0, 60);
            u8g2.print(WiFi.localIP().toString());
        } else {
            u8g2.print("STA: No connect");
        }
        u8g2.sendBuffer();
    }
    Serial.println("Config mode displayed. AP: " + String(ssid) + ", STA: " + (isHomeWifiConnected ? "Connected, IP: " + WiFi.localIP().toString() : "No connect"));
}

void exitConfigMode() {
    Serial.println("exitConfigMode started");
    isConfigMode = false;
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ssid, password);
    dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
    server.begin();
    if (displayAvailable) {
        displayMenu();
    }
    Serial.println("exitConfigMode finished. AP mode restored: " + WiFi.softAPIP().toString());
}

void startOTA() {
    Serial.println("startOTA started");
    isOTAMode = true;
    calibrationStartTime = millis();
    ArduinoOTA.setHostname("Autoloader");
    ArduinoOTA.begin();
    if (displayAvailable) {
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_8x13_t_cyrillic);
        u8g2.setCursor(0, 10);
        u8g2.print("OTA режим");
        u8g2.setCursor(0, 30);
        u8g2.print("IP: ");
        u8g2.print(WiFi.localIP().toString());
        u8g2.sendBuffer();
    }
    Serial.println("OTA mode enabled. IP: " + WiFi.localIP().toString());
}

void saveAndRestart() {
    Serial.println("saveAndRestart started");
    EEPROM.commit();
    isConfigMode = false;
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ssid, password);
    dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
    server.begin();
    if (displayAvailable) {
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_8x13_t_cyrillic);
        u8g2.setCursor(0, 10);
        u8g2.print("Перезагрузка...");
        u8g2.sendBuffer();
    }
    Serial.println("Settings saved, restarting...");
    delay(1000);
    ESP.restart();
}
