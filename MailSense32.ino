/*
 * ╔╦╗╔═╗╦╦  ╔═╗╔═╗╔╗╔╔═╗╔═╗╔═╗ ╔═╗
 * ║║║╠═╣║║  ╚═╗║╣ ║║║╚═╗║╣ ╚═╗ ╔═╝
 * ╩ ╩╩ ╩╩╩═╝╚═╝╚═╝╝╚╝╚═╝╚═╝╚═╝ ╚═╝
 * 
 * MailSense32 — Smart Mailbox Detector
 * Version : 1.0.3
 * Author  : Fab / egamaker.be
 * License : MIT
 * GitHub  : https://github.com/egamaker/MailSense32
 * 
 * Supported notifications : Email SMTP, Ntfy.sh, Home Assistant MQTT, Telegram
 * Supported sensors       : Reed Switch, PIR
 * 
 * Hardware : ESP32-DevKitC V4 + external antenna
 * Battery  : LiPo 3.7V via TP4056
 */

// ============================================================
// LIBRARIES
// ============================================================
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESP_Mail_Client.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

// ============================================================
// PIN DEFINITIONS
// ============================================================
#define PIN_SENSOR        GPIO_NUM_4   // Reed Switch or PIR signal
#define PIN_RESET_BTN     GPIO_NUM_0   // Boot button = long press → wizard mode
#define PIN_EXT_BTN       GPIO_NUM_12  // External button = short → status, long → charge mode
#define PIN_STATUS_LED    GPIO_NUM_2   // Onboard LED
#define PIN_BATTERY_ADC   35           // Battery voltage divider (100k + 220k) — GPIO35 input only

// ============================================================
// CONSTANTS
// ============================================================
#define FIRMWARE_VERSION      "1.0.3"
#define WIFI_MAX_RETRIES      5
#define WIFI_TIMEOUT_MS       10000
#define LONG_PRESS_MS         3000
#define EXT_BTN_LONG_MS       3000    // Long press external button → charge mode
#define EXT_BTN_SHORT_MS      50      // Short press → status page
#define WIZARD_TIMEOUT_MS     300000   // 5 min inactivity → sleep
#define DEFAULT_COOLDOWN_MIN  5
#define DEFAULT_MESSAGE_FR    "📬 Vous avez du courrier !"
#define DEFAULT_MESSAGE_EN    "📬 You've got mail!"
#define NTP_SERVER            "pool.ntp.org"
#define TIMEZONE              "CET-1CEST,M3.5.0,M10.5.0/3"  // Europe/Brussels — auto heure d'été
#define NVS_NAMESPACE         "mailsense"
#define AP_SSID               "MailSense32-Setup"
#define AP_PASSWORD           "mailsense32"
#define BATTERY_R1            100000.0 // 100kΩ
#define BATTERY_R2            220000.0 // 220kΩ
#define BATTERY_MAX_V         4.2
#define BATTERY_MIN_V         3.0
#define BATTERY_LOW_PCT       20
#define STATUS_SERVER_MS      180000  // Status web server active 3 min after wake
#define CHARGE_CHECK_MS       300000  // Check battery every 5 min in charging mode
#define BATTERY_FULL_PCT      95      // Notify when battery >= 95%

// ============================================================
// NOTIFICATION METHODS (bitmask — combinable)
// ============================================================
#define NOTIF_NONE        0x00
#define NOTIF_EMAIL       0x01
#define NOTIF_NTFY        0x02
#define NOTIF_HA_MQTT     0x04
#define NOTIF_TELEGRAM    0x08

// ============================================================
// SENSOR TYPES
// ============================================================
#define SENSOR_REED       0
#define SENSOR_PIR        1

// ============================================================
// LANGUAGE
// ============================================================
#define LANG_FR  0
#define LANG_EN  1

// ============================================================
// GLOBAL OBJECTS
// ============================================================
Preferences prefs;
WebServer   server(80);
WiFiClient  wifiClient;
PubSubClient mqttClient(wifiClient);
unsigned long serverStart = 0;  // Global — resettable via /ping

// ============================================================
// CONFIGURATION STRUCTURE
// ============================================================
struct Config {
  // WiFi
  char wifi_ssid[64]        = "";
  char wifi_pass[64]        = "";

  // General
  uint8_t  sensor_type      = SENSOR_REED;
  uint8_t  language         = LANG_FR;
  uint16_t cooldown_min     = DEFAULT_COOLDOWN_MIN;
  char     custom_msg[128]  = DEFAULT_MESSAGE_FR;
  char     timezone[64]     = "CET-1CEST,M3.5.0,M10.5.0/3";  // Europe/Brussels default
  bool     configured       = false;

  // Notification method (can be combined via bitmask later)
  uint8_t  notif_method     = NOTIF_NONE;

  // Email SMTP
  char smtp_host[64]        = "";
  uint16_t smtp_port        = 587;
  char smtp_user[64]        = "";
  char smtp_pass[64]        = "";
  char smtp_to[64]          = "";

  // Ntfy
  char ntfy_server[128]     = "https://ntfy.sh";
  char ntfy_topic[64]       = "";
  char ntfy_token[64]       = "";  // optional, for self-hosted auth

  // Home Assistant MQTT
  char mqtt_host[64]        = "";
  uint16_t mqtt_port        = 1883;
  char mqtt_user[64]        = "";
  char mqtt_pass[64]        = "";
  char mqtt_topic[64]       = "mailsense32/mail";

  // Telegram
  char tg_token[64]         = "";
  char tg_chat_id[32]       = "";
};

Config cfg;

// ============================================================
// RTC MEMORY — survives deep sleep
// ============================================================
RTC_DATA_ATTR uint32_t lastTriggerEpoch  = 0;
RTC_DATA_ATTR uint8_t  bootCount         = 0;
RTC_DATA_ATTR uint8_t  totalTriggers     = 0;
RTC_DATA_ATTR bool     lastNotifOk       = false;
RTC_DATA_ATTR char     lastTriggerTime[32] = "";
RTC_DATA_ATTR bool     chargingMode      = false;  // Mode charge actif
RTC_DATA_ATTR bool     maintenanceMode   = true;   // true = détection désactivée (défaut installation)

// ============================================================
// FORWARD DECLARATIONS
// ============================================================
void loadConfig();
void saveConfig();
void startWizard();
void startNormal();
void startStatusServer();
bool connectWiFi();
void handleDetection();
float readBatteryPercent();
String getBatteryIcon(float pct);
void sendNotification(bool isTest = false);
void sendEmail(bool isTest);
void sendNtfy(bool isTest);
void sendHAMQTT(bool isTest);
void sendTelegram(bool isTest);
String buildMessage(bool isTest);
String getCurrentTime();
void blinkLED(int times, int ms);
void setupRoutes();
String htmlWizard();
String htmlSuccess();
String htmlError(String msg);

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(2000);  // Laisser le temps au moniteur série de se connecter
  Serial.println("\n\n=== MailSense32 v" + String(FIRMWARE_VERSION) + " ===");

  pinMode(PIN_STATUS_LED, OUTPUT);
  pinMode(PIN_RESET_BTN, INPUT_PULLUP);
  pinMode(PIN_SENSOR, INPUT_PULLUP);
  pinMode(PIN_EXT_BTN, INPUT_PULLUP);

  bootCount++;
  Serial.printf("Boot count: %d\n", bootCount);

  loadConfig();

  // Check wake-up reason
  esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();

  // Check long press on reset button at boot → wizard
  unsigned long pressStart = millis();
  bool longPress = false;
  while (digitalRead(PIN_RESET_BTN) == LOW) {
    if (millis() - pressStart > LONG_PRESS_MS) {
      longPress = true;
      blinkLED(3, 200);
      break;
    }
  }

  // Check external button at boot
  bool extShortPress = false;
  bool extLongPress  = false;
  if (digitalRead(PIN_EXT_BTN) == LOW) {
    unsigned long extStart = millis();
    while (digitalRead(PIN_EXT_BTN) == LOW) {
      if (millis() - extStart > EXT_BTN_LONG_MS) {
        extLongPress = true;
        blinkLED(2, 300); // 2 blinks lents = mode charge
        break;
      }
    }
    if (!extLongPress && (millis() - extStart) > EXT_BTN_SHORT_MS) {
      extShortPress = true;
      blinkLED(1, 100); // 1 blink = page de statut
    }
  }

  if (longPress || !cfg.configured) {
    Serial.println("→ Starting WIZARD mode");
    startWizard();
  } else if (extLongPress) {
    Serial.println("→ External button long press → Charging mode");
    chargingMode = true;
    if (connectWiFi()) {
      configTime(0, 0, NTP_SERVER);
  setenv("TZ", cfg.timezone, 1);
  tzset();
      delay(500);
      startStatusServer();
    } else {
      // Pas de WiFi → deep sleep avec timer 5 min directement
      Serial.println("WiFi unavailable → charging mode silent");
      esp_sleep_enable_timer_wakeup((uint64_t)CHARGE_CHECK_MS * 1000ULL);
      esp_deep_sleep_start();
    }
  } else if (extShortPress) {
    Serial.println("→ External button short press → Status page");
    if (connectWiFi()) {
      configTime(0, 0, NTP_SERVER);
  setenv("TZ", cfg.timezone, 1);
  tzset();
      delay(500);
      startStatusServer();
    } else {
      startWizard(); // WiFi échoue → wizard pour reconfigurer
    }
  } else if (chargingMode) {
    Serial.println("→ Charging mode — checking battery");
    startChargingMode();
  } else if (wakeup == ESP_SLEEP_WAKEUP_EXT0) {
    // Double-check: GPIO4 must still be HIGH (reed open = mail)
    // If it's already LOW again, it was a false trigger
    delay(50);
    if (digitalRead(PIN_SENSOR) == HIGH) {
      if (maintenanceMode) {
        Serial.println("→ Wake-up from sensor — MAINTENANCE MODE — notification skipped");
        if (connectWiFi()) { configTime(0, 0, NTP_SERVER); setenv("TZ", cfg.timezone, 1); tzset(); startStatusServer(); }
        else goToSleep();
      } else {
        Serial.println("→ Wake-up from sensor — confirmed");
        startNormal();
      }
    } else {
      Serial.println("→ Wake-up from sensor — false trigger, going to sleep");
      goToSleep();
    }
  } else {
    if (cfg.configured) {
      Serial.println("→ Normal boot — starting status server");
      if (connectWiFi()) {
        configTime(0, 0, NTP_SERVER);
        setenv("TZ", cfg.timezone, 1);
        tzset();
        startStatusServer();
      } else {
        startWizard(); // WiFi échoue → wizard pour reconfigurer
      }
    } else {
      Serial.println("→ Not configured → wizard");
      startWizard();
    }
  }
}

