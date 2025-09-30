#include <Wire.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <EEPROM.h>
#include <Preferences.h>
#include <Adafruit_NeoPixel.h>
#include <string.h>

// ==================== KONSTANTOS ====================
#define BUTTON_PIN 0
#define LED_PIN 35
#define VIBRO_PIN 19
#define NEOPIXEL_PIN 38

#define OLED_SDA 17
#define OLED_SCL 18
#define OLED_RST 21
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C

#define FAST_SCAN_INTERVAL 500
#define SLOW_SCAN_INTERVAL 2500
#define NETWORKS_PER_PAGE 4
#define LONG_PRESS_TIME 1000
#define VERY_LONG_PRESS_TIME 3000
#define SUPER_LONG_PRESS_TIME 5000
#define MULTI_CLICK_WINDOW 400
#define BLINK_INTERVAL 300
#define DEBUG false

#define MAX_SAVED_BSSIDS 100
#define MAX_HIDDEN_NETWORKS 12

#define VIBRO_DURATION 100
#define VIBRO_PAUSE 100
#define VIBRO_TARGET_COUNT 2
#define VIBRO_NEW_COUNT 10
#define VIBRO_LONG_UNSEEN_COUNT 5
#define VIBRO_REENTER_COUNT 1
#define VIBRO_COOLDOWN 3600000 // 60 minučių

#define BATTERY_PIN 1
#define BATTERY_MIN 2500
#define BATTERY_MAX 4200

