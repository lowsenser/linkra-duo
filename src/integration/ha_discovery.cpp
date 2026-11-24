#include "ha_discovery.h"

#include <ArduinoJson.h>
#include <cctype>
#include <cmath>

namespace ha {

namespace {

struct EntityDef {
    const char* suffix;      // unique-id / topic suffix
    const char* component;   // sensor / binary_sensor
    const char* name;        // entity name (shown after the device name)
    const char* deviceClass;
    const char* unit;        // nullptr = none
    const char* stateClass;  // nullptr = none
    const char* valueTemplate;
    bool diagnostic;
};

const EntityDef kTemperature = {"temperature", "sensor", "Temperatur", "temperature", "°C", "measurement",
                                "{{ value_json.temperature }}", false};
const EntityDef kHumidity = {"humidity", "sensor", "Luftfeuchte", "humidity", "%", "measurement",
                             "{{ value_json.humidity }}", false};
const EntityDef kBattery = {"battery", "binary_sensor", "Batterie", "battery", nullptr, nullptr,
                            "{{ 'ON' if value_json.battery_ok == 0 else 'OFF' }}", false};
const EntityDef kRssi = {"rssi", "sensor", "Signalstärke", "signal_strength", "dBm", "measurement",
                         "{{ value_json.rssi }}", true};
const EntityDef kLastSeen = {"last_seen", "sensor", "Zuletzt gesehen", "timestamp", nullptr, nullptr,
                             "{{ value_json.last_seen | default(None) }}", true};

const EntityDef* const kAll[] = {&kTemperature, &kHumidity, &kBattery, &kRssi, &kLastSeen};

std::string configTopic(const Config& c, const char* component, const std::string& obj, const char* suffix) {
    return c.discoveryPrefix + "/" + component + "/" + c.nodeId + "/" + obj + "_" + suffix + "/config";
}

void addAvailability(JsonObject o, const Config& c) {
    JsonArray av = o["availability"].to<JsonArray>();
    JsonObject a = av.add<JsonObject>();
    a["topic"] = statusTopic(c);
}

std::string serialize(const JsonDocument& doc) {
    std::string out;
    serializeJson(doc, out);
    return out;
}

}  // namespace

std::string objectId(const std::string& key) {
    std::string out;
    out.reserve(key.size());
    bool lastUnderscore = false;
    for (unsigned char ch : key) {
        if (std::isalnum(ch)) {
            out += (char)std::tolower(ch);
            lastUnderscore = false;
        } else if (!lastUnderscore && !out.empty()) {
            out += '_';
            lastUnderscore = true;
        }
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    if (out.empty()) out = "device";
    return out;
}

std::string statusTopic(const Config& c) { return c.baseTopic + "/status"; }

std::string stateTopic(const Config& c, const std::string& key) {
    return c.baseTopic + "/sensor/" + objectId(key) + "/state";
}

std::string deviceName(const Device& d) {
    std::string name = d.model.empty() ? d.key : d.model;
    if (d.id >= 0) name += " " + std::to_string(d.id);
    if (d.channel >= 0) name += " (Kanal " + std::to_string(d.channel) + ")";
    return name;
}

Message bridgeDiscovery(const Config& c) {
    JsonDocument doc;
    doc["name"] = "Verbindung";
    doc["unique_id"] = c.nodeId + "_connectivity";
    doc["object_id"] = c.nodeId + "_connectivity";
    doc["device_class"] = "connectivity";
    doc["entity_category"] = "diagnostic";
    doc["state_topic"] = statusTopic(c);
    doc["payload_on"] = "online";
    doc["payload_off"] = "offline";
    JsonObject dev = doc["device"].to<JsonObject>();
    dev["identifiers"].to<JsonArray>().add(c.nodeId);
    dev["name"] = c.nodeName;
    dev["manufacturer"] = "Linkra";
    dev["model"] = "Linkra Duo";
    if (!c.swVersion.empty()) dev["sw_version"] = c.swVersion;
    return {c.discoveryPrefix + "/binary_sensor/" + c.nodeId + "/connectivity/config", serialize(doc), true};
}

std::vector<Message> deviceDiscovery(const Config& c, const Device& d) {
    std::vector<Message> out;
    const std::string obj = objectId(d.key);
    const std::string name = deviceName(d);
    for (const EntityDef* e : kAll) {
        if (e == &kTemperature && !d.hasTemperature) continue;
        if (e == &kHumidity && !d.hasHumidity) continue;
        if (e == &kBattery && !d.hasBattery) continue;
        JsonDocument doc;
        doc["name"] = e->name;
        doc["unique_id"] = c.nodeId + "_" + obj + "_" + e->suffix;
        doc["object_id"] = obj + "_" + e->suffix;
        doc["state_topic"] = stateTopic(c, d.key);
        doc["value_template"] = e->valueTemplate;
        doc["device_class"] = e->deviceClass;
        if (e->unit) doc["unit_of_measurement"] = e->unit;
        if (e->stateClass) doc["state_class"] = e->stateClass;
        if (e->diagnostic) doc["entity_category"] = "diagnostic";
        if (c.expireAfterS > 0 && e != &kLastSeen) doc["expire_after"] = c.expireAfterS;
        addAvailability(doc.as<JsonObject>(), c);
        JsonObject dev = doc["device"].to<JsonObject>();
        dev["identifiers"].to<JsonArray>().add(c.nodeId + "_" + obj);
        dev["name"] = name;
        dev["model"] = d.model.empty() ? d.key : d.model;
        dev["manufacturer"] = "Funksensor (über Linkra Duo)";
        dev["via_device"] = c.nodeId;
        out.push_back({configTopic(c, e->component, obj, e->suffix), serialize(doc), true});
    }
    return out;
}

std::vector<Message> deviceRemoval(const Config& c, const std::string& key) {
    std::vector<Message> out;
    const std::string obj = objectId(key);
    for (const EntityDef* e : kAll) out.push_back({configTopic(c, e->component, obj, e->suffix), "", true});
    return out;
}

Message stateMessage(const Config& c, const std::string& key, const Reading& r) {
    JsonDocument doc;
    if (r.hasTemperature && std::isfinite(r.temperature)) doc["temperature"] = std::round(r.temperature * 10.0) / 10.0;
    if (r.hasHumidity && std::isfinite(r.humidity)) doc["humidity"] = std::round(r.humidity * 10.0) / 10.0;
    if (r.batteryOk >= 0) doc["battery_ok"] = r.batteryOk;
    doc["rssi"] = r.rssi;
    if (!r.lastSeenIso.empty()) doc["last_seen"] = r.lastSeenIso;
    return {stateTopic(c, key), serialize(doc), false};
}

}  // namespace ha
