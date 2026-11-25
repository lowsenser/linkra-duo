#include "wifi_manager.h"
#include "../core/event_bus.h"
#include "../core/config_manager.h"
#include <atomic>
#include <esp_sntp.h>
#include <Preferences.h>
#include <esp_random.h>

// ---------- Reconnect with backoff ----------
// The driver's auto-reconnect retried about once per second. After many quick
// association attempts (crash loops, hundreds of esptool resets) a UniFi AP
// stopped answering (AUTH_EXPIRE) and the constant retries kept it that way.
// First 5 failures: retry after 2 s; then 15 s, 30 s, ... up to 5 min.
#include <esp_timer.h>
#include <esp_phy_init.h>
#include <esp_attr.h>
static esp_timer_handle_t s_reconnectTimer = nullptr;
static uint32_t s_failures = 0;
static uint32_t s_authExpire = 0;            // consecutive AUTH_EXPIRE since boot without success
// Survives a software reset: was the PHY calibration already erased for this problem?
static RTC_NOINIT_ATTR uint32_t s_phyResetMagic;
static constexpr uint32_t kPhyResetMagic = 0x50485952;   // "PHYR"

// After many USB/esptool resets the radio sometimes never got an answer to its
// authentication (AUTH_EXPIRE on every attempt) until the board was powered
// off – soft resets kept the problem. A power-on runs a full RF calibration,
// a soft reset reuses the calibration data in NVS. So: after 10 AUTH_EXPIRE in
// a row, erase that data once and restart (2026-11-25).
static void noteDisconnectReason(uint8_t reason) {
    if (reason != WIFI_REASON_AUTH_EXPIRE) { s_authExpire = 0; return; }
    if (++s_authExpire < 10 || s_phyResetMagic == kPhyResetMagic) return;
    Serial.println("[WiFi] Anmeldung scheitert dauerhaft (AUTH_EXPIRE) – Funk-Kalibrierung wird neu erstellt, Neustart");
    s_phyResetMagic = kPhyResetMagic;
    esp_phy_erase_cal_data_in_nvs();
    delay(100);
    ESP.restart();
}

static void reconnectNow(void*) {
    if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
}

static void scheduleReconnect() {
    if (!s_reconnectTimer) {
        esp_timer_create_args_t a = {};
        a.callback = reconnectNow;
        a.name = "wifiRetry";
        if (esp_timer_create(&a, &s_reconnectTimer) != ESP_OK) return;
    }
    s_failures++;
    uint32_t delayMs = 2000;
    if (s_failures > 5) {
        delayMs = 15000u << (s_failures - 6 < 5 ? s_failures - 6 : 5);
        if (delayMs > 300000u) delayMs = 300000u;
    }
    esp_timer_stop(s_reconnectTimer);
    esp_timer_start_once(s_reconnectTimer, (uint64_t)delayMs * 1000);
    if (s_failures == 6 || (s_failures > 6 && delayMs == 300000u && s_failures % 6 == 0))
        Serial.printf("[WiFi] %lu Fehlversuche – nächster Versuch in %lu s\n", (unsigned long)s_failures, (unsigned long)(delayMs / 1000));
}

// ---------- Setup-Hotspot-Passwort ----------
// WPA2 instead of an open hotspot (review 05 H5): a device-specific password is
// generated once and kept in NVS. It is printed on the serial console whenever
// the hotspot starts and shown to a logged-in user (Einstellungen -> WLAN).
String WiFiManager::setupApPassword() {
    Preferences p;
    p.begin("linkra", false);
    String pw = p.getString("ap_pw", "");
    if (pw.length() < 8) {
        static const char kAlphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";   // no 0/o/1/l/i
        char buf[13];
        for (int i = 0; i < 12; i++) buf[i] = kAlphabet[esp_random() % (sizeof(kAlphabet) - 1)];
        buf[12] = '\0';
        pw = buf;
        p.putString("ap_pw", pw);
    }
    p.end();
    return pw;
}

// ---------- Zeit (NTP) ----------
// Wird bei JEDEM GOT_IP neu gestartet (nicht nur beim Boot), damit die Uhr auch
// stimmt, wenn das Gerät erst später ins WLAN kommt. Voraussetzung für TOTP.
static std::atomic<bool> s_timeSynced{false};

static void onTimeSync(struct timeval*) {
    if (!s_timeSynced.exchange(true)) {
        Serial.println("[NTP] Systemzeit synchronisiert");
    }
}

void WiFiManager::startTimeSync() {
    sntp_set_time_sync_notification_cb(onTimeSync);
    // Europe/Berlin inkl. Sommerzeit; SNTP pollt danach selbst (Standard 1 h).
    configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.cloudflare.com");
}

