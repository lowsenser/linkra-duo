#include "wmbus.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

namespace wmbus {

static const uint8_t ENC3OF6[16] = {0x16, 0x0D, 0x0E, 0x0B, 0x1C, 0x19, 0x1A, 0x13,
                                    0x2C, 0x25, 0x26, 0x23, 0x34, 0x31, 0x32, 0x29};

uint16_t crc16(const uint8_t* d, size_t n) {
    uint16_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)d[i] << 8;
        for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x3D65) : (uint16_t)(crc << 1);
    }
    return (uint16_t)~crc;
}

int decode3of6Symbol(uint8_t sym) {
    for (int i = 0; i < 16; i++) if (ENC3OF6[i] == sym) return i;
    return -1;
}

std::vector<uint8_t> encode3of6(const std::vector<uint8_t>& bytes) {
    std::vector<uint8_t> out;
    uint32_t acc = 0; int bits = 0;
    for (uint8_t b : bytes) {
        acc = (acc << 12) | ((uint32_t)ENC3OF6[b >> 4] << 6) | ENC3OF6[b & 0x0F];
        bits += 12;
        while (bits >= 8) { out.push_back((uint8_t)(acc >> (bits - 8))); bits -= 8; }
    }
    if (bits) out.push_back((uint8_t)(acc << (8 - bits)));
    return out;
}

// Frame format A: L counts bytes after L, CRC every block (10, then 16-byte blocks)
static size_t frameALength(uint8_t L) {
    if (L < 9) return 0;
    const size_t rest = L + 1 > 10 ? (size_t)L + 1 - 10 : 0;
    return (size_t)L + 1 + 2 + 2 * ((rest + 15) / 16);
}

// Bits from a packed big-endian bit stream
static int symbolAt(const uint8_t* raw, size_t n, size_t symIdx) {
    const size_t bit = symIdx * 6;
    const size_t byte = bit / 8, off = bit % 8;
    if (byte + 1 >= n + 1) return -1;
    uint16_t w = (uint16_t)raw[byte] << 8 | (byte + 1 < n ? raw[byte + 1] : 0);
    return (w >> (10 - off)) & 0x3F;
}

size_t expectedRawLength(const uint8_t* head, size_t n, Mode& mode) {
    mode = Mode::Unknown;
    if (n < 3) return 0;
    if (head[0] == 0x54 && head[1] == 0xCD) {        // C1, frame A
        const size_t len = frameALength(head[2]);
        if (!len) return 0;
        mode = Mode::C1A; return 2 + len;
    }
    if (head[0] == 0x54 && head[1] == 0x3D) {        // C1, frame B
        // L = 128/129 would leave block 2 with 0 or -1 data bytes:
        // not a valid frame B, reject (see stripCrcB).
        if (head[2] < 12 || head[2] == 128 || head[2] == 129) return 0;
        mode = Mode::C1B; return 2 + (size_t)head[2] + 1;
    }
    const int hi = decode3of6Symbol((uint8_t)symbolAt(head, n, 0));
    const int lo = decode3of6Symbol((uint8_t)symbolAt(head, n, 1));
    if (hi < 0 || lo < 0) return 0;
    const size_t len = frameALength((uint8_t)(hi << 4 | lo));
    if (!len) return 0;
    mode = Mode::T1;
    return (len * 3 + 1) / 2;
}

static bool stripCrcA(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, std::string& err) {
    if (in.empty()) { err = "zu kurz"; return false; }
    const size_t L = in[0];
    size_t pos = 0, dataLeft = L + 1;
    bool first = true;
    while (dataLeft > 0) {
        const size_t blk = first ? 10 : (dataLeft < 16 ? dataLeft : 16);
        if (pos + blk + 2 > in.size()) { err = "zu kurz"; return false; }
        const uint16_t want = (uint16_t)in[pos + blk] << 8 | in[pos + blk + 1];
        if (crc16(&in[pos], blk) != want) { err = "CRC-Fehler Block " + std::to_string(out.size() / 16); return false; }
        out.insert(out.end(), in.begin() + pos, in.begin() + pos + blk);
        pos += blk + 2; dataLeft -= blk; first = false;
    }
    return true;
}

