#pragma once

#include <Arduino.h>

/**
 * Config Manager
 * Speichert WiFi Credentials und Einstellungen in NVS (Preferences)
 * Mit automatischer Backup/Wiederherstellung und Integritätsprüfung
 */
struct WiFiConfig {
    char ssid[32];
    char password[64];
    bool configured;
    bool dhcp;
    char staticIP[16];
    char gateway[16];
    char subnet[16];
};

class ConfigManager {
public:
    static ConfigManager& getInstance();
    
    // NVS Initialisierung
    bool initNVS();
    void logResetReason();
    bool wasDirtyShutdown();
    void clearDirtyShutdownFlag();
    bool verifyConfigIntegrity();
    
    // WiFi Config
    bool loadWiFiConfig(WiFiConfig& config);
    bool saveWiFiConfig(const WiFiConfig& config);
    bool clearWiFiConfig();
    bool hasWiFiConfig();
    
    // Device Config
    String getDeviceName();
    void setDeviceName(const char* name);
    
    // Backup/Restore
    bool createBackup();
    bool restoreFromBackup();
    void deleteBackup();
    
private:
    ConfigManager() {}
    
    // Namespace für Hauptkonfiguration
    static constexpr const char* NVS_NAMESPACE = "linkra";
    static constexpr const char* NVS_BACKUP_NAMESPACE = "linkra_bak";
    
    // Keys für WiFi-Konfiguration
    static constexpr const char* KEY_WIFI_SSID = "wifi_ssid";
    static constexpr const char* KEY_WIFI_PASS = "wifi_pass";
    static constexpr const char* KEY_WIFI_CONFIGURED = "wifi_cfg";
    static constexpr const char* KEY_WIFI_DHCP = "wifi_dhcp";
    
    // Keys für Gerätekonfiguration
    static constexpr const char* KEY_DEVICE_NAME = "device_name";
    
    // Keys für Diagnose und Integrität
    static constexpr const char* KEY_BOOT_COUNT = "boot_count";
    static constexpr const char* KEY_LAST_RESET_REASON = "last_reset";
    static constexpr const char* KEY_DIRTY_SHUTDOWN = "dirty_shutdown";
    static constexpr const char* KEY_CONFIG_VERSION = "cfg_version";
    static constexpr const char* KEY_BACKUP_TIMESTAMP = "backup_ts";
};