void loop() {
  server.handleClient();

  if (!serverStart) serverStart = millis();

  // Real-time sensor monitoring — log GPIO4 state changes
  static bool lastSensorState = LOW;
  static unsigned long lastTriggerLoop = 0;
  bool currentSensorState = digitalRead(PIN_SENSOR);
  if (currentSensorState != lastSensorState) {
    lastSensorState = currentSensorState;
    Serial.printf("GPIO4 changed → %s\n", currentSensorState == HIGH ? "HIGH (magnet removed — mail!)" : "LOW (magnet present)");
    // Si HIGH + détection active + pas en cooldown → envoyer notification
    unsigned long cooldownMs = (unsigned long)cfg.cooldown_min * 60 * 1000;
    if (currentSensorState == HIGH && !maintenanceMode && (millis() - lastTriggerLoop > cooldownMs)) {
      lastTriggerLoop = millis();
      Serial.printf("→ Trigger detected while awake — sending notification (cooldown: %d min)\n", cfg.cooldown_min);
      sendNotification(false);
    } else if (currentSensorState == HIGH && !maintenanceMode) {
      unsigned long remaining = (cooldownMs - (millis() - lastTriggerLoop)) / 1000;
      Serial.printf("→ Cooldown active — %lu sec remaining\n", remaining);
    }
  }

  // Real-time external button monitoring — log GPIO12 state changes
  static bool lastBtnState = HIGH;
  static unsigned long btnPressStart = 0;
  bool currentBtnState = digitalRead(PIN_EXT_BTN);
  if (currentBtnState != lastBtnState) {
    lastBtnState = currentBtnState;
    if (currentBtnState == LOW) {
      btnPressStart = millis();
      Serial.println("GPIO12 → button pressed");
    } else {
      unsigned long pressDuration = millis() - btnPressStart;
      Serial.printf("GPIO12 → button released after %lu ms\n", pressDuration);
      if (pressDuration >= EXT_BTN_LONG_MS) {
        Serial.println("→ Long press detected — charging mode activated");
        chargingMode = true;
      } else if (pressDuration >= EXT_BTN_SHORT_MS) {
        Serial.println("→ Short press detected — status page already active");
      }
    }
  }

  // Wizard timeout (AP mode)
  if (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA) {
    if (millis() - serverStart > WIZARD_TIMEOUT_MS) {
      Serial.println("Wizard timeout → sleep");
      goToSleep();
    }
  }

  // Status server timeout (STA mode)
  if (WiFi.getMode() == WIFI_STA) {
    if (millis() - serverStart > STATUS_SERVER_MS) {
      Serial.println("Status server timeout → sleep");
      if (chargingMode) {
        // En mode charge → deep sleep avec timer 5 min
        esp_sleep_enable_timer_wakeup((uint64_t)CHARGE_CHECK_MS * 1000ULL);
        esp_deep_sleep_start();
      } else {
        goToSleep();
      }
    }
  }
}

// ============================================================
// DEEP SLEEP
// ============================================================
void goToSleep() {
  Serial.println("→ Deep sleep... waiting for sensor");
  digitalWrite(PIN_STATUS_LED, LOW);
  // Réveil uniquement sur Reed Switch GPIO4 HIGH (aimant retiré = courrier)
  // Le bouton externe GPIO12 ne réveille pas en deep sleep
  // (il sert uniquement quand l'ESP32 est déjà éveillé)
  esp_sleep_enable_ext0_wakeup(PIN_SENSOR, HIGH);
  esp_deep_sleep_start();
}

// ============================================================
// NORMAL MODE — triggered by sensor
// ============================================================
void startNormal() {
  blinkLED(1, 100);

  bool wifiOk = connectWiFi();

  if (!wifiOk) {
    // WiFi failed → start wizard so user can fix credentials
    Serial.println("WiFi failed → starting wizard for reconfiguration");
    startWizard();
    return;
  }

  configTime(0, 0, NTP_SERVER);
  setenv("TZ", cfg.timezone, 1);
  tzset();
  struct tm timeinfo;
  uint32_t now = 0;
  if (getLocalTime(&timeinfo, 5000)) {
    now = mktime(&timeinfo);
    strftime(lastTriggerTime, sizeof(lastTriggerTime), "%d/%m/%Y %H:%M", &timeinfo);
  }

  uint32_t cooldownSec = (uint32_t)cfg.cooldown_min * 60;
  if (now > 0 && lastTriggerEpoch > 0 && (now - lastTriggerEpoch) < cooldownSec) {
    Serial.printf("Cooldown active (%u sec remaining)\n", cooldownSec - (now - lastTriggerEpoch));
    startStatusServer();
    return;
  }

  lastTriggerEpoch = now;
  totalTriggers++;
  handleDetection();
  startStatusServer();
}

// ============================================================
// CHARGING MODE — periodic battery check
// ============================================================
void startChargingMode() {
  Serial.println("🔋 Charging mode active");
  float bat = readBatteryPercent();
  Serial.printf("Battery: %.0f%%\n", bat);

  if (bat >= BATTERY_FULL_PCT) {
    Serial.println("🔋 Battery full! Sending notification...");
    bool wifiOk = connectWiFi();
    if (wifiOk) {
      configTime(0, 0, NTP_SERVER);
  setenv("TZ", cfg.timezone, 1);
  tzset();
      delay(1000);
      // Envoyer notification batterie pleine
      String origMsg = String(cfg.custom_msg);
      String fullMsg = cfg.language == 0
        ? "🔋 Batterie chargée ! Vous pouvez retirer le chargeur."
        : "🔋 Battery fully charged! You can remove the charger.";
      strncpy(cfg.custom_msg, fullMsg.c_str(), sizeof(cfg.custom_msg));
      sendNotification(false);
      strncpy(cfg.custom_msg, origMsg.c_str(), sizeof(cfg.custom_msg));
      lastNotifOk = true;
    }
    chargingMode = false;  // Désactiver le mode charge
    Serial.println("Charging mode disabled → back to normal");
    if (wifiOk) startStatusServer();
    else goToSleep();
  } else {
    Serial.printf("Battery at %.0f%% — not full yet, sleeping 5 min\n", bat);
    // Pas encore pleine → deep sleep 5 min
    esp_sleep_enable_timer_wakeup((uint64_t)CHARGE_CHECK_MS * 1000ULL);
    esp_deep_sleep_start();
  }
}
void handleDetection() {
  Serial.println("📬 Mail detected!");
  float bat = readBatteryPercent();
  Serial.printf("Battery: %.0f%%\n", bat);

  if (bat < BATTERY_LOW_PCT) {
    blinkLED(5, 100);
  } else {
    blinkLED(2, 200);
  }

  sendNotification(false);
  lastNotifOk = true;
}

// ============================================================
// STATUS SERVER — accessible via local IP after wake-up
// ============================================================
void startStatusServer() {
  Serial.printf("Status server active for %d sec → http://%s\n",
    STATUS_SERVER_MS / 1000, WiFi.localIP().toString().c_str());

  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html; charset=utf-8", htmlStatus());
  });

  server.on("/wizard", HTTP_GET, []() {
    server.send(200, "text/html; charset=utf-8", htmlWizard());
  });

  server.on("/save", HTTP_POST, []() {
    strncpy(cfg.wifi_ssid,   server.arg("wifi_ssid").c_str(),   sizeof(cfg.wifi_ssid));
    strncpy(cfg.wifi_pass,   server.arg("wifi_pass").c_str(),   sizeof(cfg.wifi_pass));
    cfg.sensor_type  = server.arg("sensor_type").toInt();
    cfg.language     = server.arg("language").toInt();
    cfg.cooldown_min = server.arg("cooldown_min").toInt();
    cfg.notif_method = 0;
    if (server.arg("notif_email")    == "1") cfg.notif_method |= NOTIF_EMAIL;
    if (server.arg("notif_ntfy")     == "1") cfg.notif_method |= NOTIF_NTFY;
    if (server.arg("notif_mqtt")     == "1") cfg.notif_method |= NOTIF_HA_MQTT;
    if (server.arg("notif_telegram") == "1") cfg.notif_method |= NOTIF_TELEGRAM;
    strncpy(cfg.custom_msg,  server.arg("custom_msg").c_str(),  sizeof(cfg.custom_msg));
    strncpy(cfg.timezone,    server.arg("timezone").c_str(),    sizeof(cfg.timezone));
    strncpy(cfg.smtp_host,   server.arg("smtp_host").c_str(),   sizeof(cfg.smtp_host));
    cfg.smtp_port = server.arg("smtp_port").toInt();
    strncpy(cfg.smtp_user,   server.arg("smtp_user").c_str(),   sizeof(cfg.smtp_user));
    strncpy(cfg.smtp_pass,   server.arg("smtp_pass").c_str(),   sizeof(cfg.smtp_pass));
    strncpy(cfg.smtp_to,     server.arg("smtp_to").c_str(),     sizeof(cfg.smtp_to));
    strncpy(cfg.ntfy_server, server.arg("ntfy_server").c_str(), sizeof(cfg.ntfy_server));
    strncpy(cfg.ntfy_topic,  server.arg("ntfy_topic").c_str(),  sizeof(cfg.ntfy_topic));
    strncpy(cfg.ntfy_token,  server.arg("ntfy_token").c_str(),  sizeof(cfg.ntfy_token));
    strncpy(cfg.mqtt_host,   server.arg("mqtt_host").c_str(),   sizeof(cfg.mqtt_host));
    cfg.mqtt_port = server.arg("mqtt_port").toInt();
    strncpy(cfg.mqtt_user,   server.arg("mqtt_user").c_str(),   sizeof(cfg.mqtt_user));
    strncpy(cfg.mqtt_pass,   server.arg("mqtt_pass").c_str(),   sizeof(cfg.mqtt_pass));
    strncpy(cfg.mqtt_topic,  server.arg("mqtt_topic").c_str(),  sizeof(cfg.mqtt_topic));
    strncpy(cfg.tg_token,    server.arg("tg_token").c_str(),    sizeof(cfg.tg_token));
    strncpy(cfg.tg_chat_id,  server.arg("tg_chat_id").c_str(),  sizeof(cfg.tg_chat_id));
    saveConfig();
    server.send(200, "text/html; charset=utf-8", htmlSuccess());
  });

  server.on("/test", HTTP_POST, []() {
    sendNotification(true);
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/sleep", HTTP_POST, []() {
    server.send(200, "text/plain", "Going to sleep...");
    delay(500);
    goToSleep();
  });

  server.on("/charge", HTTP_POST, []() {
    chargingMode = true;
    server.send(200, "application/json", "{\"ok\":true}");
    Serial.println("Charging mode enabled via status page");
  });

  server.on("/maintenance", HTTP_POST, []() {
    maintenanceMode = !maintenanceMode;
    String json = "{\"ok\":true,\"maintenance\":" + String(maintenanceMode ? "true" : "false") + "}";
    server.send(200, "application/json", json);
    Serial.printf("Maintenance mode: %s\n", maintenanceMode ? "ON" : "OFF");
  });

  server.on("/battery", HTTP_GET, []() {
    float pct = readBatteryPercent();
    server.send(200, "application/json", "{\"percent\":" + String((int)pct) + "}");
  });

  server.on("/uptime", HTTP_GET, []() {
    unsigned long elapsed = (millis() - serverStart) / 1000;
    unsigned long remaining = (STATUS_SERVER_MS / 1000) > elapsed ? (STATUS_SERVER_MS / 1000) - elapsed : 0;
    server.send(200, "application/json", "{\"remaining\":" + String(remaining) + "}");
  });

  server.on("/ping", HTTP_POST, []() {
    serverStart = millis();  // Reset timer — dashboard is open
    unsigned long remaining = STATUS_SERVER_MS / 1000;
    server.send(200, "application/json", "{\"remaining\":" + String(remaining) + "}");
    Serial.println("Dashboard ping — timer reset");
  });

  server.begin();
}

