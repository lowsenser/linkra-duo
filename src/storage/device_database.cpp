#include <sys/time.h>
#include "device_database.h"
#include <LittleFS.h>
#include <set>
#include <string>
#include <ArduinoJson.h>

// LittleFS default mount point on ESP32 Arduino
const char* DeviceDatabase::DB_PATH = "/littlefs/devices.db";

DeviceDatabase& DeviceDatabase::getInstance() {
    static DeviceDatabase instance;
    return instance;
}

bool DeviceDatabase::begin() {
    if (initialized) return true;
    
    dbMutex = xSemaphoreCreateMutex();
    if (!cacheMutex) cacheMutex = xSemaphoreCreateMutex();
    if (!dbMutex) {
        Serial.println("[DB] Failed to create mutex");
        return false;
    }
    
    Serial.println("[DB] Initializing SQLite...");
    sqlite3_initialize();
    
    // Remove leftover WAL/SHM files from old WAL journal mode — 
    // LittleFS doesn't handle them correctly, causing stale reads
    if (LittleFS.exists("/devices.db-wal")) {
        LittleFS.remove("/devices.db-wal");
        Serial.println("[DB] Removed leftover -wal file");
    }
    if (LittleFS.exists("/devices.db-shm")) {
        LittleFS.remove("/devices.db-shm");
        Serial.println("[DB] Removed leftover -shm file");
    }
    
    int rc = sqlite3_open(DB_PATH, &db);
    if (rc != SQLITE_OK) {
        Serial.printf("[DB] Failed to open database: %s\n", sqlite3_errmsg(db));
        // Try to recover: delete corrupt DB and retry
        Serial.println("[DB] Attempting recovery — deleting corrupt DB...");
        sqlite3_close(db);
        db = nullptr;
        LittleFS.remove("/devices.db");
        rc = sqlite3_open(DB_PATH, &db);
        if (rc != SQLITE_OK) {
            Serial.printf("[DB] Recovery failed: %s\n", sqlite3_errmsg(db));
            return false;
        }
        Serial.println("[DB] Recovery: new DB created");
    }
    
    Serial.printf("[DB] Database opened: %s\n", DB_PATH);
    
    // Use DELETE journal mode — WAL creates -wal/-shm files that
    // LittleFS does not support correctly (reads see stale/empty data)
    // PERSIST: the rollback journal is never unlinked, only its header is
    // zeroed after commit. With DELETE the journal was not removed on this
    // LittleFS VFS, so every boot replayed a stale "hot" journal and rolled
    // the DB back (schema check then dropped the table -> all devices lost).
    execSQL("PRAGMA journal_mode=PERSIST");
    
    // Reduce sync frequency - we can tolerate losing last write on crash
    execSQL("PRAGMA synchronous=NORMAL");
    // Sorting/grouping above a few dozen rows needs temp storage; SQLite's default
    // temp file cannot be created on this VFS, so ORDER BY queries silently
    // returned no rows once ~180 devices were stored. Keep temp data in RAM (PSRAM).
    execSQL("PRAGMA temp_store=MEMORY");
    
    if (!createTables()) {
        Serial.println("[DB] Failed to create tables");
        sqlite3_close(db);
        db = nullptr;
        return false;
    }
    
    // Migration: Matter-Spalten hinzufügen (ignoriert Fehler wenn schon vorhanden)
    migrateAddMatterColumns();
    
    // Verify schema: check that matter columns actually exist
    // (ALTER TABLE may silently fail on some ESP32 SQLite builds)
    if (!verifyMatterColumns()) {
        Serial.println("[DB] ✗ Matter columns missing after migration — recreating table...");
        // Drop old table and recreate with full schema
        execSQL("DROP TABLE IF EXISTS rf_devices;");
        if (!createTables()) {
            Serial.println("[DB] ✗ Failed to recreate table!");
            sqlite3_close(db);
            db = nullptr;
            return false;
        }
        Serial.println("[DB] ✓ Table recreated with matter columns");
    }
    
    initialized = true;
    repairIfCorrupt();
    
    int count = getDeviceCount();
    Serial.printf("[DB] ✓ SQLite ready, %d devices in database\n", count);
    
    // Self-test: verify INSERT + READ works (detects WAL corruption)
    bool testOk = insertOrUpdate("_selftest_", "DB-SelfTest", 0, -1, 
                                  22.5f, 55.0f, 1, -50, 433.92f, "{}");
    if (testOk) {
        // Verify READ works too — this catches WAL corruption where writes 
        // succeed but reads return stale/empty data
        int countAfter = getDeviceCount();
        if (countAfter >= 1) {
            // Clean up test record
            execSQL("DELETE FROM rf_devices WHERE device_key = '_selftest_';");
            Serial.println("[DB] ✓ Self-test PASSED (insert + read works)");
        } else {
            Serial.printf("[DB] ✗ Self-test READ FAILED: inserted but count=%d\n", countAfter);
            testOk = false;
        }
    }
    if (!testOk) {
        Serial.println("[DB] ✗ Self-test FAILED — deleting corrupt DB and retrying...");
        sqlite3_close(db);
        db = nullptr;
        initialized = false;
        LittleFS.remove("/devices.db");
        LittleFS.remove("/devices.db-journal");
        // Recursive retry (once)
        static bool retried = false;
        if (!retried) {
            retried = true;
            return begin();
        }
        Serial.println("[DB] ✗ Self-test FAILED after retry!");
    }
    
    // Load persisted device keys into memory for fast lookup
    loadPersistedKeys();
    loadMqttEnabledKeys();
    
    return true;
}

