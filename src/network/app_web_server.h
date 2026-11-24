#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

/**
 * Async Web Server mit SSE Support und OTA
 */
class LinkraWebServer {
public:
    static LinkraWebServer& getInstance();
    
    void begin();
    void sendEvent(const char* event, const char* data);
    // Antwort zuerst senden, Neustart per esp_timer nach delayMs (kein delay() im Handler).
    static void scheduleRestart(uint32_t delayMs = 300);
    
private:
    LinkraWebServer() : server(80), events("/api/events") {}
    AsyncWebServer server;
    AsyncEventSource events;
    
    void setupRoutes();
    void handleOTA(AsyncWebServerRequest* request, const String& filename, 
                   size_t index, uint8_t* data, size_t len, bool final);
    static void onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client, 
                          AwsEventType type, void* arg, uint8_t* data, size_t len);
};
