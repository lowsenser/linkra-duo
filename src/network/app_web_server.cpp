#include "../rf/rf_hunt.h"
#include "../core/version.h"
#include "../rf/rf_event_gate.h"
#include "app_web_server.h"
#include "../core/event_bus.h"
#include "../core/config_manager.h"
#include "wifi_manager.h"
#include "../rf/rf_manager.h"
#include "../storage/device_database.h"
#include "../integration/mqtt_bridge.h"
#include "update_check.h"
#include "../core/crash_log.h"
#include <LittleFS.h>
#include "signalDecoder.h"
#include <Update.h>
#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_chip_info.h>
#include <esp_idf_version.h>
#include <esp_timer.h>
#include "../auth/auth_http.h"

LinkraWebServer& LinkraWebServer::getInstance() {
    static LinkraWebServer instance;
    return instance;
}

// ---------- Verzögerter Neustart ----------
// Handler laufen im async_tcp-Task (Task-WDT 5 s): kein delay() dort. Die
// Antwort wird gesendet, ein esp_timer startet nach ~300 ms einen kurzen Task,
// der die DB schließt und neu startet (DB-Close nicht im esp_timer-Task mit
// kleinem Stack).
static void restartTask(void*) {
    DeviceDatabase::getInstance().close();
    ESP.restart();
}

static void restartTimerCb(void*) {
    xTaskCreate(restartTask, "restart", 6144, nullptr, 5, nullptr);
}

void LinkraWebServer::scheduleRestart(uint32_t delayMs) {
    static esp_timer_handle_t timer = nullptr;
    if (!timer) {
        esp_timer_create_args_t args = {};
        args.callback = restartTimerCb;
        args.name = "restart";
        if (esp_timer_create(&args, &timer) != ESP_OK) { restartTimerCb(nullptr); return; }
    }
    esp_timer_stop(timer);
    esp_timer_start_once(timer, (uint64_t)delayMs * 1000ULL);
    Serial.printf("[Web] Neustart in %u ms\n", (unsigned)delayMs);
}

// Nur die Web-UI darf hochgeladen/gelöscht werden: /index.html und /assets/<pfad>
// mit [A-Za-z0-9._-]-Segmenten (kein "..", keine leeren oder versteckten Segmente).
static bool webPathOk(const String& p) {
    if (p == "/index.html") return true;
    if (!p.startsWith("/assets/") || p.length() <= 8 || p.length() > 96) return false;
    if (p.endsWith(".part")) return false;
    char prev = '/';
    for (size_t i = 8; i < p.length(); i++) {
        const char c = p[i];
        const bool ok = isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_' || c == '/';
        if (!ok) return false;
        if (c == '/' && prev == '/') return false;
        if (c == '.' && (prev == '.' || prev == '/')) return false;
        prev = c;
    }
    return prev != '/';
}

// Zustand eines Datei-/OTA-Uploads (request->_tempObject, wird per free() freigegeben).
struct UploadState {
    int status;        // 0 = läuft/nichts, sonst HTTP-Status
    bool began;        // OTA: Update.begin() erfolgreich
    uint8_t lastPct;   // OTA-Fortschritt
};

static UploadState* uploadState(AsyncWebServerRequest* request) {
    if (!request->_tempObject) request->_tempObject = calloc(1, sizeof(UploadState));
    return (UploadState*)request->_tempObject;
}

// Helper class for accumulating request body
class RequestBodyCollector {
public:
    String body;
    
    void add(uint8_t* data, size_t len) {
        body += String((char*)data, len);
    }
    
    void clear() {
        body = "";
    }
    
    bool isComplete(size_t currentLen, size_t totalLen) {
        return currentLen == totalLen;
    }
};

// Recursive directory listing
void listDir(fs::FS& fs, const char* dirname, uint8_t levels) {
    Serial.printf("  [DIR] %s\n", dirname);
    File root = fs.open(dirname);
    if (!root) {
        Serial.println("  Failed to open directory");
        return;
    }
    if (!root.isDirectory()) {
        Serial.println("  Not a directory");
        return;
    }
    
    File file = root.openNextFile();
    while (file) {
        if (file.isDirectory()) {
            Serial.printf("  [DIR] %s/%s\n", dirname, file.name());
            if (levels > 0) {
                listDir(fs, (String(dirname) + "/" + file.name()).c_str(), levels - 1);
            }
        } else {
            Serial.printf("  [FILE] %s/%s (%d bytes)\n", dirname, file.name(), file.size());
        }
        file = root.openNextFile();
    }
}

// MIME Type Helper
const char* getContentType(const String& filename) {
    if (filename.endsWith(".html")) return "text/html";
    if (filename.endsWith(".css")) return "text/css";
    if (filename.endsWith(".js")) return "application/javascript";
    if (filename.endsWith(".json")) return "application/json";
    if (filename.endsWith(".png")) return "image/png";
    if (filename.endsWith(".jpg")) return "image/jpeg";
    if (filename.endsWith(".gif")) return "image/gif";
    if (filename.endsWith(".svg")) return "image/svg+xml";
    if (filename.endsWith(".ico")) return "image/x-icon";
    if (filename.endsWith(".woff")) return "font/woff";
    if (filename.endsWith(".woff2")) return "font/woff2";
    if (filename.endsWith(".ttf")) return "font/ttf";
    return "text/plain";
}

void LinkraWebServer::begin() {
    EventBus& bus = EventBus::getInstance();
    
    Serial.println("[Web] Initializing server...");
    
    // Mount LittleFS für statische Dateien
    if (!LittleFS.begin(true)) {
        Serial.println("[Web] WARNING: LittleFS mount failed");
    } else {
        Serial.println("[Web] LittleFS mounted");
        Serial.println("[Web] Files in LittleFS:");
        listDir(LittleFS, "/", 2);
    }
    
    // Anmeldung/CSRF für alle Anfragen (auch SSE und statische Dateien).
    // Läuft nach dem vollständigen Body, vor dem Handler; Upload-Callbacks
    // prüfen deshalb zusätzlich selbst (AuthHttp::requestAuthorized).
    server.addMiddleware(&AuthHttp::middleware);
    AuthHttp::registerRoutes(server, [this]() { events.close(); });

    // SSE Event Source hinzufügen
    server.addHandler(&events);
    
    // Routen einrichten
    setupRoutes();
    
    // Server starten
    server.begin();
    
    Serial.println("[Web] Server started on port 80");
    Serial.println("[Web] SSE endpoint: /api/events");
    Serial.println("[Web] OTA endpoint: /api/ota");
    
    // Event Handler registrieren
    bus.subscribe(SystemEvent::WIFI_AP_STARTED, 
        [this](SystemEvent evt, const String& data) {
            sendEvent("wifi", ("{\"status\":\"connected\",\"ip\":\"" + data + "\"}").c_str());
        });
    
    // Rate-limit unknown signals separately so noise cannot hide valid decodes.
    static RFEventGate signalGate;
    static int skippedSignals = 0;
    bus.subscribe(SystemEvent::RF_SIGNAL_RECEIVED,
        [this](SystemEvent evt, const String& data) {
            unsigned long now = millis();
            JsonDocument signalDoc;
            const bool parsed = !deserializeJson(signalDoc, data);
            const bool decoded = parsed && (signalDoc["decoded"] | false);
            if (!signalGate.allow(decoded, now)) {
                skippedSignals++;
                return;
            }
            if (skippedSignals > 0) {
                Serial.printf("[Web] Sent rf_signal (skipped %d)\n", skippedSignals);
                skippedSignals = 0;
            }
            sendEvent("rf_signal", data.c_str());
        });
    
    bus.subscribe(SystemEvent::RF_RSSI_UPDATE,
        [this](SystemEvent evt, const String& data) {
            sendEvent("rf_rssi", data.c_str());
        });
    
    bus.publish(SystemEvent::WEB_SERVER_READY, "80");
}

