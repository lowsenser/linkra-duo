#include "system_init.h"
#if __has_include("../../config/secrets.h")
#include "../../config/secrets.h"
#else
#error "config/secrets.h fehlt: config/secrets.example.h nach config/secrets.h kopieren und WLAN-Daten eintragen"
#endif
#include "../network/wifi_manager.h"
#include "../network/app_web_server.h"
#include "../integration/mqtt_bridge.h"
#include "../network/update_check.h"
#include "../rf/rf_manager.h"
#include "../storage/device_database.h"
#include "config_manager.h"
#include "../auth/auth_manager.h"
#include <ESPmDNS.h>
#include <esp_wifi.h>

void SystemInit::begin() {
    // Pinned to core 1: the first attachInterrupt() installs the GPIO ISR service
    // through the IPC task of the calling core (1 KB stack). On core 0 the WiFi
    // interrupts nest on top and overflowed ipc0 ("Stack canary watchpoint
    // triggered (ipc0)", 2026-11-24). Core 1 is where it always ran before.
    xTaskCreatePinnedToCore(
        initSystemTask,
        "systemInit",
        8192,
        NULL,
        1,
        NULL,
        1
    );
}

void SystemInit::initSystemTask(void* pvParameters) {
    EventBus& bus = EventBus::getInstance();
    
    // GANZ ZUERST: NVS initialisieren und Reset-Grund loggen
    ConfigManager::getInstance().initNVS();
    ConfigManager::getInstance().logResetReason();

    // Anmeldedaten laden; BOOT-Taste (GPIO0) als "Zugang wiederherstellen"
    // überwachen (eigener Task, verzögert den Start nicht).
    AuthManager::get().begin();
    AuthManager::startRecoveryButtonWatch();
    
    Serial.println("\n====================================");
    Serial.println("  Linkra Duo A0.1 Booting...");
    Serial.println("====================================\n");
    
    // === PSRAM Check ===
    #ifdef BOARD_HAS_PSRAM
    if (psramFound()) {
        Serial.printf("[INIT] ✓ PSRAM: %u KB total, %u KB free (Octal SPI)\n",
                      ESP.getPsramSize() / 1024, ESP.getFreePsram() / 1024);
    } else {
        Serial.println("[INIT] ✗ PSRAM: NOT DETECTED (check memory_type=qio_opi)");
    }
    #else
    Serial.println("[INIT] PSRAM: Disabled (BOARD_HAS_PSRAM not defined)");
    #endif
    Serial.printf("[INIT] SRAM: %u KB free, Flash: %u KB\n",
                  ESP.getFreeHeap() / 1024, ESP.getFlashChipSize() / 1024);
    
    // Zeige NVS-Status
    if (ConfigManager::getInstance().hasWiFiConfig()) {
        Serial.println("[INIT] NVS: WiFi configuration found");
    } else {
        Serial.println("[INIT] NVS: No WiFi configuration");
    }

    // Schritt 1: Hardware
    Serial.println("[INIT] Step 1/6: Hardware...");
    initHardware();
    vTaskDelay(pdMS_TO_TICKS(100));

    // Schritt 2: WiFi
    Serial.println("[INIT] Step 2/6: WiFi...");
    initWiFi();
    vTaskDelay(pdMS_TO_TICKS(500));

    // Schritt 3: Web Server
    Serial.println("[INIT] Step 3/6: Web Server...");
    initWebServer();
    vTaskDelay(pdMS_TO_TICKS(200));

    // Schritt 3.5: Device Database (SQLite on LittleFS)
    Serial.println("[INIT] Device Database...");
    if (!DeviceDatabase::getInstance().begin()) {
        Serial.println("[INIT] WARNING: Device database init failed");
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    // Schritt 4: RF Subsystem (placeholder — actual init in Step 5)
    Serial.println("[INIT] Step 4/6: RF Subsystem...");
    initRF();
    vTaskDelay(pdMS_TO_TICKS(100));

    // Schritt 5: RF Hardware (dual CC1101 init)
    Serial.println("[INIT] Step 5/6: RF Hardware...");
    RFManager::getInstance().begin();
    vTaskDelay(pdMS_TO_TICKS(100));

    // Schritt 6: Home-Assistant-Anbindung (MQTT). Verbindet erst, wenn WLAN steht
    // und ein Broker eingetragen ist; blockiert den Start nicht.
    Serial.println("[INIT] Step 6/6: MQTT...");
    MqttBridge::getInstance().begin();
    UpdateCheck::get().begin();

    Serial.println("\n====================================");
    
    // Zeige Status an
    WiFiMode mode = WiFiManager::getInstance().getMode();
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("  Linkra Duo Ready!");
        Serial.printf("  IP: %s\n", WiFi.localIP().toString().c_str());
        Serial.println("  mDNS: http://linkra-duo.local");
    } else if (mode == WiFiMode::SETUP_AP) {
        Serial.println("  SETUP MODE ACTIVE");
        Serial.println("  Connect to: Linkra-Duo-Setup");
        Serial.println("  Open: http://192.168.4.1");
    } else {
        Serial.println("  WiFi: Connecting...");
    }
    
    Serial.println("====================================\n");
    
    bus.publish(SystemEvent::BOOT_COMPLETE, "");
    
    vTaskDelete(NULL);
}