// ==================== TIPAI IR OBJEKTAI ====================
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RST);
Preferences preferences;
Adafruit_NeoPixel pixel(1, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

struct Target {
  const char* bssid;
  uint32_t color;
  const char* name;
};

Target targets[] = {
  { "B8:F8:62:04:7E:05", pixel.Color(255, 0, 0),   "RAUDONAS" },
  { "B8:F8:62:04:66:8D", pixel.Color(0, 255, 0),   "ZALIAS"   },
  { "B8:F8:62:04:64:29", pixel.Color(0, 0, 255),   "MELYNAS"  },
  { "7C:2C:67:D4:0F:C1", pixel.Color(255, 255, 0), "GELTONAS" },
  { "B8:F8:62:04:62:91", pixel.Color(255, 0, 255), "VIOLETINIS" }
};

enum DeviceState { ACTIVE, DISPLAY_OFF, POWERING_OFF };
enum MenuState { SCANNING, MENU_MAIN, SAVED_LIST, DELETE_CONFIRM };

struct HiddenNetwork {
  String bssid;
  int rssi;
  bool isNew;
  bool isTarget;
  String targetName;
};

// ==================== GLOBALŪS KINTAMIEJI ====================
DeviceState deviceState = ACTIVE;
MenuState currentMenu = SCANNING;
int buttonState = HIGH, lastButtonState = HIGH;
unsigned long lastDebounceTime = 0, debounceDelay = 50;
unsigned long pressStartTime = 0, lastClickTime = 0;
int pendingClicks = 0;
bool buttonPressed = false, longPressDetected = false;
unsigned long lastScanTime = 0;
unsigned long currentScanInterval = SLOW_SCAN_INTERVAL;
int hiddenNetworksFound = 0, currentPage = 0;
bool scanning = false;
HiddenNetwork hiddenNetworks[MAX_HIDDEN_NETWORKS];
String previouslySeenBssids[MAX_HIDDEN_NETWORKS];
int previouslySeenCount = 0;
int menuSelection = 0, deleteSelection = 1;
int savedBssidCount = 0;
int currentSavedPage = 0;
unsigned long infoScreenStartTime = 0;
bool showingInfoScreen = false;
uint32_t currentTargetColor = 0;
unsigned long lastBlinkTime = 0;
bool targetLedState = false;

int vibration_count_remaining = 0;
unsigned long last_vibration_time = 0;
bool vibrator_on = false;

// ==================== FUNKCIJŲ PROTOTIPAI ====================
void handleAsyncVibration();
void scanHiddenNetworks(int n);
void updateDisplay();
void showMainMenu();
void showSavedList();
void showDeleteConfirmation();
void showTemporaryMessage(String msg, int duration);
float getBatteryVoltage();
int getBatteryPercentage();
void handleButton();
void handleShortClick();
void handleDoubleClick();
void handleLongPress();
void handleVeryLongPress();
void handleActiveState();
void handleDisplayOffState();
void setDeviceState(DeviceState newState);
void displaySleep();
void displayWake();
int findBssidIndex(String bssid);
unsigned long getBssidTimestamp(int index);
void updateBssidTimestamp(int index);
void saveNewBssid(String bssid);
void deleteAllBSSIDs();
String formatMacAddress(String bssid);
void powerDown();
void vibrate(int times);
void handleVibration(const HiddenNetwork& network);
void displayBatteryIcon(int x, int y);

// ==================== SISTEMA IR INICIALIZACIJA ====================
void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  pinMode(VIBRO_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  digitalWrite(VIBRO_PIN, LOW);

  pixel.begin();
  pixel.clear();
  pixel.show();

  analogReadResolution(12);
  #if defined(ESP32)
    analogSetAttenuation(ADC_11db);
  #endif

  pinMode(OLED_RST, OUTPUT);
  digitalWrite(OLED_RST, LOW); delay(20); digitalWrite(OLED_RST, HIGH);
  Wire.begin(OLED_SDA, OLED_SCL);
  if(!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) { for(;;); }

  display.setTextColor(WHITE);

  preferences.begin("wifi_scanner", false);
  savedBssidCount = preferences.getUInt("count", 0);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  #if CONFIG_IDF_TARGET_ESP32S3
    esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_max_tx_power(84);
  #endif

  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(25, 10);
  display.println("Skeneris");
  display.setTextSize(1);
  display.setCursor(30, 40);
  display.println("Heltec V3");
  display.display();
  delay(1500);

  currentMenu = SCANNING;
  updateDisplay();
}

void loop() {
  if (deviceState == POWERING_OFF) { powerDown(); return; }
  handleButton();
  handleAsyncVibration();

  if (currentTargetColor != 0) {
    if (millis() - lastBlinkTime > BLINK_INTERVAL) {
      lastBlinkTime = millis();
      targetLedState = !targetLedState;
      pixel.setPixelColor(0, targetLedState ? currentTargetColor : 0);
      pixel.show();
    }
  } else if (targetLedState) {
    pixel.clear();
    pixel.show();
    targetLedState = false;
  }

  if (showingInfoScreen) {
    if (millis() - infoScreenStartTime > 2000) {
      showingInfoScreen = false;
      updateDisplay();
    }
    return;
  }

  if (scanning) {
    int scanResult = WiFi.scanComplete();
    if (scanResult >= 0) {
      scanning = false;
      scanHiddenNetworks(scanResult);
    }
  } else {
    switch (deviceState) {
      case ACTIVE: handleActiveState(); break;
      case DISPLAY_OFF: handleDisplayOffState(); break;
    }
  }
  delay(10);
}

// ==================== SKENERIO VALDYMAS ====================
void scanHiddenNetworks(int n) {
  hiddenNetworksFound = 0;
  currentTargetColor = 0;

  if (n > 0) {
    for (int i = 0; i < n && hiddenNetworksFound < MAX_HIDDEN_NETWORKS; i++) {
      if (WiFi.SSID(i).length() == 0) {
        String bssid = WiFi.BSSIDstr(i);

        hiddenNetworks[hiddenNetworksFound].bssid = bssid;
        hiddenNetworks[hiddenNetworksFound].rssi = WiFi.RSSI(i);
        hiddenNetworks[hiddenNetworksFound].isNew = (findBssidIndex(bssid) == -1);
        hiddenNetworks[hiddenNetworksFound].isTarget = false;
        hiddenNetworks[hiddenNetworksFound].targetName = "";

        for (const auto& target : targets) {
          if (bssid.equalsIgnoreCase(target.bssid)) {
            currentTargetColor = target.color;
            hiddenNetworks[hiddenNetworksFound].isTarget = true;
            hiddenNetworks[hiddenNetworksFound].targetName = target.name;
            break;
          }
        }

        handleVibration(hiddenNetworks[hiddenNetworksFound]);

        hiddenNetworksFound++;
      }
    }
    currentScanInterval = FAST_SCAN_INTERVAL;
  } else {
    currentScanInterval = SLOW_SCAN_INTERVAL;
  }

  WiFi.scanDelete();

  if (deviceState == ACTIVE) {
    if (currentPage * NETWORKS_PER_PAGE >= hiddenNetworksFound) {
      currentPage = 0;
    }
    updateDisplay();
  }

  previouslySeenCount = hiddenNetworksFound;
  for(int i=0; i<hiddenNetworksFound; i++){
    previouslySeenBssids[i] = hiddenNetworks[i].bssid;
  }
}

// ==================== EKRANO VALDYMAS ====================
void updateDisplay() {
  if (deviceState != ACTIVE || showingInfoScreen) return;

  switch(currentMenu) {
    case SCANNING:
      display.clearDisplay();
      display.setTextColor(WHITE);
      display.setTextSize(1);

      display.setCursor(0, 0);
      display.print("A:" + String(hiddenNetworksFound) + " ");
      display.print((currentScanInterval == FAST_SCAN_INTERVAL) ? "G:" : "L:");
      display.print(String(currentScanInterval));

      displayBatteryIcon(100, 0);

      display.setCursor(0, 9);
      display.println("-------------------");

      if (hiddenNetworksFound > 0) {
        int startIdx = currentPage * NETWORKS_PER_PAGE;
        int endIdx = min(startIdx + NETWORKS_PER_PAGE, hiddenNetworksFound);

        for (int i = startIdx; i < endIdx; i++) {
          int rssi = hiddenNetworks[i].rssi;
          display.setCursor(0, 18 + (i - startIdx) * 12);

          if (hiddenNetworks[i].isTarget) {
            display.setTextColor(BLACK, WHITE);
            String targetText = hiddenNetworks[i].targetName + " (" + String(rssi) + ")";
            display.print(targetText);
            display.setTextColor(WHITE, BLACK);
          } else {
            String mac = formatMacAddress(hiddenNetworks[i].bssid);
            display.print(mac + " (" + String(rssi) + ")");
            if (hiddenNetworks[i].isNew) {
              display.print(" *");
            }
          }
        }

        if (hiddenNetworksFound > NETWORKS_PER_PAGE) {
            int totalPages = (hiddenNetworksFound + NETWORKS_PER_PAGE - 1) / NETWORKS_PER_PAGE;
            String pageInfo = String(currentPage + 1) + "/" + String(totalPages);
            display.setCursor(SCREEN_WIDTH - (pageInfo.length() * 6), 56);
            display.print(pageInfo);
        }
      } else {
        display.setCursor(20, 35);
        display.print("Ieskomi tinklai...");
      }
      display.display();
      break;

    case MENU_MAIN: showMainMenu(); break;
    case SAVED_LIST: showSavedList(); break;
    case DELETE_CONFIRM: showDeleteConfirmation(); break;
  }
}

// ==================== LIKĘS KODAS ====================
void showMainMenu() {
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("== MENIU ==");
  display.println("-------------------");
  const char* menuItems[] = {"SARASAS", "ISTRINTI VISUS", "SKEN. GREITIS", "GRIZTI"};
  for (int i = 0; i < 4; i++) {
    display.setCursor(10, 18 + i * 12);
    display.print((i == menuSelection) ? "> " : "  ");
    display.println(menuItems[i]);
  }
  display.display();
}

void showSavedList() {
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Isaugota: " + String(savedBssidCount));
  display.println("-------------------");

  if (savedBssidCount > 0) {
    int startIdx = currentSavedPage * NETWORKS_PER_PAGE;
    int endIdx = min(startIdx + NETWORKS_PER_PAGE, savedBssidCount);
    for (int i = startIdx; i < endIdx; i++) {
      String bssid = preferences.getString(("bssid_" + String(i)).c_str(), "");
      display.setCursor(0, 18 + (i - startIdx) * 12);
      display.println(String(i + 1) + ". " + bssid);
    }
    if (savedBssidCount > NETWORKS_PER_PAGE) {
      int totalPages = (savedBssidCount + NETWORKS_PER_PAGE - 1) / NETWORKS_PER_PAGE;
      String pageInfo = "Psl " + String(currentSavedPage + 1) + "/" + String(totalPages);
      display.setCursor(SCREEN_WIDTH - (pageInfo.length() * 6), 56);
      display.print(pageInfo);
    }
  } else {
    display.setCursor(10, 35);
    display.println("Sarasas tuscias");
  }
  display.display();
}

void showDeleteConfirmation() {
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(0, 8);
  display.println("Istrinti visus " + String(savedBssidCount));
  display.println("isaug. adresus?");
  display.setTextSize(2);
  display.setCursor(15, 40);
  display.print((deleteSelection == 0) ? "> TAIP" : "  TAIP");
  display.setCursor(75, 40);
  display.print((deleteSelection == 1) ? "> NE" : "  NE");
  display.display();
}

void showTemporaryMessage(String msg, int duration) {
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(2);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(msg, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, (SCREEN_HEIGHT - h) / 2);
  display.println(msg);
  display.display();
  delay(duration);
}

float getBatteryVoltage() {
  uint16_t v = analogRead(BATTERY_PIN);
  return ((float)v / 4095.0) * 2.0 * 3.3 * 1.1;
}

int getBatteryPercentage() {
  float voltage = getBatteryVoltage() * 1000;
  if (voltage >= BATTERY_MAX) return 100;
  if (voltage <= BATTERY_MIN) return 0;
  return (int)((voltage - BATTERY_MIN) / (BATTERY_MAX - BATTERY_MIN) * 100);
}

void handleButton() {
  int reading = digitalRead(BUTTON_PIN);
  unsigned long currentTime = millis();
  if (reading != lastButtonState) {
    lastDebounceTime = currentTime;
  }
  if ((currentTime - lastDebounceTime) > debounceDelay) {
    if (reading != buttonState) {
      buttonState = reading;
      if (buttonState == LOW) {
        buttonPressed = true;
        longPressDetected = false;
        pressStartTime = currentTime;
        digitalWrite(LED_PIN, HIGH);
      } else {
        buttonPressed = false;
        digitalWrite(LED_PIN, LOW);
        unsigned long pressDuration = currentTime - pressStartTime;
        if (pressDuration < LONG_PRESS_TIME) {
          pendingClicks++;
          lastClickTime = currentTime;
        }
      }
    }
  }
  if (buttonPressed && !longPressDetected) {
      unsigned long pressDuration = currentTime - pressStartTime;
      if (pressDuration > SUPER_LONG_PRESS_TIME) { longPressDetected = true; powerDown(); }
      else if (pressDuration > VERY_LONG_PRESS_TIME) { longPressDetected = true; handleVeryLongPress(); }
      else if (pressDuration > LONG_PRESS_TIME) { longPressDetected = true; handleLongPress(); }
  }
  if (pendingClicks > 0 && !buttonPressed && (currentTime - lastClickTime) > MULTI_CLICK_WINDOW) {
    if (pendingClicks == 1) handleShortClick();
    else if (pendingClicks == 2) handleDoubleClick();
    pendingClicks = 0;
  }
  lastButtonState = reading;
}

void handleShortClick() {
  if (deviceState == DISPLAY_OFF) { setDeviceState(ACTIVE); return; }
  switch (currentMenu) {
    case SCANNING: if (hiddenNetworksFound > NETWORKS_PER_PAGE) { currentPage = (currentPage + 1) % ((hiddenNetworksFound + NETWORKS_PER_PAGE - 1) / NETWORKS_PER_PAGE); } break;
    case MENU_MAIN: menuSelection = (menuSelection + 1) % 4; break;
    case SAVED_LIST: if (savedBssidCount > NETWORKS_PER_PAGE) { currentSavedPage = (currentSavedPage + 1) % ((savedBssidCount + NETWORKS_PER_PAGE - 1) / NETWORKS_PER_PAGE); } break;
    case DELETE_CONFIRM: deleteSelection = (deleteSelection + 1) % 2; break;
  }
  updateDisplay();
}

void handleDoubleClick() {
  if (deviceState == DISPLAY_OFF) { setDeviceState(ACTIVE); return; }
  switch (currentMenu) {
    case SAVED_LIST:
    case DELETE_CONFIRM: currentMenu = MENU_MAIN; menuSelection = 0; break;
    case MENU_MAIN: currentMenu = SCANNING; break;
  }
  updateDisplay();
}

void handleLongPress() {
  if (deviceState == DISPLAY_OFF) { setDeviceState(ACTIVE); return; }
  switch (currentMenu) {
    case SCANNING: currentMenu = MENU_MAIN; menuSelection = 0; break;
    case MENU_MAIN:
      switch (menuSelection) {
        case 0: currentMenu = SAVED_LIST; currentSavedPage = 0; break;
        case 1: currentMenu = DELETE_CONFIRM; deleteSelection = 1; break;
        case 2:
          currentScanInterval = (currentScanInterval == FAST_SCAN_INTERVAL) ? SLOW_SCAN_INTERVAL : FAST_SCAN_INTERVAL;
          showTemporaryMessage((currentScanInterval == FAST_SCAN_INTERVAL) ? "Greitas sken." : "Letas sken.", 1000);
          return;
        case 3: currentMenu = SCANNING; break;
      }
      break;
    case DELETE_CONFIRM:
      if (deleteSelection == 0) {
        deleteAllBSSIDs();
        showTemporaryMessage("Sarasas isvalytas", 1500);
      }
      currentMenu = MENU_MAIN;
      menuSelection = 0;
      break;
  }
  updateDisplay();
}

void handleVeryLongPress() {
  showingInfoScreen = true;
  infoScreenStartTime = millis();
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Informacija");
  display.println("-------------------");
  display.println("Baterija: " + String(getBatteryPercentage()) + "%");
  display.println("Aptikta: " + String(hiddenNetworksFound));
  display.println("Isaugota: " + String(savedBssidCount));
  display.println("\nHeltec WiFi Skeneris");
  display.display();
}

void handleActiveState() {
  if (currentMenu == SCANNING && !scanning && (millis() - lastScanTime > currentScanInterval)) {
    scanning = true;
    WiFi.scanNetworks(false, true, false, 300); // Pasyvus skenavimas greitesnis
    lastScanTime = millis();
  }
}

void handleDisplayOffState() {
    if (!scanning && (millis() - lastScanTime > currentScanInterval)) {
    scanning = true;
    WiFi.scanNetworks(false, true, false, 300);
    lastScanTime = millis();
  }
}

void setDeviceState(DeviceState newState) {
  if (newState == deviceState) return;
  deviceState = newState;
  if (newState == ACTIVE) displayWake();
  else if (newState == DISPLAY_OFF) displaySleep();
}

void displaySleep() { display.clearDisplay(); display.display(); delay(50); display.ssd1306_command(SSD1306_DISPLAYOFF); }
void displayWake() { display.ssd1306_command(SSD1306_DISPLAYON); delay(50); updateDisplay(); }

int findBssidIndex(String bssid) {
  for (int i = 0; i < savedBssidCount; i++) {
    if (preferences.getString(("bssid_" + String(i)).c_str(), "") == bssid) {
      return i;
    }
  }
  return -1;
}

unsigned long getBssidTimestamp(int index) {
  return preferences.getULong(("ts_" + String(index)).c_str(), 0);
}

void updateBssidTimestamp(int index) {
  preferences.putULong(("ts_" + String(index)).c_str(), millis());
}

void saveNewBssid(String bssid) {
  if (savedBssidCount >= MAX_SAVED_BSSIDS) return;
  String bssidKey = "bssid_" + String(savedBssidCount);
  String tsKey = "ts_" + String(savedBssidCount);
  preferences.putString(bssidKey.c_str(), bssid);
  preferences.putULong(tsKey.c_str(), millis());
  savedBssidCount++;
  preferences.putUInt("count", savedBssidCount);
}

void deleteAllBSSIDs() {
  preferences.clear();
  savedBssidCount = 0;
  preferences.putUInt("count", 0);
}

String formatMacAddress(String bssid) { return bssid.substring(0, 2) + ".." + bssid.substring(15, 17); }

void powerDown() {
  deviceState = POWERING_OFF;
  pixel.clear(); pixel.show();
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(5, 15);
  display.println("ISJUNGIAMA");
  display.display();
  delay(1000);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_wifi_stop();
  digitalWrite(LED_PIN, LOW);
  digitalWrite(VIBRO_PIN, LOW);
  displaySleep();
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_PIN, 0);
  esp_deep_sleep_start();
}