// ============================================================
// HTML STATUS PAGE
// ============================================================
String htmlStatus() {
  float  bat     = readBatteryPercent();
  int    rssi    = WiFi.RSSI();
  String batCol  = bat  > 50 ? "#30d68a" : bat  > 20 ? "#f0a500" : "#f04060";
  String rssiCol = rssi > -60 ? "#30d68a" : rssi > -75 ? "#f0a500" : "#f04060";
  String notifName = "";
  if (cfg.notif_method == NOTIF_NONE) { notifName = "⚠️ Non configuré"; }
  else {
    if (cfg.notif_method & NOTIF_EMAIL)    notifName += "📧 ";
    if (cfg.notif_method & NOTIF_NTFY)     notifName += "📣 ";
    if (cfg.notif_method & NOTIF_HA_MQTT)  notifName += "🏠 ";
    if (cfg.notif_method & NOTIF_TELEGRAM) notifName += "✈️ ";
    notifName.trim();
  }
  String sensorName = cfg.sensor_type == 0 ? "🧲 Reed Switch" : "👁️ PIR";

  String html = R"rawhtml(<!DOCTYPE html>
<html lang="fr">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<meta charset="utf-8">
<title>MailSense32 — Status</title>
<link rel="icon" type="image/svg+xml" href="data:image/svg+xml,%3Csvg width='32' height='32' viewBox='0 0 32 32' xmlns='http://www.w3.org/2000/svg'%3E%3Crect width='32' height='32' rx='6' fill='%230d0f14'/%3E%3Crect x='3' y='11' width='26' height='18' rx='2' fill='%23161920' stroke='%23f0a500' stroke-width='1.2'/%3E%3Cpath d='M3 14 L16 22 L29 14' fill='none' stroke='%23f0a500' stroke-width='1.2' stroke-linecap='round'/%3E%3Ccircle cx='16' cy='7' r='1.8' fill='%23f0a500'/%3E%3Cpath d='M11.5 7 Q16 3.5 20.5 7' fill='none' stroke='%23f0a500' stroke-width='1.2' stroke-linecap='round'/%3E%3Cpath d='M8.5 7 Q16 1 23.5 7' fill='none' stroke='%23e05c00' stroke-width='1' stroke-linecap='round' opacity='0.6'/%3E%3C/svg%3E">
<style>
@import url('https://fonts.googleapis.com/css2?family=Space+Mono:wght@400;700&family=DM+Sans:wght@300;400;500;600&display=swap');
:root{--bg:#0d0f14;--surface:#161920;--surface2:#1e2029;--border:#252830;--accent:#f0a500;--accent2:#e05c00;--text:#e8eaf0;--muted:#6b7080;--good:#30d68a;--bad:#f04060}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg);color:var(--text);font-family:'DM Sans',sans-serif;min-height:100vh;padding:20px 16px 48px}
.header{text-align:center;padding:24px 0 28px}
.logo{font-family:'Space Mono',monospace;font-size:20px;font-weight:700;background:linear-gradient(135deg,#f0a500,#e05c00);-webkit-background-clip:text;-webkit-text-fill-color:transparent}
.subtitle{font-size:10px;color:var(--muted);letter-spacing:2px;text-transform:uppercase;margin-top:3px}
.ip{font-family:'Space Mono',monospace;font-size:11px;color:var(--muted);margin-top:6px}
.card{background:var(--surface);border:1px solid var(--border);border-radius:14px;padding:18px;margin-bottom:12px}
.card-title{font-family:'Space Mono',monospace;font-size:10px;letter-spacing:2px;text-transform:uppercase;color:var(--accent);margin-bottom:14px}
.stat-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.stat{background:var(--bg);border:1px solid var(--border);border-radius:10px;padding:12px;text-align:center}
.stat-label{font-size:10px;color:var(--muted);letter-spacing:1px;text-transform:uppercase;font-family:'Space Mono',monospace;margin-bottom:4px}
.stat-val{font-family:'Space Mono',monospace;font-size:22px;font-weight:700}
.stat-unit{font-size:10px;color:var(--muted);margin-top:2px;font-family:'Space Mono',monospace}
.info-row{display:flex;align-items:center;justify-content:space-between;padding:9px 0;border-bottom:1px solid var(--border)}
.info-row:last-child{border-bottom:none}
.info-label{font-size:13px;color:var(--muted)}
.info-val{font-size:13px;font-weight:500;text-align:right;max-width:55%}
.ok{color:#30d68a}.fail{color:#f04060}
.countdown{text-align:center;font-family:'Space Mono',monospace;font-size:11px;color:var(--muted);padding:8px 0 4px}
.btn{width:100%;padding:13px;border-radius:12px;border:none;font-family:'DM Sans',sans-serif;font-size:14px;font-weight:600;cursor:pointer;margin-bottom:8px;display:flex;align-items:center;justify-content:center;gap:8px}
.btn-wizard{background:linear-gradient(135deg,var(--accent),var(--accent2));color:#000}
.btn-test{background:rgba(48,214,138,.08);border:1px solid #30d68a;color:#30d68a}
.btn-charge{background:rgba(74,184,240,.08);border:1px solid #4ab8f0;color:#4ab8f0}
.btn-maintenance{background:rgba(240,96,96,.12);border:1px solid #f04060;color:#f04060}
.btn-maintenance.active{background:rgba(48,214,138,.08);border:1px solid #30d68a;color:#30d68a}
.btn-sleep{background:transparent;border:1px solid var(--border);color:var(--muted)}
.toast{position:fixed;bottom:24px;left:50%;transform:translateX(-50%) translateY(100px);background:var(--surface);border:1px solid var(--border);border-radius:8px;padding:10px 18px;font-size:13px;transition:transform .3s;z-index:100;white-space:nowrap}
.toast.show{transform:translateX(-50%) translateY(0)}
.toast.ok{border-color:#30d68a;color:#30d68a}
.toast.err{border-color:#f04060;color:#f04060}
</style>
</head>
<body>
<div class="header">
  <div class="logo">MailSense32</div>
  <div class="subtitle">Status Monitor</div>
  <div class="ip">)rawhtml";

  html += WiFi.localIP().toString();
  html += R"rawhtml( · v)rawhtml";
  html += FIRMWARE_VERSION;
  html += R"rawhtml(</div>
</div>

<div class="card">
  <div class="card-title">📊 État du système</div>
  <div class="stat-grid">
    <div class="stat">
      <div class="stat-label">Batterie</div>
      <div class="stat-val" style="color:)rawhtml";
  html += batCol + "\">" + String((int)bat);
  html += R"rawhtml(</div><div class="stat-unit">%</div>
    </div>
    <div class="stat">
      <div class="stat-label">WiFi RSSI</div>
      <div class="stat-val" style="color:)rawhtml";
  html += rssiCol + "\">" + String(rssi);
  html += R"rawhtml(</div><div class="stat-unit">dBm</div>
    </div>
  </div>
</div>

<div class="card">
  <div class="card-title">📬 Dernière détection</div>
  <div class="info-row">
    <span class="info-label">Heure</span>
    <span class="info-val">)rawhtml";
  html += strlen(lastTriggerTime) > 0 ? String(lastTriggerTime) : "—";
  html += R"rawhtml(</span></div>
  <div class="info-row">
    <span class="info-label">Total déclenchements</span>
    <span class="info-val">)rawhtml";
  html += String(totalTriggers);
  html += R"rawhtml(</span></div>
  <div class="info-row">
    <span class="info-label">Dernière notification</span>
    <span class="info-val )rawhtml";
  html += lastNotifOk ? "ok\">✅ Envoyée" : "fail\">❌ Échec";
  html += R"rawhtml(</span></div>
</div>

<div class="card">
  <div class="card-title">⚙️ Configuration active</div>
  <div class="info-row">
    <span class="info-label">Réseau WiFi</span>
    <span class="info-val">)rawhtml";
  html += String(cfg.wifi_ssid);
  html += R"rawhtml(</span></div>
  <div class="info-row">
    <span class="info-label">Capteur</span>
    <span class="info-val">)rawhtml";
  html += sensorName;
  html += R"rawhtml(</span></div>
  <div class="info-row">
    <span class="info-label">Notification</span>
    <span class="info-val">)rawhtml";
  html += notifName;
  html += R"rawhtml(</span></div>
  <div class="info-row">
    <span class="info-label">Anti-doublon</span>
    <span class="info-val">)rawhtml";
  html += String(cfg.cooldown_min);
  html += R"rawhtml( min</span></div>
</div>

<div class="card">
  <div class="card-title">🚀 Actions</div>
  <button class="btn btn-wizard" onclick="location.href='/wizard'">⚙️ Ouvrir le wizard</button>
  <button class="btn btn-test" onclick="sendTest()">🧪 Envoyer notification test</button>
  <button class="btn btn-charge" onclick="activateCharge()">🔋 Activer le mode charge</button>
  <button class="btn btn-maintenance" id="btn-maint" onclick="toggleMaintenance()">🔴 Détection désactivée</button>
  <button class="btn btn-sleep" onclick="sleep()">😴 Mettre en veille maintenant</button>
  <div class="countdown">Auto-refresh 30s · Veille dans <span id="t">)rawhtml";
  html += String(STATUS_SERVER_MS / 1000);
  html += R"rawhtml(</span>s</div>
</div>

<div id="toast" class="toast"></div>
<div style="text-align:center;padding:16px 0 4px;font-size:11px;color:var(--muted);font-family:'Space Mono',monospace">
  MailSense32 v)rawhtml";
  html += FIRMWARE_VERSION;
  html += R"rawhtml( · <a href="https://egamaker.be" style="color:var(--accent);text-decoration:none">Egalistel / egamaker.be</a>
</div>
<script>
let s=)rawhtml";
  html += String(STATUS_SERVER_MS / 1000);
  html += R"rawhtml(;
// Fetch real remaining time from ESP32 on load
fetch('/uptime').then(r=>r.json()).then(d=>{s=d.remaining;document.getElementById('t').textContent=s;}).catch(()=>{});
setInterval(()=>{if(s>0){s--;document.getElementById('t').textContent=s;}},1000);
// Ping ESP32 every 25s to reset sleep timer while dashboard is open
setInterval(async()=>{
  try{
    const r=await fetch('/ping',{method:'POST'});
    const d=await r.json();
    s=d.remaining;
    document.getElementById('t').textContent=s;
  }catch(e){}
},25000);
function toast(m,t){const e=document.getElementById('toast');e.textContent=m;e.className='toast '+t;setTimeout(()=>e.classList.add('show'),10);setTimeout(()=>e.classList.remove('show'),3000);}
async function sendTest(){toast('⏳ Envoi...','');try{const r=await fetch('/test',{method:'POST'});const d=await r.json();toast(d.ok?'✅ Envoyée !':'❌ Échec',d.ok?'ok':'err');}catch(e){toast('❌ Erreur','err');}}
async function sleep(){await fetch('/sleep',{method:'POST'});toast('😴 Mise en veille...','');}
async function activateCharge(){
  try{
    const r=await fetch('/charge',{method:'POST'});
    const d=await r.json();
    if(d.ok){
      toast('🔋 Mode charge activé ! Vérification toutes les 5 min.','ok');
      document.querySelector('.btn-charge').textContent='✅ Mode charge actif';
      document.querySelector('.btn-charge').disabled=true;
    }
  }catch(e){toast('❌ Erreur','err');}
}
async function toggleMaintenance(){
  try{
    const r=await fetch('/maintenance',{method:'POST'});
    const d=await r.json();
    if(d.ok){
      const btn=document.getElementById('btn-maint');
      if(d.maintenance){
        btn.textContent='🔴 Détection désactivée';
        btn.classList.remove('active');
        toast('🔴 Détection désactivée — aucune notification','err');
      } else {
        btn.textContent='🟢 Détection active';
        btn.classList.add('active');
        toast('🟢 Détection activée — prêt !','ok');
      }
    }
  }catch(e){toast('❌ Erreur','err');}
}
// Init button state on page load
(function(){
  const btn=document.getElementById('btn-maint');
  if()rawhtml";
  html += maintenanceMode ? "false" : "true";
  html += R"rawhtml(){
    btn.textContent='🟢 Détection active';
    btn.classList.add('active');
  }
})();
</script>
</body>
</html>)rawhtml";

  return html;
}