void SystemInit::initHardware() {
    pinMode(38, OUTPUT);
    digitalWrite(38, HIGH);
    
    EventBus::getInstance().publish(SystemEvent::HARDWARE_DETECTED, "breadboard");
}

void SystemInit::initWiFi() {
    // WiFi credentials: stored in NVS (ConfigManager, set via the web UI or taken
    // over from the build) take precedence over config/secrets.h. Release builds
    // from GitHub carry only the placeholder from secrets.example.h, so a device
    // must never depend on the credentials compiled into the image it runs.
    static WiFiConfig stored;
    const bool haveStored = ConfigManager::getInstance().loadWiFiConfig(stored);
    const bool haveBuiltin = LINKRA_WIFI_SSID[0] && strcmp(LINKRA_WIFI_SSID, "your-ssid") != 0;
    const char* WIFI_SSID = haveStored ? stored.ssid : LINKRA_WIFI_SSID;
    const char* WIFI_PASS = haveStored ? stored.password : LINKRA_WIFI_PASS;
    const char* MDNS_NAME = "linkra-duo";  // erreichbar unter linkra-duo.local
    Serial.printf("[WiFi] Zugangsdaten: %s\n", haveStored ? "gespeichert (NVS)" : haveBuiltin ? "aus dem Build" : "keine");
    
    Serial.printf("[WiFi] Connecting to: %s\n", WIFI_SSID);

    // WiFiManager vor WiFi.begin() anlegen, damit sein Event-Handler schon das
    // erste GOT_IP sieht (startet NTP bei jeder Verbindung, Review 05 M12/M13).
    WiFiManager::getInstance();

    WiFi.mode(WIFI_STA);
    // Regulatory domain: the default "01" (world-safe) only transmits on 2.4 GHz
    // channels 1-11. Access points that pick channel 12/13 (UniFi auto channel)
    // never see our auth frames -> endless AUTH_EXPIRE (2026-11-24).
#ifndef LINKRA_WIFI_COUNTRY
#define LINKRA_WIFI_COUNTRY "DE"
#endif
    esp_wifi_set_country_code(LINKRA_WIFI_COUNTRY, true);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(false);   // WiFiManager reconnects with backoff
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    
    // Warte auf Verbindung (max 15 Sekunden)
    int timeout = 30;  // 30 x 500ms = 15s
    while (WiFi.status() != WL_CONNECTED && timeout > 0) {
        vTaskDelay(pdMS_TO_TICKS(500));
        Serial.print(".");
        timeout--;
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        // Diagnose: what does the radio actually see of our network?
        const int n = WiFi.scanNetworks(false, true);
        for (int i = 0; i < n; i++) {
            if (WiFi.SSID(i) != WIFI_SSID) continue;
            Serial.printf("[WiFi] Scan: %s BSSID %s Kanal %ld RSSI %ld dBm Auth %d\n", WiFi.SSID(i).c_str(),
                          WiFi.BSSIDstr(i).c_str(), (long)WiFi.channel(i), (long)WiFi.RSSI(i), (int)WiFi.encryptionType(i));
        }
        Serial.printf("[WiFi] Scan: %d Netze gesamt\n", n);
        WiFi.scanDelete();
        // Stored credentials failed: fall back to the ones from the build, if different.
        if (haveStored && haveBuiltin && strcmp(stored.ssid, LINKRA_WIFI_SSID) != 0) {
            Serial.println("[WiFi] Versuche Zugangsdaten aus dem Build");
            WIFI_SSID = LINKRA_WIFI_SSID;
            WIFI_PASS = LINKRA_WIFI_PASS;
        }
        WiFi.begin(WIFI_SSID, WIFI_PASS);
        for (int t = 30; WiFi.status() != WL_CONNECTED && t > 0; t--) vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
        // credentials are stored by WiFiManager on GOT_IP (persistCredentials)
        Serial.printf("[WiFi] RSSI: %d dBm\n", WiFi.RSSI());
        // NTP startet im GOT_IP-Handler von WiFiManager (bei jeder Verbindung).
        Serial.printf("[WiFi] MAC: %s\n", WiFi.macAddress().c_str());
        
        // mDNS starten
        if (MDNS.begin(MDNS_NAME)) {
            MDNS.addService("http", "tcp", 80);
            Serial.printf("[mDNS] Erreichbar unter: http://%s.local\n", MDNS_NAME);
        } else {
            Serial.println("[mDNS] FEHLER: mDNS konnte nicht gestartet werden");
        }
        
        EventBus::getInstance().publish(SystemEvent::WIFI_AP_STARTED, WiFi.localIP().toString().c_str());
    } else {
        Serial.println("[WiFi] FEHLER: Verbindung fehlgeschlagen!");
        Serial.println("[WiFi] Starte Fallback AP...");
        WiFiManager::getInstance().beginSetupAP("Linkra-Duo-Setup");
    }
}

void SystemInit::initWebServer() {
    LinkraWebServer::getInstance().begin();
}

void SystemInit::initRF() {
    // RF Subsystem wird in Step 6 initialisiert (nach Web Server)
    // Damit Event Subscriptions vor dem RF Task starten
}