void vibrate(int times) {
  if (vibration_count_remaining > 0) return;
  vibration_count_remaining = times;
}

void handleAsyncVibration() {
  if (vibration_count_remaining == 0) return;

  unsigned long currentTime = millis();
  if (vibrator_on) {
    if (currentTime - last_vibration_time >= VIBRO_DURATION) {
      digitalWrite(VIBRO_PIN, LOW);
      vibrator_on = false;
      last_vibration_time = currentTime;
      vibration_count_remaining--;
    }
  } else {
    if (currentTime - last_vibration_time >= VIBRO_PAUSE) {
      if (vibration_count_remaining > 0) {
        digitalWrite(VIBRO_PIN, HIGH);
        vibrator_on = true;
        last_vibration_time = currentTime;
      }
    }
  }
}

void handleVibration(const HiddenNetwork& network) {
  if (network.isTarget) {
    vibrate(VIBRO_TARGET_COUNT);
    return;
  }

  int index = findBssidIndex(network.bssid);
  if (index == -1) { // Visiškai naujas
    vibrate(VIBRO_NEW_COUNT);
    // Išsaugome IŠKART, kad išvengtume pasikartojimo
    saveNewBssid(network.bssid);
    return;
  }

  // Jei tinklas nėra naujas, patikriname kitas sąlygas
  unsigned long lastSeen = getBssidTimestamp(index);
  if (millis() - lastSeen > VIBRO_COOLDOWN) {
    vibrate(VIBRO_LONG_UNSEEN_COUNT);
    updateBssidTimestamp(index); // Atnaujiname laiką
    return;
  }

  bool wasSeenInLastScan = false;
  for(int i = 0; i < previouslySeenCount; i++){
    if(previouslySeenBssids[i] == network.bssid){
      wasSeenInLastScan = true;
      break;
    }
  }
  if(!wasSeenInLastScan){
    vibrate(VIBRO_REENTER_COUNT);
  }
}

void displayBatteryIcon(int x, int y) {
  int percentage = getBatteryPercentage();
  display.drawRect(x, y, 18, 9, WHITE);
  display.drawRect(x + 18, y + 2, 2, 5, WHITE);
  int width = map(percentage, 0, 100, 0, 14);
  if (width > 0) {
    display.fillRect(x + 2, y + 2, width, 5, WHITE);
  }
}