#pragma once

#include <Arduino.h>

/**
 * SPI Bus Management
 * Zentrale Verwaltung für CC1101 und andere SPI-Geräte
 */
class SPIManager {
public:
    static SPIManager& getInstance();
    
    void begin();
    uint8_t readByte(uint8_t addr);
    void writeByte(uint8_t addr, uint8_t value);
    
private:
    SPIManager() {}
};