bool DeviceDatabase::createTables() {
    const char* sql = 
        "CREATE TABLE IF NOT EXISTS rf_devices ("
        "  device_key TEXT PRIMARY KEY,"
        "  model TEXT NOT NULL,"
        "  device_id INTEGER,"
        "  channel INTEGER DEFAULT -1,"
        "  temperature REAL,"
        "  humidity REAL,"
        "  battery_ok INTEGER DEFAULT -1,"
        "  rssi INTEGER,"
        "  frequency REAL,"
        "  first_seen INTEGER,"
        "  last_seen INTEGER,"
        "  signal_count INTEGER DEFAULT 0,"
        "  raw_json TEXT,"
        "  matter_enabled INTEGER DEFAULT 0,"
        "  matter_type TEXT DEFAULT ''"
        ");";
    
    return execSQL(sql);
}

bool DeviceDatabase::execSQL(const char* sql) {
    if (!db) return false;
    
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
    
    if (rc != SQLITE_OK) {
        Serial.printf("[DB] SQL error: %s\n", errMsg ? errMsg : "unknown");
        if (errMsg) sqlite3_free(errMsg);
        return false;
    }
    return true;
}

bool DeviceDatabase::verifyMatterColumns() {
    if (!db) return false;
    // PRAGMA table_info returns no rows in this SQLite build (schema pragmas
    // are omitted), which made this check fail on every boot and DROP the
    // table. Preparing a statement that names the columns is a reliable test.
    sqlite3_stmt* stmt = nullptr;
    const int rc = sqlite3_prepare_v2(db,
        "SELECT matter_enabled, matter_type FROM rf_devices LIMIT 0;", -1, &stmt, nullptr);
    if (stmt) sqlite3_finalize(stmt);
    const bool ok = rc == SQLITE_OK;
    Serial.printf("[DB] Schema check: matter columns %s\n", ok ? "present" : "MISSING");
    return ok;
}

