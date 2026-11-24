#pragma once

#include <Arduino.h>
#include <vector>

// RF Protocol Types
enum class RFProtocolType {
    UNKNOWN,
    EV1527,      // Common for remote controls (24-bit address + 4-bit data)
    PT2262,      // Princeton encoder (8-bit address + 4-bit data)
    PT2260,      // Similar to PT2262
    HT12E,       // Holtek encoder
    WAVEMAN,     // Waveman protocol
    HCS301,      // KeeLoq rolling code
    RAW_OOK      // Raw OOK/ASK signal
};

// Timing structure for pulse-width decoding
struct PulseTiming {
    uint16_t shortPulse;   // Short pulse duration (us)
    uint16_t longPulse;    // Long pulse duration (us)
    uint16_t syncPulse;    // Sync pulse duration (us)
    uint16_t tolerance;    // Timing tolerance (us)
};

// Protocol definitions
struct ProtocolDef {
    RFProtocolType type;
    const char* name;
    PulseTiming timing;
    uint8_t minBits;
    uint8_t maxBits;
};

// Decoded RF message
struct DecodedRFMessage {
    RFProtocolType protocol;
    String protocolName;
    uint32_t address;      // Device address
    uint8_t data;          // Command/data bits
    uint32_t rawData;      // Raw decoded bits
    uint8_t bitCount;
    int rssi;
    uint32_t timestamp;
};

// Signal pulse for raw recording
struct SignalPulse {
    bool level;            // true = high, false = low
    uint16_t duration;     // duration in microseconds
};

/**
 * RF Protocol Decoder
 * Handles various 433 MHz protocols
 */
class RFProtocolDecoder {
public:
    static RFProtocolDecoder& getInstance();
    
    // Decode raw pulses into protocol message
    DecodedRFMessage decode(const std::vector<SignalPulse>& pulses, int rssi);
    
    // Add pulse for processing
    void addPulse(bool level, uint16_t duration);
    
    // Check if we have a complete message
    bool hasMessage();
    
    // Get decoded message
    DecodedRFMessage getMessage();
    
    // Get current pulse count
    size_t getPulseCount() const { return pulseBuffer.size(); }
    
    // Clear buffer
    void clear();
    
private:
    RFProtocolDecoder();
    
    std::vector<SignalPulse> pulseBuffer;
    static const size_t MAX_PULSES = 256;
    
    // Protocol definitions
    static const ProtocolDef PROTOCOLS[];
    static const int PROTOCOL_COUNT;
    
    // Decoders for specific protocols
    DecodedRFMessage decodeEV1527();
    DecodedRFMessage decodePT2262();
    DecodedRFMessage decodeRawOOK();
    
    // Helper functions
    bool checkTiming(uint16_t duration, uint16_t expected, uint16_t tolerance);
    uint32_t extractBits(const std::vector<SignalPulse>& pulses, 
                         uint16_t shortDur, uint16_t longDur, 
                         uint8_t startIdx, uint8_t count);
};

// EV1527 specific: 24-bit address + 4-bit data
// Pulse timing: Short = 300us, Long = 900us, Sync = 9300us
#define EV1527_SHORT_MIN 250
#define EV1527_SHORT_MAX 450
#define EV1527_LONG_MIN 750
#define EV1527_LONG_MAX 1200
#define EV1527_SYNC_MIN 8000
#define EV1527_SYNC_MAX 11000

// PT2262 specific: 8-bit address + 4-bit data (tri-state)
// Uses short/low, short/high, long/low, long/high combinations
#define PT2262_SHORT_MIN 200
#define PT2262_SHORT_MAX 600
#define PT2262_LONG_MIN 1000
#define PT2262_LONG_MAX 1800
#define PT2262_SYNC_MIN 5000
#define PT2262_SYNC_MAX 9000
