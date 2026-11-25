// Host test (from the repo root, with AddressSanitizer + UndefinedBehaviorSanitizer):
//   c++ -std=c++17 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
//       -I src tests/wmbus_test.cpp src/rf/wmbus.cpp -o /tmp/wmbus_test && /tmp/wmbus_test
// Must print "wmbus tests OK" without any ASan/UBSan report.
#include "rf/wmbus.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <random>
#include <string>
using namespace wmbus;

static void putCrc(std::vector<uint8_t>& out, const uint8_t* d, size_t n) {
    out.insert(out.end(), d, d + n);
    uint16_t c = crc16(d, n); out.push_back(c >> 8); out.push_back(c & 0xFF);
}
// link-layer bytes (L..end) -> frame format A with CRCs
static std::vector<uint8_t> frameA(const std::vector<uint8_t>& ll) {
    std::vector<uint8_t> o; putCrc(o, ll.data(), 10);
    for (size_t p = 10; p < ll.size(); p += 16) putCrc(o, &ll[p], std::min<size_t>(16, ll.size() - p));
    return o;
}
static uint16_t man(const char* s) { return (uint16_t)(((s[0] - 64) << 10) | ((s[1] - 64) << 5) | (s[2] - 64)); }

static std::vector<uint8_t> sampleLinkLayer() {
    // unencrypted water meter: CI 0x7A short header, volume 12.345 m3 (VIF 0x13 = litres), date record
    uint16_t m = man("ITW");
    std::vector<uint8_t> d = {0, 0x44, (uint8_t)(m & 0xFF), (uint8_t)(m >> 8), 0x78, 0x56, 0x34, 0x12, 0x01, 0x07,
                              0x7A, 0x2A, 0x00, 0x00, 0x00,          // ACC, STS, CFG=0 (no encryption)
                              0x04, 0x13, 0x39, 0x30, 0x00, 0x00,    // 12345 L
                              0x42, 0x6C, 0x5F, 0x2C};               // storage 1 date 2026-12-31? (see assert)
    d[0] = (uint8_t)(d.size() - 1);
    return d;
}

static int g_checks = 0;
#define CHECK(cond) do { g_checks++; if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