static bool stripCrcB(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, std::string& err) {
    if (in.empty()) { err = "zu kurz"; return false; }
    const size_t total = (size_t)in[0] + 1;
    // Block 1 = up to 126 data bytes + CRC, block 2 starts at 128 and needs >= 1 data byte.
    // total 129/130 would give block 2 a negative/zero length (size_t underflow -> OOB read).
    if (total < 3 || total == 129 || total == 130) { err = "ungültige Länge"; return false; }
    if (in.size() < total) { err = "zu kurz"; return false; }
    auto blockOk = [&](size_t start, size_t dataLen) {
        if (dataLen < 1 || start + dataLen + 2 > in.size()) return false;
        const uint16_t want = (uint16_t)in[start + dataLen] << 8 | in[start + dataLen + 1];
        return crc16(&in[start], dataLen) == want;
    };
    if (total <= 128) {
        if (!blockOk(0, total - 2)) { err = "CRC-Fehler"; return false; }
        out.assign(in.begin(), in.begin() + total - 2);
    } else {
        if (!blockOk(0, 126) || !blockOk(128, total - 130)) { err = "CRC-Fehler"; return false; }
        out.assign(in.begin(), in.begin() + 126);
        out.insert(out.end(), in.begin() + 128, in.begin() + total - 2);
    }
    return true;
}

static std::string hex(const uint8_t* d, size_t n) {
    static const char* H = "0123456789abcdef";
    std::string s; s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) { s += H[d[i] >> 4]; s += H[d[i] & 15]; }
    return s;
}

static int64_t leInt(const uint8_t* p, int len) {
    if (len <= 0 || len > 8) return 0;
    uint64_t u = 0;   // unsigned: shifting 8 bytes into int64_t would overflow (UB)
    for (int i = len - 1; i >= 0; i--) u = (u << 8) | p[i];
    if (len < 8 && (p[len - 1] & 0x80)) u |= ~(uint64_t)0 << (8 * len);  // sign extend
    return (int64_t)u;
}
static bool bcd(const uint8_t* p, int len, int64_t& v) {
    v = 0; bool neg = false;
    for (int i = len - 1; i >= 0; i--) {
        uint8_t hi = p[i] >> 4, lo = p[i] & 15;
        if (i == len - 1 && hi == 0xF) { neg = true; hi = 0; }
        if (hi > 9 || lo > 9) return false;
        v = v * 100 + hi * 10 + lo;
    }
    if (neg) v = -v;
    return true;
}

