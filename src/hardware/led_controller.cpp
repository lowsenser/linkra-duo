#include "led_controller.h"

LEDController& LEDController::getInstance() {
    static LEDController instance;
    return instance;
}

void LEDController::begin() {
    // TODO: GPIO / WS2812B Konfiguration
}

void LEDController::setRGB(uint8_t r, uint8_t g, uint8_t b) {
    // TODO: Farben setzen
}

void LEDController::pulse() {
    // TODO: Puls-Animation
}

void LEDController::animate(const char* animation) {
    // TODO: Custom Animationen
}