void LinkraWebServer::setupRoutes() {
    // Root und /index.html (öffentlich). Sonst liefert der Server aus LittleFS
    // nur /assets/* aus – nie devices.db, hunt.json o. Ä.
    auto sendIndex = [](AsyncWebServerRequest* request) {
        if (LittleFS.exists("/index.html")) {
            AsyncWebServerResponse* r = request->beginResponse(LittleFS, "/index.html", "text/html");
            r->addHeader("Cache-Control", "no-cache");
            request->send(r);
        } else {
            request->send(200, "text/html",
                "<html><body><h1>Linkra Duo</h1><p>Web interface not found.</p></body></html>");
        }
    };
    server.on("/index.html", HTTP_GET | HTTP_HEAD, sendIndex);
    server.on("/", HTTP_GET | HTTP_HEAD, sendIndex);

    // API: Config Status (Setup Wizard)
    server.on("/api/config/status", HTTP_GET, [](AsyncWebServerRequest* request) {
        bool hasConfig = ConfigManager::getInstance().hasWiFiConfig();
        WiFiMode mode = WiFiManager::getInstance().getMode();
        
        AsyncResponseStream* response = request->beginResponseStream("application/json");
        response->print("{");
        response->printf("\"has_wifi_config\":%s,", hasConfig ? "true" : "false");
        response->printf("\"mode\":\"%s\",", mode == WiFiMode::SETUP_AP ? "setup" : "normal");
        response->printf("\"connected\":%s", WiFiManager::getInstance().isConnected() ? "true" : "false");
        response->print("}");
        request->send(response);
    });
    
    // API: WiFi Scan (Setup Wizard)
    server.on("/api/wifi/scan", HTTP_GET, [](AsyncWebServerRequest* request) {
        auto networks = WiFiManager::getInstance().scanNetworks();
        
        AsyncResponseStream* response = request->beginResponseStream("application/json");
        response->print("{\"networks\":[");
        
        for (size_t i = 0; i < networks.size(); i++) {
            if (i > 0) response->print(",");
            response->print("{");
            response->printf("\"ssid\":\"%s\",", networks[i].ssid.c_str());
            response->printf("\"rssi\":%d,", networks[i].rssi);
            response->printf("\"channel\":%d,", networks[i].channel);
            response->printf("\"secure\":%s", networks[i].encryptionType != WIFI_AUTH_OPEN ? "true" : "false");
            response->print("}");
        }
        
        response->print("]}");
        request->send(response);
    });
    
    // API: WiFi Connect (Setup Wizard) — uses new JSON handler API
    server.on("/api/wifi/connect", HTTP_POST, [](AsyncWebServerRequest *request, JsonVariant &json) {
        JsonObject doc = json.as<JsonObject>();
        
        const char* ssid = doc["ssid"] | "";
        const char* password = doc["password"] | "";
        
        Serial.printf("[WiFi Connect] SSID: %s, Password length: %d\n", ssid, strlen(password));
        
        if (strlen(ssid) == 0) {
            request->send(400, "application/json", "{\"error\":\"SSID required\"}");
            return;
        }
        
        // Save config
        WiFiConfig config;
        strncpy(config.ssid, ssid, sizeof(config.ssid) - 1);
        config.ssid[sizeof(config.ssid) - 1] = '\0';
        strncpy(config.password, password, sizeof(config.password) - 1);
        config.password[sizeof(config.password) - 1] = '\0';
        config.configured = true;
        config.dhcp = true;
        
        bool saved = ConfigManager::getInstance().saveWiFiConfig(config);
        if (!saved) {
            request->send(500, "application/json", "{\"error\":\"Failed to save config\"}");
            return;
        }
        
        // Start connection
        WiFiManager::getInstance().connectToWiFi(ssid, password);
        
        request->send(200, "application/json", "{\"status\":\"connecting\"}");
    });
    
    // API: WiFi Connection Status
    // WiFi is started in system_init (compile-time credentials) and may bypass
    // WiFiManager, so report the real driver state.
    server.on("/api/wifi/status", HTTP_GET, [](AsyncWebServerRequest* request) {
        JsonDocument d;
        const bool sta = WiFi.status() == WL_CONNECTED;
        WiFiMode mode = WiFiManager::getInstance().getMode();
        d["mode"] = sta ? "connected" : (mode == WiFiMode::SETUP_AP ? "setup_ap" :
                    mode == WiFiMode::STA_CONNECTING ? "connecting" : "disconnected");
        d["connected"] = sta;
        d["ip"] = sta ? WiFi.localIP().toString() : "";
        d["ap_ip"] = WiFi.softAPIP().toString();
        if (sta) {
            d["ssid"] = WiFi.SSID();
            d["bssid"] = WiFi.BSSIDstr();
            d["rssi"] = WiFi.RSSI();
            d["channel"] = WiFi.channel();
            d["gateway"] = WiFi.gatewayIP().toString();
            d["dns"] = WiFi.dnsIP().toString();
            d["subnet"] = WiFi.subnetMask().toString();
        }
        d["mac"] = WiFi.macAddress();
        d["hostname"] = WiFi.getHostname();
        // Setup hotspot credentials (logged-in users only; the route is protected)
        d["setup_ap_ssid"] = "Linkra-Duo-Setup";
        d["setup_ap_password"] = WiFiManager::setupApPassword();
        String out; serializeJson(d, out);
        request->send(200, "application/json", out);
    });
    
    // API: Reset Config
    server.on("/api/config/reset", HTTP_POST, [](AsyncWebServerRequest* request) {
        ConfigManager::getInstance().clearWiFiConfig();
        request->send(200, "application/json", "{\"status\":\"reset\",\"reboot\":true}");
        LinkraWebServer::scheduleRestart(300);
    });
    
    // API: System Info
    server.on("/api/system", HTTP_GET, [](AsyncWebServerRequest* request) {
        JsonDocument d;
        d["version"] = LINKRA_FW_VERSION;
        d["hardware"] = "A0.1";
        d["build"] = __DATE__ " " __TIME__;
        d["idf"] = esp_get_idf_version();
        d["uptime"] = millis() / 1000;
        d["uptime_ms"] = millis();
        { struct timeval tv; gettimeofday(&tv, nullptr);
          d["epoch_ms"] = tv.tv_sec > 1700000000 ? (double)tv.tv_sec * 1000.0 + tv.tv_usec / 1000 : 0; }
        d["time_synced"] = WiFiManager::isTimeSynced();
        d["free_heap"] = ESP.getFreeHeap();
        d["min_heap"] = ESP.getMinFreeHeap();
        d["max_alloc"] = ESP.getMaxAllocHeap();
        d["heap_total"] = ESP.getHeapSize();
        d["psram_total"] = ESP.getPsramSize();
        d["psram_free"] = ESP.getFreePsram();
        d["chip"] = ESP.getChipModel();
        d["chip_rev"] = ESP.getChipRevision();
        d["cores"] = ESP.getChipCores();
        d["cpu_mhz"] = ESP.getCpuFreqMHz();
        d["temp_c"] = temperatureRead();
        d["flash_size"] = ESP.getFlashChipSize();
        d["sketch_size"] = ESP.getSketchSize();
        d["sketch_free"] = ESP.getFreeSketchSpace();
        d["fs_total"] = LittleFS.totalBytes();
        d["fs_used"] = LittleFS.usedBytes();
        const esp_partition_t* run = esp_ota_get_running_partition();
        d["partition"] = run ? run->label : "?";
        d["partition_size"] = run ? run->size : 0;
        d["ota_possible"] = esp_ota_get_next_update_partition(nullptr) != nullptr;
        static const char* reasons[] = {"unknown","poweron","ext","sw","panic","int_wdt","task_wdt","wdt","deepsleep","brownout","sdio"};
        int rr = (int)esp_reset_reason();
        d["reset_reason"] = (rr >= 0 && rr < 11) ? reasons[rr] : "other";
        d["clients"] = WiFi.softAPgetStationNum();
        d["has_config"] = ConfigManager::getInstance().hasWiFiConfig();
        String out; serializeJson(d, out);
        request->send(200, "application/json", out);
    });
    
    // API: Status
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* request) {
        AsyncResponseStream* response = request->beginResponseStream("application/json");
        response->printf("{\"status\":\"ok\",\"version\":\"A0.1\",\"uptime\":%lu}", millis() / 1000);
        request->send(response);
    });
    
    // API: WiFi Clients
    server.on("/api/wifi/clients", HTTP_GET, [](AsyncWebServerRequest* request) {
        int clients = WiFi.softAPgetStationNum();
        AsyncResponseStream* response = request->beginResponseStream("application/json");
        response->printf("{\"connected\":%d}", clients);
        request->send(response);
    });
    
    // API: OTA Update
    server.on("/api/ota", HTTP_POST,
        [](AsyncWebServerRequest* request) {
            UploadState* st = (UploadState*)request->_tempObject;
            const int status = st ? st->status : 400;
            switch (status) {
                case 200:
                    request->send(200, "application/json", "{\"status\":\"success\",\"reboot\":true}");
                    LinkraWebServer::scheduleRestart(300);
                    break;
                case 401: AuthHttp::sendError(request, 401, "unauthorized", "Anmeldung erforderlich"); break;
                case 415: AuthHttp::sendError(request, 400, "filesystem_ota", "Dateisystem-Images werden nicht angenommen (nur Firmware)"); break;
                case 409: AuthHttp::sendError(request, 409, "busy", "Es läuft bereits ein Update"); break;
                case 0:
                case 400: AuthHttp::sendError(request, 400, "no_file", "Keine Firmware-Datei empfangen"); break;
                default:  AuthHttp::sendError(request, 500, "update_failed", Update.errorString()); break;
            }
        },
        [this](AsyncWebServerRequest* request, const String& filename, size_t index, uint8_t* data, size_t len, bool final) {
            handleOTA(request, filename, index, data, len, final);
        }
    );
    
    // API: Reboot
    server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest* request) {
        request->send(200, "application/json", "{\"status\":\"rebooting\"}");
        LinkraWebServer::scheduleRestart(300);
    });
    
    // ===== MQTT / Home Assistant =====
    // GET  /api/mqtt          -> Status + Einstellungen (ohne Passwort)
    // POST /api/mqtt          -> Einstellungen speichern und neu verbinden
    //   {"enabled":true,"host":"192.168.1.10","port":1883,"tls":false,"user":"linkra",
    //    "pass":"...","clear_pass":false,"base_topic":"linkra",
    //    "discovery_prefix":"homeassistant","expire_after":3600}
    //   Leeres "pass" behält das gespeicherte Passwort.
    // GET  /api/mqtt/devices  -> freigegebene Geräte
    // POST /api/mqtt/device   -> {"device_key":"Nexus-TH_160","enabled":true}
    // Specific routes first: AsyncWebServer matches "/api/mqtt" also as a prefix of "/api/mqtt/...".
    // Firmware update check (GitHub Releases), see src/network/update_check.h
    //   GET  /api/update            -> status
    //   POST /api/update/check      -> check now
    //   POST /api/update/install    -> install the release found (202, progress via GET)
    //   POST /api/update/settings   -> {"auto_check":bool,"ha_install":bool}
    server.on("/api/update/check", HTTP_POST, [](AsyncWebServerRequest* request) {
        String err;
        if (!UpdateCheck::get().requestCheck(err)) { AuthHttp::sendError(request, 409, "busy", err.c_str()); return; }
        request->send(202, "application/json", UpdateCheck::get().statusJson());
    });
    server.on("/api/update/install", HTTP_POST, [](AsyncWebServerRequest* request) {
        String err;
        if (!UpdateCheck::get().requestInstall(err)) { AuthHttp::sendError(request, 409, "not_possible", err.c_str()); return; }
        request->send(202, "application/json", UpdateCheck::get().statusJson());
    });
    server.on("/api/update/settings", HTTP_POST, [](AsyncWebServerRequest* request, JsonVariant& json) {
        JsonObject doc = json.as<JsonObject>();
        UpdateCheck& u = UpdateCheck::get();
        const bool autoCheck = doc["auto_check"].is<bool>() ? doc["auto_check"].as<bool>() : u.autoCheck();
        const bool haInstall = doc["ha_install"].is<bool>() ? doc["ha_install"].as<bool>() : u.haInstall();
        u.setSettings(autoCheck, haInstall);
        request->send(200, "application/json", u.statusJson());
    });
    server.on("/api/update", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send(200, "application/json", UpdateCheck::get().statusJson());
    });

    server.on("/api/diag/crash", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send(200, "application/json", CrashLog::json());
    });

    server.on("/api/mqtt/devices", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send(200, "application/json", DeviceDatabase::getInstance().getMqttEnabledDevicesJson());
    });

    server.on("/api/mqtt/device", HTTP_POST, [](AsyncWebServerRequest* request, JsonVariant& json) {
        JsonObject doc = json.as<JsonObject>();
        const char* key = doc["device_key"] | "";
        if (!strlen(key) || !doc["enabled"].is<bool>()) {
            request->send(400, "application/json", "{\"error\":{\"code\":\"invalid\",\"message\":\"device_key und enabled erforderlich\"}}");
            return;
        }
        const bool on = doc["enabled"].as<bool>();
        if (!DeviceDatabase::getInstance().setMqttEnabled(key, on)) {
            request->send(404, "application/json", "{\"error\":{\"code\":\"not_found\",\"message\":\"Gerät nicht gespeichert\"}}");
            return;
        }
        if (on) MqttBridge::getInstance().deviceReleased(key);
        else MqttBridge::getInstance().deviceWithdrawn(key);
        request->send(200, "application/json", on ? "{\"mqtt_enabled\":true}" : "{\"mqtt_enabled\":false}");
    });

    server.on("/api/mqtt", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send(200, "application/json", MqttBridge::getInstance().statusJson());
    });

    server.on("/api/mqtt", HTTP_POST, [](AsyncWebServerRequest* request, JsonVariant& json) {
        JsonObject doc = json.as<JsonObject>();
        MqttSettings s = MqttBridge::getInstance().settings();
        if (doc["enabled"].is<bool>()) s.enabled = doc["enabled"].as<bool>();
        if (doc["host"].is<const char*>()) s.host = doc["host"].as<const char*>();
        s.host.trim();
        if (doc["port"].is<int>()) {
            const int port = doc["port"].as<int>();
            s.port = (port > 0 && port < 65536) ? (uint16_t)port : 0;
        }
        if (doc["tls"].is<bool>()) s.tls = doc["tls"].as<bool>();
        if (doc["user"].is<const char*>()) s.user = doc["user"].as<const char*>();
        s.pass = doc["pass"] | "";
        const bool clearPass = doc["clear_pass"] | false;
        if (doc["base_topic"].is<const char*>()) s.baseTopic = doc["base_topic"].as<const char*>();
        if (doc["discovery_prefix"].is<const char*>()) s.discoveryPrefix = doc["discovery_prefix"].as<const char*>();
        if (doc["expire_after"].is<int>()) s.expireAfterS = (uint32_t)max(0, doc["expire_after"].as<int>());
        String error;
        if (!MqttBridge::getInstance().applySettings(s, clearPass, error)) {
            JsonDocument e;
            e["error"]["code"] = "invalid";
            e["error"]["message"] = error;
            String out;
            serializeJson(e, out);
            request->send(400, "application/json", out);
            return;
        }
        request->send(200, "application/json", MqttBridge::getInstance().statusJson());
    });

    // API: WiFi Scan
    server.on("/api/wifi/scan", HTTP_GET, [](AsyncWebServerRequest* request) {
        auto networks = WiFiManager::getInstance().scanNetworks();
        
        AsyncResponseStream* response = request->beginResponseStream("application/json");
        response->print("{\"networks\":[");
        
        for (size_t i = 0; i < networks.size(); i++) {
            if (i > 0) response->print(",");
            response->print("{");
            response->printf("\"ssid\":\"%s\",", networks[i].ssid.c_str());
            response->printf("\"rssi\":%d,", networks[i].rssi);
            response->printf("\"channel\":%d,", networks[i].channel);
            response->printf("\"secure\":%s", networks[i].encryptionType != WIFI_AUTH_OPEN ? "true" : "false");
            response->print("}");
        }
        
        response->print("]}");
        request->send(response);
    });
    
    // API: WiFi Connect (from Settings) — uses new JSON handler API
    server.on("/api/wifi/settings/connect", HTTP_POST, [](AsyncWebServerRequest *request, JsonVariant &json) {
        JsonObject doc = json.as<JsonObject>();
        
        const char* ssid = doc["ssid"];
        const char* password = doc["password"];
        
        if (!ssid || strlen(ssid) == 0) {
            request->send(400, "application/json", "{\"error\":\"SSID required\"}");
            return;
        }
        
        WiFiManager::getInstance().connectToWiFi(ssid, password ? password : "");
        request->send(200, "application/json", "{\"status\":\"connecting\"}");
    });
    
    // API: RF Status - Combined (both modules)
    server.on("/api/rf/status", HTTP_GET, [](AsyncWebServerRequest* request) {
        RFStatusCache cache = RFManager::getInstance().getStatusCache();
        
        AsyncResponseStream* response = request->beginResponseStream("application/json");
        response->print("{");
        response->printf("\"dualModule\":true,");
        // Module 433
        response->printf("\"module433\":{");
        response->printf("\"ok\":%s,", cache.module433.moduleOk ? "true" : "false");
        response->printf("\"freq\":%.2f,", cache.module433.currentFreq);
        response->printf("\"rssi\":%d,", cache.module433.rssi);
        response->printf("\"signals\":%u", cache.module433.totalSignals);
        response->print("},");
        // Module 868
        response->printf("\"module868\":{");
        response->printf("\"ok\":%s,", cache.module868.moduleOk ? "true" : "false");
        response->printf("\"freq\":%.2f,", cache.module868.currentFreq);
        response->printf("\"rssi\":%d,", cache.module868.rssi);
        response->printf("\"signals\":%u", cache.module868.totalSignals);
        response->print("}");
        response->print("}");
        request->send(response);
    });
    
    // API: RF Status - Individual modules
    server.on("/api/rf/433/status", HTTP_GET, [](AsyncWebServerRequest* request) {
        auto status = RFManager::getInstance().getModuleStatus(false);
        AsyncResponseStream* response = request->beginResponseStream("application/json");
        response->printf("{");
        response->printf("\"moduleOk\":%s,", status.moduleOk ? "true" : "false");
        response->printf("\"frequency\":%.2f,", status.currentFreq);
        response->printf("\"rssi\":%d,", status.rssi);
        response->printf("\"signals\":%u,", status.totalSignals);
        response->printf("\"decoded\":%u", status.decodedSignals);
        response->print("}");
        request->send(response);
    });
    
    server.on("/api/rf/868/status", HTTP_GET, [](AsyncWebServerRequest* request) {
        auto status = RFManager::getInstance().getModuleStatus(true);
        AsyncResponseStream* response = request->beginResponseStream("application/json");
        response->printf("{");
        response->printf("\"moduleOk\":%s,", status.moduleOk ? "true" : "false");
        response->printf("\"frequency\":%.2f,", status.currentFreq);
        response->printf("\"rssi\":%d,", status.rssi);
        response->printf("\"signals\":%u,", status.totalSignals);
        response->printf("\"decoded\":%u", status.decodedSignals);
        response->print("}");
        request->send(response);
    });
    
    // API: RF Debug Info - Dual Module (THREAD SAFE)
    server.on("/api/rf/debug", HTTP_GET, [](AsyncWebServerRequest* request) {
        RFStatusCache cache = RFManager::getInstance().getStatusCache();
        
        AsyncResponseStream* response = request->beginResponseStream("application/json");
        response->print("{");
        response->printf("\"dualMode\":true,");
        // Module 433
        response->printf("\"module433\":{");
        response->printf("\"ok\":%s,", cache.module433.moduleOk ? "true" : "false");
        response->printf("\"rssi\":%d,", cache.module433.rssi);
        response->printf("\"freq\":%.2f,", cache.module433.currentFreq);
        response->printf("\"total\":%u,", cache.module433.totalSignals);
        response->printf("\"decoded\":%u", cache.module433.decodedSignals);
        response->print("},");
        // Module 868
        response->printf("\"module868\":{");
        response->printf("\"ok\":%s,", cache.module868.moduleOk ? "true" : "false");
        response->printf("\"rssi\":%d,", cache.module868.rssi);
        response->printf("\"freq\":%.2f,", cache.module868.currentFreq);
        response->printf("\"total\":%u,", cache.module868.totalSignals);
        response->printf("\"decoded\":%u", cache.module868.decodedSignals);
        response->print("}");
        response->print("}");
        request->send(response);
    });
    
    // API: 433 Library Internal Diagnostics
    server.on("/api/rf/diag433", HTTP_GET, [](AsyncWebServerRequest* request) {
        // Sample GDO0 pin normally
        int gdo0High = 0;
        for (int i = 0; i < 1000; i++) {
            if (digitalRead(RF_MODULE_GDO0)) gdo0High++;
        }
        
        // The forced-GDO0 register test was removed: it rewrote IOCFG0 from the web
        // task without the radio mutex, racing the capture (review 05 H4).
        const int gdo0ForcedHigh = -1, gdo0ForcedLow = -1;
        
        AsyncResponseStream* response = request->beginResponseStream("application/json");
        response->printf("{\"libTotalSignals\":%d,", rtl_433_ESP::totalSignals);
        response->printf("\"libMessageCount\":%d,", rtl_433_ESP::messageCount);
        response->printf("\"libIgnoredSignals\":%d,", rtl_433_ESP::ignoredSignals);
        response->printf("\"libUnparsedSignals\":%d,", rtl_433_ESP::unparsedSignals);
        response->printf("\"currentRssi\":%d,", rtl_433_ESP::currentRssi);
        response->printf("\"averageRssi\":%d,", rtl_433_ESP::averageRssi);
        response->printf("\"rssiThreshold\":%d,", rtl_433_ESP::rssiThreshold);
        response->printf("\"rssiThresholdDelta\":%d,", rtl_433_ESP::rssiThresholdDelta);
        response->printf("\"ookModulation\":%s,", rtl_433_ESP::ookModulation ? "true" : "false");
        response->printf("\"lastIgnoredPulses\":%d,", rtl_433_ESP::lastIgnoredPulses);
        response->printf("\"lastIgnoredDurationUs\":%lu,", rtl_433_ESP::lastIgnoredDuration);
        response->printf("\"lastIgnoredRssi\":%d,", rtl_433_ESP::lastIgnoredRssi);
        response->printf("\"lastUnparsedPulses\":%d,", rtl_433_ESP::lastUnparsedPulses);
        response->printf("\"lastUnparsedDurationUs\":%lu,", rtl_433_ESP::lastUnparsedDuration);
        response->printf("\"lastUnparsedRssi\":%d,", rtl_433_ESP::lastUnparsedRssi);
        response->printf("\"lastUnparsedIs868\":%s,", rtl_433_ESP::lastUnparsedIs868 ? "true" : "false");
        response->print("\"lastUnparsedPG\":[");
        for (int i = 0; i < 16; i++) {
            if (i > 0) response->print(",");
            response->printf("%d", rtl_433_ESP::lastUnparsedPulseGap[i]);
        }
        response->print("],");
        response->printf("\"gdo0_pin\":%d,", RF_MODULE_GDO0);
        response->printf("\"gdo0_normal\":%d,", gdo0High);
        response->printf("\"gdo0_forcedHigh\":%d,", gdo0ForcedHigh);
        response->printf("\"gdo0_forcedLow\":%d,", gdo0ForcedLow);
        response->printf("\"cc1101_IOCFG0\":\"0x%02X\",", rtl_433_ESP::readCC1101Reg(0x02));
        response->printf("\"cc1101_PKTCTRL0\":\"0x%02X\",", rtl_433_ESP::readCC1101Reg(0x08));
        response->printf("\"cc1101_FSCTRL1\":\"0x%02X\",", rtl_433_ESP::readCC1101Reg(0x0B));
        response->printf("\"cc1101_MDMCFG2\":\"0x%02X\",", rtl_433_ESP::readCC1101Reg(0x12));
        response->printf("\"cc1101_MDMCFG4\":\"0x%02X\",", rtl_433_ESP::readCC1101Reg(0x10));
        response->printf("\"cc1101_MDMCFG3\":\"0x%02X\",", rtl_433_ESP::readCC1101Reg(0x11));
        response->printf("\"cc1101_FOCCFG\":\"0x%02X\",", rtl_433_ESP::readCC1101Reg(0x19));
        response->printf("\"cc1101_AGCCTRL2\":\"0x%02X\",", rtl_433_ESP::readCC1101Reg(0x1B));
        response->printf("\"cc1101_AGCCTRL1\":\"0x%02X\",", rtl_433_ESP::readCC1101Reg(0x1C));
        response->printf("\"cc1101_AGCCTRL0\":\"0x%02X\",", rtl_433_ESP::readCC1101Reg(0x1D));
        response->printf("\"cc1101_FREND1\":\"0x%02X\",", rtl_433_ESP::readCC1101Reg(0x21));
        // FREQ registers: verify actual tuned frequency
        // f_carrier = (26000000 / 65536) * FREQ[23:0]
        uint8_t freq2 = rtl_433_ESP::readCC1101Reg(0x0D);
        uint8_t freq1 = rtl_433_ESP::readCC1101Reg(0x0E);
        uint8_t freq0 = rtl_433_ESP::readCC1101Reg(0x0F);
        uint32_t freqWord = ((uint32_t)freq2 << 16) | ((uint32_t)freq1 << 8) | freq0;
        float actualFreqMHz = (26000000.0f / 65536.0f) * freqWord / 1000000.0f;
        response->printf("\"cc1101_FREQ2\":\"0x%02X\",", freq2);
        response->printf("\"cc1101_FREQ1\":\"0x%02X\",", freq1);
        response->printf("\"cc1101_FREQ0\":\"0x%02X\",", freq0);
        response->printf("\"cc1101_actualFreqMHz\":%.4f,", actualFreqMHz);
        // CC1101 state machine status (0x35 = MARCSTATE, burst read for status regs)
        uint8_t marcState = rtl_433_ESP::readCC1101Reg(0x35 | 0x40);
        response->printf("\"cc1101_MARCSTATE\":\"0x%02X\",", marcState);
        // Direct RSSI register read (0x34 = RSSI status reg, 0xC0 = burst status)
        uint8_t rssiRaw = rtl_433_ESP::readCC1101Reg(0x34 | 0xC0);
        int rssiDbm = (rssiRaw >= 128) ? ((int)rssiRaw - 256) / 2 - 74 : ((int)rssiRaw) / 2 - 74;
        response->printf("\"cc1101_rawRssiReg\":\"0x%02X\",", rssiRaw);
        response->printf("\"cc1101_rssiDbm\":%d,", rssiDbm);
        response->printf("\"cc1101_radioGetRSSI\":%d,", rtl_433_ESP::readRSSI());
        response->printf("\"enabledReceiver\":%s,", rtl_433_ESP::isReceiverEnabled() ? "true" : "false");
        response->printf("\"receiverTaskLoops\":%d,", rtl_433_ESP::receiverTaskLoops);
        // GDO0 edge activity: count transitions over 100ms
        int gdo0Edges = 0;
        int lastState = digitalRead(RF_MODULE_GDO0);
        unsigned long edgeStart = micros();
        while (micros() - edgeStart < 100000) { // 100ms
            int s = digitalRead(RF_MODULE_GDO0);
            if (s != lastState) { gdo0Edges++; lastState = s; }
        }
        response->printf("\"gdo0_edges_100ms\":%d,", gdo0Edges);
        // ISR activity
        response->printf("\"isrNoiseCount\":%d,", rtl_433_ESP::getNoiseCount());
        response->printf("\"isrCurrentPulses\":%d,", rtl_433_ESP::getCurrentPulses());
        response->printf("\"isrReceiveMode\":%s,", rtl_433_ESP::getReceiveMode() ? "true" : "false");
        // Peak signal tracking (never reset, only reboot clears)
        response->printf("\"peakRssi\":%d,", rtl_433_ESP::peakRssi);
        response->printf("\"recvModeEntries\":%d,", rtl_433_ESP::recvModeEntries);
        response->printf("\"maxPulsesInSignal\":%d,", rtl_433_ESP::maxPulsesInSignal);
        response->printf("\"maxSignalDurationUs\":%lu}", rtl_433_ESP::maxSignalDuration);
        request->send(response);
    });

    // API: 868 Module Diagnostics (raw SPI register reads)
    server.on("/api/rf/diag868", HTTP_GET, [](AsyncWebServerRequest* request) {
        String json = RFManager::getInstance().getDiag868JSON();
        request->send(200, "application/json", json);
    });
    
    // API: Set RSSI Sensitivity Level (per module)
    // POST /api/rf/sensitivity - Set sensitivity (0-3)
    // Body: {"level":1, "module":"433"} or {"level":1, "module":"868"}
    server.on("/api/rf/sensitivity", HTTP_POST, [](AsyncWebServerRequest *request, JsonVariant &json) {
        JsonObject doc = json.as<JsonObject>();
        int level = doc["level"] | 2;
        const char* module = doc["module"] | "433";
        bool is868 = (strcmp(module, "868") == 0);
        
        RFManager::getInstance().setRSSISensitivity(level, is868);
        
        char resp[128];
        snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"level\":%d,\"module\":\"%s\"}", 
                 level, is868 ? "868" : "433");
        request->send(200, "application/json", resp);
    });
    
    // GET /api/rf/sensitivity?module=433 or ?module=868
    server.on("/api/rf/sensitivity", HTTP_GET, [](AsyncWebServerRequest* request) {
        const char* module = request->getParam("module") ? request->getParam("module")->value().c_str() : "433";
        bool is868 = (strcmp(module, "868") == 0);
        int level = RFManager::getInstance().getRSSISensitivity(is868);
        char json[64];
        snprintf(json, sizeof(json), "{\"level\":%d,\"module\":\"%s\"}", level, is868 ? "868" : "433");
        request->send(200, "application/json", json);
    });
    
    // Tuning: POST {"band":"868","freqMHz":868.32,"bwKHz":203,"bitrateKbps":17.24,"devKHz":40,
    //  "mod":"FSK","agc2":7,"agc1":0,"agc0":145,"frend1":182,"fsctrl1":6,"durationMs":60000}
    server.on("/api/rf/tune", HTTP_POST, [](AsyncWebServerRequest *request, JsonVariant &json) {
        RFManager& rf = RFManager::getInstance();
        const bool is868 = strcmp(json["band"] | "868", "868") == 0;
        RxProfile t = RX_PROFILES[0];
        t.is868 = is868;
        t.mod = strcmp(json["mod"] | (is868 ? "FSK" : "OOK"), "OOK") == 0 ? RxMod::OOK : RxMod::FSK;
        t.freqMHz = json["freqMHz"] | (is868 ? 868.35f : 433.92f);
        t.bwKHz = json["bwKHz"] | 270.0f;
        t.bitrateKbps = json["bitrateKbps"] | (t.mod == RxMod::OOK ? 3.79f : 17.24f);
        t.devKHz = json["devKHz"] | 40.0f;
        t.agc2 = json["agc2"] | 0x07; t.agc1 = json["agc1"] | 0x00; t.agc0 = json["agc0"] | 0x91;
        t.frend1 = json["frend1"] | 0xB6; t.fsctrl1 = json["fsctrl1"] | 0x06;
        t.minPulseUs = json["minPulseUs"] | 0;
        const uint32_t dur = json["durationMs"] | 60000U;
        if (rf.getScanMode() != ScanMode::FIXED || dur < 5000 || dur > 1800000 ||
            (is868 ? (t.freqMHz < 860 || t.freqMHz > 870) : (t.freqMHz < 430 || t.freqMHz > 436))) {
            request->send(400, "application/json", "{\"error\":\"invalid tune parameters\"}");
            return;
        }
        rf.setTune(t, dur);
        request->send(200, "application/json", "{\"status\":\"tuning\"}");
    });

    // Receive profiles: status + per-profile hit statistics
    server.on("/api/rf/profiles", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "application/json", RFManager::getInstance().getRxProfilesJSON());
    });

    // POST {"profile":"somfy-..","durationMs":60000} pins one profile for a test,
    // POST {"rotation":true|false} switches the time-division rotation.
    server.on("/api/rf/profile", HTTP_POST, [](AsyncWebServerRequest *request, JsonVariant &json) {
        RFManager& rf = RFManager::getInstance();
        if (json["rotation"].is<bool>()) {
            rf.setProfileRotation(json["rotation"].as<bool>());
            request->send(200, "application/json", rf.getRxProfilesJSON());
            return;
        }
        const char* id = json["profile"] | "";
        uint32_t duration = json["durationMs"] | 60000U;
        if (duration < 5000 || duration > 600000) {
            request->send(400, "application/json", "{\"error\":\"durationMs 5000..600000\"}");
            return;
        }
        if (rf.getScanMode() != ScanMode::FIXED) {
            request->send(409, "application/json", "{\"error\":\"stop sweep first\"}");
            return;
        }
        if (!rf.pinProfile(id, duration)) {
            request->send(400, "application/json", "{\"error\":\"Unknown receive profile\"}");
            return;
        }
        request->send(202, "application/json", "{\"status\":\"profile_pinned\"}");
    });

    // API: Set RF Scan Mode (toggle without body)
    // POST /api/rf/mode (no body) - Toggle between fixed and sweep
    server.on("/api/rf/mode/toggle", HTTP_POST, [](AsyncWebServerRequest* request) {
        // Only handle truly empty-body POSTs (toggle mode)
        Serial.println("[API] POST /api/rf/mode (no body → toggle)");
        auto currentMode = RFManager::getInstance().getScanMode();
        if (currentMode == ScanMode::FIXED) {
            SweepConfig cfg;
            RFManager::getInstance().setScanMode(ScanMode::CONTINUOUS_SWEEP, cfg);
            request->send(200, "application/json", "{\"status\":\"sweep_started\"}");
        } else {
            RFManager::getInstance().setScanMode(ScanMode::FIXED);
            request->send(200, "application/json", "{\"status\":\"fixed\"}");
        }
    });
    
    // API: Set RF Scan Mode (with JSON body)
    // POST /api/rf/mode - Switch between fixed, continuous sweep, and dual modulation
    // Body: {"mode":"sweep"} or {"mode":"fixed"} or {"mode":"dualmod"}
    // Optional sweep config: {"mode":"sweep","startMHz":433.0,"endMHz":435.0,"stepMHz":0.2,"dwellMs":30000,"bw":"270k"}
    // Optional dualmod config: {"mode":"dualmod","ookDwellMs":24000,"fskDwellMs":6000,"fskDeviationKHz":47.6,"bw":"203k"}
    server.on("/api/rf/mode", HTTP_POST, [](AsyncWebServerRequest *request, JsonVariant &json) {
        JsonObject doc = json.as<JsonObject>();
        String mode = doc["mode"] | "fixed";
        
        if (mode == "sweep") {
            SweepConfig cfg;
            if (doc["startMHz"])  cfg.startFreqMHz = doc["startMHz"];
            if (doc["endMHz"])    cfg.endFreqMHz = doc["endMHz"];
            if (doc["stepMHz"])   cfg.stepMHz = doc["stepMHz"];
            if (doc["dwellMs"])   cfg.dwellMs = doc["dwellMs"];
            if (doc["durationMin"]) {
                uint32_t mins = doc["durationMin"];
                cfg.durationMs = mins * 60000;
            }
            if (doc["durationMs"]) cfg.durationMs = doc["durationMs"];
            
            if (doc["bw"]) {
                String bw = doc["bw"].as<String>();
                if (bw == "58k")       cfg.bwMDMCFG4 = 0xf7;
                else if (bw == "102k") cfg.bwMDMCFG4 = 0xc7;
                else if (bw == "203k") cfg.bwMDMCFG4 = 0x87;
                else if (bw == "325k") cfg.bwMDMCFG4 = 0x57;
            }
            
            RFManager::getInstance().setScanMode(ScanMode::CONTINUOUS_SWEEP, cfg);
            request->send(200, "application/json", "{\"status\":\"sweep_started\"}");
            
        } else if (mode == "dualscan") {
            Serial.printf("[API] POST /api/rf/mode dualscan — heap=%d\n", ESP.getFreeHeap());
            
            SweepConfig cfg433;
            cfg433.startFreqMHz = 433.0;
            cfg433.endFreqMHz = 435.0;
            SweepConfig cfg868;
            cfg868.startFreqMHz = 868.0;
            cfg868.endFreqMHz = 870.0;
            
            if (doc["dwellMs"]) {
                cfg433.dwellMs = doc["dwellMs"];
                cfg868.dwellMs = doc["dwellMs"];
            }
            if (doc["durationMs"]) {
                cfg433.durationMs = doc["durationMs"];
                cfg868.durationMs = doc["durationMs"];
            }
            if (doc["dwellMs433"]) cfg433.dwellMs = doc["dwellMs433"];
            if (doc["dwellMs868"]) cfg868.dwellMs = doc["dwellMs868"];
            if (doc["durationMs433"]) cfg433.durationMs = doc["durationMs433"];
            if (doc["durationMs868"]) cfg868.durationMs = doc["durationMs868"];
            
            if (doc["bw"]) {
                String bw = doc["bw"].as<String>();
                uint8_t val = 0xc7;
                if (bw == "58k")       val = 0xf7;
                else if (bw == "102k") val = 0xc7;
                else if (bw == "203k") val = 0x87;
                else if (bw == "325k") val = 0x57;
                cfg433.bwMDMCFG4 = val;
                cfg868.bwMDMCFG4 = val;
            }
            if (doc["bw433"]) {
                String bw = doc["bw433"].as<String>();
                if (bw == "58k")       cfg433.bwMDMCFG4 = 0xf7;
                else if (bw == "102k") cfg433.bwMDMCFG4 = 0xc7;
                else if (bw == "203k") cfg433.bwMDMCFG4 = 0x87;
                else if (bw == "325k") cfg433.bwMDMCFG4 = 0x57;
            }
            if (doc["bw868"]) {
                String bw = doc["bw868"].as<String>();
                if (bw == "58k")       cfg868.bwMDMCFG4 = 0xf7;
                else if (bw == "102k") cfg868.bwMDMCFG4 = 0xc7;
                else if (bw == "203k") cfg868.bwMDMCFG4 = 0x87;
                else if (bw == "325k") cfg868.bwMDMCFG4 = 0x57;
            }
            
            RFManager::getInstance().startDualScan(cfg433, cfg868);
            request->send(200, "application/json", "{\"status\":\"dualscan_started\"}");
            
        } else if (mode == "dualmod") {
            DualModConfig dmCfg;
            if (doc["ookDwellMs"])      dmCfg.ookDwellMs = doc["ookDwellMs"];
            if (doc["fskDwellMs"])      dmCfg.fskDwellMs = doc["fskDwellMs"];
            if (doc["fskDeviationKHz"]) dmCfg.fskDeviationKHz = doc["fskDeviationKHz"];
            
            if (doc["bw"]) {
                String bw = doc["bw"].as<String>();
                if (bw == "58k")       dmCfg.bwMDMCFG4 = 0xf7;
                else if (bw == "102k") dmCfg.bwMDMCFG4 = 0xc7;
                else if (bw == "203k") dmCfg.bwMDMCFG4 = 0x87;
                else if (bw == "325k") dmCfg.bwMDMCFG4 = 0x57;
            }
            
            RFManager::getInstance().setDualModulation(true, dmCfg);
            request->send(200, "application/json", "{\"status\":\"dualmod_started\"}");
            
        } else {
            RFManager::getInstance().setScanMode(ScanMode::FIXED);
            request->send(200, "application/json", "{\"status\":\"fixed\"}");
        }
    });
    
    // GET /api/rf/sweep/status - Current sweep state (mode, freq, cycle, dwell remaining)
    server.on("/api/rf/sweep/status", HTTP_GET, [](AsyncWebServerRequest* request) {
        String json = RFManager::getInstance().getSweepStatusJSON();
        AsyncWebServerResponse* response = request->beginResponse(200, "application/json", json);
        response->addHeader("Cache-Control", "no-cache");
        request->send(response);
    });
    
    // GET /api/rf/sweep/heatmap - RSSI heatmap across all sweep frequencies
    server.on("/api/rf/sweep/heatmap", HTTP_GET, [](AsyncWebServerRequest* request) {
        String json = RFManager::getInstance().getHeatmapJSON();
        AsyncWebServerResponse* response = request->beginResponse(200, "application/json", json);
        response->addHeader("Cache-Control", "no-cache");
        request->send(response);
    });
    
    // GET /api/rf/dualmod/status - Current dual modulation state (OOK/FSK phase, cycle count, signals per mode)
    server.on("/api/rf/dualmod/status", HTTP_GET, [](AsyncWebServerRequest* request) {
        String json = RFManager::getInstance().getDualModStatusJSON();
        AsyncWebServerResponse* response = request->beginResponse(200, "application/json", json);
        response->addHeader("Cache-Control", "no-cache");
        request->send(response);
    });
    
    // GET /api/rf/dualscan/status - Dual parallel scan status (both modules)
    server.on("/api/rf/dualscan/status", HTTP_GET, [](AsyncWebServerRequest* request) {
        String json = RFManager::getInstance().getDualScanStatusJSON();
        AsyncWebServerResponse* response = request->beginResponse(200, "application/json", json);
        response->addHeader("Cache-Control", "no-cache");
        request->send(response);
    });
    
    // GET /api/rf/dualscan/heatmap - Dual scan heatmap (both bands)
    server.on("/api/rf/dualscan/heatmap", HTTP_GET, [](AsyncWebServerRequest* request) {
        String json = RFManager::getInstance().getDualScanHeatmapJSON();
        AsyncWebServerResponse* response = request->beginResponse(200, "application/json", json);
        response->addHeader("Cache-Control", "no-cache");
        request->send(response);
    });
    
    // POST /api/rf/devices/tag - Tag/untag a device for smart tracking
    // Body: {"key":"Somfy-RTS_12345","tagged":true}
    server.on("/api/rf/devices/tag", HTTP_POST, [](AsyncWebServerRequest *request, JsonVariant &json) {
        JsonObject doc = json.as<JsonObject>();
        const char* key = doc["key"] | "";
        bool tagged = doc["tagged"] | false;
        if (strlen(key) > 0) {
            RFManager::getInstance().tagDevice(key, tagged);
            request->send(200, "application/json", "{\"status\":\"ok\"}");
        } else {
            request->send(400, "application/json", "{\"error\":\"missing_key\"}");
        }
    });
    
    // GET /api/rf/devices/profiles - Get all device profiles with interval data
    server.on("/api/rf/devices/profiles", HTTP_GET, [](AsyncWebServerRequest* request) {
        String json = RFManager::getInstance().getDeviceProfilesJSON();
        AsyncWebServerResponse* response = request->beginResponse(200, "application/json", json);
        response->addHeader("Cache-Control", "no-cache");
        request->send(response);
    });
    
    // API: Persist a device to SQLite (user clicks 'Speichern')
    // Body: {"device_key":"LaCrosse-TX29IT_44","model":"LaCrosse-TX29IT","device_id":44,
    //        "channel":-1,"temperature":21.5,"humidity":55,"battery_ok":1,
    //        "rssi":-60,"frequency":868.35}
    server.on("/api/rf/devices/persist", HTTP_POST, [](AsyncWebServerRequest *request, JsonVariant &json) {
        JsonObject doc = json.as<JsonObject>();
        
        const char* key = doc["device_key"] | "";
        const char* model = doc["model"] | "";
        int deviceId = doc["device_id"] | -1;
        int channel = doc["channel"] | -1;
        float temp = doc["temperature"] | (float)NAN;
        float hum = doc["humidity"] | (float)NAN;
        int bat = doc["battery_ok"] | -1;
        int rssi = doc["rssi"] | -128;
        float freq = doc["frequency"] | 0.0f;
        
        if (strlen(key) == 0 || strlen(model) == 0) {
            request->send(400, "application/json", "{\"error\":\"missing device_key or model\"}");
            return;
        }
        
        bool ok = DeviceDatabase::getInstance().insertOrUpdate(
            key, model, deviceId, channel, temp, hum, bat, rssi, freq, "{}");
        
        if (ok) {
            Serial.printf("[DB] Device persisted via API: %s\n", key);
            request->send(200, "application/json", "{\"status\":\"persisted\"}");
        } else {
            request->send(500, "application/json", "{\"error\":\"db_write_failed\"}");
        }
    });
    
    // API: RF Devices from SQLite (persistent storage)
    server.on("/api/rf/devices", HTTP_GET, [](AsyncWebServerRequest* request) {
        String json = DeviceDatabase::getInstance().getAllDevicesJson();
        AsyncWebServerResponse* response = request->beginResponse(200, "application/json", json);
        response->addHeader("Cache-Control", "no-cache");
        request->send(response);
    });
    
    // API: DB Debug - test write + read cycle
    server.on("/api/db/test", HTTP_GET, [](AsyncWebServerRequest* request) {
        auto& db = DeviceDatabase::getInstance();
        String result = "{";
        
        // Test insert
        bool writeOk = db.insertOrUpdate("_apitest_", "APITest", 99, 1, 
                                          21.0f, 55.0f, 1, -60, 868.35f, "{}");
        result += "\"write\":" + String(writeOk ? "true" : "false") + ",";
        
        // Test count
        int count = db.getDeviceCount();
        result += "\"count\":" + String(count) + ",";
        
        // Test read
        String all = db.getAllDevicesJson();
        result += "\"read_length\":" + String(all.length()) + ",";
        result += "\"read_preview\":\"" + all.substring(0, 200) + "\",";
        
        // Clean up
        db.deleteDevice("_apitest_");
        
        int countAfter = db.getDeviceCount();
        result += "\"count_after_delete\":" + String(countAfter);
        result += "}";
        
        request->send(200, "application/json", result);
    });
    
    // API: Clear all stored RF devices
    server.on("/api/rf/devices", HTTP_DELETE, [](AsyncWebServerRequest* request) {
        std::vector<MqttDeviceInfo> released;
        DeviceDatabase::getInstance().getMqttDevices(released);
        DeviceDatabase::getInstance().clearAll();
        for (const auto& d : released) MqttBridge::getInstance().deviceWithdrawn(d.key.c_str());
        request->send(200, "application/json", "{\"status\":\"cleared\"}");
    });
    
    // API: Delete single RF device by key
    server.on("/api/rf/devices/delete", HTTP_POST, [](AsyncWebServerRequest *request, JsonVariant &json) {
        JsonObject doc = json.as<JsonObject>();
        const char* key = doc["device_key"] | "";
        const bool wasReleased = strlen(key) > 0 && DeviceDatabase::getInstance().isMqttEnabled(key);
        if (strlen(key) > 0 && DeviceDatabase::getInstance().deleteDevice(key)) {
            if (wasReleased) MqttBridge::getInstance().deviceWithdrawn(key);
            request->send(200, "application/json", "{\"status\":\"deleted\"}");
        } else {
            request->send(404, "application/json", "{\"error\":\"not found\"}");
        }
    });
    
    // 868 receive trace: GET /api/rf/trace868?since=<ms> (CSV)
    server.on("/api/rf/trace868", HTTP_GET, [](AsyncWebServerRequest* request) {
        extern String getTrace868(uint32_t);
        const uint32_t since = request->hasParam("since") ? request->getParam("since")->value().toInt() : 0;
        request->send(200, "text/plain", getTrace868(since));
    });

    // Raw undecoded pulse trains as rtl_433 pulse-data text (rtl_433 -r file.ook)
    server.on("/api/rf/decprof", HTTP_GET, [](AsyncWebServerRequest* request) {
        int n = request->hasParam("n") ? request->getParam("n")->value().toInt() : 25;
        request->send(200, "application/json", linkraDecoderProfile(n < 1 ? 1 : n > 100 ? 100 : n));
    });
    server.on("/api/rf/raw", HTTP_GET, [](AsyncWebServerRequest* request) {
        int n = request->hasParam("n") ? request->getParam("n")->value().toInt() : 20;
        int band = request->hasParam("band") ? request->getParam("band")->value().toInt() : 0;
        request->send(200, "text/plain", linkraRawCaptureDump(n < 1 ? 1 : n > 40 ? 40 : n, band,
            request->hasParam("strong") && request->getParam("strong")->value() != "0"));
    });

    // Weather-station hunt (runs on the device, survives reboots)
    server.on("/api/rf/hunt", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (RfHunt::get().hasStoredResult()) {      // finished hunt: stream the file, no RAM copy
            request->send(LittleFS, "/hunt.json", "application/json");
            return;
        }
        AsyncWebServerResponse* r = request->beginResponse(200, "application/json", RfHunt::get().statusJson());
        r->addHeader("Cache-Control", "no-store");
        request->send(r);
    });
    server.on("/api/rf/hunt", HTTP_POST, [](AsyncWebServerRequest *request, JsonVariant &json) {
        if (json["stop"] | false) { RfHunt::get().stop(); request->send(200, "application/json", "{\"status\":\"stopped\"}"); return; }
        if (!RfHunt::clockSynced()) { request->send(503, "application/json", "{\"error\":\"Uhrzeit nicht synchronisiert\"}"); return; }
        // until = "HH:MM" local time (next occurrence) or durationMin
        time_t now = time(nullptr), until = 0;
        if (json["durationMin"].is<int>()) until = now + (time_t)(json["durationMin"].as<int>()) * 60;
        else {
            int hh = 6, mm = 0;
            sscanf(json["until"] | "06:00", "%d:%d", &hh, &mm);
            struct tm lt; localtime_r(&now, &lt);
            lt.tm_hour = hh; lt.tm_min = mm; lt.tm_sec = 0;
            until = mktime(&lt);
            if (until <= now) { lt.tm_mday += 1; until = mktime(&lt); }
        }
        if (!RfHunt::get().start(until)) { request->send(400, "application/json", "{\"error\":\"Start fehlgeschlagen\"}"); return; }
        request->send(200, "application/json", String("{\"status\":\"started\",\"until\":") + (long long)until + "}");
    });

    // RF overview: both radios + profiles (alias of /api/rf/profiles)
    server.on("/api/rf/overview", HTTP_GET, [](AsyncWebServerRequest *request) {
        AsyncWebServerResponse* r = request->beginResponse(200, "application/json",
            RFManager::getInstance().getRxProfilesJSON());
        r->addHeader("Cache-Control", "no-store");
        request->send(r);
    });

    // Web asset management — writes only /index.html* and /assets/*, never the DB.
    server.on("/api/fs/list", HTTP_GET, [](AsyncWebServerRequest* request) {
        JsonDocument d;
        JsonArray arr = d["files"].to<JsonArray>();
        std::function<void(const char*)> walk = [&](const char* dir) {
            File root = LittleFS.open(dir);
            if (!root || !root.isDirectory()) return;
            File f = root.openNextFile();
            while (f) {
                String path = String(dir) + (String(dir).endsWith("/") ? "" : "/") + f.name();
                if (f.isDirectory()) walk(path.c_str());
                else { JsonObject o = arr.add<JsonObject>(); o["path"] = path; o["size"] = f.size(); }
                f = root.openNextFile();
            }
        };
        walk("/");
        d["total"] = LittleFS.totalBytes();
        d["used"] = LittleFS.usedBytes();
        String out; serializeJson(d, out);
        request->send(200, "application/json", out);
    });
    server.on("/api/fs/delete", HTTP_POST, [](AsyncWebServerRequest* request) {
        if (!request->hasParam("path")) { AuthHttp::sendError(request, 400, "invalid", "path fehlt"); return; }
        String p = request->getParam("path")->value();
        if (!webPathOk(p)) { AuthHttp::sendError(request, 403, "forbidden_path", "Nur /index.html und /assets/* erlaubt"); return; }
        bool ok = LittleFS.remove(p);
        if (ok) request->send(200, "application/json", "{\"status\":\"deleted\"}");
        else AuthHttp::sendError(request, 404, "not_found", "Datei nicht gefunden");
    });
    // Upload der Web-UI. Der Datei-Callback läuft VOR der Middleware, prüft
    // Anmeldung/CSRF und Pfad daher selbst und merkt sich das Ergebnis.
    server.on("/api/fs/upload", HTTP_POST,
        [](AsyncWebServerRequest* request) {
            UploadState* st = (UploadState*)request->_tempObject;
            const int status = st ? st->status : 400;
            switch (status) {
                case 200: request->send(200, "application/json", "{\"status\":\"uploaded\"}"); break;
                case 401: AuthHttp::sendError(request, 401, "unauthorized", "Anmeldung erforderlich"); break;
                case 403: AuthHttp::sendError(request, 403, "forbidden_path", "Nur /index.html und /assets/* erlaubt"); break;
                case 507: AuthHttp::sendError(request, 507, "no_space", "Zu wenig Speicher im Dateisystem"); break;
                case 0:
                case 400: AuthHttp::sendError(request, 400, "no_file", "Keine Datei empfangen"); break;
                default:  AuthHttp::sendError(request, 500, "write_failed", "Schreiben fehlgeschlagen"); break;
            }
        },
        [](AsyncWebServerRequest* request, const String& filename, size_t index,
           uint8_t* data, size_t len, bool final) {
            UploadState* st = uploadState(request);
            if (!st) return;
            const String p = request->hasParam("path") ? request->getParam("path")->value() : "";
            if (!index) {
                if (request->_tempFile) request->_tempFile.close();
                if (!AuthHttp::requestAuthorized(request)) { st->status = 401; return; }
                if (!webPathOk(p)) { st->status = 403; return; }
                const size_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
                const size_t reserve = 256 * 1024;   // Platz für SQLite-DB/Journal
                if (request->contentLength() + reserve > freeBytes) { st->status = 507; return; }
                request->_tempFile = LittleFS.open(p + ".part", "w", true);
                if (!request->_tempFile) { st->status = 500; return; }
                st->status = 0;
            }
            if (st->status != 0 || !request->_tempFile) return;
            if (len && request->_tempFile.write(data, len) != len) {
                request->_tempFile.close();
                LittleFS.remove(p + ".part");
                st->status = 500;
                return;
            }
            if (final) {
                request->_tempFile.close();
                const String tmp = p + ".part";
                bool ok = LittleFS.rename(tmp, p);
                if (!ok) { LittleFS.remove(p); ok = LittleFS.rename(tmp, p); }
                if (!ok) LittleFS.remove(tmp);
                st->status = ok ? 200 : 500;
                Serial.printf("[FS] Upload %s: %s\n", p.c_str(), ok ? "ok" : "FEHLER");
            }
        });

    // Static files: hashed assets are immutable, index must revalidate.
    // Nur /assets/ statisch; "/" und "/index.html" haben eigene Routen oben.
    server.serveStatic("/assets/", LittleFS, "/assets/").setCacheControl("public, max-age=31536000, immutable");
    
    // 404 Handler
    server.onNotFound([](AsyncWebServerRequest* request) {
        Serial.printf("[Web] 404: %s\n", request->url().c_str());
        request->send(404, "text/plain", "Not Found");
    });
}