// link-layer bytes (L..end, L ignored) -> raw C1 frame B after sync (54 3D), CRCs in place.
// Block 1 = first 126 data bytes, block 2 = rest; L = total length incl. CRCs - 1.
static std::vector<uint8_t> c1FrameB(std::vector<uint8_t> ll) {
    const bool two = ll.size() > 126;
    ll[0] = (uint8_t)(ll.size() + (two ? 4 : 2) - 1);
    std::vector<uint8_t> o = {0x54, 0x3D};
    if (!two) { putCrc(o, ll.data(), ll.size()); return o; }
    putCrc(o, ll.data(), 126);
    putCrc(o, ll.data() + 126, ll.size() - 126);
    return o;
}
static std::vector<uint8_t> c1FrameA(const std::vector<uint8_t>& ll) {
    std::vector<uint8_t> o = {0x54, 0xCD}; auto fa = frameA(ll); o.insert(o.end(), fa.begin(), fa.end()); return o;
}
static Frame dec(const std::vector<uint8_t>& v) {
    // copy into an exactly sized heap buffer so ASan catches any read past the end
    std::vector<uint8_t> exact(v.begin(), v.end());
    Frame f = decode(exact.data(), exact.size());
    if (f.ok) (void)toJson(f, -80, 40);
    return f;
}
// L = 128 / 129: block 2 would have 0 / -1 data bytes. Prepared as in the review:
// 54 3D L + 125 data bytes + valid CRC over block 1, padded to the announced length.
static void testFrameBBadLength(uint8_t L) {
    std::vector<uint8_t> in(126, 0x2F); in[0] = L; in[1] = 0x44;
    uint16_t c = crc16(in.data(), 126); in.push_back(c >> 8); in.push_back(c & 0xFF);
    std::vector<uint8_t> raw = {0x54, 0x3D}; raw.insert(raw.end(), in.begin(), in.end());
    Mode mode;
    CHECK(expectedRawLength(raw.data(), raw.size(), mode) == 0);
    for (size_t extra = 0; extra <= 4; extra++) {      // as captured (126 bytes) and padded beyond L+1
        Frame f = dec(raw); CHECK(!f.ok && !f.error.empty());
        raw.push_back(0x00);
    }
}
// every strict prefix of a valid frame must be rejected without reading past it
static void testTruncated(const std::vector<uint8_t>& raw) {
    CHECK(dec(raw).ok);
    for (size_t n = 0; n < raw.size(); n++) {
        std::vector<uint8_t> pre(raw.begin(), raw.begin() + n);
        CHECK(!dec(pre).ok);
    }
}
static void testRegressions() {
    // --- K1: frame B with L = 128 / 129 ---
    testFrameBBadLength(128);
    testFrameBBadLength(129);

    // --- valid frame B with two blocks (smallest: block 2 with 1 data byte, L = 130) and a larger one ---
    auto ll = sampleLinkLayer();
    for (size_t dataLen : {127u, 136u, 200u, 251u}) {
        auto big = ll; big.resize(dataLen, 0x2F);          // idle filler after the records
        auto raw = c1FrameB(big);
        Mode mode; CHECK(expectedRawLength(raw.data(), raw.size(), mode) == raw.size() && mode == Mode::C1B);
        Frame f = dec(raw);
        if (!f.ok) printf("frame B %zu: %s\n", dataLen, f.error.c_str());
        CHECK(f.ok && f.data.size() == dataLen && f.records.size() == 2 && f.records[0].name == "volume_m3");
        if (dataLen == 127) CHECK(raw[2] == 130);
        // corrupted CRC of block 2 -> rejected
        raw.back() ^= 1; CHECK(!dec(raw).ok);
        raw.back() ^= 1; testTruncated(raw);
    }
    // largest single-block frame B: L = 127 (125 data bytes + CRC)
    { auto b = ll; b.resize(126, 0x2F); auto raw = c1FrameB(b); CHECK(raw[2] == 127); Frame f = dec(raw); CHECK(f.ok && f.records.size() == 2); }

    // --- minimal lengths ---
    // frame A: L = 9 (only the 10-byte first block, no CI)
    std::vector<uint8_t> minA = {9, 0x44, 0x93, 0x15, 0x78, 0x56, 0x34, 0x12, 0x01, 0x07};
    { Frame f = dec(c1FrameA(minA)); CHECK(f.ok && f.mode == Mode::C1A && f.id == "12345678" && f.ci == 0); }
    { auto t = encode3of6(frameA(minA)); Frame f = dec(t); CHECK(f.ok && f.mode == Mode::T1); testTruncated(t); }
    testTruncated(c1FrameA(minA));
    // frame A: L = 8 is too short
    { std::vector<uint8_t> raw = {0x54, 0xCD, 8}; raw.resize(16, 0); CHECK(!dec(raw).ok); }
    // frame B: L = 12 (10 header bytes + CI + CRC)
    std::vector<uint8_t> minB = minA; minB.push_back(0x78);
    { auto raw = c1FrameB(minB); CHECK(raw[2] == 12); Frame f = dec(raw); CHECK(f.ok && f.mode == Mode::C1B && f.ci == 0x78); testTruncated(raw); }
    // frame B: L = 11 is rejected
    { std::vector<uint8_t> raw = {0x54, 0x3D, 11}; raw.resize(16, 0); Mode m; CHECK(expectedRawLength(raw.data(), raw.size(), m) == 0); CHECK(!dec(raw).ok); }
    // header-only frames with CI that announce more than they carry
    for (uint8_t ci : {0x7A, 0x72, 0x8C, 0x8D, 0xA0, 0xA1, 0xA2}) {
        for (size_t extra = 0; extra < 14; extra++) {
            auto h = minB; h[9] = 0x80; h[10] = ci; h[2] = 0x48; h[3] = 0x50;   // "TCH" for the Techem branch
            h.resize(11 + extra, 0x01); h[0] = (uint8_t)(h.size() - 1);
            CHECK(dec(c1FrameB(h)).ok);
            CHECK(dec(c1FrameA(h)).ok);
        }
    }
    // --- truncated versions of the original test frames ---
    testTruncated(encode3of6(frameA(ll)));
    testTruncated(c1FrameA(ll));
    testTruncated(c1FrameB(ll));
    // record parser: 9+ DIFEs, 8-byte integers, float with Inf/NaN, variable length, all with valid CRCs
    {
        auto r = ll; r.resize(15);
        const uint8_t recs[] = {0x84, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0x13, 0x39, 0x30, 0, 0,  // 10 DIFEs
                                0x07, 0x13, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F,                            // int64 max
                                0x07, 0x13, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80,                            // int64 min
                                0x05, 0x13, 0x00, 0x00, 0x80, 0x7F,                                                    // +Inf
                                0x0D, 0x13, 0x05, 1, 2, 3, 4, 5,                                                        // variable len
                                0x0D, 0x13, 0xFF, 1};                                                                    // var len past end
        r.insert(r.end(), recs, recs + sizeof recs);
        r[0] = (uint8_t)(r.size() - 1);
        Frame f = dec(encode3of6(frameA(r))); CHECK(f.ok && f.payloadParsed && !f.records.empty());
        f = dec(c1FrameB(r)); CHECK(f.ok && f.payloadParsed);
    }
}

