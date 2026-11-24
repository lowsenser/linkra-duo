#include "wifi_manager.h"
#include "../core/event_bus.h"
#include "../core/config_manager.h"
#include <atomic>
#include <esp_sntp.h>

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

void WiFiManager::handleWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
            Serial.println("[WiFi] STA Connected to AP");
            currentMode = WiFiMode::STA_CONNECTING;
            break;
            
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            Serial.print("[WiFi] Got IP: ");
            Serial.println(WiFi.localIP());
            currentMode = WiFiMode::STA_CONNECTED;
            
            startTimeSync();

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
    
    // Open network for setup (no password required)
    bool success = WiFi.softAP(apName, nullptr, 6, 0, 4);
    
    if (!success) {
        Serial.println("[WiFi] ERROR: Setup AP creation failed!");
        return;
    }
    
    delay(100);
    WiFi.softAPsetHostname("linkra-setup");
    
    IPAddress ip = WiFi.softAPIP();
    Serial.printf("[WiFi] Setup AP started: %s (Open)\n", apName);
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
    Serial.println("[WiFi] AP_STA Mode - AP bleibt aktiv während Verbindung!");
    
    // WICHTIG: Wir bleiben im AP_STA Mode!
    // Der AP bleibt erreichbar während wir versuchen zu verbinden
    WiFi.mode(WIFI_AP_STA);
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
