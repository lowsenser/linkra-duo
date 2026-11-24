#pragma once

#include <Arduino.h>

/**
 * LED Controller Abstraction
 * Unterstützt WS2812B (PCB) und separate GPIOs (Breadboard)
 */
class LEDController {
public:
    static LEDController& getInstance();
    
    void begin();
    void setRGB(uint8_t r, uint8_t g, uint8_t b);
    void pulse();
    void animate(const char* animation);
    
private:
    LEDController() {}
};
