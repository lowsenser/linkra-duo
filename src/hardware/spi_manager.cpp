#include "spi_manager.h"

SPIManager& SPIManager::getInstance() {
    static SPIManager instance;
    return instance;
}

void SPIManager::begin() {
    // TODO: SPI Konfiguration
}

uint8_t SPIManager::readByte(uint8_t addr) {
    // TODO: SPI Read
    return 0;
}

void SPIManager::writeByte(uint8_t addr, uint8_t value) {
    // TODO: SPI Write
}