bool DeviceDatabase::insertOrUpdate(const char* deviceKey, const char* model, int deviceId,
                                     int channel, float temperature, float humidity,
                                     int batteryOk, int rssi, float frequency,
                                     const char* rawJson) {
    if (!initialized || !db) return false;
    
    if (xSemaphoreTake(dbMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        Serial.println("[DB] Mutex timeout on insertOrUpdate");
        return false;
    }
    
    bool success = false;
    // Wall-clock epoch ms once NTP has synced, otherwise ms since boot.
    // (Clients tell them apart: epoch ms > 1e12.)
    struct timeval tv; gettimeofday(&tv, nullptr);
    const int64_t now = tv.tv_sec > 1700000000
        ? (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000
        : (int64_t)millis();
    
    // Two-step UPSERT: try UPDATE first, then INSERT if device is new.
    // This avoids nested subqueries that cause "parser stack overflow" on ESP32 SQLite.
    
    // Step 1: Try UPDATE existing device
    const char* updateSql = 
        "UPDATE rf_devices SET "
        "  temperature = ?1, humidity = ?2, battery_ok = ?3, "
        "  rssi = ?4, last_seen = ?5, signal_count = signal_count + 1, "
        "  raw_json = ?6 "
        "WHERE device_key = ?7;";
    
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, updateSql, -1, &stmt, nullptr);
    
    if (rc != SQLITE_OK) {
        Serial.printf("[DB] Update prepare error: %s\n", sqlite3_errmsg(db));
        xSemaphoreGive(dbMutex);
        return false;
    }
    
    if (isnan(temperature)) {
        sqlite3_bind_null(stmt, 1);
    } else {
        sqlite3_bind_double(stmt, 1, temperature);
    }
    if (isnan(humidity)) {
        sqlite3_bind_null(stmt, 2);
    } else {
        sqlite3_bind_double(stmt, 2, humidity);
    }
    sqlite3_bind_int(stmt, 3, batteryOk);
    sqlite3_bind_int(stmt, 4, rssi);
    sqlite3_bind_int64(stmt, 5, (sqlite3_int64)now);
    sqlite3_bind_text(stmt, 6, rawJson, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, deviceKey, -1, SQLITE_TRANSIENT);
    
    rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(db);
    sqlite3_finalize(stmt);
    
    if (changes > 0) {
        // Device existed and was updated
        success = true;
    } else {
        // Step 2: INSERT new device
        const char* insertSql = 
            "INSERT INTO rf_devices "
            "(device_key, model, device_id, channel, temperature, humidity, "
            " battery_ok, rssi, frequency, first_seen, last_seen, signal_count, raw_json) "
            "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?10, 1, ?11);";
        
        rc = sqlite3_prepare_v2(db, insertSql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            Serial.printf("[DB] Insert prepare error: %s\n", sqlite3_errmsg(db));
            xSemaphoreGive(dbMutex);
            return false;
        }
        
        sqlite3_bind_text(stmt, 1, deviceKey, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, model, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, deviceId);
        sqlite3_bind_int(stmt, 4, channel);
        if (isnan(temperature)) {
            sqlite3_bind_null(stmt, 5);
        } else {
            sqlite3_bind_double(stmt, 5, temperature);
        }
        if (isnan(humidity)) {
            sqlite3_bind_null(stmt, 6);
        } else {
            sqlite3_bind_double(stmt, 6, humidity);
        }
        sqlite3_bind_int(stmt, 7, batteryOk);
        sqlite3_bind_int(stmt, 8, rssi);
        sqlite3_bind_double(stmt, 9, frequency);
        sqlite3_bind_int64(stmt, 10, (sqlite3_int64)now);
        sqlite3_bind_text(stmt, 11, rawJson, -1, SQLITE_TRANSIENT);
        
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            Serial.printf("[DB] Insert error: %s\n", sqlite3_errmsg(db));
        } else {
            success = true;
            cacheLock(); persistedKeys.insert(std::string(deviceKey)); cacheUnlock();
            Serial.printf("[DB] New device saved: %s\n", deviceKey);
        }
        sqlite3_finalize(stmt);
    }
    
    xSemaphoreGive(dbMutex);
    
    
    return success;
}

