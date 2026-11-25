// Host test for src/integration/ha_discovery.cpp (Home Assistant MQTT discovery).
// Build: c++ -std=c++17 -fsanitize=address,undefined -I.pio/libdeps/linkra-s3-breadboard/ArduinoJson/src \
//        tests/ha_discovery_test.cpp src/integration/ha_discovery.cpp -o /tmp/ha_test && /tmp/ha_test
#include "../src/integration/ha_discovery.h"

#include <ArduinoJson.h>
#include <cstdio>
#include <string>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static ha::Config cfg() {
    ha::Config c;
    c.nodeId = "linkra_duo_a1b2c3";
    c.swVersion = "0.6.0-beta";
    return c;
}

static JsonDocument parse(const std::string& s) {
    JsonDocument d;
    CHECK(!deserializeJson(d, s));
    return d;
}

static void testObjectId() {
    CHECK(ha::objectId("Nexus-TH_160") == "nexus_th_160");
    CHECK(ha::objectId("wMBus-TCH_12345678") == "wmbus_tch_12345678");
    CHECK(ha::objectId("Bresser 3CH / #1") == "bresser_3ch_1");
    CHECK(ha::objectId("__x__") == "x");
    CHECK(ha::objectId("") == "device");
    CHECK(ha::objectId("+#/") == "device");
}

static void testTopics() {
    const auto c = cfg();
    CHECK(ha::statusTopic(c) == "linkra/status");
    CHECK(ha::stateTopic(c, "Nexus-TH_160") == "linkra/sensor/nexus_th_160/state");
}

static void testDeviceDiscoveryTempHum() {
    const auto c = cfg();
    ha::Device d;
    d.key = "Nexus-TH_160"; d.model = "Nexus-TH"; d.id = 160; d.channel = 2;
    d.hasTemperature = true; d.hasHumidity = true; d.hasBattery = true;
    const auto msgs = ha::deviceDiscovery(c, d);
    CHECK(msgs.size() == 5);  // temperature, humidity, battery, rssi, last_seen
    bool sawTemp = false, sawBattery = false;
    for (const auto& m : msgs) {
        CHECK(m.retain);
        CHECK(m.topic.rfind("homeassistant/", 0) == 0);
        CHECK(m.topic.size() > 7 && m.topic.compare(m.topic.size() - 7, 7, "/config") == 0);
        auto j = parse(m.payload);
        CHECK(j["state_topic"] == "linkra/sensor/nexus_th_160/state");
        CHECK(std::string(j["unique_id"].as<const char*>()).rfind("linkra_duo_a1b2c3_nexus_th_160_", 0) == 0);
        CHECK(j["device"]["identifiers"][0] == "linkra_duo_a1b2c3_nexus_th_160");
        CHECK(j["device"]["name"] == "Nexus-TH 160 (Kanal 2)");
        CHECK(j["device"]["via_device"] == "linkra_duo_a1b2c3");
        CHECK(j["availability"][0]["topic"] == "linkra/status");
        if (m.topic == "homeassistant/sensor/linkra_duo_a1b2c3/nexus_th_160_temperature/config") {
            sawTemp = true;
            CHECK(j["device_class"] == "temperature");
            CHECK(j["unit_of_measurement"] == "°C");
            CHECK(j["state_class"] == "measurement");
            CHECK(j["expire_after"] == 3600);
            CHECK(j["value_template"] == "{{ value_json.temperature }}");
        }
        if (m.topic == "homeassistant/binary_sensor/linkra_duo_a1b2c3/nexus_th_160_battery/config") {
            sawBattery = true;
            CHECK(j["device_class"] == "battery");
        }
        if (std::string(j["device_class"] | "") == "timestamp") CHECK(j["expire_after"].isNull());
        if (std::string(j["device_class"] | "") == "signal_strength") CHECK(j["entity_category"] == "diagnostic");
    }
    CHECK(sawTemp);
    CHECK(sawBattery);
}

static void testDeviceDiscoveryOnlyReportedEntities() {
    auto c = cfg();
    c.expireAfterS = 0;
    ha::Device d;
    d.key = "Somfy-RTS_1234567"; d.model = "Somfy-RTS";
    const auto msgs = ha::deviceDiscovery(c, d);
    CHECK(msgs.size() == 2);  // only rssi + last_seen
    for (const auto& m : msgs) {
        auto j = parse(m.payload);
        CHECK(j["expire_after"].isNull());
        CHECK(j["device"]["name"] == "Somfy-RTS");
    }
}