bool WiFiManager::isTimeSynced() {
    return s_timeSynced.load();
}

// Singleton Instance
WiFiManager& WiFiManager::getInstance() {
    static WiFiManager instance;
    return instance;
}

WiFiManager::WiFiManager() : currentMode(WiFiMode::SETUP_AP) {
    WiFi.onEvent(WiFiEventHandler);
}

void WiFiManager::WiFiEventHandler(WiFiEvent_t event, WiFiEventInfo_t info) {
    WiFiManager::getInstance().handleWiFiEvent(event, info);
}

// Keep working credentials on the device. Release builds from GitHub carry no
// WiFi credentials, so after an online update the device must not depend on the
// ones compiled into the image. Runs on every GOT_IP (also after a late reconnect),
// writes only when something changed.
void WiFiManager::persistCredentials() {
    const String ssid = WiFi.SSID(), pass = WiFi.psk();
    if (ssid.isEmpty() || ssid.length() > 31 || pass.length() > 63) return;
    WiFiConfig stored = {};
    if (ConfigManager::getInstance().loadWiFiConfig(stored) && ssid == stored.ssid && pass == stored.password) return;
    WiFiConfig c = {};
    strlcpy(c.ssid, ssid.c_str(), sizeof(c.ssid));
    strlcpy(c.password, pass.c_str(), sizeof(c.password));
    c.configured = true;
    c.dhcp = true;
    Serial.println(ConfigManager::getInstance().saveWiFiConfig(c) ? "[WiFi] Zugangsdaten im Gerät gespeichert"
                                                                  : "[WiFi] Zugangsdaten speichern fehlgeschlagen");
}

void WiFiManager::handleWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
            Serial.println("[WiFi] STA Connected to AP");
            currentMode = WiFiMode::STA_CONNECTING;
            break;
            
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            s_failures = 0;
            s_authExpire = 0;
            s_phyResetMagic = 0;
            if (s_reconnectTimer) esp_timer_stop(s_reconnectTimer);
            Serial.print("[WiFi] Got IP: ");
            Serial.println(WiFi.localIP());
            currentMode = WiFiMode::STA_CONNECTED;
            
            startTimeSync();
            persistCredentials();

            // Verbindung erfolgreich - einen evtl. laufenden Setup-AP abschalten.
            if (WiFi.getMode() & WIFI_MODE_AP) {
                Serial.println("[WiFi] Connection successful - disabling AP...");
                WiFi.softAPdisconnect(true);
                WiFi.mode(WIFI_STA);
                Serial.println("[WiFi] AP disabled, STA mode active");
            }

            for (auto& cb : onConnectedCallbacks) {
                cb();
            }
            EventBus::getInstance().publish(SystemEvent::WIFI_AP_STARTED, WiFi.localIP().toString().c_str());
            break;
            
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            Serial.println("[WiFi] STA Disconnected");
            noteDisconnectReason(info.wifi_sta_disconnected.reason);
            scheduleReconnect();
            if (currentMode == WiFiMode::STA_CONNECTED || currentMode == WiFiMode::STA_CONNECTING) {
                currentMode = WiFiMode::STA_FAILED;
                for (auto& cb : onDisconnectedCallbacks) {
                    cb();
                }
            }
            break;
            
        case ARDUINO_EVENT_WIFI_AP_START:
            Serial.println("[WiFi] AP Started");
            break;
            
        case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
            Serial.println("[WiFi] Client connected to AP");
            EventBus::getInstance().publish(SystemEvent::WIFI_CLIENT_CONNECTED, "");
            break;
            
        case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
            Serial.println("[WiFi] Client disconnected from AP");
            EventBus::getInstance().publish(SystemEvent::WIFI_CLIENT_DISCONNECTED, "");
            break;
            
        default:
            break;
    }
}

void WiFiManager::beginSetupAP(const char* apName) {
    currentMode = WiFiMode::SETUP_AP;
    
    Serial.println("[WiFi] Starting Setup AP...");
    
    // WICHTIG: Verwende WIFI_AP_STA statt WIFI_AP
    // Damit können wir später STA-Verbindung aufbauen ohne AP zu verlieren
    WiFi.mode(WIFI_AP_STA);
    WiFi.setSleep(false);
    delay(10);
    
    // WPA2 with the device's own setup password (never an open network)
    const String pw = setupApPassword();
    bool success = WiFi.softAP(apName, pw.c_str(), 6, 0, 4);
    
    if (!success) {
        Serial.println("[WiFi] ERROR: Setup AP creation failed!");
        return;
    }
    
    delay(100);
    WiFi.softAPsetHostname("linkra-setup");
    
    IPAddress ip = WiFi.softAPIP();
    Serial.printf("[WiFi] Setup AP started: %s (WPA2, Passwort: %s)\n", apName, pw.c_str());
    Serial.printf("[WiFi] Setup IP: %s\n", ip.toString().c_str());
    
    EventBus::getInstance().publish(SystemEvent::WIFI_AP_STARTED, ip.toString());
}