// ============================================================
// WIFI
// ============================================================
bool connectWiFi() {
  if (strlen(cfg.wifi_ssid) == 0) return false;

  Serial.printf("Connecting to %s ", cfg.wifi_ssid);
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_19_5dBm); // Puissance MAX pour boîte aux lettres éloignée
  WiFi.setSleep(false);                 // Désactive power save WiFi
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  WiFi.begin(cfg.wifi_ssid, cfg.wifi_pass);

  for (int i = 0; i < WIFI_MAX_RETRIES * 10; i++) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("\nConnected! IP: %s\n", WiFi.localIP().toString().c_str());
      return true;
    }
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi failed!");
  return false;
}

// ============================================================
// BATTERY
// ============================================================
float readBatteryPercent() {
  int raw = analogRead(PIN_BATTERY_ADC);
  float voltage = raw * (3.3 / 4095.0) * ((BATTERY_R1 + BATTERY_R2) / BATTERY_R2);
  float pct = ((voltage - BATTERY_MIN_V) / (BATTERY_MAX_V - BATTERY_MIN_V)) * 100.0;
  return constrain(pct, 0.0, 100.0);
}

String getBatteryIcon(float pct) {
  if (pct > 75) return "🔋";
  if (pct > 50) return "🔋";
  if (pct > 25) return "🪫";
  return "⚠️";
}

// ============================================================
// TIME
// ============================================================
String getCurrentTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 3000)) return "??:??";
  char buf[32];
  strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M", &timeinfo);
  return String(buf);
}

// ============================================================
// NOTIFICATION DISPATCHER
// ============================================================
void sendNotification(bool isTest) {
  if (cfg.notif_method == NOTIF_NONE) {
    Serial.println("No notification method configured");
    lastNotifOk = false;
    return;
  }
  lastNotifOk = true;
  if (cfg.notif_method & NOTIF_EMAIL)    { Serial.println("→ Sending Email");    sendEmail(isTest);   }
  if (cfg.notif_method & NOTIF_NTFY)     { Serial.println("→ Sending Ntfy");     sendNtfy(isTest);    }
  if (cfg.notif_method & NOTIF_HA_MQTT)  { Serial.println("→ Sending HA MQTT");  sendHAMQTT(isTest);  }
  if (cfg.notif_method & NOTIF_TELEGRAM) { Serial.println("→ Sending Telegram"); sendTelegram(isTest);}
}

String buildMessage(bool isTest) {
  float bat = readBatteryPercent();
  String time = getCurrentTime();
  String msg = isTest ? "[TEST] " : "";
  msg += String(cfg.custom_msg);
  msg += "\n🕐 " + time;
  msg += "\n" + getBatteryIcon(bat) + " Battery: " + String((int)bat) + "%";
  if (bat < BATTERY_LOW_PCT) msg += " ⚠️ Low battery!";
  return msg;
}

// ============================================================
// EMAIL
// ============================================================
void sendEmail(bool isTest) {
  Serial.println("→ Sending Email...");
  SMTPSession smtp;
  smtp.debug(1);  // Debug SMTP dans le moniteur série
  ESP_Mail_Session session;
  session.server.host_name = cfg.smtp_host;
  session.server.port      = cfg.smtp_port;
  session.login.email      = cfg.smtp_user;
  session.login.password   = cfg.smtp_pass;
  session.login.user_domain = "";
  session.time.ntp_server  = NTP_SERVER;
  session.time.gmt_offset  = 1;
  session.time.day_light_offset = 1;

  SMTP_Message message;
  message.sender.name  = "MailSense32";
  message.sender.email = cfg.smtp_user;
  message.subject      = isTest ? "[TEST] MailSense32" : "📬 Nouveau courrier";
  message.addRecipient("", cfg.smtp_to);
  message.text.content = buildMessage(isTest).c_str();

  if (!smtp.connect(&session)) {
    Serial.println("Email connect failed: " + smtp.errorReason());
    return;
  }
  if (!MailClient.sendMail(&smtp, &message)) {
    Serial.println("Email send failed: " + smtp.errorReason());
  } else {
    Serial.println("Email sent!");
  }
}

// ============================================================
// NTFY
// ============================================================
void sendNtfy(bool isTest) {
  Serial.println("→ Sending Ntfy...");
  HTTPClient http;
  String url = String(cfg.ntfy_server) + "/" + String(cfg.ntfy_topic);
  http.begin(url);
  http.addHeader("Content-Type", "text/plain");
  http.addHeader("Title", isTest ? "MailSense32 TEST" : "MailSense32");
  http.addHeader("Priority", "high");
  http.addHeader("Tags", "mailbox,envelope");
  if (strlen(cfg.ntfy_token) > 0) {
    http.addHeader("Authorization", "Bearer " + String(cfg.ntfy_token));
  }
  int code = http.POST(buildMessage(isTest));
  Serial.printf("Ntfy response: %d\n", code);
  http.end();
}

// ============================================================
// HOME ASSISTANT MQTT
// ============================================================
void sendHAMQTT(bool isTest) {
  Serial.println("→ Sending MQTT...");
  mqttClient.setServer(cfg.mqtt_host, cfg.mqtt_port);
  if (strlen(cfg.mqtt_user) > 0) {
    mqttClient.connect("MailSense32", cfg.mqtt_user, cfg.mqtt_pass);
  } else {
    mqttClient.connect("MailSense32");
  }
  if (!mqttClient.connected()) {
    Serial.println("MQTT connect failed");
    return;
  }

  float bat = readBatteryPercent();
  StaticJsonDocument<256> doc;
  doc["state"]    = "triggered";
  doc["message"]  = cfg.custom_msg;
  doc["time"]     = getCurrentTime();
  doc["battery"]  = (int)bat;
  doc["test"]     = isTest;

  char payload[256];
  serializeJson(doc, payload);
  mqttClient.publish(cfg.mqtt_topic, payload, false); // false = pas de retain → HA déclenche à chaque fois
  Serial.println("MQTT published!");
  mqttClient.disconnect();
}

// ============================================================
// TELEGRAM
// ============================================================
void sendTelegram(bool isTest) {
  Serial.println("→ Sending Telegram...");
  HTTPClient http;
  String url = "https://api.telegram.org/bot" + String(cfg.tg_token) + "/sendMessage";
  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<512> doc;
  doc["chat_id"]    = cfg.tg_chat_id;
  doc["text"]       = buildMessage(isTest);
  doc["parse_mode"] = "HTML";

  String payload;
  serializeJson(doc, payload);
  int code = http.POST(payload);
  Serial.printf("Telegram response: %d\n", code);
  http.end();
}

// ============================================================
// LED
// ============================================================
void blinkLED(int times, int ms) {
  for (int i = 0; i < times; i++) {
    digitalWrite(PIN_STATUS_LED, HIGH);
    delay(ms);
    digitalWrite(PIN_STATUS_LED, LOW);
    delay(ms);
  }
}

// ============================================================
// CONFIG — NVS
// ============================================================
void loadConfig() {
  prefs.begin(NVS_NAMESPACE, true);
  cfg.configured    = prefs.getBool("configured", false);
  cfg.sensor_type   = prefs.getUChar("sensor_type", SENSOR_REED);
  cfg.language      = prefs.getUChar("language", LANG_FR);
  cfg.cooldown_min  = prefs.getUShort("cooldown_min", DEFAULT_COOLDOWN_MIN);
  cfg.notif_method  = prefs.getUChar("notif_method", NOTIF_NONE);

  prefs.getString("wifi_ssid",   cfg.wifi_ssid,   sizeof(cfg.wifi_ssid));
  prefs.getString("wifi_pass",   cfg.wifi_pass,   sizeof(cfg.wifi_pass));
  prefs.getString("custom_msg",  cfg.custom_msg,  sizeof(cfg.custom_msg));
  prefs.getString("timezone",    cfg.timezone,    sizeof(cfg.timezone));
  if (strlen(cfg.timezone) == 0) strcpy(cfg.timezone, "CET-1CEST,M3.5.0,M10.5.0/3");

  prefs.getString("smtp_host",   cfg.smtp_host,   sizeof(cfg.smtp_host));
  cfg.smtp_port = prefs.getUShort("smtp_port", 587);
  prefs.getString("smtp_user",   cfg.smtp_user,   sizeof(cfg.smtp_user));
  prefs.getString("smtp_pass",   cfg.smtp_pass,   sizeof(cfg.smtp_pass));
  prefs.getString("smtp_to",     cfg.smtp_to,     sizeof(cfg.smtp_to));

  prefs.getString("ntfy_server", cfg.ntfy_server, sizeof(cfg.ntfy_server));
  prefs.getString("ntfy_topic",  cfg.ntfy_topic,  sizeof(cfg.ntfy_topic));
  prefs.getString("ntfy_token",  cfg.ntfy_token,  sizeof(cfg.ntfy_token));

  prefs.getString("mqtt_host",   cfg.mqtt_host,   sizeof(cfg.mqtt_host));
  cfg.mqtt_port = prefs.getUShort("mqtt_port", 1883);
  prefs.getString("mqtt_user",   cfg.mqtt_user,   sizeof(cfg.mqtt_user));
  prefs.getString("mqtt_pass",   cfg.mqtt_pass,   sizeof(cfg.mqtt_pass));
  prefs.getString("mqtt_topic",  cfg.mqtt_topic,  sizeof(cfg.mqtt_topic));

  prefs.getString("tg_token",    cfg.tg_token,    sizeof(cfg.tg_token));
  prefs.getString("tg_chat_id",  cfg.tg_chat_id,  sizeof(cfg.tg_chat_id));

  prefs.end();

  if (strlen(cfg.custom_msg) == 0) {
    strcpy(cfg.custom_msg, cfg.language == LANG_FR ? DEFAULT_MESSAGE_FR : DEFAULT_MESSAGE_EN);
  }
  Serial.printf("Config loaded. Configured: %s\n", cfg.configured ? "YES" : "NO");
}

