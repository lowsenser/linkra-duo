#pragma once
// Home Assistant MQTT discovery: topics and payloads (pure logic, no Arduino).
// Host-testable: tests/ha_discovery_test.cpp.
//
// Layout (base = "linkra", prefix = "homeassistant", node = "linkra_duo_a1b2c3"):
//   linkra/status                                   online/offline (retained, LWT)
//   linkra/sensor/<obj>/state                       JSON state per RF device
//   homeassistant/<component>/<node>/<obj>_<ent>/config   discovery (retained)
// <obj> is the RF device key made topic-safe ("Nexus-TH_160" -> "nexus_th_160").

#include <string>
#include <vector>

namespace ha {

struct Config {
    std::string baseTopic = "linkra";
    std::string discoveryPrefix = "homeassistant";
    std::string nodeId;          // e.g. "linkra_duo_a1b2c3"
    std::string nodeName = "Linkra Duo";
    std::string swVersion;
    int expireAfterS = 3600;     // 0 = never mark a sensor unavailable
};

struct Device {
    std::string key;             // RF device key, e.g. "Nexus-TH_160"
    std::string model;           // e.g. "Nexus-TH"
    int id = -1;
    int channel = -1;
    bool hasTemperature = false;
    bool hasHumidity = false;
    bool hasBattery = false;
};

struct Reading {
    bool hasTemperature = false; double temperature = 0;
    bool hasHumidity = false;    double humidity = 0;
    int batteryOk = -1;          // -1 = not reported
    int rssi = 0;
    std::string lastSeenIso;     // empty when the clock is not set
};

struct Message {
    std::string topic;
    std::string payload;         // empty payload + retain = delete discovery entry
    bool retain = true;
};

std::string objectId(const std::string& key);
std::string statusTopic(const Config& c);
std::string stateTopic(const Config& c, const std::string& key);
std::string deviceName(const Device& d);

// Discovery for the bridge itself (connectivity sensor).
Message bridgeDiscovery(const Config& c);
// Discovery for one RF device: only entities it actually reports, plus signal
// strength and last-seen (diagnostic).
std::vector<Message> deviceDiscovery(const Config& c, const Device& d);
// Retained empty payloads for every entity a device could have had.
std::vector<Message> deviceRemoval(const Config& c, const std::string& key);
// State JSON for one reading (not retained).
Message stateMessage(const Config& c, const std::string& key, const Reading& r);

}  // namespace ha