static void testRemoval() {
    const auto msgs = ha::deviceRemoval(cfg(), "Nexus-TH_160");
    CHECK(msgs.size() == 5);
    for (const auto& m : msgs) { CHECK(m.retain); CHECK(m.payload.empty()); }
    // must match every discovery topic of a full device
    ha::Device d;
    d.key = "Nexus-TH_160"; d.hasTemperature = d.hasHumidity = d.hasBattery = true;
    const auto disc = ha::deviceDiscovery(cfg(), d);
    for (const auto& m : disc) {
        bool found = false;
        for (const auto& r : msgs) found |= r.topic == m.topic;
        CHECK(found);
    }
}

static void testBridge() {
    const auto m = ha::bridgeDiscovery(cfg());
    CHECK(m.topic == "homeassistant/binary_sensor/linkra_duo_a1b2c3/connectivity/config");
    auto j = parse(m.payload);
    CHECK(j["state_topic"] == "linkra/status");
    CHECK(j["payload_on"] == "online");
    CHECK(j["device"]["identifiers"][0] == "linkra_duo_a1b2c3");
    CHECK(j["device"]["sw_version"] == "0.6.0-beta");
}

static void testState() {
    ha::Reading r;
    r.hasTemperature = true; r.temperature = 7.4499;
    r.hasHumidity = true; r.humidity = 73;
    r.batteryOk = 1; r.rssi = -84; r.lastSeenIso = "2026-11-24T16:00:00+00:00";
    const auto m = ha::stateMessage(cfg(), "Nexus-TH_160", r);
    CHECK(!m.retain);
    CHECK(m.topic == "linkra/sensor/nexus_th_160/state");
    auto j = parse(m.payload);
    CHECK(j["temperature"].as<double>() > 7.39 && j["temperature"].as<double>() < 7.41);
    CHECK(j["humidity"] == 73);
    CHECK(j["battery_ok"] == 1);
    CHECK(j["rssi"] == -84);
    CHECK(j["last_seen"] == "2026-11-24T16:00:00+00:00");

    ha::Reading empty;
    empty.hasTemperature = true; empty.temperature = 0.0 / 0.0;  // NaN must be omitted
    empty.rssi = -90;
    auto j2 = parse(ha::stateMessage(cfg(), "x", empty).payload);
    CHECK(j2["temperature"].isNull());
    CHECK(j2["battery_ok"].isNull());
    CHECK(j2["last_seen"].isNull());
}

static void testUpdate() {
    const auto c = cfg();
    auto m = ha::updateDiscovery(c, false);
    CHECK(m.retain);
    CHECK(m.topic == "homeassistant/update/linkra_duo_a1b2c3/firmware/config");
    auto j = parse(m.payload);
    CHECK(j["device_class"] == "firmware");
    CHECK(j["state_topic"] == "linkra/update/state");
    CHECK(j["command_topic"].isNull());
    CHECK(j["device"]["identifiers"][0] == "linkra_duo_a1b2c3");
    CHECK(j["availability"][0]["topic"] == "linkra/status");
    j = parse(ha::updateDiscovery(c, true).payload);
    CHECK(j["command_topic"] == "linkra/update/install");
    CHECK(j["payload_install"] == "install");

    ha::UpdateInfo u;
    u.installed = "0.7.0-beta";
    auto s = ha::updateState(c, u);
    CHECK(s.retain && s.topic == "linkra/update/state");
    j = parse(s.payload);
    CHECK(j["latest_version"] == "0.7.0-beta");
    CHECK(j["in_progress"] == false);
    CHECK(j["update_percentage"].isNull());
    CHECK(j["release_summary"].isNull());
    u.latest = "0.7.1-beta"; u.title = "Linkra Duo 0.7.1-beta"; u.releaseUrl = "https://example/r";
    u.summary = std::string(300, 'x'); u.progress = 40;
    j = parse(ha::updateState(c, u).payload);
    CHECK(j["latest_version"] == "0.7.1-beta");
    CHECK(j["in_progress"] == true);
    CHECK(j["update_percentage"] == 40);
    CHECK(std::string(j["release_summary"].as<const char*>()).size() <= 255);
}

int main() {
    testUpdate();
    testObjectId();
    testTopics();
    testDeviceDiscoveryTempHum();
    testDeviceDiscoveryOnlyReportedEntities();
    testRemoval();
    testBridge();
    testState();
    if (failures) { std::printf("%d Fehler\n", failures); return 1; }
    std::printf("ha_discovery: alle Tests ok\n");
    return 0;
}
