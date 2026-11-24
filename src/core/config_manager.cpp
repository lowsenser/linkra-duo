#include "config_manager.h"
#include <Preferences.h>
#include <nvs_flash.h>

ConfigManager& ConfigManager::getInstance() {
    static ConfigManager instance;
    return instance;
}

bool ConfigManager::initNVS() {
    // Prüfe NVS-Status und initialisiere bei Bedarf neu
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS-Partition beschädigt oder falsche Version - löschen und neu initialisieren
        Serial.println("[Config] NVS corrupted, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    
    if (ret != ESP_OK) {
        Serial.printf("[Config] NVS init failed: %d\n", ret);
        return false;
    }
    
    return true;
}

void ConfigManager::logResetReason() {
    esp_reset_reason_t reason = esp_reset_reason();
    const char* reasonStr = "Unknown";
    
    switch (reason) {
        case ESP_RST_UNKNOWN:    reasonStr = "Unknown"; break;
        case ESP_RST_POWERON:    reasonStr = "Power-on"; break;
        case ESP_RST_EXT:        reasonStr = "External reset"; break;
        case ESP_RST_SW:         reasonStr = "Software reset"; break;
        case ESP_RST_PANIC:      reasonStr = "Panic/Exception"; break;
        case ESP_RST_INT_WDT:    reasonStr = "Interrupt watchdog"; break;
        case ESP_RST_TASK_WDT:   reasonStr = "Task watchdog"; break;
        case ESP_RST_WDT:        reasonStr = "Other watchdog"; break;
        case ESP_RST_DEEPSLEEP:  reasonStr = "Deep sleep wakeup"; break;
        case ESP_RST_BROWNOUT:   reasonStr = "Brownout"; break;
        case ESP_RST_SDIO:       reasonStr = "SDIO"; break;
        case ESP_RST_USB:        reasonStr = "USB"; break;
        case ESP_RST_JTAG:       reasonStr = "JTAG"; break;
        case ESP_RST_EFUSE:      reasonStr = "eFuse error"; break;
        case ESP_RST_PWR_GLITCH: reasonStr = "Power glitch"; break;
        case ESP_RST_CPU_LOCKUP: reasonStr = "CPU lockup"; break;
        default:                 reasonStr = "Other"; break;
    }
    
    Serial.printf("[Config] Reset reason: %s\n", reasonStr);
    
    // Zähle Resets für Diagnose
    Preferences prefs;
    if (prefs.begin(NVS_NAMESPACE, false)) {
        uint32_t bootCount = prefs.getUInt(KEY_BOOT_COUNT, 0);
        uint32_t prevReason = prefs.getUInt(KEY_LAST_RESET_REASON, 0);
        
        prefs.putUInt(KEY_BOOT_COUNT, bootCount + 1);
        prefs.putUInt(KEY_LAST_RESET_REASON, reason);
        
        // Bei kritischen Resets (Watchdog, Panic) prüfen ob Config noch intakt
        if (reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || 
            reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT) {
            Serial.println("[Config] Watchdog/Panic reset detected - checking config integrity");
            prefs.putBool(KEY_DIRTY_SHUTDOWN, true);
        }
        
        prefs.end();
        
        if (bootCount > 0) {
            Serial.printf("[Config] Boot count: %lu (previous reset: %lu)\n", 
                         bootCount + 1, prevReason);
        }
    }
}

bool ConfigManager::wasDirtyShutdown() {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, true)) {
        return false;
    }
    
    bool dirty = prefs.getBool(KEY_DIRTY_SHUTDOWN, false);
    prefs.end();
    
    return dirty;
}

void ConfigManager::clearDirtyShutdownFlag() {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, false)) {
        return;
    }
    
    prefs.putBool(KEY_DIRTY_SHUTDOWN, false);
    prefs.end();
}

bool ConfigManager::verifyConfigIntegrity() {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, true)) {
        return false;
    }
    
    // Prüfe ob alle Schlüssel existieren
    bool hasSsid = prefs.isKey(KEY_WIFI_SSID);
    bool hasPass = prefs.isKey(KEY_WIFI_PASS);
    bool hasConfigured = prefs.isKey(KEY_WIFI_CONFIGURED);
    
    prefs.end();
    
    if (!hasConfigured) {
        Serial.println("[Config] Integrity check failed: wifi_cfg missing");
        return false;
    }
    
    // Wenn konfiguriert, müssen auch SSID und Passwort existieren
    if (hasConfigured) {
        Preferences p;
        p.begin(NVS_NAMESPACE, true);
        bool configured = p.getBool(KEY_WIFI_CONFIGURED, false);
        p.end();
        
        if (configured && (!hasSsid || !hasPass)) {
            Serial.println("[Config] Integrity check failed: credentials incomplete");
            return false;
        }
    }
    
    return true;
}

