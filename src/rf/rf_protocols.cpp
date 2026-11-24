#include "rf_protocols.h"

// Protocol definitions with timing parameters
const ProtocolDef RFProtocolDecoder::PROTOCOLS[] = {
    // EV1527: 24-bit address + 4-bit data
    {RFProtocolType::EV1527, "EV1527", 
     {320, 960, 9300, 150}, 24, 28},
    
    // PT2262: 8-bit address + 4-bit data (tri-state)
    {RFProtocolType::PT2262, "PT2262", 
     {400, 1400, 7000, 200}, 12, 12},
    
    // HT12E: Similar to PT2262
    {RFProtocolType::HT12E, "HT12E", 
     {400, 1200, 6800, 200}, 12, 12},
};

const int RFProtocolDecoder::PROTOCOL_COUNT = sizeof(PROTOCOLS) / sizeof(PROTOCOLS[0]);

RFProtocolDecoder& RFProtocolDecoder::getInstance() {
    static RFProtocolDecoder instance;
    return instance;
}

RFProtocolDecoder::RFProtocolDecoder() {
    pulseBuffer.reserve(MAX_PULSES);
}

void RFProtocolDecoder::addPulse(bool level, uint16_t duration) {
    if (pulseBuffer.size() >= MAX_PULSES) {
        // Shift buffer if full
        pulseBuffer.erase(pulseBuffer.begin());
    }
    pulseBuffer.push_back({level, duration});
}

void RFProtocolDecoder::clear() {
    pulseBuffer.clear();
}

bool RFProtocolDecoder::hasMessage() {
    if (pulseBuffer.size() < 20) return false;
    
    // Check for sync pulse at the end
    const SignalPulse& lastPulse = pulseBuffer.back();
    if (!lastPulse.level) {
        // Check if duration matches any sync timing
        for (int i = 0; i < PROTOCOL_COUNT; i++) {
            if (checkTiming(lastPulse.duration, PROTOCOLS[i].timing.syncPulse, 
                           PROTOCOLS[i].timing.tolerance)) {
                return true;
            }
        }
    }
    return false;
}

DecodedRFMessage RFProtocolDecoder::getMessage() {
    DecodedRFMessage msg = decode(pulseBuffer, -70);
    clear();
    return msg;
}

DecodedRFMessage RFProtocolDecoder::decode(const std::vector<SignalPulse>& pulses, int rssi) {
    DecodedRFMessage msg;
    msg.protocol = RFProtocolType::UNKNOWN;
    msg.protocolName = "UNKNOWN";
    msg.address = 0;
    msg.data = 0;
    msg.rawData = 0;
    msg.bitCount = 0;
    msg.rssi = rssi;
    msg.timestamp = millis();
    
    if (pulses.size() < 20) return msg;
    
    // Try EV1527 first
    msg = decodeEV1527();
    if (msg.protocol != RFProtocolType::UNKNOWN) return msg;
    
    // Try PT2262
    msg = decodePT2262();
    if (msg.protocol != RFProtocolType::UNKNOWN) return msg;
    
    // Fallback to raw OOK
    return decodeRawOOK();
}

DecodedRFMessage RFProtocolDecoder::decodeEV1527() {
    DecodedRFMessage msg;
    msg.protocol = RFProtocolType::UNKNOWN;
    msg.protocolName = "UNKNOWN";
    msg.rssi = -70;
    msg.timestamp = millis();
    
    // EV1527 format: 24 address bits + 4 data bits
    // Encoding: 0 = short high + long low, 1 = long high + short low
    // Short = ~320us, Long = ~960us, Sync = ~9300us low
    
    // Find sync pulse (last pulse should be the sync)
    size_t syncIdx = 0;
    for (size_t i = pulseBuffer.size() - 1; i > 0; i--) {
        if (!pulseBuffer[i].level && 
            pulseBuffer[i].duration >= EV1527_SYNC_MIN && 
            pulseBuffer[i].duration <= EV1527_SYNC_MAX) {
            syncIdx = i;
            break;
        }
    }
    
    if (syncIdx == 0 || syncIdx < 48) return msg;  // Need at least 24 data bit pairs
    
    // Decode bits
    uint32_t decodedBits = 0;
    uint8_t bitCount = 0;
    
    for (size_t i = 0; i < syncIdx - 1 && bitCount < 28; i += 2) {
        const SignalPulse& highPulse = pulseBuffer[i];
        const SignalPulse& lowPulse = pulseBuffer[i + 1];
        
        if (highPulse.level && !lowPulse.level) {
            // Check for 0 (short high, long low)
            if (highPulse.duration >= EV1527_SHORT_MIN && highPulse.duration <= EV1527_SHORT_MAX &&
                lowPulse.duration >= EV1527_LONG_MIN && lowPulse.duration <= EV1527_LONG_MAX) {
                decodedBits = (decodedBits << 1) | 0;
                bitCount++;
            }
            // Check for 1 (long high, short low)
            else if (highPulse.duration >= EV1527_LONG_MIN && highPulse.duration <= EV1527_LONG_MAX &&
                     lowPulse.duration >= EV1527_SHORT_MIN && lowPulse.duration <= EV1527_SHORT_MAX) {
                decodedBits = (decodedBits << 1) | 1;
                bitCount++;
            }
        }
    }
    
    // EV1527 should have 24-28 bits
    if (bitCount >= 24) {
        msg.protocol = RFProtocolType::EV1527;
        msg.protocolName = "EV1527";
        msg.rawData = decodedBits;
        msg.bitCount = bitCount;
        
        // Extract 24-bit address and 4-bit data
        if (bitCount >= 28) {
            msg.address = (decodedBits >> 4) & 0xFFFFFF;
            msg.data = decodedBits & 0x0F;
        } else {
            msg.address = decodedBits & 0xFFFFFF;
            msg.data = 0;
        }
    }
    
    return msg;
}