void saveConfig() {
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putBool("configured",   true);
  prefs.putUChar("sensor_type", cfg.sensor_type);
  prefs.putUChar("language",    cfg.language);
  prefs.putUShort("cooldown_min", cfg.cooldown_min);
  prefs.putUChar("notif_method",  cfg.notif_method);

  prefs.putString("wifi_ssid",   cfg.wifi_ssid);
  prefs.putString("wifi_pass",   cfg.wifi_pass);
  prefs.putString("custom_msg",  cfg.custom_msg);
  prefs.putString("timezone",    cfg.timezone);

  prefs.putString("smtp_host",   cfg.smtp_host);
  prefs.putUShort("smtp_port",   cfg.smtp_port);
  prefs.putString("smtp_user",   cfg.smtp_user);
  prefs.putString("smtp_pass",   cfg.smtp_pass);
  prefs.putString("smtp_to",     cfg.smtp_to);

  prefs.putString("ntfy_server", cfg.ntfy_server);
  prefs.putString("ntfy_topic",  cfg.ntfy_topic);
  prefs.putString("ntfy_token",  cfg.ntfy_token);

  prefs.putString("mqtt_host",   cfg.mqtt_host);
  prefs.putUShort("mqtt_port",   cfg.mqtt_port);
  prefs.putString("mqtt_user",   cfg.mqtt_user);
  prefs.putString("mqtt_pass",   cfg.mqtt_pass);
  prefs.putString("mqtt_topic",  cfg.mqtt_topic);

  prefs.putString("tg_token",    cfg.tg_token);
  prefs.putString("tg_chat_id",  cfg.tg_chat_id);

  prefs.end();
  Serial.println("Config saved.");
}

// ============================================================
// WIZARD MODE
// ============================================================
void startWizard() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.printf("AP started: %s\n", AP_SSID);
  Serial.printf("IP: %s\n", WiFi.softAPIP().toString().c_str());

  blinkLED(5, 100);

  setupRoutes();
  server.begin();
  Serial.println("Web server started");
}

// ============================================================
// WEB ROUTES
// ============================================================
void setupRoutes() {

  // Main wizard page
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html; charset=utf-8", htmlWizard());
  });

  // Save config
  server.on("/save", HTTP_POST, []() {
    // WiFi
    strncpy(cfg.wifi_ssid, server.arg("wifi_ssid").c_str(), sizeof(cfg.wifi_ssid));
    strncpy(cfg.wifi_pass, server.arg("wifi_pass").c_str(), sizeof(cfg.wifi_pass));

    // General
    cfg.sensor_type  = server.arg("sensor_type").toInt();
    cfg.language     = server.arg("language").toInt();
    cfg.cooldown_min = server.arg("cooldown_min").toInt();
    cfg.notif_method = 0;
    if (server.arg("notif_email")    == "1") cfg.notif_method |= NOTIF_EMAIL;
    if (server.arg("notif_ntfy")     == "1") cfg.notif_method |= NOTIF_NTFY;
    if (server.arg("notif_mqtt")     == "1") cfg.notif_method |= NOTIF_HA_MQTT;
    if (server.arg("notif_telegram") == "1") cfg.notif_method |= NOTIF_TELEGRAM;
    strncpy(cfg.custom_msg, server.arg("custom_msg").c_str(), sizeof(cfg.custom_msg));
    strncpy(cfg.timezone,   server.arg("timezone").c_str(),   sizeof(cfg.timezone));

    // Email
    strncpy(cfg.smtp_host, server.arg("smtp_host").c_str(), sizeof(cfg.smtp_host));
    cfg.smtp_port = server.arg("smtp_port").toInt();
    strncpy(cfg.smtp_user, server.arg("smtp_user").c_str(), sizeof(cfg.smtp_user));
    strncpy(cfg.smtp_pass, server.arg("smtp_pass").c_str(), sizeof(cfg.smtp_pass));
    strncpy(cfg.smtp_to,   server.arg("smtp_to").c_str(),   sizeof(cfg.smtp_to));

    // Ntfy
    strncpy(cfg.ntfy_server, server.arg("ntfy_server").c_str(), sizeof(cfg.ntfy_server));
    strncpy(cfg.ntfy_topic,  server.arg("ntfy_topic").c_str(),  sizeof(cfg.ntfy_topic));
    strncpy(cfg.ntfy_token,  server.arg("ntfy_token").c_str(),  sizeof(cfg.ntfy_token));

    // MQTT
    strncpy(cfg.mqtt_host,  server.arg("mqtt_host").c_str(),  sizeof(cfg.mqtt_host));
    cfg.mqtt_port = server.arg("mqtt_port").toInt();
    strncpy(cfg.mqtt_user,  server.arg("mqtt_user").c_str(),  sizeof(cfg.mqtt_user));
    strncpy(cfg.mqtt_pass,  server.arg("mqtt_pass").c_str(),  sizeof(cfg.mqtt_pass));
    strncpy(cfg.mqtt_topic, server.arg("mqtt_topic").c_str(), sizeof(cfg.mqtt_topic));

    // Telegram
    strncpy(cfg.tg_token,   server.arg("tg_token").c_str(),   sizeof(cfg.tg_token));
    strncpy(cfg.tg_chat_id, server.arg("tg_chat_id").c_str(), sizeof(cfg.tg_chat_id));

    saveConfig();
    server.send(200, "text/html; charset=utf-8", htmlSuccess());
  });

  // Test notification (always available)
  server.on("/test", HTTP_POST, []() {
    // Try to connect WiFi first
    bool connected = connectWiFi();
    if (!connected) {
      server.send(200, "application/json", "{\"ok\":false,\"msg\":\"WiFi connection failed\"}");
      return;
    }
    configTime(0, 0, NTP_SERVER);
  setenv("TZ", cfg.timezone, 1);
  tzset();
    delay(1000);
    sendNotification(true);
    server.send(200, "application/json", "{\"ok\":true,\"msg\":\"Test notification sent!\"}");
  });

  // Battery status
  server.on("/battery", HTTP_GET, []() {
    float pct = readBatteryPercent();
    String json = "{\"percent\":" + String((int)pct) + "}";
    server.send(200, "application/json", json);
  });

  // Reboot to normal mode
  server.on("/reboot", HTTP_POST, []() {
    server.send(200, "text/plain", "Rebooting...");
    delay(500);
    ESP.restart();
  });
}

