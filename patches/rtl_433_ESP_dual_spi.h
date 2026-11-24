/*
 * Dual CC1101 Support Extension for rtl_433_ESP
 * 
 * This header extends rtl_433_ESP to support two CC1101 modules:
 * - Module 1: 433 MHz OOK (default SPI)
 * - Module 2: 868 MHz FSK (secondary SPI)
 */

#pragma once

#include <rtl_433_ESP.h>
#include <SPI.h>

// Second SPI bus for 868 MHz module
extern SPIClass spi2;
extern CC1101 radio2;

class rtl_433_ESP_Dual : public rtl_433_ESP {
public:
    // Initialize with specific SPI bus
    void initReceiver(byte inputPin, float receiveFrequency, SPIClass* spiBus = nullptr);
    
    // Set modulation per instance
    void setOOKModulation(bool ook);
    
    // Get RSSI from specific radio
    int getCurrentRssi();
    
private:
    SPIClass* _spiBus = nullptr;
    bool _isRadio2 = false;
    bool _ookModulation = true;
};
