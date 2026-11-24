#pragma once
// Wireless M-Bus (EN 13757-4) link layer decoding for CC1101 packet-mode capture.
// Pure C++ (no Arduino dependencies) so it can be unit-tested on the host:
//   c++ -std=c++17 -I src tests/wmbus_test.cpp src/rf/wmbus.cpp -o /tmp/t && /tmp/t
#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>

namespace wmbus {

enum class Mode : uint8_t { Unknown, T1, C1A, C1B };

// EN 13757 CRC-16 (poly 0x3D65, init 0, xorout 0xFFFF, MSB first)
uint16_t crc16(const uint8_t* d, size_t n);

// 3-out-of-6 symbol -> nibble, or -1 if the symbol is invalid
int decode3of6Symbol(uint8_t sym);

// Number of raw bytes on air for a frame whose first bytes after the
// CC1101 sync word (0x543D) are `head` (>= 3 bytes). 0 = not a frame.
size_t expectedRawLength(const uint8_t* head, size_t n, Mode& mode);

struct Record {
    std::string name;   // e.g. "volume_m3", "energy_kWh", "date"
    double value = 0;
    std::string text;   // for dates / non-numeric
    uint32_t storage = 0, tariff = 0;  // from DIF/DIFEs (up to 8 DIFEs are evaluated)
    uint8_t function = 0;              // 0 inst, 1 max, 2 min, 3 err
};

struct Frame {
    bool ok = false;
    std::string error;           // why decoding failed
    Mode mode = Mode::Unknown;
    std::vector<uint8_t> data;   // link-layer bytes without CRCs (L C M M A..)
    uint8_t L = 0, C = 0, version = 0, type = 0, ci = 0;
    std::string manufacturer;    // 3-letter FLAG code, e.g. "ITW"
    std::string id;              // 8 digit meter address as printed on the device
    int encryption = -1;         // TPL security mode (0 = none, 5 = AES-CBC, 7 = AES-CTR), -1 unknown
    uint8_t accessNo = 0, status = 0;
    bool payloadParsed = false;
    std::vector<Record> records;
    std::string driver;          // e.g. "Techem FHKV data III" when a manufacturer decoder matched
    std::string media;           // human readable device kind from the manufacturer decoder
};

// Decode a raw capture (bytes after sync word). Validates 3-of-6 (T1) and all CRCs.
Frame decode(const uint8_t* raw, size_t n);

const char* deviceTypeName(uint8_t type);  // German
const char* modeName(Mode m);

// JSON object (no trailing newline) in the shape the gateway publishes for rtl_433 decodes.
std::string toJson(const Frame& f, int rssi, int lqi);

// Helpers for tests: encode bytes to 3-of-6 symbols packed MSB first
std::vector<uint8_t> encode3of6(const std::vector<uint8_t>& bytes);

}  // namespace wmbus