// ============================================================
// HTML WIZARD
// ============================================================
String htmlWizard() {
  float bat = readBatteryPercent();
  String html = R"rawhtml(
<!DOCTYPE html>
<html lang="fr">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>MailSense32 — Setup</title>
<link rel="icon" type="image/svg+xml" href="data:image/svg+xml,%3Csvg width='32' height='32' viewBox='0 0 32 32' xmlns='http://www.w3.org/2000/svg'%3E%3Crect width='32' height='32' rx='6' fill='%230d0f14'/%3E%3Crect x='3' y='11' width='26' height='18' rx='2' fill='%23161920' stroke='%23f0a500' stroke-width='1.2'/%3E%3Cpath d='M3 14 L16 22 L29 14' fill='none' stroke='%23f0a500' stroke-width='1.2' stroke-linecap='round'/%3E%3Ccircle cx='16' cy='7' r='1.8' fill='%23f0a500'/%3E%3Cpath d='M11.5 7 Q16 3.5 20.5 7' fill='none' stroke='%23f0a500' stroke-width='1.2' stroke-linecap='round'/%3E%3Cpath d='M8.5 7 Q16 1 23.5 7' fill='none' stroke='%23e05c00' stroke-width='1' stroke-linecap='round' opacity='0.6'/%3E%3C/svg%3E">
<style>
  @import url('https://fonts.googleapis.com/css2?family=Space+Mono:wght@400;700&family=DM+Sans:wght@300;400;500;600&display=swap');

  :root {
    --bg:       #0d0f14;
    --surface:  #161920;
    --border:   #252830;
    --accent:   #f0a500;
    --accent2:  #e05c00;
    --text:     #e8eaf0;
    --muted:    #6b7080;
    --success:  #30d68a;
    --error:    #f04060;
    --radius:   12px;
  }

  * { box-sizing: border-box; margin: 0; padding: 0; }

  body {
    background: var(--bg);
    color: var(--text);
    font-family: 'DM Sans', sans-serif;
    font-size: 15px;
    min-height: 100vh;
    padding: 24px 16px 60px;
  }

  .header {
    text-align: center;
    padding: 32px 0 40px;
  }

  .logo {
    font-family: 'Space Mono', monospace;
    font-size: 28px;
    font-weight: 700;
    background: linear-gradient(135deg, var(--accent), var(--accent2));
    -webkit-background-clip: text;
    -webkit-text-fill-color: transparent;
    letter-spacing: -1px;
  }
  .tagline { color: var(--muted); font-size: 13px; margin-top: 6px; letter-spacing: 2px; text-transform: uppercase; }

  .battery-bar {
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 8px;
    margin-top: 16px;
    font-size: 13px;
    color: var(--muted);
  }

  .bat-icon { font-size: 18px; }

  .container { max-width: 540px; margin: 0 auto; }

  .card {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: var(--radius);
    padding: 24px;
    margin-bottom: 16px;
  }

  .card-title {
    font-family: 'Space Mono', monospace;
    font-size: 11px;
    text-transform: uppercase;
    letter-spacing: 2px;
    color: var(--accent);
    margin-bottom: 20px;
    display: flex;
    align-items: center;
    gap: 8px;
  }

  .card-title::after {
    content: '';
    flex: 1;
    height: 1px;
    background: var(--border);
  }

  label {
    display: block;
    font-size: 12px;
    color: var(--muted);
    text-transform: uppercase;
    letter-spacing: 1px;
    margin-bottom: 6px;
    margin-top: 16px;
  }

  label:first-of-type { margin-top: 0; }

  input[type="text"],
  input[type="password"],
  input[type="email"],
  input[type="number"],
  select,
  textarea {
    width: 100%;
    background: var(--bg);
    border: 1px solid var(--border);
    border-radius: 8px;
    color: var(--text);
    font-family: 'DM Sans', sans-serif;
    font-size: 14px;
    padding: 10px 14px;
    transition: border-color 0.2s;
    appearance: none;
  }

  input:focus, select:focus, textarea:focus {
    outline: none;
    border-color: var(--accent);
  }

  select {
    background-image: url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='12' height='8' fill='none'%3E%3Cpath d='M1 1l5 5 5-5' stroke='%236b7080' stroke-width='1.5' stroke-linecap='round'/%3E%3C/svg%3E");
    background-repeat: no-repeat;
    background-position: right 14px center;
    padding-right: 36px;
  }

  textarea { resize: vertical; min-height: 70px; }

  .notif-tabs {
    display: flex;
    gap: 8px;
    flex-wrap: wrap;
    margin-bottom: 20px;
  }

  .tab-btn {
    padding: 8px 16px;
    border-radius: 20px;
    border: 1px solid var(--border);
    background: transparent;
    color: var(--muted);
    font-family: 'DM Sans', sans-serif;
    font-size: 13px;
    cursor: pointer;
    transition: all 0.2s;
  }

  .tab-btn.active, .tab-btn:hover {
    border-color: var(--accent);
    color: var(--accent);
    background: rgba(240,165,0,0.08);
  }

  .notif-panel { display: block; margin-top: 8px; }
  .notif-panel[style*="display:none"] { display: none !important; }

  .btn {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    gap: 8px;
    padding: 12px 24px;
    border-radius: 8px;
    border: none;
    font-family: 'DM Sans', sans-serif;
    font-size: 14px;
    font-weight: 600;
    cursor: pointer;
    transition: all 0.2s;
    width: 100%;
    margin-top: 8px;
  }

  .btn-primary {
    background: linear-gradient(135deg, var(--accent), var(--accent2));
    color: #000;
  }

  .btn-primary:hover { opacity: 0.9; transform: translateY(-1px); }

  .btn-secondary {
    background: transparent;
    border: 1px solid var(--border);
    color: var(--muted);
  }

  .btn-secondary:hover { border-color: var(--accent); color: var(--accent); }

.notif-check{display:flex;align-items:center;gap:10px;padding:10px 12px;border-radius:8px;border:1px solid var(--border);cursor:pointer;font-size:14px}
.notif-check input[type=checkbox]{width:18px;height:18px;accent-color:var(--accent);cursor:pointer;flex-shrink:0}
.notif-check:has(input:checked){border-color:var(--accent);background:rgba(240,165,0,0.06)}
    background: rgba(48, 214, 138, 0.1);
    border: 1px solid var(--success);
    color: var(--success);
  }

  .btn-test:hover { background: rgba(48, 214, 138, 0.2); }

  .toast {
    position: fixed;
    bottom: 24px;
    left: 50%;
    transform: translateX(-50%) translateY(100px);
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 12px 20px;
    font-size: 14px;
    transition: transform 0.3s ease;
    z-index: 100;
    white-space: nowrap;
  }

  .toast.show { transform: translateX(-50%) translateY(0); }
  .toast.success { border-color: var(--success); color: var(--success); }
  .toast.error   { border-color: var(--error); color: var(--error); }

  .hint {
    font-size: 11px;
    color: var(--muted);
    margin-top: 5px;
    line-height: 1.5;
  }

  .row2 { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; }

  .lang-toggle {
    display: flex;
    gap: 0;
    border: 1px solid var(--border);
    border-radius: 8px;
    overflow: hidden;
    width: fit-content;
  }

  .lang-btn {
    padding: 8px 18px;
    background: transparent;
    border: none;
    color: var(--muted);
    font-family: 'DM Sans', sans-serif;
    font-size: 13px;
    cursor: pointer;
    transition: all 0.2s;
  }

  .lang-btn.active {
    background: var(--accent);
    color: #000;
    font-weight: 600;
  }

  .version {
    text-align: center;
    font-size: 11px;
    color: var(--muted);
    margin-top: 32px;
    font-family: 'Space Mono', monospace;
  }

  .version a { color: var(--accent); text-decoration: none; }
</style>
</head>
<body>