// Fuzzing: (1) completely random byte strings, (2) random link-layer payloads wrapped
// in valid CRCs so that header parsing, parseRecords and decodeTechem are reached.
static void fuzz(unsigned seed, int runs, size_t& okRandom, size_t& okWrapped) {
    std::mt19937 rng(seed);
    auto rnd = [&](unsigned n) { return (unsigned)(rng() % n); };
    for (int i = 0; i < runs; i++) {
        std::vector<uint8_t> v(rnd(301));
        for (auto& b : v) b = (uint8_t)rng();
        // bias some inputs towards the C1 sync continuations
        if (v.size() >= 2 && rnd(4) == 0) { v[0] = 0x54; v[1] = rnd(2) ? 0xCD : 0x3D; }
        if (dec(v).ok) okRandom++;
    }
    static const uint8_t CIS[] = {0x7A, 0x72, 0x78, 0x8C, 0x8D, 0x90, 0xA0, 0xA1, 0xA2};
    static const uint8_t VERS[] = {0x69, 0x94, 0x74, 0x95, 0x70, 0x45, 0x22, 0x39, 0x27, 0x76};
    static const uint8_t TYPES[] = {0x80, 0x62, 0x72, 0x04, 0x43, 0xC3, 0xF0};
    for (int i = 0; i < runs; i++) {
        std::vector<uint8_t> ll(10 + rnd(242));
        for (auto& b : ll) b = (uint8_t)rng();
        if (rnd(2)) { ll[2] = 0x48; ll[3] = 0x50; }                           // TCH
        if (rnd(2)) { ll[8] = VERS[rnd(sizeof VERS)]; ll[9] = TYPES[rnd(sizeof TYPES)]; }
        if (ll.size() > 10 && rnd(4)) ll[10] = CIS[rnd(sizeof CIS)];
        if (ll.size() > 14 && ll[10] == 0x7A && rnd(2)) ll[14] = 0;           // unencrypted -> records
        if (ll.size() > 22 && ll[10] == 0x72 && rnd(2)) ll[22] = 0;
        if (ll.size() > 13 && ll[10] == 0x8C && rnd(2)) ll[13] = 0x7A;
        std::vector<uint8_t> raw;
        switch (rnd(3)) {
            case 0: ll[0] = (uint8_t)(ll.size() - 1); if (ll.size() > 245) ll.resize(245); ll[0] = (uint8_t)(ll.size() - 1); raw = c1FrameA(ll); break;
            case 1: ll[0] = (uint8_t)(ll.size() - 1); if (ll.size() > 245) ll.resize(245); ll[0] = (uint8_t)(ll.size() - 1); raw = encode3of6(frameA(ll)); break;
            default: if (ll.size() > 251) ll.resize(251); if (ll.size() == 125 || ll.size() == 126) ll.resize(124); raw = c1FrameB(ll); break;
        }
        if (rnd(8) == 0 && !raw.empty()) raw.resize(rnd((unsigned)raw.size()));   // truncate
        if (rnd(8) == 0 && !raw.empty()) raw[rnd((unsigned)raw.size())] ^= (uint8_t)(1u << rnd(8));  // bit flip
        if (dec(raw).ok) okWrapped++;
    }
}

