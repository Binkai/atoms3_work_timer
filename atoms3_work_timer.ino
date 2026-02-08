/**
 * @file AtomS3_Wuerfel_Webhook.ino
 * @author Kai Kuhfeld
 * @brief Zeit-Tracking Cube mit M5Stack AtomS3.
 * * Sendet HTTP-POST-Requests an Google Apps Script basierend nach Nutzerbedineung. Inklusive Web-basiertem Setup-Modus.
 * @version 1.0
 * @date 2026-01-26
 */
#include <M5Unified.h>
#include <M5GFX.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include <vector>
#include <Preferences.h>
#include <LittleFS.h>
#include <WiFiMulti.h>

// BILDER EINBINDEN
#include "images.h"
#include "secrets.h"
#define DEBUG 1  // Auf 0 setzen, um alle Logs zu deaktivieren

#if DEBUG
#define LOG_INFO(msg, ...) Serial.printf("[INFO ] [%6lu] " msg "\n", millis(), ##__VA_ARGS__)
#define LOG_WARN(msg, ...) Serial.printf("[WARN ] [%6lu] " msg "\n", millis(), ##__VA_ARGS__)
#define LOG_ERR(msg, ...) Serial.printf("[ERROR] [%6lu] " msg "\n", millis(), ##__VA_ARGS__)
#else
#define LOG_INFO(msg, ...)
#define LOG_WARN(msg, ...)
#define LOG_ERR(msg, ...)
#endif
#include <WebServer.h>
#include <DNSServer.h>
WiFiMulti wifiMulti;
String wifi_ssid = SECRET_SSID;
String wifi_pass = SECRET_PASS;
String api_token = apiToken_SEC;
String wifi_ssid2 = SECRET_SSID2;
String wifi_pass2 = SECRET_PASS2;
const char* apName = "M5Timer Setup";
const char* apPass = "S3TUP445561";  // min. 8 Zeichen!

// Globale Konfiguration (Standardwerte)
String ntfy_url = ntfy_url_SEC;
int push_interval_minutes = 45;
unsigned long lastPushTime = 0;

unsigned long lastIdleRemindTime = 0;
const int IDLE_REMIND_INTERVAL_MIN = 20;
// DNS und Webserver Instanzen
DNSServer dnsServer;
WebServer server(80);

const String googleScriptUrl = WEBHOOK_URL_SIDE1;

// Einstellungen
const unsigned long DISPLAY_TIMEOUT_MS = 30000;  // 30 Sek bis Display aus
const float WAKE_UP_THRESHOLD = 0.7;
const int DISPLAY_BRIGHTNESS = 80;

struct Customer {
  String name;
  const unsigned short* image;
};

Customer customers[] = {
  { "Kunde 1", image_Kunde1 },
  { "Kunde 2", image_Kunde2 },
  { "Kunde 3", image_Kunde3 },
  { "Kunde 4", image_Kunde4 }
};
int customerCount = sizeof(customers) / sizeof(customers[0]);
;
const char* ntpServer = "pool.ntp.org";
const char* tzInfo = "CET-1CEST,M3.5.0,M10.5.0/3";
int currentCustomerIndex = 0;

// --- TECHNIK ---
M5Canvas sprite(&M5.Display);
Preferences prefs;

enum State { STATE_IDLE,
             STATE_WORK_MENU,
             STATE_PROJECT_RUNNING,
             STATE_SENDING };
State currentState = STATE_IDLE;

time_t startTimestampUnix = 0;
unsigned long startTimeMillis = 0;
String startTimeString = "";

time_t workStartUnix = 0;
unsigned long workStartMillis = 0;
String workStartString = "";

std::vector<String> offlineQueue;

unsigned long lastActivityTime = 0;
bool isDisplayOn = true;
float lastAx, lastAy, lastAz;
int lastDrawnSecond = -1;
bool needsRedraw = true;
/**
 * Initialisiert Hardware, lädt Einstellungen aus dem Flash (Preferences)
 * und prüft, ob der Setup-Modus (Button gehalten) aktiviert werden soll.
 */