String DeviceDatabase::getAllDevicesJson() {
    String result = "[";
    
    if (!initialized || !db) return "[]";
    
    if (xSemaphoreTake(dbMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return "[]";
    }
    
    const char* sql = 
        "SELECT device_key, model, device_id, channel, temperature, humidity, "
        "battery_ok, rssi, frequency, first_seen, last_seen, signal_count, "
        "COALESCE(matter_enabled, 0), COALESCE(matter_type, '') "
        "FROM rf_devices ORDER BY last_seen DESC;";    
    
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    
    if (rc != SQLITE_OK) {
        Serial.printf("[DB] getAllDevicesJson prepare FAILED: %s (rc=%d)\n", 
                      sqlite3_errmsg(db), rc);
        xSemaphoreGive(dbMutex);
        return "[]";
    }
    
    bool first = true;
    result.reserve(32768);
    int stepRc;
    while ((stepRc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (!first) result += ",";
        first = false;
        
        result += "{";
        result += "\"device_key\":\"" + String((const char*)sqlite3_column_text(stmt, 0)) + "\",";
        result += "\"model\":\"" + String((const char*)sqlite3_column_text(stmt, 1)) + "\",";
        result += "\"device_id\":" + String(sqlite3_column_int(stmt, 2)) + ",";
        result += "\"channel\":" + String(sqlite3_column_int(stmt, 3)) + ",";
        
        // Temperature (nullable)
        if (sqlite3_column_type(stmt, 4) != SQLITE_NULL) {
            result += "\"temperature\":" + String(sqlite3_column_double(stmt, 4), 1) + ",";
        }
        
        // Humidity (nullable)
        if (sqlite3_column_type(stmt, 5) != SQLITE_NULL) {
            result += "\"humidity\":" + String(sqlite3_column_double(stmt, 5), 0) + ",";
        }
        
        result += "\"battery_ok\":" + String(sqlite3_column_int(stmt, 6)) + ",";
        result += "\"rssi\":" + String(sqlite3_column_int(stmt, 7)) + ",";
        result += "\"frequency\":" + String(sqlite3_column_double(stmt, 8), 2) + ",";
        result += "\"first_seen\":" + String((long long)sqlite3_column_int64(stmt, 9)) + ",";
        result += "\"last_seen\":" + String((long long)sqlite3_column_int64(stmt, 10)) + ",";
        result += "\"signal_count\":" + String(sqlite3_column_int(stmt, 11)) + ",";
        result += "\"mqtt_enabled\":" + String(sqlite3_column_int(stmt, 12));
        result += "}";
    }
    
    if (stepRc != SQLITE_DONE)
        Serial.printf("[DB] getAllDevicesJson step failed: %s (rc=%d)\n", sqlite3_errmsg(db), stepRc);
    sqlite3_finalize(stmt);
    xSemaphoreGive(dbMutex);
    
    result += "]";
    return result;
}

int DeviceDatabase::getDeviceCount() {
    if (!initialized || !db) return 0;
    
    if (xSemaphoreTake(dbMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return 0;
    }
    
    int count = 0;
    const char* sql = "SELECT COUNT(*) FROM rf_devices;";
    sqlite3_stmt* stmt = nullptr;
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            count = sqlite3_column_int(stmt, 0);
        }
    }
    
    sqlite3_finalize(stmt);
    xSemaphoreGive(dbMutex);
    
    return count;
}

bool DeviceDatabase::deleteDevice(const char* deviceKey) {
    if (!initialized || !db) return false;
    
    if (xSemaphoreTake(dbMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return false;
    }
    
    const char* sql = "DELETE FROM rf_devices WHERE device_key = ?;";
    sqlite3_stmt* stmt = nullptr;
    bool success = false;
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, deviceKey, -1, SQLITE_TRANSIENT);
        success = (sqlite3_step(stmt) == SQLITE_DONE);
        if (success) {
            cacheLock(); persistedKeys.erase(std::string(deviceKey)); mqttEnabledKeys.erase(std::string(deviceKey)); cacheUnlock();
        }
    }
    
    sqlite3_finalize(stmt);
    xSemaphoreGive(dbMutex);
    
    return success;
}

bool DeviceDatabase::clearAll() {
    if (!initialized || !db) return false;
    
    if (xSemaphoreTake(dbMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return false;
    }
    
    bool success = false;
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db, "DELETE FROM rf_devices;", nullptr, nullptr, &errMsg);
    
    if (rc == SQLITE_OK) {
        // Reclaim space
        sqlite3_exec(db, "VACUUM;", nullptr, nullptr, nullptr);
        success = true;
        cacheLock(); persistedKeys.clear(); mqttEnabledKeys.clear(); cacheUnlock();
        Serial.println("[DB] All devices cleared");
    } else {
        Serial.printf("[DB] Clear error: %s\n", errMsg ? errMsg : "unknown");
        if (errMsg) sqlite3_free(errMsg);
    }
    
    xSemaphoreGive(dbMutex);
    return success;
}

bool DeviceDatabase::isDevicePersisted(const char* deviceKey) {
    cacheLock();
    const bool known = persistedKeys.count(std::string(deviceKey)) > 0;
    cacheUnlock();
    return known;
}

void DeviceDatabase::loadPersistedKeys() {
    std::set<std::string> keys;
    if (!initialized || !db) return;
    if (xSemaphoreTake(dbMutex, pdMS_TO_TICKS(2000)) != pdTRUE) return;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT device_key FROM rf_devices;", -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* key = (const char*)sqlite3_column_text(stmt, 0);
            if (key) keys.insert(std::string(key));
        }
    }
    sqlite3_finalize(stmt);
    xSemaphoreGive(dbMutex);
    cacheLock(); persistedKeys.swap(keys); const int n = (int)persistedKeys.size(); cacheUnlock();
    Serial.printf("[DB] Loaded %d persisted device keys into cache\n", n);
}