<div class="container">

  <div class="header">
    <div class="logo">MailSense32</div>
    <div class="tagline">Smart Mailbox Detector — Setup Wizard</div>
    <div class="battery-bar">
      <span class="bat-icon">🔋</span>
      <span id="bat-pct">)rawhtml";

  html += String((int)bat) + R"rawhtml(% battery</span>
    </div>
  </div>

  <form id="mainForm" action="/save" method="POST">

    <!-- LANGUAGE -->
    <div class="card">
      <div class="card-title">🌐 Language / Langue</div>
      <div class="lang-toggle">
        <button type="button" class="lang-btn active" id="lang-fr" onclick="setLang(0)">Français</button>
        <button type="button" class="lang-btn" id="lang-en" onclick="setLang(1)">English</button>
      </div>
      <input type="hidden" name="language" id="language_input" value="0">
    </div>

    <!-- WIFI -->
    <div class="card">
      <div class="card-title">📶 WiFi</div>
      <label id="lbl-ssid">Nom du réseau (SSID)</label>
      <input type="text" name="wifi_ssid" id="wifi_ssid" value=")rawhtml";
  html += String(cfg.wifi_ssid);
  html += R"rawhtml(" placeholder="MonWiFi" autocomplete="off">
      <label id="lbl-wpass">Mot de passe</label>
      <input type="password" name="wifi_pass" id="wifi_pass" value=")rawhtml";
  html += String(cfg.wifi_pass);
  html += R"rawhtml(" placeholder="••••••••" autocomplete="off">
    </div>

    <!-- SENSOR -->
    <div class="card">
      <div class="card-title">🔍 Capteur / Sensor</div>
      <label id="lbl-sensor">Type de capteur</label>
      <select name="sensor_type" id="sensor_type">
        <option value="0")rawhtml";
  html += (cfg.sensor_type == 0 ? " selected" : "");
  html += R"rawhtml(>Reed Switch (aimant / magnet)</option>
        <option value="1")rawhtml";
  html += (cfg.sensor_type == 1 ? " selected" : "");
  html += R"rawhtml(>PIR (mouvement / motion)</option>
      </select>
      <p class="hint" id="hint-sensor">Reed Switch : plus fiable, zéro consommation au repos. PIR : détection sans contact physique, orientez-le vers l'intérieur de la boîte.</p>
    </div>

    <!-- GENERAL -->
    <div class="card">
      <div class="card-title">⚙️ Général / General</div>
      <label id="lbl-msg">Message de notification</label>
      <textarea name="custom_msg" id="custom_msg" placeholder="📬 Vous avez du courrier !">)rawhtml";
  html += String(cfg.custom_msg);
  html += R"rawhtml(</textarea>
      <p class="hint" id="hint-msg">Ce message sera envoyé avec l'heure et le niveau de batterie.</p>
      <label id="lbl-cooldown">Délai anti-doublon (minutes)</label>
      <input type="number" name="cooldown_min" id="cooldown_min" value=")rawhtml";
  html += String(cfg.cooldown_min);
  html += R"rawhtml(" min="1" max="120">
      <p class="hint" id="hint-cooldown">Délai minimum entre deux notifications. Défaut : 5 minutes.</p>
      <label id="lbl-tz">Fuseau horaire</label>
      <select name="timezone" id="timezone">
        <optgroup label="─── Europe ───">
          <option value="GMT0BST,M3.5.0/1,M10.5.0" )rawhtml"; html += (String(cfg.timezone) == "GMT0BST,M3.5.0/1,M10.5.0" ? "selected" : ""); html += R"rawhtml(>UK / Irlande (GMT/BST)</option>
          <option value="CET-1CEST,M3.5.0,M10.5.0/3" )rawhtml"; html += (String(cfg.timezone) == "CET-1CEST,M3.5.0,M10.5.0/3" ? "selected" : ""); html += R"rawhtml(>Belgique / France / Espagne / Pays-Bas (CET)</option>
          <option value="CET-1CEST,M3.5.0,M10.5.0/3" )rawhtml"; html += R"rawhtml(>Allemagne / Italie / Suisse / Autriche (CET)</option>
          <option value="EET-2EEST,M3.5.0/3,M10.5.0/4" )rawhtml"; html += (String(cfg.timezone) == "EET-2EEST,M3.5.0/3,M10.5.0/4" ? "selected" : ""); html += R"rawhtml(>Grèce / Finlande / Roumanie (EET)</option>
          <option value="MSK-3" )rawhtml"; html += (String(cfg.timezone) == "MSK-3" ? "selected" : ""); html += R"rawhtml(>Russie / Moscou (MSK)</option>
        </optgroup>
        <optgroup label="─── Afrique ───">
          <option value="WAT-1" )rawhtml"; html += (String(cfg.timezone) == "WAT-1" ? "selected" : ""); html += R"rawhtml(>Afrique de l'Ouest (WAT)</option>
          <option value="CAT-2" )rawhtml"; html += (String(cfg.timezone) == "CAT-2" ? "selected" : ""); html += R"rawhtml(>Afrique Centrale (CAT)</option>
          <option value="EAT-3" )rawhtml"; html += (String(cfg.timezone) == "EAT-3" ? "selected" : ""); html += R"rawhtml(>Afrique de l'Est (EAT)</option>
          <option value="SAST-2" )rawhtml"; html += (String(cfg.timezone) == "SAST-2" ? "selected" : ""); html += R"rawhtml(>Afrique du Sud (SAST)</option>
        </optgroup>
        <optgroup label="─── Amériques ───">
          <option value="EST5EDT,M3.2.0,M11.1.0" )rawhtml"; html += (String(cfg.timezone) == "EST5EDT,M3.2.0,M11.1.0" ? "selected" : ""); html += R"rawhtml(>USA Est / New York (EST/EDT)</option>
          <option value="CST6CDT,M3.2.0,M11.1.0" )rawhtml"; html += (String(cfg.timezone) == "CST6CDT,M3.2.0,M11.1.0" ? "selected" : ""); html += R"rawhtml(>USA Centre / Chicago (CST/CDT)</option>
          <option value="MST7MDT,M3.2.0,M11.1.0" )rawhtml"; html += (String(cfg.timezone) == "MST7MDT,M3.2.0,M11.1.0" ? "selected" : ""); html += R"rawhtml(>USA Montagne / Denver (MST/MDT)</option>
          <option value="PST8PDT,M3.2.0,M11.1.0" )rawhtml"; html += (String(cfg.timezone) == "PST8PDT,M3.2.0,M11.1.0" ? "selected" : ""); html += R"rawhtml(>USA Ouest / Los Angeles (PST/PDT)</option>
          <option value="AST4ADT,M3.2.0,M11.1.0" )rawhtml"; html += (String(cfg.timezone) == "AST4ADT,M3.2.0,M11.1.0" ? "selected" : ""); html += R"rawhtml(>Canada Est / Halifax (AST/ADT)</option>
          <option value="EST5" )rawhtml"; html += (String(cfg.timezone) == "EST5" ? "selected" : ""); html += R"rawhtml(>Panama / Colombie (EST)</option>
          <option value="BRT3BRST" )rawhtml"; html += (String(cfg.timezone) == "BRT3BRST" ? "selected" : ""); html += R"rawhtml(>Brésil / Sao Paulo (BRT)</option>
          <option value="ART3" )rawhtml"; html += (String(cfg.timezone) == "ART3" ? "selected" : ""); html += R"rawhtml(>Argentine (ART)</option>
          <option value="CLT4CLST,M10.2.6/24,M3.2.6/24" )rawhtml"; html += (String(cfg.timezone) == "CLT4CLST,M10.2.6/24,M3.2.6/24" ? "selected" : ""); html += R"rawhtml(>Chili (CLT)</option>
        </optgroup>
        <optgroup label="─── Asie ───">
          <option value="IST-5:30" )rawhtml"; html += (String(cfg.timezone) == "IST-5:30" ? "selected" : ""); html += R"rawhtml(>Inde (IST)</option>
          <option value="CST-8" )rawhtml"; html += (String(cfg.timezone) == "CST-8" ? "selected" : ""); html += R"rawhtml(>Chine / Singapour / Taïwan (CST)</option>
          <option value="JST-9" )rawhtml"; html += (String(cfg.timezone) == "JST-9" ? "selected" : ""); html += R"rawhtml(>Japon / Corée (JST)</option>
          <option value="ICT-7" )rawhtml"; html += (String(cfg.timezone) == "ICT-7" ? "selected" : ""); html += R"rawhtml(>Thaïlande / Vietnam (ICT)</option>
          <option value="WIB-7" )rawhtml"; html += (String(cfg.timezone) == "WIB-7" ? "selected" : ""); html += R"rawhtml(>Indonésie Ouest / Jakarta (WIB)</option>
          <option value="PKT-5" )rawhtml"; html += (String(cfg.timezone) == "PKT-5" ? "selected" : ""); html += R"rawhtml(>Pakistan (PKT)</option>
          <option value="AST-3" )rawhtml"; html += (String(cfg.timezone) == "AST-3" ? "selected" : ""); html += R"rawhtml(>Arabie Saoudite / Qatar (AST)</option>
          <option value="TRT-3" )rawhtml"; html += (String(cfg.timezone) == "TRT-3" ? "selected" : ""); html += R"rawhtml(>Turquie (TRT)</option>
        </optgroup>
        <optgroup label="─── Océanie ───">
          <option value="AEST-10AEDT,M10.1.0,M4.1.0/3" )rawhtml"; html += (String(cfg.timezone) == "AEST-10AEDT,M10.1.0,M4.1.0/3" ? "selected" : ""); html += R"rawhtml(>Australie Est / Sydney (AEST)</option>
          <option value="AWST-8" )rawhtml"; html += (String(cfg.timezone) == "AWST-8" ? "selected" : ""); html += R"rawhtml(>Australie Ouest / Perth (AWST)</option>
          <option value="NZST-12NZDT,M9.5.0,M4.1.0/3" )rawhtml"; html += (String(cfg.timezone) == "NZST-12NZDT,M9.5.0,M4.1.0/3" ? "selected" : ""); html += R"rawhtml(>Nouvelle-Zélande (NZST)</option>
        </optgroup>
        <optgroup label="─── UTC ───">
          <option value="UTC0" )rawhtml"; html += (String(cfg.timezone) == "UTC0" ? "selected" : ""); html += R"rawhtml(>UTC / GMT (pas de décalage)</option>
        </optgroup>
      </select>
      <p class="hint" id="hint-tz">Sélectionnez votre fuseau horaire pour afficher l'heure correcte dans les notifications.</p>
    </div>

    <!-- NOTIFICATION METHOD -->
    <div class="card">
      <div class="card-title">🔔 Notification</div>
      <label id="lbl-method">Méthodes (plusieurs choix possibles)</label>
      <div style="display:flex;flex-direction:column;gap:10px;margin:10px 0">
        <label class="notif-check">
          <input type="checkbox" name="notif_email" value="1" id="chk-email" )rawhtml";
  html += (cfg.notif_method & NOTIF_EMAIL ? "checked" : "");
  html += R"rawhtml( onchange="togglePanel('panel-email',this.checked)">
          <span>📧 Email SMTP</span>
        </label>
        <label class="notif-check">
          <input type="checkbox" name="notif_ntfy" value="1" id="chk-ntfy" )rawhtml";
  html += (cfg.notif_method & NOTIF_NTFY ? "checked" : "");
  html += R"rawhtml( onchange="togglePanel('panel-ntfy',this.checked)">
          <span>📣 Ntfy.sh</span>
        </label>
        <label class="notif-check">
          <input type="checkbox" name="notif_mqtt" value="1" id="chk-mqtt" )rawhtml";
  html += (cfg.notif_method & NOTIF_HA_MQTT ? "checked" : "");
  html += R"rawhtml( onchange="togglePanel('panel-mqtt',this.checked)">
          <span>🏠 Home Assistant MQTT</span>
        </label>
        <label class="notif-check">
          <input type="checkbox" name="notif_telegram" value="1" id="chk-telegram" )rawhtml";
  html += (cfg.notif_method & NOTIF_TELEGRAM ? "checked" : "");
  html += R"rawhtml( onchange="togglePanel('panel-telegram',this.checked)">
          <span>✈️ Telegram</span>
        </label>
      </div>

      <!-- EMAIL -->
      <div id="panel-email" class="notif-panel" )rawhtml";
  html += (cfg.notif_method & NOTIF_EMAIL ? "" : "style=\"display:none\"");
  html += R"rawhtml(>
        <div class="row2" style="margin-top:16px">
          <div>
            <label id="lbl-smtp-host">Serveur SMTP</label>
            <input type="text" name="smtp_host" value=")rawhtml";
  html += String(cfg.smtp_host);
  html += R"rawhtml(" placeholder="smtp.gmail.com">
          </div>
          <div>
            <label id="lbl-smtp-port">Port</label>
            <input type="number" name="smtp_port" value=")rawhtml";
  html += String(cfg.smtp_port);
  html += R"rawhtml(" placeholder="587">
          </div>
        </div>
        <label id="lbl-smtp-user">Utilisateur</label>
        <input type="email" name="smtp_user" value=")rawhtml";
  html += String(cfg.smtp_user);
  html += R"rawhtml(" placeholder="vous@gmail.com">
        <label id="lbl-smtp-pass">Mot de passe (App Password)</label>
        <input type="password" name="smtp_pass" value=")rawhtml";
  html += String(cfg.smtp_pass);
  html += R"rawhtml(">
        <label id="lbl-smtp-to">Destinataire</label>
        <input type="email" name="smtp_to" value=")rawhtml";
  html += String(cfg.smtp_to);
  html += R"rawhtml(" placeholder="vous@gmail.com">
        <p class="hint" id="hint-gmail">Gmail : activez la vérification en 2 étapes et créez un "App Password" dans votre compte Google.</p>
      </div>

      <!-- NTFY -->
      <div id="panel-ntfy" class="notif-panel" )rawhtml";
  html += (cfg.notif_method & NOTIF_NTFY ? "" : "style=\"display:none\"");
  html += R"rawhtml(>
        <label id="lbl-ntfy-server" style="margin-top:16px">Serveur Ntfy</label>
        <input type="text" name="ntfy_server" value=")rawhtml";
  html += String(cfg.ntfy_server);
  html += R"rawhtml(" placeholder="https://ntfy.sh">
        <p class="hint">Laissez https://ntfy.sh pour le service public, ou entrez l'adresse de votre instance auto-hébergée.</p>
        <label id="lbl-ntfy-topic">Topic (unique)</label>
        <input type="text" name="ntfy_topic" value=")rawhtml";
  html += String(cfg.ntfy_topic);
  html += R"rawhtml(" placeholder="mailsense-monnom-2024">
        <label id="lbl-ntfy-token">Token (optionnel, self-hosted)</label>
        <input type="text" name="ntfy_token" value=")rawhtml";
  html += String(cfg.ntfy_token);
  html += R"rawhtml(" placeholder="tk_xxxxxxx">
      </div>

      <!-- MQTT HA -->
      <div id="panel-mqtt" class="notif-panel" )rawhtml";
  html += (cfg.notif_method & NOTIF_HA_MQTT ? "" : "style=\"display:none\"");
  html += R"rawhtml(>
        <div class="row2" style="margin-top:16px">
          <div>
            <label id="lbl-mqtt-host">Broker MQTT</label>
            <input type="text" name="mqtt_host" value=")rawhtml";
  html += String(cfg.mqtt_host);
  html += R"rawhtml(" placeholder="192.168.0.x">
          </div>
          <div>
            <label id="lbl-mqtt-port">Port</label>
            <input type="number" name="mqtt_port" value=")rawhtml";
  html += String(cfg.mqtt_port);
  html += R"rawhtml(" placeholder="1883">
          </div>
        </div>
        <label id="lbl-mqtt-user">Utilisateur (optionnel)</label>
        <input type="text" name="mqtt_user" value=")rawhtml";
  html += String(cfg.mqtt_user);
  html += R"rawhtml(">
        <label id="lbl-mqtt-pass">Mot de passe (optionnel)</label>
        <input type="password" name="mqtt_pass" value=")rawhtml";
  html += String(cfg.mqtt_pass);
  html += R"rawhtml(">
        <label id="lbl-mqtt-topic">Topic MQTT</label>
        <input type="text" name="mqtt_topic" value=")rawhtml";
  html += String(cfg.mqtt_topic);
  html += R"rawhtml(" placeholder="mailsense32/mail">
      </div>

      <!-- TELEGRAM -->
      <div id="panel-telegram" class="notif-panel" )rawhtml";
  html += (cfg.notif_method & NOTIF_TELEGRAM ? "" : "style=\"display:none\"");
  html += R"rawhtml(>
        <label id="lbl-tg-token" style="margin-top:16px">Bot Token</label>
        <input type="text" name="tg_token" value=")rawhtml";
  html += String(cfg.tg_token);
  html += R"rawhtml(" placeholder="123456789:AAxxxxxx">
        <p class="hint">Créez un bot via @BotFather sur Telegram et copiez le token ici.</p>
        <label id="lbl-tg-chat">Chat ID</label>
        <input type="text" name="tg_chat_id" value=")rawhtml";
  html += String(cfg.tg_chat_id);
  html += R"rawhtml(" placeholder="123456789">
        <p class="hint">Envoyez un message à votre bot puis visitez : api.telegram.org/bot{TOKEN}/getUpdates</p>
      </div>
    </div>

    <!-- ACTIONS -->
    <div class="card">
      <div class="card-title">🚀 Actions</div>
      <button type="submit" class="btn btn-primary" id="btn-save">💾 Sauvegarder la configuration</button>
      <button type="button" class="btn btn-test" id="btn-test" onclick="sendTest()">🧪 Envoyer une notification test</button>
      <button type="button" class="btn btn-secondary" id="btn-reboot" onclick="reboot()">🔄 Redémarrer en mode normal</button>
    </div>

  </form>

  <div class="version">
    MailSense32 v)rawhtml";
  html += FIRMWARE_VERSION;
  html += R"rawhtml( — By <a href="https://egamaker.be" target="_blank">Egalistel / egamaker.be</a> — MIT License
  </div>

