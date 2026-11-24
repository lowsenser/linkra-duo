#include "../src/rf/rf_event_gate.h"
#include <cassert>
#include <cstdint>
int main() {
    RFEventGate gate;
    int noiseDelivered = 0, decodedDelivered = 0;
    // Replay a noisy second with an independent valid sensor frame every 10ms.
    for (uint32_t now = 0; now < 1000; now += 10) {
        noiseDelivered += gate.allow(false, now);
        decodedDelivered += gate.allow(true, now);
    }
    assert(noiseDelivered == 2);
    assert(decodedDelivered == 100);
    RFEventGate wrap;
    assert(wrap.allow(false, UINT32_MAX - 100));
    assert(!wrap.allow(false, 100));
    assert(wrap.allow(true, 100));
    assert(wrap.allow(false, 500));
}
