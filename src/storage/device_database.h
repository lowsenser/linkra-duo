#pragma once

#include <Arduino.h>
#include <sqlite3.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <set>
#include <string>
#include <vector>

/**
 * SQLite-based persistent storage for discovered RF devices.
 * 
 * Stores decoded RF devices on LittleFS (/littlefs/devices.db)
 * so they survive browser refreshes, tab switches, and ESP32 reboots.
 * 
 * MQTT: Geräte, die der Nutzer freigibt ("Per MQTT senden"), werden vom
 * MqttBridge an Home Assistant veröffentlicht.
 * 
 * Thread safety: All operations protected by FreeRTOS mutex.
 * Flash wear: Only writes on new device or value change (~few writes/hour).
 */

// Freigegebenes Gerät für die MQTT-Discovery
struct MqttDeviceInfo {
    std::string key;
    std::string model;
    int id = -1;
    int channel = -1;
    bool hasTemperature = false;
    bool hasHumidity = false;
    bool hasBattery = false;
};

class DeviceDatabase {
public:
    static DeviceDatabase& getInstance();
    
    /**
     * Initialize SQLite and create tables.
     * Must be called AFTER LittleFS.begin().
     */
    bool begin();
    
    /**
     * Insert or update a device from decoded RF signal data.
     */
    bool insertOrUpdate(const char* deviceKey, const char* model, int deviceId,
                        int channel, float temperature, float humidity,
                        int batteryOk, int rssi, float frequency,
                        const char* rawJson);
    
    /**
     * Get all stored devices as JSON array string.
     */
    String getAllDevicesJson();
    
    /**
     * Get number of stored devices.
     */
    int getDeviceCount();
    
    /**
     * Delete a specific device by key.
     */
    bool deleteDevice(const char* deviceKey);
    
    /**
     * Delete all stored devices.
     */
    bool clearAll();
    void close();                 // flush and close (call before ESP.restart)
    bool integrityOk = true;      // result of the last startup check
    int salvagedRows = -1;        // rows rescued from a damaged DB (-1 = no repair needed)
    bool repairIfCorrupt();
    
    /**
     * Check if a device is already persisted in the database.
     */
    bool isDevicePersisted(const char* deviceKey);
    
    // ===== MQTT-Freigabe ("Per MQTT senden") =====
    // Spalte heißt aus Kompatibilität weiter matter_enabled.
    bool setMqttEnabled(const char* deviceKey, bool enabled);
    bool isMqttEnabled(const char* deviceKey);          // O(1), thread-safe
    bool getMqttDevices(std::vector<MqttDeviceInfo>& out);
    String getMqttEnabledDevicesJson();

    /**
     * Close database.
     */
    
private:
    DeviceDatabase() : db(nullptr), dbMutex(nullptr), initialized(false) {}
    ~DeviceDatabase() { close(); }
    
    sqlite3* db;
    SemaphoreHandle_t dbMutex;
    bool initialized;
    
    // In-memory caches for O(1) lookup
    std::set<std::string> persistedKeys;
    std::set<std::string> mqttEnabledKeys;
    SemaphoreHandle_t cacheMutex = nullptr;   // guards persistedKeys + mqttEnabledKeys
    void cacheLock();
    void cacheUnlock();
    
    
    static const char* DB_PATH;
    
    bool execSQL(const char* sql);
    bool createTables();
    bool verifyMatterColumns();
    void migrateAddMatterColumns();
    void loadPersistedKeys();
    void loadMqttEnabledKeys();
};