static void parseRecords(Frame& f, size_t pos) {
    const std::vector<uint8_t>& d = f.data;
    static const int LEN[16] = {0, 1, 2, 3, 4, 4, 6, 8, 0, 1, 2, 3, 4, -1, 6, -2};
    int guard = 0;
    while (pos < d.size() && guard++ < 64) {
        const uint8_t dif = d[pos++];
        if (dif == 0x2F) continue;                       // idle filler
        if ((dif & 0x0F) == 0x0F) break;                 // manufacturer specific data follows
        Record r;
        r.function = (dif >> 4) & 3;
        r.storage = (dif >> 6) & 1;
        uint8_t ext = dif; int i = 0;
        while ((ext & 0x80) && pos < d.size()) {
            ext = d[pos++];
            if (i < 8) {   // EN 13757-3 allows max. 10 DIFEs; from the 9th on the shift would exceed 32 bits (UB)
                r.storage |= (uint32_t)(ext & 0x0F) << (1 + 4 * i);
                r.tariff |= (uint32_t)((ext >> 4) & 3) << (2 * i);
            }
            i++;
        }
        if (pos >= d.size()) return;
        uint8_t vif = d[pos++];
        uint8_t vifCode = vif & 0x7F;
        bool extTable = (vif == 0xFD || vif == 0xFB);
        uint8_t extCode = 0;
        uint8_t vife = vif;
        if (extTable && pos < d.size()) { vife = d[pos++]; extCode = vife & 0x7F; }
        while ((vife & 0x80) && pos < d.size()) vife = d[pos++];
        int len = LEN[dif & 0x0F];
        if (len == -1) { if (pos >= d.size()) return; len = d[pos++]; }
        if (len < 0 || pos + len > d.size()) return;
        const uint8_t* p = &d[pos];
        pos += len;

        int64_t iv = 0; double v = NAN;
        const uint8_t df = dif & 0x0F;
        if (df >= 9 && df <= 0x0E && df != 0x0D) { if (bcd(p, len, iv)) v = (double)iv; }
        else if (df == 5 && len == 4) { float fl; memcpy(&fl, p, 4); v = fl; }
        else if (len > 0 && len <= 8 && df != 0x0D) { iv = leInt(p, len); v = (double)iv; }

        auto p10 = [](int e) { return pow(10.0, e); };
        if (extTable) { char b[24]; snprintf(b, sizeof b, "vif_%s_%02x", vif == 0xFD ? "fd" : "fb", extCode); r.name = b; r.value = v; }
        else if (vifCode <= 0x07) { r.name = "energy_kWh"; r.value = v * p10((vifCode & 7) - 3) / 1000.0; }
        else if (vifCode <= 0x0F) { r.name = "energy_MJ"; r.value = v * p10(vifCode & 7) / 1e6; }
        else if (vifCode <= 0x17) { r.name = "volume_m3"; r.value = v * p10((vifCode & 7) - 6); }
        else if (vifCode <= 0x1F) { r.name = "mass_kg"; r.value = v * p10((vifCode & 7) - 3); }
        else if (vifCode >= 0x28 && vifCode <= 0x2F) { r.name = "power_W"; r.value = v * p10((vifCode & 7) - 3); }
        else if (vifCode >= 0x38 && vifCode <= 0x3F) { r.name = "flow_m3h"; r.value = v * p10((vifCode & 7) - 6); }
        else if (vifCode >= 0x58 && vifCode <= 0x5B) { r.name = "flow_temp_C"; r.value = v * p10((vifCode & 3) - 3); }
        else if (vifCode >= 0x5C && vifCode <= 0x5F) { r.name = "return_temp_C"; r.value = v * p10((vifCode & 3) - 3); }
        else if (vifCode >= 0x64 && vifCode <= 0x67) { r.name = "ext_temp_C"; r.value = v * p10((vifCode & 3) - 3); }
        else if (vifCode == 0x6C && len == 2) {
            char b[16]; snprintf(b, sizeof b, "%04d-%02d-%02d", 2000 + (((p[0] & 0xE0) >> 5) | ((p[1] & 0xF0) >> 1)), p[1] & 0x0F, p[0] & 0x1F);
            r.name = "date"; r.text = b;
        } else if (vifCode == 0x6D && len == 4) {
            char b[24]; snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d", 2000 + (((p[2] & 0xE0) >> 5) | ((p[3] & 0xF0) >> 1)), p[3] & 0x0F, p[2] & 0x1F, p[1] & 0x1F, p[0] & 0x3F);
            r.name = "datetime"; r.text = b;
        } else if (vifCode == 0x6E) { r.name = "hca_units"; r.value = v; }
        else if (vifCode == 0x78) { r.name = "fabrication_no"; r.value = v; }
        else { char b[12]; snprintf(b, sizeof b, "vif_%02x", vifCode); r.name = b; r.value = v; }
        if (r.function == 1) r.name += "_max"; else if (r.function == 2) r.name += "_min"; else if (r.function == 3) r.name += "_err";
        if (r.storage) r.name += "_s" + std::to_string(r.storage);
        if (r.tariff) r.name += "_t" + std::to_string(r.tariff);
        f.records.push_back(r);
    }
}

// ---------------- Techem proprietary payloads (CI 0xA0..0xA2) ----------------
// Byte layouts follow the public format descriptions of the wmbusmeters project
// (drivers fhkvdataiii, mkradio3, mkradio4, compact5, vario451, tsd2).
static void addVal(Frame& f, const char* name, double v) { Record r; r.name = name; r.value = v; f.records.push_back(r); }
static void addText(Frame& f, const char* name, const std::string& t) { Record r; r.name = name; r.text = t; f.records.push_back(r); }
static uint16_t w16(const std::vector<uint8_t>& d, size_t i) { return d[i] | d[i + 1] << 8; }
static uint32_t w24(const std::vector<uint8_t>& d, size_t i) { return d[i] | d[i + 1] << 8 | (uint32_t)d[i + 2] << 16; }
static std::string ymd(int y, int m, int d) { char b[16]; snprintf(b, sizeof b, "%04d-%02d-%02d", y, m, d); return b; }