void setup() {
  Serial.begin(115200);
  pinMode(GPIO_NUM_41, INPUT_PULLUP);
  auto cfg = M5.config();
  M5.begin(cfg);

  M5.Display.setBrightness(DISPLAY_BRIGHTNESS);
  sprite.createSprite(128, 128);
  sprite.setTextDatum(middle_center);

  prefs.begin("m5timer", false);
  loadSettings();

  // SETUP CHECK ---
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_ORANGE);
  M5.Display.drawString("Hold for Setup...", 1, 64);

  // Wir prüfen für ca 3 Sekunden, ob gedrückt wird
  bool setupRequested = false;
  unsigned long checkStart = millis();
  while (millis() - checkStart < 2500) {
    M5.update();
    if (M5.BtnA.isPressed()) {
      setupRequested = true;
      break;  // Sofort raus, wenn gedrückt
    }
    delay(50);
  }

  if (setupRequested) {
    startSetupMode();
  }


  setCpuFrequencyMhz(80);

  M5.Display.setBrightness(DISPLAY_BRIGHTNESS);
  sprite.createSprite(128, 128);
  sprite.setTextDatum(middle_center);

  prefs.begin("m5timer", false);

  // 1. Initial Connect & NTP
  connectWiFi();

  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.drawString("NTP Sync...", 1, 64);

  configTime(0, 0, ntpServer);
  setenv("TZ", tzInfo, 1);
  tzset();

  struct tm timeinfo;
  int retry = 0;
  while (!getLocalTime(&timeinfo) && retry < 20) {
    delay(500);
    retry++;
  }

  disconnectWiFi();

  M5.Imu.getAccel(&lastAx, &lastAy, &lastAz);
  lastActivityTime = millis();
  if (!LittleFS.begin(true)) {
    LOG_ERR("LittleFS Mount fehlgeschlagen");
  } else {
    LOG_INFO("LittleFS gemountet");
    loadQueueFromFlash();
  }
  restoreSession();
  drawUI();
}
/**
 * Hauptschleife: Überwacht die Ausrichtung des Würfels, verwaltet 
 * das Display-Update und steuert die Sleep-Timer.
 */
void loop() {
  M5.update();

  checkActivity();

  if (currentState != STATE_SENDING) {

    // --- KNÖPFE ---
    if (M5.BtnA.wasDoubleClicked()) {
      resetActivity();
      // Doppelklick im Menü -> Ende!
      if (currentState == STATE_WORK_MENU) {
        stopWorkDay();
      }
    } else if (M5.BtnA.wasHold()) {
      resetActivity();
      if (currentState == STATE_WORK_MENU) {
        currentCustomerIndex = (currentCustomerIndex + 1) % customerCount;
        needsRedraw = true;
      }
    }

    if (M5.BtnA.wasSingleClicked()) {
      resetActivity();

      if (currentState == STATE_IDLE) {
        startWorkDay();
      }
      // Fall 2: Im Menü -> Projekt starten
      else if (currentState == STATE_WORK_MENU) {
        startProject();
      }
      // Fall 3: Projekt läuft -> Projekt stoppen
      else if (currentState == STATE_PROJECT_RUNNING) {
        stopProjectAndSend();
      }
    }
    // --- TIMER UPDATE ---
    if (currentState == STATE_PROJECT_RUNNING) {
      unsigned long currentSec = (millis() - startTimeMillis) / 1000;
      unsigned long intervalMs = (unsigned long)push_interval_minutes * 60000;
      if (millis() - lastPushTime > intervalMs) {
        LOG_INFO("45 Minuten vorbei - sende Erinnerung...");
        sendKeepAlivePush(currentSec);
        lastPushTime = millis();
      }
      if (isDisplayOn && currentSec != lastDrawnSecond) {
        lastDrawnSecond = currentSec;
        needsRedraw = true;
      }
    } else if (currentState == STATE_WORK_MENU) {
      // Prüfen ob 20 Minuten vergangen sind seit der letzten Erinnerung
      if (millis() - lastIdleRemindTime > (unsigned long)IDLE_REMIND_INTERVAL_MIN * 60000) {
        LOG_INFO("20 Min Idle vorbei - sende Erinnerung...");

        sendIdlePush();
        lastIdleRemindTime = millis();  // Timer resetten

        if (isDisplayOn) needsRedraw = true;
      }
    }
    // --- ZEICHNEN ---
    if (needsRedraw && isDisplayOn) {
      if (currentState == STATE_PROJECT_RUNNING) {
        updateTimerDisplay();
      } else {
        drawUI();
      }
      needsRedraw = false;
    }
  }

  // SMART SLEEP
  if (isDisplayOn) {
    delay(40);  // Normaler Betrieb
  } else {
    delay(200);  // Display aus: Prozessor schläft länger (Low Power)
  }
}

