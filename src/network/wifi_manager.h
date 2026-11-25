#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <vector>

/**
 * WiFi Network Info
 */
struct WiFiNetwork {
    String ssid;
    int32_t rssi;
    uint8_t encryptionType;
    uint8_t channel;
};

/**
 * WiFi Manager
 * Unterstützt AP Mode (Setup) und STA Mode (Client)
 */
enum class WiFiMode {
    SETUP_AP,       // Setup Wizard AP Mode
    STA_CONNECTING, // Verbindung wird hergestellt
    STA_CONNECTED,  // Verbunden mit externem AP
    STA_FAILED      // Verbindung fehlgeschlagen
};

class WiFiManager {
public:
    static WiFiManager& getInstance();
    
    // Setup Mode (AP)
    void beginSetupAP(const char* apName = "Linkra-Duo-Setup");
    
    // STA Mode
    bool connectToWiFi(const char* ssid, const char* password);
    void disconnect();
    bool isConnected();
    
    // Scan
    std::vector<WiFiNetwork> scanNetworks();
    bool isScanning();
    
    // Status
    WiFiMode getMode();
    String getIP();
    String getAPIP();
    int getClientCount();
    
    // Event Handlers
    void onConnected(std::function<void()> callback);
    void onDisconnected(std::function<void()> callback);
    
    // WebSocket/Events Update
    void update();

    // Zeit: NTP wird bei jedem GOT_IP gestartet; true nach der ersten Synchronisation.
    static void startTimeSync();
    static String setupApPassword();   // WPA2 password of the setup hotspot (NVS, generated once)
    static bool isTimeSynced();
    
private:
    WiFiManager();
    WiFiMode currentMode;
    std::vector<std::function<void()>> onConnectedCallbacks;
    std::vector<std::function<void()>> onDisconnectedCallbacks;
    
    static void WiFiEventHandler(WiFiEvent_t event, WiFiEventInfo_t info);
    void handleWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info);
};