std::vector<WiFiNetwork> WiFiManager::scanNetworks() {
    std::vector<WiFiNetwork> networks;
    
    Serial.println("[WiFi] Starting network scan...");
    
    // WICHTIG: Im AP_STA Mode können wir scannen ohne den AP zu stören
    if (WiFi.getMode() == WIFI_MODE_NULL) {
        WiFi.mode(WIFI_AP_STA);
        delay(100);
    }
    
    int n = WiFi.scanNetworks(true, false, false, 300);
    
    if (n == WIFI_SCAN_RUNNING) {
        int attempts = 0;
        while (WiFi.scanComplete() == WIFI_SCAN_RUNNING && attempts < 50) {
            delay(100);
            attempts++;
        }
        n = WiFi.scanComplete();
    }
    
    if (n < 0) {
        Serial.println("[WiFi] Scan failed");
        return networks;
    }
    
    Serial.printf("[WiFi] Found %d networks\n", n);
    
    for (int i = 0; i < n; i++) {
        WiFiNetwork net;
        net.ssid = WiFi.SSID(i);
        net.rssi = WiFi.RSSI(i);
        net.encryptionType = WiFi.encryptionType(i);
        net.channel = WiFi.channel(i);
        
        bool duplicate = false;
        for (const auto& existing : networks) {
            if (existing.ssid == net.ssid) {
                if (net.rssi > existing.rssi) {
                    const_cast<WiFiNetwork&>(existing) = net;
                }
                duplicate = true;
                break;
            }
        }
        
        if (!duplicate && net.ssid.length() > 0) {
            networks.push_back(net);
        }
    }
    
    std::sort(networks.begin(), networks.end(), 
        [](const WiFiNetwork& a, const WiFiNetwork& b) {
            return a.rssi > b.rssi;
        });
    
    WiFi.scanDelete();
    return networks;
}

bool WiFiManager::connectToWiFi(const char* ssid, const char* password) {
    currentMode = WiFiMode::STA_CONNECTING;
    
    Serial.printf("[WiFi] Connecting to: %s\n", ssid);
    // Keep the hotspot only if it is already running (setup mode). Switching a
    // connected station to AP_STA would open the chip's default, unprotected
    // SoftAP (review 05 H6).
    WiFi.mode((WiFi.getMode() & WIFI_MODE_AP) ? WIFI_AP_STA : WIFI_STA);
    WiFi.setSleep(false);
    delay(10);
    
    // Start connection - AP bleibt aktiv!
    WiFi.begin(ssid, password);
    
    return true;
}

void WiFiManager::disconnect() {
    WiFi.disconnect(true);
    currentMode = WiFiMode::SETUP_AP;
}

bool WiFiManager::isConnected() {
    return currentMode == WiFiMode::STA_CONNECTED && WiFi.status() == WL_CONNECTED;
}

WiFiMode WiFiManager::getMode() {
    return currentMode;
}

String WiFiManager::getIP() {
    if (currentMode == WiFiMode::STA_CONNECTED) {
        return WiFi.localIP().toString();
    }
    return "";
}

String WiFiManager::getAPIP() {
    return WiFi.softAPIP().toString();
}

int WiFiManager::getClientCount() {
    return WiFi.softAPgetStationNum();
}

void WiFiManager::onConnected(std::function<void()> callback) {
    onConnectedCallbacks.push_back(callback);
}

void WiFiManager::onDisconnected(std::function<void()> callback) {
    onDisconnectedCallbacks.push_back(callback);
}

void WiFiManager::update() {
    // Check for connection timeout
    if (currentMode == WiFiMode::STA_CONNECTING) {
        static unsigned long connectStart = 0;
        static bool timerStarted = false;
        
        if (!timerStarted) {
            connectStart = millis();
            timerStarted = true;
        }
        
        // Timeout after 30 seconds
        if (millis() - connectStart > 30000) {
            Serial.println("[WiFi] Connection timeout");
            currentMode = WiFiMode::STA_FAILED;
            timerStarted = false;
            
            for (auto& cb : onDisconnectedCallbacks) {
                cb();
            }
        }
        
        if (currentMode == WiFiMode::STA_CONNECTED) {
            timerStarted = false;
        }
    }
}