// --- LOGIK ---
// Queue vom Chip in den RAM laden (beim Start)
void loadQueueFromFlash() {
  if (!LittleFS.exists("/queue.txt")) return;  // Gab noch keine Datei

  File file = LittleFS.open("/queue.txt", "r");
  if (!file) return;

  offlineQueue.clear();  // Sichergehen, dass RAM leer ist

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();  // Leerzeichen/Umbruch am Ende entfernen
    if (line.length() > 0) {
      offlineQueue.push_back(line);
    }
  }
  file.close();
  LOG_INFO("Queue aus Flash geladen: %d Eintraege", offlineQueue.size());
}

void connectWiFi() {
  LOG_INFO("Connecting WiFi (Multi)...");
  if (WiFi.status() == WL_CONNECTED) return;

  WiFi.mode(WIFI_STA);

  if (wifi_ssid.length() > 0) wifiMulti.addAP(wifi_ssid.c_str(), wifi_pass.c_str());
  if (wifi_ssid2.length() > 0) wifiMulti.addAP(wifi_ssid2.c_str(), wifi_pass2.c_str());

  int t = 0;
  while (wifiMulti.run() != WL_CONNECTED && t < 15) {  // Leicht erhöhtes Timeout für Scan
    delay(500);
    t++;
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    LOG_INFO("\nVerbunden mit: %s", WiFi.SSID().c_str());
  } else {
    LOG_ERR("\nKeine Verbindung möglich.");
  }
}
// Queue vom RAM auf den Chip speichern (nach Änderungen)
void saveQueueToFlash() {
  File file = LittleFS.open("/queue.txt", "w");
  if (!file) {
    LOG_ERR("Konnte Queue nicht speichern!");
    return;
  }

  for (String json : offlineQueue) {
    file.println(json);
  }
  file.close();
  LOG_INFO("Queue im Flash gesichert (%d Eintraege).", offlineQueue.size());
}
/**
 * Funktion zur Wiederherstellung von Sessiondaten (bei Spannungsverlust z. B.)
 */
void restoreSession() {
  bool workRecovered = false;
  bool projectRecovered = false;
  LOG_INFO("Restoring Session");
  if (prefs.getBool("isWorkRunning", false)) {
    workStartUnix = prefs.getUInt("workStartUnix", 0);

    if (workStartUnix > 0) {

      time_t now;
      time(&now);
      // Schutz gegen negative Zeit (falls Uhr noch nicht synchronisiert)
      if (now >= workStartUnix) {
        unsigned long secondsPassed = (unsigned long)(now - workStartUnix);
        workStartMillis = millis() - (secondsPassed * 1000);
      } else {
        workStartMillis = millis();
      }

      // String rekonstruieren
      struct tm timeinfoStart;
      localtime_r(&workStartUnix, &timeinfoStart);
      char tB[20];
      strftime(tB, 20, "%H:%M:%S", &timeinfoStart);
      workStartString = String(tB);

      workRecovered = true;
      currentState = STATE_WORK_MENU;
      LOG_INFO("Arbeitstag wiederhergestellt. Start: %s", workStartString.c_str());
    }
  }
  if (prefs.getBool("isTracking", false)) {
    currentCustomerIndex = prefs.getInt("custIdx", 0);
    startTimestampUnix = prefs.getULong("startUnix", 0);
    startTimeString = prefs.getString("startStr", "00:00:00");
    time_t now;
    time(&now);
    long passedSeconds = now - startTimestampUnix;
    if (passedSeconds < 0) passedSeconds = 0;
    startTimeMillis = millis() - (passedSeconds * 1000);
    currentState = STATE_PROJECT_RUNNING;
    projectRecovered = true;
    LOG_INFO("Projekt wiederhergestellt für Kunde Index %d", currentCustomerIndex);
  }
  if (projectRecovered || workRecovered) {
    sprite.fillScreen(TFT_BLUE);
    sprite.setTextColor(TFT_WHITE);
    sprite.drawString("Recovered!", 64, 64);
    sprite.pushSprite(0, 0);
    delay(1000);
  } else if (!workRecovered && !projectRecovered) {
    currentState = STATE_IDLE;
    prefs.putBool("isWorkRunning", false);
    prefs.putBool("isTracking", false);
    LOG_INFO("Keine aktive Sitzung gefunden.");
  }
  needsRedraw = true;
}
/**
 * Funktion zum Senden eines JSONs an die gegebene Google App Script URL (oder eine andere WebHoook API).
 */
