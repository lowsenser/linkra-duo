#include "mqtt_bridge.h"
#include "ha_discovery.h"
#include "../core/version.h"
#include "../storage/device_database.h"
#include "../network/update_check.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <mqtt_client.h>
#include <esp_crt_bundle.h>
#include <time.h>
#include <cmath>

static constexpr int QUEUE_LEN = 48;              // ~48 * 64 B in PSRAM; drops beyond
static constexpr const char* NVS_NS = "mqtt";

MqttBridge& MqttBridge::getInstance() {
    static MqttBridge inst;
    return inst;
}

// ---------------------------------------------------------------- settings

void MqttBridge::loadSettings() {
    Preferences p;
    p.begin(NVS_NS, true);
    cfg.enabled = p.getBool("en", false);
    cfg.host = p.getString("host", "");
    cfg.port = p.getUShort("port", 1883);
    cfg.tls = p.getBool("tls", false);
    cfg.user = p.getString("user", "");
    cfg.pass = p.getString("pass", "");
    cfg.baseTopic = p.getString("base", "linkra");
    cfg.discoveryPrefix = p.getString("prefix", "homeassistant");
    cfg.expireAfterS = p.getUInt("expire", 3600);
    p.end();
}

void MqttBridge::saveSettings() {
    Preferences p;
    p.begin(NVS_NS, false);
    p.putBool("en", cfg.enabled);
    p.putString("host", cfg.host);
    p.putUShort("port", cfg.port);
    p.putBool("tls", cfg.tls);
    p.putString("user", cfg.user);
    p.putString("pass", cfg.pass);
    p.putString("base", cfg.baseTopic);
    p.putString("prefix", cfg.discoveryPrefix);
    p.putUInt("expire", cfg.expireAfterS);
    p.end();
}

MqttSettings MqttBridge::settings() const {
    xSemaphoreTake(lock, portMAX_DELAY);
    MqttSettings s = cfg;
    xSemaphoreGive(lock);
    return s;
}

static bool validTopicPart(const String& t) {
    if (t.isEmpty() || t.length() > 48) return false;
    for (char ch : t) if (ch == '+' || ch == '#' || ch == ' ' || ch == '\0') return false;
    return !t.startsWith("/") && !t.endsWith("/");
}

bool MqttBridge::applySettings(const MqttSettings& in, bool clearPass, String& error) {
    if (in.enabled && in.host.isEmpty()) { error = "Broker-Adresse fehlt"; return false; }
    if (in.host.length() > 64) { error = "Broker-Adresse zu lang"; return false; }
    if (in.port == 0) { error = "Port ungültig"; return false; }
    if (in.user.length() > 64 || in.pass.length() > 128) { error = "Benutzer oder Passwort zu lang"; return false; }
    if (!validTopicPart(in.baseTopic) || !validTopicPart(in.discoveryPrefix)) {
        error = "Topic ungültig (keine Leerzeichen, + oder #)"; return false;
    }
    if (in.expireAfterS > 7 * 24 * 3600) { error = "Ablaufzeit zu groß"; return false; }

    stopClient();
    xSemaphoreTake(lock, portMAX_DELAY);
    const String oldPass = cfg.pass;
    cfg = in;
    if (in.pass.isEmpty() && !clearPass) cfg.pass = oldPass;
    saveSettings();
    xSemaphoreGive(lock);
    startClient();
    return true;
}

String MqttBridge::statusJson() const {
    JsonDocument doc;
    xSemaphoreTake(lock, portMAX_DELAY);
    doc["enabled"] = cfg.enabled;
    doc["host"] = cfg.host;
    doc["port"] = cfg.port;
    doc["tls"] = cfg.tls;
    doc["user"] = cfg.user;
    doc["has_password"] = !cfg.pass.isEmpty();
    doc["base_topic"] = cfg.baseTopic;
    doc["discovery_prefix"] = cfg.discoveryPrefix;
    doc["expire_after"] = cfg.expireAfterS;
    xSemaphoreGive(lock);
    doc["node_id"] = nodeId;
    doc["connected"] = (bool)connected;
    doc["published"] = (uint32_t)published;
    doc["dropped"] = (uint32_t)dropped;
    doc["failed"] = (uint32_t)failed;
    doc["reconnects"] = (uint32_t)reconnects;
    doc["connected_for_s"] = connected ? (millis() - lastConnectMs) / 1000 : 0;
    doc["last_error"] = (const char*)lastError;
    doc["queue"] = queue ? (int)uxQueueMessagesWaiting(queue) : 0;
    String out;
    serializeJson(doc, out);
    return out;
}

