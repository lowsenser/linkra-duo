#include "event_bus.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <Arduino.h>

static SemaphoreHandle_t eventMutex = nullptr;
static int publishCount = 0;

EventBus::EventBus() {
    if (eventMutex == nullptr) {
        eventMutex = xSemaphoreCreateMutex();
    }
}

void EventBus::subscribe(SystemEvent event, EventHandler handler) {
    if (xSemaphoreTake(eventMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        listeners[event].push_back(handler);
        Serial.printf("[EventBus] Subscribed to event %d, total handlers: %d\n", 
                      (int)event, listeners[event].size());
        xSemaphoreGive(eventMutex);
    }
}

void EventBus::unsubscribe(SystemEvent event) {
    if (xSemaphoreTake(eventMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        listeners.erase(event);
        xSemaphoreGive(eventMutex);
    }
}

void EventBus::publish(SystemEvent event, const String& data) {
    publishCount++;
    if (xSemaphoreTake(eventMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        auto it = listeners.find(event);
        if (it != listeners.end()) {
            // Use const reference — don't copy the handler vector!
            const auto& handlers = it->second;
            int handlerCount = handlers.size();
            xSemaphoreGive(eventMutex);
            // Only log non-RF events (RF events flood the serial at ~3/sec)
            if (event != SystemEvent::RF_SIGNAL_RECEIVED && event != SystemEvent::RF_RSSI_UPDATE) {
                Serial.printf("[EventBus] Publishing event %d to %d handlers\n",
                              (int)event, handlerCount);
            }
            for (const auto& handler : handlers) {
                handler(event, data);
            }
        } else {
            xSemaphoreGive(eventMutex);
        }
    } else {
        Serial.println("[EventBus] Mutex timeout!");
    }
}