bool sendJson(String payload) {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  http.begin(googleScriptUrl);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(payload);
  if (code < 0) {
    LOG_ERR("HTTP POST fehlgeschlagen: %s", http.errorToString(code).c_str());
  } else {
    LOG_INFO("HTTP Response Code: %d", code);
  }
  http.end();
  return (code == 200 || code == 201 || code == 302);
}

void loadSettings() {
  // WLAN
  wifi_ssid = prefs.getString("ssid", SECRET_SSID);
  wifi_pass = prefs.getString("pass", SECRET_PASS);
  wifi_ssid2 = prefs.getString("ssid", SECRET_SSID2);
  wifi_pass2 = prefs.getString("pass", SECRET_PASS2);
  api_token = prefs.getString("token", apiToken_SEC);
  ntfy_url = prefs.getString("ntfyUrl", ntfy_url_SEC);
  push_interval_minutes = prefs.getInt("pushMin", push_interval_minutes);
  customers[0].name = prefs.getString("c0", customers[0].name);
  customers[1].name = prefs.getString("c1", customers[1].name);
  customers[2].name = prefs.getString("c2", customers[2].name);
  customers[3].name = prefs.getString("c3", customers[3].name);

  LOG_INFO("Settings geladen. SSID: %s", wifi_ssid.c_str());
}
/**
 * Startet einen Access Point und einen Webserver, um Konfigurationen des Codes im Webbrowser anzupassen.
 */
void startSetupMode() {
  // --- KONFIGURATION ---

  // Display Setup
  M5.Display.fillScreen(TFT_BLUE);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.drawString("SETUP MODE", 0, 30);

  M5.Display.setTextSize(1);
  M5.Display.setTextColor(TFT_LIGHTGREY);
  M5.Display.drawString("WLAN Name:", 0, 55);
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.drawString(apName, 0, 68);

  M5.Display.setTextColor(TFT_LIGHTGREY);
  M5.Display.drawString("WLAN Password:", 0, 85);
  M5.Display.setTextColor(TFT_YELLOW);
  M5.Display.drawString(apPass, 0, 98);

  M5.Display.setTextColor(TFT_GREEN);
  M5.Display.drawString("IP: 192.168.4.1", 0, 118);
  M5.Display.setTextColor(TFT_WHITE);

  // SoftAP starten
  WiFi.softAP(apName, apPass);
  dnsServer.start(53, "*", WiFi.softAPIP());

  // --- ROUTE: HAUPTSEITE (GET) ---
  server.on("/", HTTP_GET, []() {
    String html = "<!DOCTYPE HTML><html><head>";
    html += "<title>M5Timer Setup</title>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>";
    html += "body { font-family: sans-serif; text-align: center; background: #222; color: #fff; }";
    html += "input { width: 80%; padding: 10px; margin: 5px 0; border-radius: 5px; border: none; }";
    html += "input[type=submit] { background-color: #4CAF50; color: white; cursor: pointer; font-weight: bold; margin-top: 15px;}";
    html += "h2 { border-bottom: 2px solid #555; padding-bottom: 10px; }";
    html += ".group { background: #333; padding: 10px; margin: 10px; border-radius: 8px; }";
    html += "label { display:block; font-size: 0.8em; color: #ccc; text-align: left; margin-left: 10%; }";
    html += "</style></head><body>";

    html += "<h2>M5Timer Setup</h2>";
    html += "<form action='/save' method='POST'>";

    // WLAN
    html += "<div class='group'><h3>WLAN 1</h3>";
    html += "<label>SSID:</label><input type='text' name='ssid' value='" + wifi_ssid + "'>";
    html += "<label>Passwort:</label><input type='password' name='pass' value='" + wifi_pass + "'>";
    html += "</div>";
    // WLAN 2
    html += "<div class='group'><h3>WLAN 2 (Büro)</h3>";
    html += "<label>SSID 2:</label><input type='text' name='ssid2' value='" + wifi_ssid2 + "'>";
    html += "<label>Passwort 2:</label><input type='password' name='pass2' value='" + wifi_pass2 + "'>";
    html += "</div>";
    // API
    html += "<div class='group'><h3>API</h3>";
    html += "<label>Token:</label><input type='text' name='token' value='" + api_token + "'>";
    html += "</div>";

    // --- NEU: BENACHRICHTIGUNG ---
    html += "<div class='group'><h3>Benachrichtigung</h3>";
    html += "<label>Ntfy URL:</label><input type='text' name='ntfyUrl' value='" + ntfy_url + "'>";
    html += "<label>Intervall (Minuten):</label><input type='number' name='pushMin' value='" + String(push_interval_minutes) + "'>";
    html += "</div>";
    // -----------------------------

    // KUNDEN
    html += "<div class='group'><h3>Kunden Namen</h3>";
    html += "<label>Kunde 1:</label><input type='text' name='c0' value='" + customers[0].name + "'>";
    html += "<label>Kunde 2:</label><input type='text' name='c1' value='" + customers[1].name + "'>";
    html += "<label>Kunde 3:</label><input type='text' name='c2' value='" + customers[2].name + "'>";
    html += "<label>Kunde 4:</label><input type='text' name='c3' value='" + customers[3].name + "'>";
    html += "</div>";

    html += "<input type='submit' value='SPEICHERN & NEUSTART'>";
    html += "</form></body></html>";

    server.send(200, "text/html", html);
  });

  // --- ROUTE: SPEICHERN (POST) ---
  server.on("/save", HTTP_POST, []() {
    auto saveIfValid = [](const char* argName, const char* prefKey) {
      if (server.hasArg(argName) && server.arg(argName).length() > 0) {
        prefs.putString(prefKey, server.arg(argName));
      }
    };

    saveIfValid("ssid", "ssid");
    saveIfValid("pass", "pass");
    if (server.hasArg("ssid2")) {
      prefs.putString("ssid2", server.arg("ssid2"));
    }
    if (server.hasArg("pass2")) {
      prefs.putString("pass2", server.arg("pass2"));
    }
    saveIfValid("token", "token");

    // --- NEU: Push speichern ---
    saveIfValid("ntfyUrl", "ntfyUrl");
    if (server.hasArg("pushMin")) {
      int val = server.arg("pushMin").toInt();
      if (val > 0) prefs.putInt("pushMin", val);  // Nur speichern wenn > 0
    }
    // ---------------------------

    saveIfValid("c0", "c0");
    saveIfValid("c1", "c1");
    saveIfValid("c2", "c2");
    saveIfValid("c3", "c3");

    String html = "<html><body style='background:#222; color:#fff; text-align:center; font-family:sans-serif;'>";
    html += "<h1>Gespeichert!</h1>";
    html += "<p>Der Wuerfel startet neu...</p>";
    html += "</body></html>";

    server.send(200, "text/html", html);
    delay(1000);
    ESP.restart();
  });

  server.onNotFound([]() {
    server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString(), true);
    server.send(302, "text/plain", "");
  });

  server.begin();

  while (true) {
    dnsServer.processNextRequest();
    server.handleClient();
    M5.update();
    delay(10);
  }
}
/**
 * Verarbeitung der offline Queue (wenn sie vorliegt).
 */
