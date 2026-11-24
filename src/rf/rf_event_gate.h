#pragma once
#include <stdint.h>

// Only diagnostic noise is throttled. Valid telegrams must not compete with it.
class RFEventGate {
public:
    bool allow(bool decoded, uint32_t now) {
        if (decoded) return true;
        if (hasUnknown && uint32_t(now - lastUnknown) < 500) return false;
        hasUnknown = true;
        lastUnknown = now;
        return true;
    }
private:
    bool hasUnknown = false;
    uint32_t lastUnknown = 0;
};