// ---------------------------------------------------------------- client

static ha::Config haConfig(const MqttSettings& s, const String& nodeId) {
    ha::Config c;
    c.baseTopic = s.baseTopic.c_str();
    c.discoveryPrefix = s.discoveryPrefix.c_str();
    c.nodeId = nodeId.c_str();
    c.swVersion = LINKRA_FW_VERSION;
    c.expireAfterS = (int)s.expireAfterS;
    return c;
}

// esp-mqtt copies all config strings, so these only need to live during init.
void MqttBridge::startClient() {
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!cfg.enabled || cfg.host.isEmpty() || client) { xSemaphoreGive(lock); return; }
    const String lwtTopic = cfg.baseTopic + "/status";
    esp_mqtt_client_config_t mc = {};
    mc.broker.address.hostname = cfg.host.c_str();
    mc.broker.address.port = cfg.port;
    mc.broker.address.transport = cfg.tls ? MQTT_TRANSPORT_OVER_SSL : MQTT_TRANSPORT_OVER_TCP;
    if (cfg.tls) mc.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    mc.credentials.client_id = nodeId.c_str();
    if (!cfg.user.isEmpty()) mc.credentials.username = cfg.user.c_str();
    if (!cfg.pass.isEmpty()) mc.credentials.authentication.password = cfg.pass.c_str();
    mc.session.last_will.topic = lwtTopic.c_str();
    mc.session.last_will.msg = "offline";
    mc.session.last_will.qos = 1;
    mc.session.last_will.retain = 1;
    mc.session.keepalive = 30;
    mc.network.reconnect_timeout_ms = 10000;
    mc.network.timeout_ms = 5000;
    mc.task.priority = 3;
    mc.task.stack_size = 6144;
    mc.buffer.size = 1024;
    mc.buffer.out_size = 1024;
    esp_mqtt_client_handle_t h = esp_mqtt_client_init(&mc);
    if (!h) {
        snprintf(lastError, sizeof(lastError), "Client konnte nicht erstellt werden");
        xSemaphoreGive(lock);
        return;
    }
    esp_mqtt_client_register_event(h, MQTT_EVENT_ANY, &MqttBridge::onEvent, this);
    client = h;
    xSemaphoreGive(lock);
    esp_mqtt_client_start(h);
    Serial.printf("[MQTT] Verbinde mit %s:%u%s\n", cfg.host.c_str(), cfg.port, cfg.tls ? " (TLS)" : "");
}

void MqttBridge::stopClient() {
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_mqtt_client_handle_t h = (esp_mqtt_client_handle_t)client;
    client = nullptr;
    xSemaphoreGive(lock);
    if (!h) return;
    if (connected) {
        const String t = cfg.baseTopic + "/status";
        esp_mqtt_client_publish(h, t.c_str(), "offline", 0, 1, 1);
    }
    esp_mqtt_client_stop(h);
    esp_mqtt_client_destroy(h);
    connected = false;
}