void processOfflineQueue() {
  if (offlineQueue.empty()) return;

  LOG_INFO("Versuche %d Offline-Eintraege zu senden...", offlineQueue.size());

  std::vector<String> remaining;
  bool changed = false;

  for (String json : offlineQueue) {
    if (!sendJson(json)) {
      // Fehlgeschlagen: Bleibt in der Liste
      remaining.push_back(json);
    } else {
      // Erfolg: Wird nicht übernommen -> ist damit gelöscht
      changed = true;
    }
    delay(50);
  }

  offlineQueue = remaining;

  if (changed) {
    if (offlineQueue.empty()) {
      // Fall 1: Alles wurde gesendet -> Datei komplett löschen!
      if (LittleFS.exists("/queue.txt")) {
        LittleFS.remove("/queue.txt");
        LOG_INFO("Queue leer -> Datei gelöscht.");
      }
    } else {
      // Fall 2: Es sind noch Reste da -> Datei überschreiben
      saveQueueToFlash();
      LOG_INFO("Queue aktualisiert -> Reste gespeichert.");
    }
  }
  // -------------------------------------

  LOG_INFO("Queue Status: %d verbleibend.", offlineQueue.size());
}
/**
 * NTFY Benachrichtigung der aktuellen Projektzeit (wenn erfasst wird).
 */
void sendKeepAlivePush(long durationSec) {
  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;

    http.begin(ntfy_url);
    http.addHeader("Content-Type", "text/plain");
    http.addHeader("Title", "⏳ Tracker läuft noch!");

    // Nachricht generieren
    int minutes = durationSec / 60;
    String msg = "Kunde: " + customers[currentCustomerIndex].name + "\n";
    msg += "Laufzeit: " + String(minutes) + " Minuten.\n";
    msg += "Akku: " + String(M5.Power.getBatteryLevel()) + "%";

    int code = http.POST(msg);
    if (code > 0) {
      LOG_INFO("Erinnerungs-Push gesendet!");
    }
    http.end();
  }

  disconnectWiFi();
}