// Billing date (previous period): day bits 0-4, month bits 5-8, year bits 9-15
static void techemPrevDate(uint16_t raw, int& y, int& m, int& d) { d = raw & 0x1F; m = (raw >> 5) & 0x0F; y = 2000 + (raw >> 9); }
// Current date: day bits 4-8, month bits 9-12; year derived from the previous billing date
static std::string techemCurrDate(uint16_t raw, int py, int pm, int pd) {
    int d = (raw >> 4) & 0x1F; if (!d) d = 1;
    int m = (raw >> 9) & 0x0F; if (!m) m = 12;
    const int y = py + ((m < pm) || (m == pm && d <= pd) ? 1 : 0);
    return ymd(y, m, d);
}

static bool decodeTechem(Frame& f, size_t pos) {
    const auto& d = f.data;
    const size_t n = d.size() > pos ? d.size() - pos : 0;
    const uint8_t v = f.version, t = f.type;
    int py, pm, pd;
    if ((v == 0x69 || v == 0x94) && t == 0x80 && n >= 13) {               // heat cost allocator
        const uint8_t kind = d[pos];
        if (kind != 0x01 && kind != 0x11 && kind != 0x0F) return false;
        const size_t extra = kind == 0x0F ? 1 : 0;
        if (n < 13 + extra) return false;
        techemPrevDate(w16(d, pos + 1), py, pm, pd);
        addVal(f, "hca_current", w16(d, pos + 7));
        addText(f, "date_current", techemCurrDate(w16(d, pos + 5), py, pm, pd));
        addVal(f, "hca_previous", w16(d, pos + 3));
        addText(f, "date_previous", ymd(py, pm, pd));
        addVal(f, "temp_room_C", w16(d, pos + 9 + extra) / 100.0);
        addVal(f, "temp_radiator_C", w16(d, pos + 11 + extra) / 100.0);
        f.driver = "Techem FHKV data III"; f.media = "Heizkostenverteiler";
        return true;
    }
    if (v == 0x74 && (t == 0x62 || t == 0x72) && n >= 9) {                // MK Radio 3 water meter
        techemPrevDate(w16(d, pos + 1), py, pm, pd);
        const double prev = w16(d, pos + 3) / 10.0, curr = w16(d, pos + 7) / 10.0;
        addVal(f, "volume_m3", prev + curr);
        addVal(f, "volume_period_m3", curr);
        addText(f, "date_current", techemCurrDate(w16(d, pos + 5), py, pm, pd));
        addVal(f, "volume_target_m3", prev);
        addText(f, "date_previous", ymd(py, pm, pd));
        f.driver = "Techem MK Radio 3"; f.media = t == 0x62 ? "Warmwasserzähler" : "Kaltwasserzähler";
        return true;
    }
    if ((v == 0x95 || v == 0x70) && (t == 0x62 || t == 0x72) && n >= 9) {  // MK Radio 4 water meter
        const double prev = w16(d, pos + 3) / 10.0, curr = w16(d, pos + 7) / 10.0;
        addVal(f, "volume_m3", prev + curr);
        addVal(f, "volume_period_m3", curr);
        addVal(f, "volume_target_m3", prev);
        f.driver = "Techem MK Radio 4"; f.media = t == 0x62 ? "Warmwasserzähler" : "Kaltwasserzähler";
        return true;
    }
    if ((v == 0x45 || v == 0x22 || v == 0x39) && (t == 0x04 || t == 0x43 || t == 0xC3) && n >= 10) {  // Compact V heat meter
        const double prev = w24(d, pos + 3), curr = w24(d, pos + 7);
        addVal(f, "energy_kWh", prev + curr);
        addVal(f, "energy_period_kWh", curr);
        addVal(f, "energy_previous_kWh", prev);
        f.driver = "Techem Compact V"; f.media = "Wärmezähler";
        return true;
    }
    if (v == 0x27 && (t == 0x04 || t == 0xC3) && n >= 9) {               // Vario 451 heat meter (1/1000 GJ)
        const double prev = w16(d, pos + 3) / 3.6, curr = w16(d, pos + 7) / 3.6;
        addVal(f, "energy_kWh", prev + curr);
        addVal(f, "energy_period_kWh", curr);
        addVal(f, "energy_previous_kWh", prev);
        f.driver = "Techem Vario 451"; f.media = "Wärmezähler";
        return true;
    }
    if (v == 0x76 && t == 0xF0 && n >= 3) {                                 // TSD2 smoke detector
        const uint8_t st = d[pos];
        addText(f, "smoke_status", st == 0x00 ? "OK" : st == 0x01 ? "RAUCH" : "Fehler/Status 0x" + std::string(1, "0123456789ABCDEF"[st >> 4]) + "0123456789ABCDEF"[st & 15]);
        techemPrevDate(w16(d, pos + 1), py, pm, pd);
        addText(f, "date_previous", ymd(py, pm, pd));
        f.driver = "Techem TSD2"; f.media = "Rauchmelder";
        return true;
    }
    return false;
}