void DeviceDatabase::close() {
    if (db) {
        if (dbMutex && xSemaphoreTake(dbMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
            sqlite3_close(db);
            db = nullptr;
            initialized = false;
            xSemaphoreGive(dbMutex);
        }
    }
}

// ===== MQTT-Freigabe ("Per MQTT senden") =====
// Die Spalten heißen aus Kompatibilität mit bestehenden Datenbanken weiter
// matter_enabled / matter_type; matter_enabled bedeutet seit 0.6.0
// "per MQTT an Home Assistant senden", matter_type wird nicht mehr genutzt.

void DeviceDatabase::migrateAddMatterColumns() {
    // ALTER TABLE ignoriert Fehler falls Spalten schon existieren
    sqlite3_exec(db, "ALTER TABLE rf_devices ADD COLUMN matter_enabled INTEGER DEFAULT 0;",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(db, "ALTER TABLE rf_devices ADD COLUMN matter_type TEXT DEFAULT '';",
                 nullptr, nullptr, nullptr);
}

void DeviceDatabase::cacheLock()   { if (cacheMutex) xSemaphoreTake(cacheMutex, portMAX_DELAY); }
void DeviceDatabase::cacheUnlock() { if (cacheMutex) xSemaphoreGive(cacheMutex); }

void DeviceDatabase::loadMqttEnabledKeys() {
    std::set<std::string> keys;
    if (!initialized || !db) return;
    if (xSemaphoreTake(dbMutex, pdMS_TO_TICKS(2000)) != pdTRUE) return;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT device_key FROM rf_devices WHERE matter_enabled = 1;",
                           -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* key = (const char*)sqlite3_column_text(stmt, 0);
            if (key) keys.insert(std::string(key));
        }
    }
    sqlite3_finalize(stmt);
    xSemaphoreGive(dbMutex);
    cacheLock(); mqttEnabledKeys.swap(keys); const int n = (int)mqttEnabledKeys.size(); cacheUnlock();
    Serial.printf("[DB] %d Geraete fuer MQTT freigegeben\n", n);
}

bool DeviceDatabase::setMqttEnabled(const char* deviceKey, bool enabled) {
    if (!initialized || !db) return false;
    if (xSemaphoreTake(dbMutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    sqlite3_stmt* stmt = nullptr;
    bool success = false;
    if (sqlite3_prepare_v2(db, "UPDATE rf_devices SET matter_enabled = ?, matter_type = '' WHERE device_key = ?;",
                           -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, enabled ? 1 : 0);
        sqlite3_bind_text(stmt, 2, deviceKey, -1, SQLITE_TRANSIENT);
        success = (sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) > 0);
    }
    sqlite3_finalize(stmt);
    xSemaphoreGive(dbMutex);
    if (success) {
        cacheLock();
        if (enabled) mqttEnabledKeys.insert(std::string(deviceKey));
        else         mqttEnabledKeys.erase(std::string(deviceKey));
        cacheUnlock();
        Serial.printf("[DB] MQTT %s fuer %s\n", enabled ? "an" : "aus", deviceKey);
    }
    return success;
}

bool DeviceDatabase::isMqttEnabled(const char* deviceKey) {
    cacheLock();
    const bool on = mqttEnabledKeys.count(std::string(deviceKey)) > 0;
    cacheUnlock();
    return on;
}

bool DeviceDatabase::getMqttDevices(std::vector<MqttDeviceInfo>& out) {
    out.clear();
    if (!initialized || !db) return false;
    if (xSemaphoreTake(dbMutex, pdMS_TO_TICKS(2000)) != pdTRUE) return false;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT device_key, model, device_id, channel, temperature, humidity, battery_ok "
            "FROM rf_devices WHERE matter_enabled = 1 ORDER BY device_key;", -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            MqttDeviceInfo d;
            const char* k = (const char*)sqlite3_column_text(stmt, 0);
            const char* m = (const char*)sqlite3_column_text(stmt, 1);
            d.key = k ? k : "";
            d.model = m ? m : "";
            d.id = sqlite3_column_int(stmt, 2);
            d.channel = sqlite3_column_int(stmt, 3);
            d.hasTemperature = sqlite3_column_type(stmt, 4) != SQLITE_NULL;
            d.hasHumidity = sqlite3_column_type(stmt, 5) != SQLITE_NULL;
            d.hasBattery = sqlite3_column_type(stmt, 6) != SQLITE_NULL && sqlite3_column_int(stmt, 6) >= 0;
            out.push_back(d);
        }
    }
    sqlite3_finalize(stmt);
    xSemaphoreGive(dbMutex);
    return true;
}