void disconnectWiFi() {
  LOG_INFO("Disconnecting WiFi");
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

void resetActivity() {
  lastActivityTime = millis();
  if (!isDisplayOn) {
    isDisplayOn = true;
    M5.Display.wakeup();  // Sicherstellen, dass Displaytreiber wach ist
    M5.Display.setBrightness(DISPLAY_BRIGHTNESS);
    needsRedraw = true;
  }
}
/**
 * Liest den IMU Sensor aus um zu prüfen, ob eine Bewegung vorliegt - ansonsten schläft der Atom S3 ein.
 */
void checkActivity() {
  float ax, ay, az;
  M5.Imu.getAccel(&ax, &ay, &az);

  float movement = abs(ax - lastAx) + abs(ay - lastAy) + abs(az - lastAz);
  if (movement > WAKE_UP_THRESHOLD) {
    resetActivity();
    checkOrientation(ax, ay, az);
  }
  lastAx = ax;
  lastAy = ay;
  lastAz = az;

  if (isDisplayOn && (millis() - lastActivityTime > DISPLAY_TIMEOUT_MS)) {
    Serial.flush();
    enterDeepSleep();
  }
}
/**
 * Funktion zur Prüfung der Rotation und Rückmeldung. Hilft zur korrekten Darstellung des Texts.
 */
void checkOrientation(float ax, float ay, float az) {
  int detectedRot = M5.Display.getRotation();
  if (ay > 0.75) detectedRot = 0;
  else if (ay < -0.75) detectedRot = 2;
  else if (ax > 0.75) detectedRot = 1;
  else if (ax < -0.75) detectedRot = 3;

  if (detectedRot != M5.Display.getRotation()) {
    M5.Display.setRotation(detectedRot);
    needsRedraw = true;
  }
}
/**
 * Allgeimeine Funktion zur Darstellung der Kundenbilder und entsprechender UI. 
 * Solange die Arbeitszeit noch nicht begonnen wurde STATE_IDLE wird nur angezeigt, dass diese zu starten ist.
 */
void drawUI() {

  sprite.fillScreen(TFT_BLACK);
  if (currentState == STATE_IDLE) {
    sprite.setTextColor(TFT_ORANGE);
    sprite.setTextSize(2);
    sprite.drawString("START", 64, 50);
    sprite.setTextSize(1);
    sprite.setTextColor(TFT_WHITE);
    sprite.drawString("Arbeitstag", 64, 80);
    sprite.pushSprite(0, 0);
    return;
  }
  sprite.setSwapBytes(true);
  sprite.pushImage(0, 0, 128, 128, customers[currentCustomerIndex].image);
  sprite.setSwapBytes(false);

  sprite.setTextColor(TFT_BLACK);
  sprite.drawString(customers[currentCustomerIndex].name, 66, 66);
  sprite.setTextColor(TFT_WHITE);
  sprite.drawString(customers[currentCustomerIndex].name, 64, 64);

  if (!offlineQueue.empty()) sprite.fillCircle(120, 10, 3, TFT_RED);

  sprite.pushSprite(0, 0);
}
/**
 * Funktion zur Darstellung eines Timers bei Erfassung einer Projektzeit.
 */
void updateTimerDisplay() {
  time_t now;
  time(&now);  // Aktuelle Zeit holen

  // Berechnung basierend auf Unix-Timestamp (Sekunden)
  unsigned long currentDuration = (unsigned long)difftime(now, startTimestampUnix);

  int hours = currentDuration / 3600;
  int minutes = (currentDuration % 3600) / 60;
  int seconds = currentDuration % 60;

  char buf[20];
  sprintf(buf, "%02d:%02d:%02d", hours, minutes, seconds);

  sprite.fillScreen(TFT_BLACK);
  sprite.setTextColor(TFT_WHITE);
  sprite.setTextSize(2);
  sprite.drawString(customers[currentCustomerIndex].name, 64, 30);
  sprite.setTextSize(3);
  sprite.drawString(buf, 64, 70);
  sprite.setTextSize(2);

  sprite.pushSprite(0, 0);
}
/**
 * Beginn des Projektzeittimers
 */
void startProject() {
  currentState = STATE_PROJECT_RUNNING;
  ;
  startTimeMillis = millis();

  struct tm timeinfo;
  time(&startTimestampUnix);

  if (getLocalTime(&timeinfo)) {
    char timeStringBuff[20];
    strftime(timeStringBuff, sizeof(timeStringBuff), "%H:%M:%S", &timeinfo);
    startTimeString = String(timeStringBuff);
  }

  prefs.putBool("isTracking", true);
  prefs.putInt("custIdx", currentCustomerIndex);
  prefs.putULong("startUnix", startTimestampUnix);
  prefs.putString("startStr", startTimeString);

  resetActivity();

  sprite.fillScreen(TFT_BLACK);
  sprite.drawString("START", 64, 64);
  sprite.pushSprite(0, 0);
  delay(500);

  needsRedraw = true;
}
/**
 * Beginn des Arbeitszeittimers
 */
void startWorkDay() {
  currentState = STATE_WORK_MENU;
  workStartMillis = millis();
  lastIdleRemindTime = millis();
  struct tm timeinfo;
  time(&workStartUnix);

  prefs.putBool("isWorkRunning", true);
  prefs.putUInt("workStartUnix", workStartUnix);

  if (getLocalTime(&timeinfo)) {
    char tB[20];
    strftime(tB, 20, "%H:%M:%S", &timeinfo);
    workStartString = String(tB);
  }

  sprite.fillScreen(TFT_GREEN);
  sprite.setTextColor(TFT_BLACK);
  sprite.drawString("Arbeitszeitbeginn", 64, 64);
  sprite.pushSprite(0, 0);
  delay(1000);
  needsRedraw = true;
}
/**
 * Bei Abschluss der Arbeitszeit (!) wird diese Funtkion ausgelöst. Dient der Verarbeitung und Versand an Webhook API als JSON.
 */
void stopWorkDay() {
  currentState = STATE_SENDING;  // Blockiert Eingaben
  sprite.fillScreen(TFT_YELLOW);
  sprite.setTextColor(TFT_BLACK);
  sprite.drawString("Sende!", 64, 40);
  sprite.drawString("Arbeitszeit!", 64, 80);

  sprite.pushSprite(0, 0);
  // 1. Endzeit ermitteln
  String workEndString = "";
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char tB[20];
    strftime(tB, 20, "%H:%M:%S", &timeinfo);
    workEndString = String(tB);
  }
  long durationSec = (millis() - workStartMillis) / 1000;

  // 2. Datum für Google Sheet (siehe Schritt 1 vorhin)
  char dateStringBuff[15];
  struct tm timeinfoStart;
  localtime_r(&workStartUnix, &timeinfoStart);
  strftime(dateStringBuff, sizeof(dateStringBuff), "%Y-%m-%d", &timeinfoStart);
  String dateString = String(dateStringBuff);

  // 3. JSON bauen - Als "Kunde" senden wir "ARBEITSZEIT"
  String jsonPayload = "{";
  jsonPayload += "\"token\":\"" + api_token + "\",";
  jsonPayload += "\"date\":\"" + dateString + "\",";
  jsonPayload += "\"customer\":\"ARBEITSZEIT\",";  // <--- Kennzeichnung im Sheet
  jsonPayload += "\"startTime\":\"" + workStartString + "\",";
  jsonPayload += "\"endTime\":\"" + workEndString + "\",";
  jsonPayload += "\"duration\":" + String(durationSec) + ",";
  jsonPayload += "\"battery\":" + String(M5.Power.getBatteryLevel());
  jsonPayload += "}";

  // 4. Senden
  connectWiFi();
  bool success = sendJson(jsonPayload);
  disconnectWiFi();

  // 5. Aufräumen
  prefs.putBool("isWorkRunning", false);
  currentState = STATE_IDLE;
  prefs.putUInt("workStartUnix", 0);
  sprite.fillScreen(TFT_BLUE);
  sprite.setTextColor(TFT_WHITE);
  sprite.drawString("Feierabend!", 64, 64);
  sprite.pushSprite(0, 0);
  delay(2000);
  needsRedraw = true;
}
/**
 * Bei Abschluss einer Projektzeit wird diese Funtkion ausgelöst. Dient der Verarbeitung und Versand an Webhook API als JSON.
 */