void LinkraWebServer::handleOTA(AsyncWebServerRequest* request, const String& filename,
                                 size_t index, uint8_t* data, size_t len, bool final) {
    UploadState* st = uploadState(request);
    if (!st) return;
    if (!index) {
        // Body-Callbacks laufen vor der Middleware: Anmeldung hier prüfen, bevor
        // irgendetwas in den Flash geschrieben wird.
        if (!AuthHttp::requestAuthorized(request)) { st->status = 401; return; }
        Serial.printf("[OTA] Starting update: %s\n", filename.c_str());
        // Dateisystem-OTA (U_SPIFFS) gibt es nicht mehr: es überschrieb das
        // gemountete LittleFS samt devices.db (Review 05 K2).
        String lower = filename;
        lower.toLowerCase();
        if (lower.indexOf("littlefs") >= 0 || lower.indexOf("filesystem") >= 0 || lower.indexOf("spiffs") >= 0) {
            Serial.println("[OTA] Abgelehnt: Dateisystem-Image");
            st->status = 415;
            return;
        }
        if (Update.isRunning() || UpdateCheck::get().busy()) { st->status = 409; return; }
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
            Update.printError(Serial);
            st->status = 500;
            return;
        }
        st->began = true;
        st->status = 0;
        request->onDisconnect([]() {
            if (Update.isRunning()) { Update.abort(); Serial.println("[OTA] Abgebrochen (Verbindung getrennt)"); }
        });
        sendEvent("ota", "{\"status\":\"started\",\"progress\":0}");
    }
    if (!st->began || st->status != 0) return;

    if (len > 0 && Update.write(data, len) != len) {
        Update.printError(Serial);
        Update.abort();
        st->status = 500;
        sendEvent("ota", "{\"status\":\"error\"}");
        return;
    }

    if (final) {
        if (Update.end(true)) {
            Serial.printf("[OTA] Update Success: %u bytes\n", (unsigned)(index + len));
            sendEvent("ota", "{\"status\":\"complete\"}");
            st->status = 200;
        } else {
            Update.printError(Serial);
            sendEvent("ota", "{\"status\":\"error\"}");
            st->status = 500;
        }
        return;
    }

    const size_t total = request->contentLength();
    if (total > 0) {
        const uint8_t pct = (uint8_t)(((uint64_t)(index + len) * 100) / total);
        if (pct >= st->lastPct + 10) {
            st->lastPct = pct;
            char buf[64];
            snprintf(buf, sizeof(buf), "{\"status\":\"progress\",\"progress\":%u}", (unsigned)pct);
            sendEvent("ota", buf);
        }
    }
}

void LinkraWebServer::sendEvent(const char* event, const char* data) {
    static unsigned long lastLogTime = 0;
    static int eventsSinceLog = 0;
    eventsSinceLog++;
    unsigned long now = millis();
    if (now - lastLogTime > 5000) {
        Serial.printf("[SSE] Sent %d events in last 5s to %d clients\n", 
                      eventsSinceLog, events.count());
        eventsSinceLog = 0;
        lastLogTime = now;
    }
    events.send(data, event, millis());
}

void LinkraWebServer::onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client, 
                          AwsEventType type, void* arg, uint8_t* data, size_t len) {
}