bool ConfigManager::loadWiFiConfig(WiFiConfig& config) {
    // Initialisiere NVS falls noch nicht geschehen
    if (!initNVS()) {
        Serial.println("[Config] NVS not available");
        return false;
    }
    
    // Prüfe auf dirty shutdown
    if (wasDirtyShutdown()) {
        Serial.println("[Config] Previous shutdown was dirty - verifying config");
        if (!verifyConfigIntegrity()) {
            Serial.println("[Config] Config corrupted after dirty shutdown!");
            // Versuche Wiederherstellung aus Backup
            if (!restoreFromBackup()) {
                return false;
            }
        }
        clearDirtyShutdownFlag();
    }
    
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, true)) {  // Read-only
        Serial.println("[Config] Failed to open NVS");
        return false;
    }
    
    config.configured = prefs.getBool(KEY_WIFI_CONFIGURED, false);
    if (!config.configured) {
        prefs.end();
        return false;
    }
    
    String ssid = prefs.getString(KEY_WIFI_SSID, "");
    String pass = prefs.getString(KEY_WIFI_PASS, "");
    
    // Validierung
    if (ssid.length() == 0 || ssid.length() > 31) {
        Serial.println("[Config] Invalid SSID in NVS");
        prefs.end();
        return false;
    }
    
    strncpy(config.ssid, ssid.c_str(), sizeof(config.ssid) - 1);
    config.ssid[sizeof(config.ssid) - 1] = '\0';
    
    strncpy(config.password, pass.c_str(), sizeof(config.password) - 1);
    config.password[sizeof(config.password) - 1] = '\0';
    
    config.dhcp = prefs.getBool(KEY_WIFI_DHCP, true);  // Default to DHCP
    
    prefs.end();
    
    Serial.printf("[Config] Loaded WiFi config: SSID=%s, DHCP=%d\n", config.ssid, config.dhcp);
    return true;
}

bool ConfigManager::saveWiFiConfig(const WiFiConfig& config) {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, false)) {  // Read-write
        Serial.println("[Config] Failed to open NVS for writing");
        return false;
    }
    
    // Validiere vor dem Speichern
    if (config.configured && (strlen(config.ssid) == 0 || strlen(config.ssid) > 31)) {
        Serial.println("[Config] Invalid SSID, not saving");
        prefs.end();
        return false;
    }
    
    prefs.putString(KEY_WIFI_SSID, config.ssid);
    prefs.putString(KEY_WIFI_PASS, config.password);
    prefs.putBool(KEY_WIFI_CONFIGURED, config.configured);
    prefs.putBool(KEY_WIFI_DHCP, config.dhcp);
    
    // Speichere Timestamp der letzten Änderung
    prefs.putULong(KEY_CONFIG_VERSION, millis());
    
    prefs.end();
    
    // Erstelle Backup nach erfolgreichem Speichern
    createBackup();
    
    Serial.printf("[Config] Saved WiFi config: SSID=%s, configured=%d\n", 
                  config.ssid, config.configured);
    return true;
}

bool ConfigManager::clearWiFiConfig() {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, false)) {
        return false;
    }
    
    prefs.remove(KEY_WIFI_SSID);
    prefs.remove(KEY_WIFI_PASS);
    prefs.remove(KEY_WIFI_CONFIGURED);
    prefs.remove(KEY_WIFI_DHCP);
    prefs.remove(KEY_CONFIG_VERSION);
    
    prefs.end();
    
    // Lösche auch Backup
    deleteBackup();
    
    Serial.println("[Config] WiFi config cleared");
    return true;
}

bool ConfigManager::hasWiFiConfig() {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, true)) {
        return false;
    }
    
    bool configured = prefs.getBool(KEY_WIFI_CONFIGURED, false);
    prefs.end();
    
    return configured;
}

String ConfigManager::getDeviceName() {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, true)) {
        return "Linkra-Duo";
    }
    
    String name = prefs.getString(KEY_DEVICE_NAME, "Linkra-Duo");
    prefs.end();
    
    return name;
}

void ConfigManager::setDeviceName(const char* name) {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, false)) {
        return;
    }
    
    prefs.putString(KEY_DEVICE_NAME, name);
    prefs.end();
}

// Backup-Funktionen
bool ConfigManager::createBackup() {
    Preferences src, dst;
    
    if (!src.begin(NVS_NAMESPACE, true)) return false;
    if (!dst.begin(NVS_BACKUP_NAMESPACE, false)) {
        src.end();
        return false;
    }
    
    // Kopiere alle Werte
    String ssid = src.getString(KEY_WIFI_SSID, "");
    String pass = src.getString(KEY_WIFI_PASS, "");
    bool configured = src.getBool(KEY_WIFI_CONFIGURED, false);
    bool dhcp = src.getBool(KEY_WIFI_DHCP, true);
    
    dst.putString(KEY_WIFI_SSID, ssid);
    dst.putString(KEY_WIFI_PASS, pass);
    dst.putBool(KEY_WIFI_CONFIGURED, configured);
    dst.putBool(KEY_WIFI_DHCP, dhcp);
    dst.putULong(KEY_BACKUP_TIMESTAMP, millis());
    
    src.end();
    dst.end();
    
    Serial.println("[Config] Backup created");
    return true;
}

bool ConfigManager::restoreFromBackup() {
    Preferences src, dst;
    
    if (!src.begin(NVS_BACKUP_NAMESPACE, true)) return false;
    if (!dst.begin(NVS_NAMESPACE, false)) {
        src.end();
        return false;
    }
    
    // Prüfe ob Backup existiert
    if (!src.isKey(KEY_WIFI_CONFIGURED)) {
        Serial.println("[Config] No backup available");
        src.end();
        dst.end();
        return false;
    }
    
    // Kopiere alle Werte zurück
    String ssid = src.getString(KEY_WIFI_SSID, "");
    String pass = src.getString(KEY_WIFI_PASS, "");
    bool configured = src.getBool(KEY_WIFI_CONFIGURED, false);
    bool dhcp = src.getBool(KEY_WIFI_DHCP, true);
    
    dst.putString(KEY_WIFI_SSID, ssid);
    dst.putString(KEY_WIFI_PASS, pass);
    dst.putBool(KEY_WIFI_CONFIGURED, configured);
    dst.putBool(KEY_WIFI_DHCP, dhcp);
    
    src.end();
    dst.end();
    
    Serial.println("[Config] Restored from backup");
    return true;
}

void ConfigManager::deleteBackup() {
    nvs_flash_erase_partition(NVS_BACKUP_NAMESPACE);
}