String DeviceDatabase::getMqttEnabledDevicesJson() {
    std::vector<MqttDeviceInfo> list;
    getMqttDevices(list);
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (const auto& d : list) {
        JsonObject o = arr.add<JsonObject>();
        o["device_key"] = d.key.c_str();
        o["model"] = d.model.c_str();
        o["device_id"] = d.id;
        o["channel"] = d.channel;
    }
    String out;
    serializeJson(doc, out);
    return out;
}

// ===================================================================
//  Robustness: startup integrity check + salvage, clean close
// ===================================================================
bool DeviceDatabase::repairIfCorrupt() {
    if (!db) return false;
    // PRAGMA quick_check returns nothing in this SQLite build, so test what actually
    // matters: can every row be read with the same query the API uses?
    sqlite3_stmt* st = nullptr;
    int rc = sqlite3_prepare_v2(db,
        "SELECT device_key, model, device_id, channel, temperature, humidity, battery_ok, rssi, "
        "frequency, first_seen, last_seen, signal_count, matter_enabled, matter_type "
        "FROM rf_devices ORDER BY last_seen DESC;", -1, &st, nullptr);
    if (rc == SQLITE_OK) { while ((rc = sqlite3_step(st)) == SQLITE_ROW) {} sqlite3_finalize(st); }
    integrityOk = rc == SQLITE_DONE;
    Serial.printf("[DB] Read check: %s\n", integrityOk ? "ok" : sqlite3_errmsg(db));
    if (integrityOk) return true;

    // Salvage readable rows (one rowid at a time) into a new file with the full schema
    LittleFS.remove("/devices_fixed.db"); LittleFS.remove("/devices_fixed.db-journal");
    sqlite3_exec(db, "ATTACH DATABASE '/littlefs/devices_fixed.db' AS fx;", nullptr, nullptr, nullptr);
    sqlite3_exec(db,
        "CREATE TABLE fx.rf_devices (device_key TEXT PRIMARY KEY, model TEXT NOT NULL, device_id INTEGER, "
        "channel INTEGER DEFAULT -1, temperature REAL, humidity REAL, battery_ok INTEGER DEFAULT -1, "
        "rssi INTEGER, frequency REAL, first_seen INTEGER, last_seen INTEGER, signal_count INTEGER DEFAULT 0, "
        "raw_json TEXT, matter_enabled INTEGER DEFAULT 0, matter_type TEXT DEFAULT '');", nullptr, nullptr, nullptr);
    sqlite3_int64 maxId = 0;
    if (sqlite3_prepare_v2(db, "SELECT max(rowid) FROM main.rf_devices;", -1, &st, nullptr) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) maxId = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    int copied = 0;
    sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr);
    for (sqlite3_int64 id = 1; id <= maxId && id < 100000; id++) {
        char sql[128];
        snprintf(sql, sizeof sql, "INSERT OR IGNORE INTO fx.rf_devices SELECT * FROM main.rf_devices WHERE rowid=%lld;", (long long)id);
        if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK && sqlite3_changes(db) > 0) copied++;
    }
    sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "DETACH DATABASE fx;", nullptr, nullptr, nullptr);
    salvagedRows = copied;
    Serial.printf("[DB] Salvaged %d of max %lld rows\n", copied, (long long)maxId);
    sqlite3_close(db); db = nullptr;
    LittleFS.remove("/devices.db"); LittleFS.remove("/devices.db-journal");
    LittleFS.rename("/devices_fixed.db", "/devices.db");
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) { initialized = false; return false; }
    execSQL("PRAGMA journal_mode=PERSIST"); execSQL("PRAGMA synchronous=NORMAL"); execSQL("PRAGMA temp_store=MEMORY");
    Serial.printf("[DB] Repair done, %d devices\n", getDeviceCount());
    return false;
}