</div>

<div id="toast" class="toast"></div>

<script>
const t = {
  fr: {
    ssid:"Nom du réseau (SSID)", wpass:"Mot de passe",
    sensor:"Type de capteur", hint_sensor:"Reed Switch : plus fiable, zéro consommation au repos. PIR : détection sans contact physique, orientez-le vers l'intérieur de la boîte.",
    msg:"Message de notification", hint_msg:"Ce message sera envoyé avec l'heure et le niveau de batterie.",
    cooldown:"Délai anti-doublon (minutes)", hint_cooldown:"Délai minimum entre deux notifications. Défaut : 5 minutes.",
    method:"Méthodes de notification (plusieurs choix possibles)",
    smtp_host:"Serveur SMTP", smtp_port:"Port", smtp_user:"Utilisateur", smtp_pass:"Mot de passe (App Password)", smtp_to:"Destinataire",
    ntfy_server:"Serveur Ntfy", ntfy_topic:"Topic (unique)", ntfy_token:"Token (optionnel, self-hosted)",
    mqtt_host:"Broker MQTT", mqtt_port:"Port", mqtt_user:"Utilisateur (optionnel)", mqtt_pass:"Mot de passe (optionnel)", mqtt_topic:"Topic MQTT",
    tg_token:"Bot Token", tg_chat:"Chat ID",
    def_msg:"📬 Vous avez du courrier !",
    hint_gmail:"Gmail : activez la vérification en 2 étapes et créez un \"App Password\" dans votre compte Google.",
    btn_save:"💾 Sauvegarder la configuration",
    btn_test:"🧪 Envoyer une notification test",
    btn_reboot:"🔄 Redémarrer en mode normal"
  },
  en: {
    ssid:"Network name (SSID)", wpass:"Password",
    sensor:"Sensor type", hint_sensor:"Reed Switch: more reliable, zero standby power. PIR: contactless detection, point it toward the inside of the mailbox.",
    msg:"Notification message", hint_msg:"This message will be sent with the time and battery level.",
    cooldown:"Anti-spam delay (minutes)", hint_cooldown:"Minimum delay between two notifications. Default: 5 minutes.",
    method:"Notification methods (multiple choices allowed)",
    smtp_host:"SMTP Server", smtp_port:"Port", smtp_user:"Username", smtp_pass:"Password (App Password)", smtp_to:"Recipient",
    ntfy_server:"Ntfy Server", ntfy_topic:"Topic (unique)", ntfy_token:"Token (optional, self-hosted)",
    mqtt_host:"MQTT Broker", mqtt_port:"Port", mqtt_user:"Username (optional)", mqtt_pass:"Password (optional)", mqtt_topic:"MQTT Topic",
    tg_token:"Bot Token", tg_chat:"Chat ID",
    def_msg:"📬 You've got mail!",
    hint_gmail:"Gmail: enable 2-step verification and create an App Password in your Google account.",
    btn_save:"💾 Save configuration",
    btn_test:"🧪 Send test notification",
    btn_reboot:"🔄 Restart in normal mode"
  }
};

let lang = )rawhtml";
  html += String(cfg.language);
  html += R"rawhtml(;

function setLang(l) {
  lang = l;
  document.getElementById('language_input').value = l;
  document.getElementById('lang-fr').classList.toggle('active', l === 0);
  document.getElementById('lang-en').classList.toggle('active', l === 1);
  const d = l === 0 ? t.fr : t.en;
  document.getElementById('lbl-ssid').textContent      = d.ssid;
  document.getElementById('lbl-wpass').textContent     = d.wpass;
  document.getElementById('lbl-sensor').textContent    = d.sensor;
  document.getElementById('hint-sensor').textContent   = d.hint_sensor;
  document.getElementById('lbl-msg').textContent       = d.msg;
  document.getElementById('hint-msg').textContent      = d.hint_msg;
  document.getElementById('lbl-cooldown').textContent  = d.cooldown;
  document.getElementById('hint-cooldown').textContent = d.hint_cooldown;
  document.getElementById('lbl-method').textContent    = d.method;
  document.getElementById('lbl-smtp-host').textContent = d.smtp_host;
  document.getElementById('lbl-smtp-port').textContent = d.smtp_port;
  document.getElementById('lbl-smtp-user').textContent = d.smtp_user;
  document.getElementById('lbl-smtp-pass').textContent = d.smtp_pass;
  document.getElementById('lbl-smtp-to').textContent   = d.smtp_to;
  document.getElementById('lbl-ntfy-server').textContent = d.ntfy_server;
  document.getElementById('lbl-ntfy-topic').textContent  = d.ntfy_topic;
  document.getElementById('lbl-ntfy-token').textContent  = d.ntfy_token;
  document.getElementById('lbl-mqtt-host').textContent = d.mqtt_host;
  document.getElementById('lbl-mqtt-port').textContent = d.mqtt_port;
  document.getElementById('lbl-mqtt-user').textContent = d.mqtt_user;
  document.getElementById('lbl-mqtt-pass').textContent = d.mqtt_pass;
  document.getElementById('lbl-mqtt-topic').textContent= d.mqtt_topic;
  document.getElementById('lbl-tg-token').textContent  = d.tg_token;
  document.getElementById('lbl-tg-chat').textContent   = d.tg_chat;
  document.getElementById('hint-gmail').textContent    = d.hint_gmail;
  document.getElementById('btn-save').textContent   = d.btn_save;
  document.getElementById('btn-test').textContent   = d.btn_test;
  document.getElementById('btn-reboot').textContent = d.btn_reboot;
  const msgField = document.getElementById('custom_msg');
  if (!msgField.value || msgField.value === t.fr.def_msg || msgField.value === t.en.def_msg) {
    msgField.value = d.def_msg;
  }
}

function togglePanel(id, show) {
  const panel = document.getElementById(id);
  if (panel) panel.style.display = show ? 'block' : 'none';
}

function showToast(msg, type) {
  const el = document.getElementById('toast');
  el.textContent = msg;
  el.className = 'toast ' + type;
  setTimeout(() => el.classList.add('show'), 10);
  setTimeout(() => el.classList.remove('show'), 3500);
}

async function sendTest() {
  showToast(lang === 0 ? '⏳ Envoi en cours...' : '⏳ Sending...', '');
  try {
    const res = await fetch('/test', { method: 'POST' });
    const data = await res.json();
    showToast(data.ok ? (lang === 0 ? '✅ Notification envoyée !' : '✅ Notification sent!') : '❌ ' + data.msg, data.ok ? 'success' : 'error');
  } catch(e) {
    showToast('❌ Error: ' + e, 'error');
  }
}

async function reboot() {
  await fetch('/reboot', { method: 'POST' });
  showToast(lang === 0 ? '🔄 Redémarrage...' : '🔄 Rebooting...', '');
}

// Init
setLang(lang);

// Refresh battery every 30s
setInterval(async () => {
  const r = await fetch('/battery');
  const d = await r.json();
  document.getElementById('bat-pct').textContent = d.percent + '% battery';
}, 30000);
</script>
</body>
</html>
)rawhtml";

  return html;
}

// ============================================================
// HTML SUCCESS PAGE
// ============================================================
String htmlSuccess() {
  return R"rawhtml(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>MailSense32 — Saved</title>
<link rel="icon" type="image/svg+xml" href="data:image/svg+xml,%3Csvg width='32' height='32' viewBox='0 0 32 32' xmlns='http://www.w3.org/2000/svg'%3E%3Crect width='32' height='32' rx='6' fill='%230d0f14'/%3E%3Crect x='3' y='11' width='26' height='18' rx='2' fill='%23161920' stroke='%23f0a500' stroke-width='1.2'/%3E%3Cpath d='M3 14 L16 22 L29 14' fill='none' stroke='%23f0a500' stroke-width='1.2' stroke-linecap='round'/%3E%3Ccircle cx='16' cy='7' r='1.8' fill='%23f0a500'/%3E%3Cpath d='M11.5 7 Q16 3.5 20.5 7' fill='none' stroke='%23f0a500' stroke-width='1.2' stroke-linecap='round'/%3E%3Cpath d='M8.5 7 Q16 1 23.5 7' fill='none' stroke='%23e05c00' stroke-width='1' stroke-linecap='round' opacity='0.6'/%3E%3C/svg%3E">
<style>
  @import url('https://fonts.googleapis.com/css2?family=Space+Mono:wght@700&family=DM+Sans:wght@400;500&display=swap');
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { background: #0d0f14; color: #e8eaf0; font-family: 'DM Sans', sans-serif;
         display: flex; align-items: center; justify-content: center; min-height: 100vh; padding: 24px; }
  .box { text-align: center; max-width: 400px; }
  .icon { font-size: 64px; margin-bottom: 24px; }
  h1 { font-family: 'Space Mono', monospace; font-size: 22px;
       background: linear-gradient(135deg, #f0a500, #e05c00);
       -webkit-background-clip: text; -webkit-text-fill-color: transparent; margin-bottom: 12px; }
  p { color: #6b7080; line-height: 1.6; margin-bottom: 24px; }
  a { display: inline-block; padding: 12px 28px; background: linear-gradient(135deg, #f0a500, #e05c00);
      color: #000; border-radius: 8px; text-decoration: none; font-weight: 600; font-size: 14px; }
</style>
</head>
<body>
<div class="box">
  <div class="icon">✅</div>
  <h1>Configuration saved!</h1>
  <p>MailSense32 is ready. Close this page, put your device in the mailbox, and enjoy!</p>
  <a href="/">← Back to wizard</a>
</div>
</body>
</html>
)rawhtml";
}