Frame decode(const uint8_t* raw, size_t n) {
    Frame f;
    Mode mode;
    const size_t want = expectedRawLength(raw, n, mode);
    if (!want) { f.error = "kein gültiger Rahmenanfang"; return f; }
    if (n < want) { f.error = "unvollständig"; return f; }
    f.mode = mode;
    std::vector<uint8_t> bytes;
    if (mode == Mode::T1) {
        const size_t nBytes = (want * 2) / 3;
        for (size_t i = 0; i < nBytes; i++) {
            const int hi = decode3of6Symbol((uint8_t)symbolAt(raw, n, 2 * i));
            const int lo = decode3of6Symbol((uint8_t)symbolAt(raw, n, 2 * i + 1));
            if (hi < 0 || lo < 0) { f.error = "3-aus-6-Fehler bei Byte " + std::to_string(i); return f; }
            bytes.push_back((uint8_t)(hi << 4 | lo));
        }
        if (!stripCrcA(bytes, f.data, f.error)) return f;
    } else {
        bytes.assign(raw + 2, raw + want);
        if (!(mode == Mode::C1A ? stripCrcA(bytes, f.data, f.error) : stripCrcB(bytes, f.data, f.error))) return f;
    }
    const auto& d = f.data;
    if (d.size() < 10) { f.error = "zu kurz"; return f; }
    f.L = d[0]; f.C = d[1];
    const uint16_t m = d[2] | d[3] << 8;
    f.manufacturer = {(char)(((m >> 10) & 0x1F) + 64), (char)(((m >> 5) & 0x1F) + 64), (char)((m & 0x1F) + 64)};
    char id[12]; snprintf(id, sizeof id, "%02X%02X%02X%02X", d[7], d[6], d[5], d[4]); f.id = id;
    f.version = d[8]; f.type = d[9];
    f.ok = true;
    if (d.size() < 11) return f;
    size_t pos = 10;
    f.ci = d[pos++];
    if (f.ci == 0x8C && d.size() > pos + 2) { pos += 2; f.ci = d[pos++]; }           // ELL I: CC ACC
    else if (f.ci == 0x8D) { f.encryption = 100; return f; }                          // ELL II: AES encrypted
    if (f.ci == 0x7A && d.size() >= pos + 4) {
        f.accessNo = d[pos]; f.status = d[pos + 1];
        const uint16_t cfg = d[pos + 2] | d[pos + 3] << 8;
        f.encryption = (cfg >> 8) & 0x1F; pos += 4;
    } else if (f.ci == 0x72 && d.size() >= pos + 12) {
        char mid[12]; snprintf(mid, sizeof mid, "%02X%02X%02X%02X", d[pos + 3], d[pos + 2], d[pos + 1], d[pos]); f.id = mid;
        const uint16_t mm = d[pos + 4] | d[pos + 5] << 8;
        f.manufacturer = {(char)(((mm >> 10) & 0x1F) + 64), (char)(((mm >> 5) & 0x1F) + 64), (char)((mm & 0x1F) + 64)};
        f.version = d[pos + 6]; f.type = d[pos + 7];
        f.accessNo = d[pos + 8]; f.status = d[pos + 9];
        const uint16_t cfg = d[pos + 10] | d[pos + 11] << 8;
        f.encryption = (cfg >> 8) & 0x1F; pos += 12;
    } else if (f.ci == 0x78) { f.encryption = 0; }
    else {
        // Techem: proprietary but unencrypted payload right after the CI field
        if (f.manufacturer == "TCH" && f.ci >= 0xA0 && f.ci <= 0xA2 && decodeTechem(f, pos)) {
            f.encryption = 0; f.payloadParsed = true;
        }
        return f;   // other manufacturer specific / unsupported CI: header only
    }
    if (f.encryption == 0) { parseRecords(f, pos); f.payloadParsed = true; }
    return f;
}