void stopProjectAndSend() {

  currentState = STATE_SENDING;
  resetActivity();
  lastIdleRemindTime = millis();
  prefs.putBool("isTracking", false);

  String endTimeString = "";
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char tB[20];
    strftime(tB, 20, "%H:%M:%S", &timeinfo);
    endTimeString = String(tB);
  }
  long durationSec = (millis() - startTimeMillis) / 1000;
  char dateStringBuff[15];
  struct tm timeinfoStart;
  localtime_r(&startTimestampUnix, &timeinfoStart);
  strftime(dateStringBuff, sizeof(dateStringBuff), "%Y-%m-%d", &timeinfoStart);
  String dateString = String(dateStringBuff);

  String jsonPayload = "{";
  jsonPayload += "\"token\":\"" + api_token + "\",";
  jsonPayload += "\"date\":\"" + dateString + "\",";
  jsonPayload += "\"customer\":\"" + customers[currentCustomerIndex].name + "\",";
  jsonPayload += "\"startTime\":\"" + startTimeString + "\",";
  jsonPayload += "\"endTime\":\"" + endTimeString + "\",";
  jsonPayload += "\"duration\":" + String(durationSec) + ",";
  jsonPayload += "\"battery\":" + String(M5.Power.getBatteryLevel());
  jsonPayload += "}";
  LOG_INFO("Tracking gestoppt für Kunde: %s", customers[currentCustomerIndex].name.c_str());
  LOG_INFO("Dauer: %ld Sekunden", durationSec);
  connectWiFi();

  bool success = false;
  processOfflineQueue();

  if (WiFi.status() == WL_CONNECTED) {
    sprite.fillScreen(TFT_ORANGE);
    sprite.setTextColor(TFT_BLACK);
    sprite.drawString("Senden...", 64, 64);
    sprite.pushSprite(0, 0);
    success = sendJson(jsonPayload);
  }

  if (success) {
    sprite.fillScreen(TFT_GREEN);
    sprite.drawString("Gespeichert!", 64, 64);
  } else {
    offlineQueue.push_back(jsonPayload);
    saveQueueToFlash();
    sprite.fillScreen(TFT_RED);
    sprite.setTextColor(TFT_WHITE);
    sprite.drawString("Offline", 64, 40);
    sprite.drawString("Gespeichert", 64, 80);
  }

  sprite.pushSprite(0, 0);
  delay(2000);

  disconnectWiFi();

  currentState = STATE_WORK_MENU;
  needsRedraw = true;
}
/**
 * Sendet per ntfy eine Benachrichtigung über aktuelle Arbeitszeit (!)
 */