// ELL (8C) + AFL (90) + long transport header (72, mode 7): a radio module that
// relays an encrypted meter (structure as seen from Techem modules; IDs invented).
static void testLayers() {
    std::vector<uint8_t> ll = {0, 0x44, 0x68, 0x50, 0x78, 0x56, 0x34, 0x12, 0x59, 0x37,   // TCH 12345678
        0x8C, 0x20, 0x16,                                                                   // ELL I: CC ACC
        0x90, 0x0F, 0x00, 0x2C, 0x25, 0xA9, 0x25, 0x12, 0x00,                               // AFL: FCL MCL MCR
        0xE1, 0xCE, 0x34, 0xDC, 0x02, 0x74, 0x9E, 0x8D,                                     //      MAC (8, AT=5)
        0x72, 0x21, 0x43, 0x65, 0x87, 0xA8, 0x5C, 0xFE, 0x07,                               // TPL: meter 87654321 WEH
        0xF9, 0x00, 0x20, 0x07, 0x10};                                                      //      ACC ST CW(mode 7, 2 blocks) CFE
    for (int i = 0; i < 32; i++) ll.push_back((uint8_t)(i * 37));                           // encrypted payload
    ll[0] = (uint8_t)(ll.size() - 1);
    Frame f = dec(c1FrameA(ll));
    CHECK(f.ok);
    CHECK(f.manufacturer == "TCH" && f.id == "12345678");              // transmitter stays the identity
    CHECK(f.hasEll && f.ellCc == 0x20 && f.ellAcc == 0x16);
    CHECK(f.hasAfl && f.aflMcl == 0x25 && f.aflMcr == 0x001225A9 && f.aflMac.size() == 8 && f.aflMac[0] == 0xE1);
    CHECK(f.hasMeter && f.meterId == "87654321" && f.meterManufacturer == "WEH" && f.meterType == 0x07);
    CHECK(f.ci == 0x72 && f.encryption == 7 && f.encBlocks == 2);
    CHECK(f.payloadOffset == ll.size() - 32);
    CHECK(f.records.empty());
    const std::string js = toJson(f, -60, 40);
    CHECK(js.find("\"meter_id\":\"87654321\"") != std::string::npos);
    CHECK(js.find("\"afl_mcr\":1189289") != std::string::npos);
    CHECK(js.find("Mode 7") != std::string::npos);
    // short header after ELL, unencrypted -> records are parsed
    std::vector<uint8_t> s2 = {0, 0x44, 0x68, 0x50, 0x78, 0x56, 0x34, 0x12, 0x59, 0x37, 0x8C, 0x20, 0x16,
                               0x7A, 0x01, 0x00, 0x00, 0x00, 0x04, 0x13, 0x10, 0x27, 0x00, 0x00};
    s2[0] = (uint8_t)(s2.size() - 1);
    Frame g = dec(c1FrameA(s2));
    CHECK(g.ok && g.hasEll && g.ci == 0x7A && g.encryption == 0 && !g.records.empty());
    // invalid manufacturer bits never produce characters outside A..Z
    std::vector<uint8_t> s3 = {0, 0x44, 0xFF, 0xFF, 0x78, 0x56, 0x34, 0x12, 0x01, 0x07, 0x78};
    s3[0] = (uint8_t)(s3.size() - 1);
    Frame h = dec(c1FrameA(s3));
    CHECK(h.ok && h.manufacturer.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ?") == std::string::npos);
}

int main() {
    testLayers();
    // CRC-16/EN-13757 catalogue check value
    assert(crc16((const uint8_t*)"123456789", 9) == 0xC2B7);
    // every 3-of-6 symbol roundtrips
    for (int i = 0; i < 16; i++) {
        auto e = encode3of6({(uint8_t)(i << 4 | i)});
        assert(decode3of6Symbol(e[0] >> 2) == i);
    }
    auto ll = sampleLinkLayer();
    // --- T1 ---
    auto raw = encode3of6(frameA(ll));
    Mode mode; size_t want = expectedRawLength(raw.data(), raw.size(), mode);
    assert(mode == Mode::T1 && want <= raw.size());
    Frame f = decode(raw.data(), raw.size());
    if (!f.ok) printf("T1 error: %s\n", f.error.c_str());
    assert(f.ok && f.manufacturer == "ITW" && f.id == "12345678" && f.type == 0x07 && f.ci == 0x7A && f.encryption == 0);
    assert(f.records.size() == 2 && f.records[0].name == "volume_m3" && fabs(f.records[0].value - 12.345) < 1e-9);
    printf("date record: %s = %s\n", f.records[1].name.c_str(), f.records[1].text.c_str());
    // corrupt one bit -> must fail CRC or 3-of-6
    raw[8] ^= 0x10; assert(!decode(raw.data(), raw.size()).ok);
    // --- C1 frame A ---
    std::vector<uint8_t> c1 = {0x54, 0xCD}; auto fa = frameA(ll); c1.insert(c1.end(), fa.begin(), fa.end());
    f = decode(c1.data(), c1.size()); assert(f.ok && f.mode == Mode::C1A && f.id == "12345678");
    // --- C1 frame B (single CRC at end, L includes CRC) ---
    std::vector<uint8_t> b = ll; b[0] = (uint8_t)(ll.size() + 1); std::vector<uint8_t> c1b = {0x54, 0x3D};
    putCrc(c1b, b.data(), b.size());
    f = decode(c1b.data(), c1b.size()); assert(f.ok && f.mode == Mode::C1B && f.records[0].name == "volume_m3");
    // --- encrypted (mode 5): header only, no records ---
    auto enc = ll; enc[14] = 0x05; f = decode(encode3of6(frameA(enc)).data(), encode3of6(frameA(enc)).size());
    assert(f.ok && f.encryption == 5 && f.records.empty());
    printf("%s\n", toJson(f, -80, 40).c_str());
    // --- Techem: link-layer telegrams from the wmbusmeters driver test suites ---
    struct TV { const char* hex; std::vector<std::pair<const char*, double>> nums; std::vector<std::pair<const char*, const char*>> texts; };
    const TV tvs[] = {
        {"34446850226677116980A0119F27020480048300C408F709143C003D341A2B0B2A0707000000000000062D114457563D71A1850000",
         {{"hca_current", 131}, {"hca_previous", 1026}, {"temp_room_C", 22.44}, {"temp_radiator_C", 25.51}},
         {{"date_current", "2020-02-08"}, {"date_previous", "2019-12-31"}}},
        {"33446850341211119480A20F9F292D005024040011BD08380904000000070000000000000000000000000001000000000003140E",
         {{"hca_current", 4}, {"hca_previous", 45}, {"temp_room_C", 22.37}, {"temp_radiator_C", 23.6}},
         {{"date_current", "2021-02-05"}, {"date_previous", "2020-12-31"}}},
        {"32446850059605416980A0019F334805400CFE029808B1080000000000060F4F3467AEA9A88D7B8D503E000000000000000000",
         {{"hca_current", 766}, {"hca_previous", 1352}, {"temp_room_C", 22.0}, {"temp_radiator_C", 22.25}},
         {{"date_current", "2026-06-04"}, {"date_previous", "2025-12-31"}}},
        {"2F446850313233347462A2069F255900B029310000000306060906030609070606050509050505050407040605070500",
         {{"volume_m3", 13.8}, {"volume_target_m3", 8.9}}, {{"date_current", "2019-04-27"}, {"date_previous", "2018-12-31"}}},
        {"2F446850200141029562A206702901006017030004000300000000000000000000000000000000000000000000000000",
         {{"volume_m3", 0.4}, {"volume_target_m3", 0.1}}, {}},
        {"36446850626262624543A1009F2777010060780000000A000000000000000000000000000000000000000000000000A0400000B4010000",
         {{"energy_kWh", 495}, {"energy_previous_kWh", 375}, {"energy_period_kWh", 120}}, {}},
        {"374468506549235827C3A2129F25383300A8622600008200800A2AF862115175552877A36F26C9AB1CB24400000004000000000004908002",
         {{"energy_kWh", 6371.666667}, {"energy_period_kWh", 2729.444444}, {"energy_previous_kWh", 3642.222222}}, {}},
        {"294468506935639176F0A0019F2782290060822900000401D6311AF93E1BF93E008DC3009ED4000FE500",
         {}, {{"smoke_status", "RAUCH"}, {"date_previous", "2019-12-31"}}},
    };
    for (const TV& tv : tvs) {
        std::vector<uint8_t> ll;
        for (const char* p = tv.hex; p[0] && p[1]; p += 2) { unsigned b; sscanf(p, "%2x", &b); ll.push_back((uint8_t)b); }
        auto air = encode3of6(frameA(ll));
        Frame tf = decode(air.data(), air.size());
        if (!tf.ok || tf.driver.empty()) { printf("Techem decode failed: %s (%s)\n", tv.hex, tf.error.c_str()); return 1; }
        for (auto& kv : tv.nums) {
            bool found = false;
            for (auto& r : tf.records) if (r.name == kv.first) { found = fabs(r.value - kv.second) < 1e-3; break; }
            if (!found) { printf("%s: %s != %g\n", tf.driver.c_str(), kv.first, kv.second); return 1; }
        }
        for (auto& kv : tv.texts) {
            bool found = false;
            for (auto& r : tf.records) if (r.name == kv.first) { found = r.text == kv.second; break; }
            if (!found) { printf("%s: %s != %s\n", tf.driver.c_str(), kv.first, kv.second); return 1; }
        }
        printf("  ok  %-22s %s\n", tf.driver.c_str(), toJson(tf, -80, 40).c_str());
    }
    testRegressions();
    printf("regression checks: %d passed\n", g_checks);
    const int RUNS = 200000;
    size_t okRandom = 0, okWrapped = 0;
    fuzz(0x4C494E4B, RUNS, okRandom, okWrapped);   // fixed seed "LINK"
    printf("fuzz: %d random inputs (%zu decoded ok), %d CRC-valid random frames (%zu decoded ok)\n",
           RUNS, okRandom, RUNS, okWrapped);
    puts("wmbus tests OK");
}