DecodedRFMessage RFProtocolDecoder::decodePT2262() {
    DecodedRFMessage msg;
    msg.protocol = RFProtocolType::UNKNOWN;
    msg.protocolName = "UNKNOWN";
    msg.rssi = -70;
    msg.timestamp = millis();
    
    // PT2262 uses tri-state coding: 0, 1, F (floating)
    // Each bit is encoded as two pulses:
    // 0 = short high, long low
    // 1 = long high, short low  
    // F = short high, short low (floating/address bit)
    
    size_t syncIdx = 0;
    for (size_t i = pulseBuffer.size() - 1; i > 0; i--) {
        if (!pulseBuffer[i].level && 
            pulseBuffer[i].duration >= PT2262_SYNC_MIN && 
            pulseBuffer[i].duration <= PT2262_SYNC_MAX) {
            syncIdx = i;
            break;
        }
    }
    
    if (syncIdx == 0 || syncIdx < 24) return msg;
    
    // PT2262 has 12 bits (8 address + 4 data)
    uint32_t address = 0;
    uint8_t data = 0;
    uint8_t bitCount = 0;
    
    for (size_t i = 0; i < syncIdx - 1 && bitCount < 12; i += 2) {
        const SignalPulse& highPulse = pulseBuffer[i];
        const SignalPulse& lowPulse = pulseBuffer[i + 1];
        
        if (highPulse.level && !lowPulse.level) {
            bool isShortHigh = (highPulse.duration >= PT2262_SHORT_MIN && 
                               highPulse.duration <= PT2262_SHORT_MAX);
            bool isLongHigh = (highPulse.duration >= PT2262_LONG_MIN && 
                              highPulse.duration <= PT2262_LONG_MAX);
            bool isShortLow = (lowPulse.duration >= PT2262_SHORT_MIN && 
                              lowPulse.duration <= PT2262_SHORT_MAX);
            bool isLongLow = (lowPulse.duration >= PT2262_LONG_MIN && 
                             lowPulse.duration <= PT2262_LONG_MAX);
            
            if (isShortHigh && isLongLow) {
                // Bit = 0
                if (bitCount < 8) {
                    address = (address << 1) | 0;
                } else {
                    data = (data << 1) | 0;
                }
                bitCount++;
            } else if (isLongHigh && isShortLow) {
                // Bit = 1
                if (bitCount < 8) {
                    address = (address << 1) | 1;
                } else {
                    data = (data << 1) | 1;
                }
                bitCount++;
            } else if (isShortHigh && isShortLow) {
                // Floating bit (F) - skip or mark as special
                if (bitCount < 8) {
                    address = (address << 1) | 0;  // Treat as 0 for now
                } else {
                    data = (data << 1) | 0;
                }
                bitCount++;
            }
        }
    }
    
    if (bitCount == 12) {
        msg.protocol = RFProtocolType::PT2262;
        msg.protocolName = "PT2262";
        msg.address = address;
        msg.data = data;
        msg.rawData = (address << 4) | data;
        msg.bitCount = bitCount;
    }
    
    return msg;
}

DecodedRFMessage RFProtocolDecoder::decodeRawOOK() {
    DecodedRFMessage msg;
    msg.protocol = RFProtocolType::RAW_OOK;
    msg.protocolName = "RAW_OOK";
    msg.rssi = -70;
    msg.timestamp = millis();
    
    // Count pulses and create simple hash
    uint32_t hash = 0;
    uint16_t totalDuration = 0;
    
    for (const auto& pulse : pulseBuffer) {
        hash ^= (pulse.duration * 31);
        hash = (hash << 1) | (hash >> 31);  // Rotate
        totalDuration += pulse.duration;
    }
    
    msg.rawData = hash;
    msg.bitCount = pulseBuffer.size() / 2;  // Approximate
    msg.address = hash >> 8;
    msg.data = hash & 0xFF;
    
    return msg;
}

bool RFProtocolDecoder::checkTiming(uint16_t duration, uint16_t expected, uint16_t tolerance) {
    return (duration >= expected - tolerance) && (duration <= expected + tolerance);
}

uint32_t RFProtocolDecoder::extractBits(const std::vector<SignalPulse>& pulses, 
                                        uint16_t shortDur, uint16_t longDur, 
                                        uint8_t startIdx, uint8_t count) {
    uint32_t result = 0;
    
    for (uint8_t i = 0; i < count && (startIdx + i * 2 + 1) < pulses.size(); i++) {
        const SignalPulse& high = pulses[startIdx + i * 2];
        const SignalPulse& low = pulses[startIdx + i * 2 + 1];
        
        if (!high.level || low.level) continue;
        
        // Determine bit value based on pulse lengths
        uint16_t highDur = high.duration;
        uint16_t lowDur = low.duration;
        
        // Simple threshold comparison
        uint16_t threshold = (shortDur + longDur) / 2;
        
        if (highDur < threshold && lowDur > threshold) {
            result = (result << 1) | 0;  // Short high, long low = 0
        } else if (highDur > threshold && lowDur < threshold) {
            result = (result << 1) | 1;  // Long high, short low = 1
        }
    }
    
    return result;
}
