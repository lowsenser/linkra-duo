#pragma once

#include <Arduino.h>
#include <map>
#include <vector>
#include <functional>

/**
 * System Event Types
 * Event-Driven Architecture für minimales Blocking
 */
enum class SystemEvent {
    BOOT_COMPLETE,
    WIFI_AP_STARTED,
    WIFI_CLIENT_CONNECTED,
    WIFI_CLIENT_DISCONNECTED,
    WEB_SERVER_READY,
    HARDWARE_DETECTED,
    RF_SIGNAL_RECEIVED,
    RF_RSSI_UPDATE,
    OTA_STARTED,
    OTA_PROGRESS,
    OTA_COMPLETE,
    ERROR_OCCURRED,
    MQTT_STATUS          // MQTT connection changed (data: status JSON)
};

/**
 * Event Handler Callback
 */
using EventHandler = std::function<void(SystemEvent event, const String& data)>;

/**
 * Event Bus - Zentrale Event-Verwaltung
 */
class EventBus {
public:
    static EventBus& getInstance() {
        static EventBus instance;
        return instance;
    }

    void subscribe(SystemEvent event, EventHandler handler);
    void unsubscribe(SystemEvent event);
    void publish(SystemEvent event, const String& data = "");

private:
    EventBus();
    std::map<SystemEvent, std::vector<EventHandler>> listeners;
};