const char* deviceTypeName(uint8_t t) {
    switch (t) {
        case 0x00: return "Sonstiges"; case 0x01: return "Öl"; case 0x02: return "Strom"; case 0x03: return "Gas";
        case 0x04: return "Wärme"; case 0x05: return "Dampf"; case 0x06: return "Warmwasser (30-90 °C)";
        case 0x07: return "Wasser"; case 0x08: return "Heizkostenverteiler"; case 0x09: return "Druckluft";
        case 0x0A: case 0x0B: return "Kälte"; case 0x0C: return "Wärme (Vorlauf)"; case 0x0D: return "Wärme/Kälte";
        case 0x0E: return "Bus/System"; case 0x15: return "Warmwasser (>90 °C)"; case 0x16: return "Kaltwasser";
        case 0x17: return "Dual-Wasser"; case 0x18: return "Druck"; case 0x19: return "A/D-Wandler";
        case 0x1A: return "Rauchmelder"; case 0x1B: return "Raumsensor"; case 0x1C: return "Gasdetektor";
        case 0x25: return "Funkadapter"; case 0x31: return "OMS-Repeater"; case 0x32: return "Funk-Konverter";
        default: return "unbekannt";
    }
}

const char* modeName(Mode m) {
    switch (m) { case Mode::T1: return "T1"; case Mode::C1A: return "C1-A"; case Mode::C1B: return "C1-B"; default: return "?"; }
}

static std::string encName(int e) {
    switch (e) {
        case -1: return "unbekannt (herstellerspezifisch)"; case 0: return "keine"; case 5: return "AES-128-CBC (Mode 5)";
        case 7: return "AES-128-CBC (Mode 7)"; case 13: return "TLS (Mode 13)"; case 100: return "AES (ELL)";
        default: return "Mode " + std::to_string(e);
    }
}

std::string toJson(const Frame& f, int rssi, int lqi) {
    char b[320];
    std::string s;
    snprintf(b, sizeof b,
             "{\"model\":\"wMBus-%s\",\"id\":\"%s\",\"manufacturer\":\"%s\",\"mode\":\"%s\",\"C\":\"0x%02x\","
             "\"version\":%u,\"device_type\":%u,\"device_type_name\":\"%s\",\"ci\":\"0x%02x\",\"access_no\":%u,"
             "\"encryption\":\"%s\",\"mic\":\"CRC\",\"rssi\":%d,\"lqi\":%d",
             f.manufacturer.c_str(), f.id.c_str(), f.manufacturer.c_str(), modeName(f.mode), f.C, f.version, f.type,
             f.media.empty() ? deviceTypeName(f.type) : f.media.c_str(), f.ci, f.accessNo, encName(f.encryption).c_str(), rssi, lqi);
    s = b;
    if (!f.driver.empty()) s += ",\"driver\":\"" + f.driver + "\",\"media\":\"" + f.media + "\"";
    for (const Record& r : f.records) {
        if (!r.text.empty()) { s += ",\"" + r.name + "\":\"" + r.text + "\""; continue; }
        if (isnan(r.value)) continue;
        snprintf(b, sizeof b, ",\"%s\":%.6g", r.name.c_str(), r.value);
        s += b;
    }
    s += ",\"raw\":\"" + hex(f.data.data(), f.data.size()) + "\"}";
    return s;
}

}  // namespace wmbus