void sendIdlePush() {
  connectWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(ntfy_url);
    http.addHeader("Content-Type", "text/plain");
    http.addHeader("Title", "⚠️ Kein Projekt aktiv!");
    long workDuration = (millis() - workStartMillis) / 1000 / 60;

    String msg = "Arbeitszeit läuft seit " + String(workDuration) + " Min.\n";
    msg += "Bitte Projekt starten!\n";
    msg += "Akku: " + String(M5.Power.getBatteryLevel()) + "%";

    int code = http.POST(msg);
    if (code > 0) {
      LOG_INFO("Idle-Erinnerung gesendet!");
    }
    http.end();
  }
  disconnectWiFi();
}
/**
 * Versetzt den ESP32-S3 in den Light-Sleep, um Strom zu sparen. (statt DeepSleep - aktuell nur lightSleep)
 * Wacht durch den Button oder Timer wieder auf.
 */
// TODO: DeepSleep statt lightSleep
void enterDeepSleep() {
  LOG_INFO("Gehe in den Deep Sleep...");

  M5.Display.setBrightness(0);
  M5.Display.sleep();
  gpio_wakeup_enable(GPIO_NUM_41, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  long timeUntilNextPush = (unsigned long)IDLE_REMIND_INTERVAL_MIN * 60000 - (millis() - lastIdleRemindTime);
  if (timeUntilNextPush < 0) timeUntilNextPush = 1000;
  esp_sleep_enable_timer_wakeup((uint64_t)timeUntilNextPush * 1000);

  esp_light_sleep_start();

  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO) {
    while (digitalRead(GPIO_NUM_41) == LOW) {
      delay(10);
    }
    LOG_INFO("Aufgewacht durch Button.");
  } else {
    LOG_INFO("Aufgewacht durch Timer-Timeout.");
  }
  M5.update();
  M5.Display.wakeup();
  M5.Display.setBrightness(DISPLAY_BRIGHTNESS);

  lastActivityTime = millis();
  isDisplayOn = true;
}