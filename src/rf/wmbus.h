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
// Why a capture was rejected (statistics for tuning the receiver)
enum class ErrKind : uint8_t { None, Start, Incomplete, Symbol, Crc, Length };

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
    ErrKind errKind = ErrKind::None;
    int errAt = -1;              // Crc: block index (0 = header block); Symbol: byte index
    Mode mode = Mode::Unknown;
    std::vector<uint8_t> data;   // link-layer bytes without CRCs (L C M M A..)
    uint8_t L = 0, C = 0, version = 0, type = 0, ci = 0;
    // Identity of the TRANSMITTER (data link layer). Stable device key in the
    // gateway (wMBus-<manufacturer>_<id>), also for radio modules that relay a meter.
    std::string manufacturer;    // 3-letter FLAG code, e.g. "ITW"
    std::string id;              // 8 digit address
    // Meter behind the radio module, from a long transport header (CI 0x72/0x8x...).
    bool hasMeter = false;
    std::string meterManufacturer, meterId;
    uint8_t meterVersion = 0, meterType = 0;
    // Extended link layer (CI 0x8C-0x8F) and authentication/fragmentation layer (0x90)
    bool hasEll = false, hasAfl = false;
    uint8_t ellCc = 0, ellAcc = 0;
    uint16_t aflFcl = 0, aflKi = 0;
    uint8_t aflMcl = 0;
    uint32_t aflMcr = 0;         // message counter (needed for AES mode 7 key derivation)
    std::vector<uint8_t> aflMac;
    uint8_t encBlocks = 0;       // number of encrypted 16-byte blocks (config field)
    size_t payloadOffset = 0;    // index in data[] where the (possibly encrypted) payload starts
    int encryption = -1;         // TPL security mode (0 none, 5 AES-CBC, 7 AES-CBC with derived key, 100 ELL AES-CTR), -1 unknown
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