void MqttBridge::onEvent(void* arg, const char*, int32_t id, void* data) {
    auto* self = static_cast<MqttBridge*>(arg);
    auto* ev = static_cast<esp_mqtt_event_handle_t>(data);
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
        self->connected = true;
        self->reconnects++;
        self->lastConnectMs = millis();
        self->lastError[0] = '\0';
        const MqttSettings s = self->settings();
        const String st = s.baseTopic + "/status";
        esp_mqtt_client_publish(ev->client, st.c_str(), "online", 0, 1, 1);
        const String ha = s.discoveryPrefix + "/status";
        esp_mqtt_client_subscribe_single(ev->client, ha.c_str(), 0);
        const String inst = s.baseTopic + "/update/install";
        esp_mqtt_client_subscribe_single(ev->client, inst.c_str(), 1);
        Item it = {};
        it.type = ItemType::AllDiscovery;
        self->enqueue(it);
        Serial.println("[MQTT] Verbunden");
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        if (self->connected) Serial.println("[MQTT] Verbindung getrennt");
        self->connected = false;
        if (!self->lastError[0]) snprintf(self->lastError, sizeof(self->lastError), "Verbindung getrennt");
        break;
    case MQTT_EVENT_DATA: {
        // Home Assistant restarted -> publish discovery again
        const MqttSettings s = self->settings();
        const String ha = s.discoveryPrefix + "/status";
        if (ev->topic_len == (int)ha.length() && !strncmp(ev->topic, ha.c_str(), ev->topic_len) &&
            ev->data_len == 6 && !strncmp(ev->data, "online", 6)) {
            Item it = {};
            it.type = ItemType::AllDiscovery;
            self->enqueue(it);
        }
        // Install button of the HA update entity (only honoured if allowed in the web UI)
        const String inst = s.baseTopic + "/update/install";
        if (ev->topic_len == (int)inst.length() && !strncmp(ev->topic, inst.c_str(), ev->topic_len) &&
            ev->data_len == 7 && !strncmp(ev->data, "install", 7)) {
            String err;
            if (!UpdateCheck::get().haInstall()) Serial.println("[MQTT] Update aus Home Assistant ist nicht erlaubt");
            else if (!UpdateCheck::get().requestInstall(err)) Serial.printf("[MQTT] Update nicht gestartet: %s\n", err.c_str());
            else Serial.println("[MQTT] Update aus Home Assistant gestartet");
        }
        break;
    }
    case MQTT_EVENT_ERROR:
        if (ev->error_handle) {
            if (ev->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                const int rc = ev->error_handle->connect_return_code;
                snprintf(self->lastError, sizeof(self->lastError), "%s",
                         rc == MQTT_CONNECTION_REFUSE_BAD_USERNAME || rc == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED
                             ? "Benutzer oder Passwort falsch" : "Broker hat die Verbindung abgelehnt");
            } else if (ev->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                snprintf(self->lastError, sizeof(self->lastError), "Broker nicht erreichbar");
            }
        }
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------- queue + task

bool MqttBridge::enqueue(const Item& it) {
    if (!queue || xQueueSend(queue, &it, 0) != pdTRUE) { dropped++; return false; }
    return true;
}

void MqttBridge::begin() {
    if (!lock) lock = xSemaphoreCreateMutex();
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char id[24];
    snprintf(id, sizeof(id), "linkra_duo_%02x%02x%02x", mac[3], mac[4], mac[5]);
    nodeId = id;
    loadSettings();

    if (!queue) {
        // Queue storage in PSRAM: the radio path must never wait on MQTT.
        auto* buf = (uint8_t*)heap_caps_malloc(QUEUE_LEN * sizeof(Item), MALLOC_CAP_SPIRAM);
        static StaticQueue_t qs;
        if (buf) queue = xQueueCreateStatic(QUEUE_LEN, sizeof(Item), buf, &qs);
        else queue = xQueueCreate(QUEUE_LEN, sizeof(Item));
    }
    if (!task) xTaskCreatePinnedToCore(&MqttBridge::taskEntry, "mqttPub", 6144, this, 1, &task, 0);
    startClient();
    Serial.printf("[MQTT] %s (Node %s)\n", cfg.enabled ? "aktiviert" : "aus", nodeId.c_str());
}

void MqttBridge::publishReading(const char* key, const RFSignal& s) {
    if (!cfg.enabled || !connected || !key) return;
    Item it = {};
    it.type = ItemType::State;
    strlcpy(it.key, key, sizeof(it.key));
    it.temperature = s.temperature;
    it.humidity = s.humidity;
    it.batteryOk = (int8_t)(s.battery_ok < 0 ? -1 : (s.battery_ok ? 1 : 0));
    it.rssi = (int16_t)s.rssi;
    const time_t now = time(nullptr);
    it.epoch = now > 1700000000 ? (uint32_t)now : 0;
    enqueue(it);
}

void MqttBridge::deviceReleased(const char* key) {
    Item it = {};
    it.type = ItemType::Release;
    strlcpy(it.key, key, sizeof(it.key));
    enqueue(it);
}

void MqttBridge::deviceWithdrawn(const char* key) {
    Item it = {};
    it.type = ItemType::Withdraw;
    strlcpy(it.key, key, sizeof(it.key));
    enqueue(it);
}

void MqttBridge::republishDiscovery() {
    Item it = {};
    it.type = ItemType::AllDiscovery;
    enqueue(it);
}

void MqttBridge::publishUpdateState() {
    if (!cfg.enabled || !connected) return;
    Item it = {};
    it.type = ItemType::UpdateState;
    enqueue(it);
}

void MqttBridge::taskEntry(void* arg) { static_cast<MqttBridge*>(arg)->taskLoop(); }

void MqttBridge::taskLoop() {
    Item it;
    for (;;) {
        if (xQueueReceive(queue, &it, portMAX_DELAY) != pdTRUE) continue;
        xSemaphoreTake(lock, portMAX_DELAY);
        esp_mqtt_client_handle_t h = (esp_mqtt_client_handle_t)client;
        const MqttSettings s = cfg;
        xSemaphoreGive(lock);
        if (!h || !connected) continue;
        const ha::Config hc = haConfig(s, nodeId);

        std::vector<ha::Message> msgs;
        switch (it.type) {
        case ItemType::State: {
            ha::Reading r;
            r.hasTemperature = !std::isnan(it.temperature);
            r.temperature = it.temperature;
            r.hasHumidity = !std::isnan(it.humidity);
            r.humidity = it.humidity;
            r.batteryOk = it.batteryOk;
            r.rssi = it.rssi;
            if (it.epoch) {
                const time_t t = it.epoch;
                struct tm tmv;
                gmtime_r(&t, &tmv);
                char iso[32];
                strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%S+00:00", &tmv);
                r.lastSeenIso = iso;
            }
            msgs.push_back(ha::stateMessage(hc, it.key, r));
            break;
        }
        case ItemType::Release:
        case ItemType::AllDiscovery: {
            std::vector<MqttDeviceInfo> list;
            DeviceDatabase::getInstance().getMqttDevices(list);
            if (it.type == ItemType::AllDiscovery) {
                msgs.push_back(ha::bridgeDiscovery(hc));
                msgs.push_back(ha::updateDiscovery(hc, UpdateCheck::get().haInstall()));
                ha::UpdateInfo u;
                UpdateCheck::get().fillHaInfo(u);
                msgs.push_back(ha::updateState(hc, u));
            }
            for (const auto& d : list) {
                if (it.type == ItemType::Release && d.key != it.key) continue;
                ha::Device hd;
                hd.key = d.key;
                hd.model = d.model;
                hd.id = d.id;
                hd.channel = d.channel;
                hd.hasTemperature = d.hasTemperature;
                hd.hasHumidity = d.hasHumidity;
                hd.hasBattery = d.hasBattery;
                for (auto& m : ha::deviceDiscovery(hc, hd)) msgs.push_back(std::move(m));
            }
            break;
        }
        case ItemType::Withdraw:
            msgs = ha::deviceRemoval(hc, it.key);
            break;
        case ItemType::UpdateState: {
            ha::UpdateInfo u;
            UpdateCheck::get().fillHaInfo(u);
            msgs.push_back(ha::updateState(hc, u));
            break;
        }
        }
        for (const auto& m : msgs) {
            const int qos = m.retain ? 1 : 0;
            const int rc = esp_mqtt_client_publish(h, m.topic.c_str(), m.payload.c_str(),
                                                   (int)m.payload.size(), qos, m.retain ? 1 : 0);
            if (rc < 0) failed++; else published++;
        }
    }
}
