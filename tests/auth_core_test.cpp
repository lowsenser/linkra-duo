// Host-Tests für src/auth/auth_core.* (reine Logik, ohne ESP-IDF).
// Build & Lauf (aus dem Projektordner):
//   c++ -std=c++17 -Wall -Wextra -g -fsanitize=address,undefined \
//       tests/auth_core_test.cpp src/auth/auth_core.cpp -o /tmp/auth_core_test && /tmp/auth_core_test
// Auf dem Host gibt es kein mbedTLS; HMAC-SHA1 kommt deshalb aus einer kleinen
// Referenzimplementierung unten (gegen RFC 2202 geprüft). Nur Testvektoren aus
// RFC 6238 / RFC 4648 / RFC 2202, keine echten Geheimnisse.
#include "../src/auth/auth_core.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

using namespace auth;

// ---------- Mini-SHA1 + HMAC (nur Test) ----------
namespace {
struct Sha1 {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint8_t buf[64];
    size_t bl = 0;
    uint64_t total = 0;
    static uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
    void block(const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    void update(const uint8_t* p, size_t n) {
        total += n;
        while (n--) { buf[bl++] = *p++; if (bl == 64) { block(buf); bl = 0; } }
    }
    void final(uint8_t out[20]) {
        uint64_t bits = total * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t z = 0;
        while (bl != 56) update(&z, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
        update(len, 8);
        for (int i = 0; i < 5; i++) for (int j = 0; j < 4; j++) out[4 * i + j] = (uint8_t)(h[i] >> (24 - 8 * j));
    }
};

void hmacSha1(const uint8_t* key, size_t keyLen, const uint8_t* msg, size_t msgLen, uint8_t out[20]) {
    uint8_t k[64] = {0};
    if (keyLen > 64) { Sha1 s; s.update(key, keyLen); s.final(k); }
    else memcpy(k, key, keyLen);
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    uint8_t inner[20];
    Sha1 a; a.update(ipad, 64); a.update(msg, msgLen); a.final(inner);
    Sha1 b; b.update(opad, 64); b.update(inner, 20); b.final(out);
}

std::string hex(const uint8_t* p, size_t n) { char b[129]; hexEncode(p, n, b); return b; }

// Deterministischer "Zufall" für Code-Generierung.
uint32_t g_seed = 1;
void fakeRandom(uint8_t* buf, size_t len) {
    for (size_t i = 0; i < len; i++) { g_seed = g_seed * 1103515245u + 12345u; buf[i] = (uint8_t)(g_seed >> 16); }
}
}  // namespace

static void testHmacReference() {
    // RFC 2202 Testfall 2
    uint8_t mac[20];
    hmacSha1((const uint8_t*)"Jefe", 4, (const uint8_t*)"what do ya want for nothing?", 28, mac);
    assert(hex(mac, 20) == "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
}

static void testBase32() {
    struct { const char* in; const char* out; } v[] = {
        {"", ""}, {"f", "MY======"}, {"fo", "MZXQ===="}, {"foo", "MZXW6==="},
        {"foob", "MZXW6YQ="}, {"fooba", "MZXW6YTB"}, {"foobar", "MZXW6YTBOI======"},
    };
    for (auto& t : v) {
        char enc[64];
        size_t n = base32Encode((const uint8_t*)t.in, strlen(t.in), enc, sizeof(enc), true);
        assert(n == strlen(t.out));
        assert(strcmp(enc, t.out) == 0);
        uint8_t dec[64];
        int d = base32Decode(t.out, dec, sizeof(dec));
        assert(d == (int)strlen(t.in));
        assert(memcmp(dec, t.in, (size_t)d) == 0);
    }
    // Ohne Padding, Kleinbuchstaben, Leerzeichen
    char enc[64];
    assert(base32Encode((const uint8_t*)"foobar", 6, enc, sizeof(enc), false) == 10);
    assert(strcmp(enc, "MZXW6YTBOI") == 0);
    uint8_t dec[16];
    assert(base32Decode("mzxw 6ytb oi", dec, sizeof(dec)) == 6 && memcmp(dec, "foobar", 6) == 0);
    // Ungültig
    assert(base32Decode("MZ1W", dec, sizeof(dec)) == -1);
    assert(base32Decode("MY==MY", dec, sizeof(dec)) == -1);
    assert(base32Decode("MZXW6YTBOI", dec, 3) == -1);  // Puffer zu klein
    assert(base32Encode((const uint8_t*)"foobar", 6, enc, 10, false) == 0);
    // RFC-6238-Geheimnis
    const char* secret = "12345678901234567890";
    assert(base32Encode((const uint8_t*)secret, 20, enc, sizeof(enc), false) == 32);
    assert(strcmp(enc, "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ") == 0);
}

static void testTotp() {
    const uint8_t* key = (const uint8_t*)"12345678901234567890";
    struct { int64_t t; uint32_t code8; } v[] = {
        {59, 94287082}, {1111111109, 7081804}, {1111111111, 14050471},
        {1234567890, 89005924}, {2000000000, 69279037}, {20000000000LL, 65353130},
    };
    for (auto& t : v) {
        assert(hotp(hmacSha1, key, 20, (uint64_t)totpStep(t.t), 8) == t.code8);
        const uint32_t c6 = t.code8 % 1000000;
        assert(hotp(hmacSha1, key, 20, (uint64_t)totpStep(t.t), 6) == c6);
        // Exakt, ±1 Fenster, ±2 abgelehnt
        assert(totpVerify(hmacSha1, key, 20, c6, t.t, -1) == totpStep(t.t));
        assert(totpVerify(hmacSha1, key, 20, c6, t.t + 30, -1) == totpStep(t.t));
        assert(totpVerify(hmacSha1, key, 20, c6, t.t - 30, -1) == totpStep(t.t));
        assert(totpVerify(hmacSha1, key, 20, c6, t.t + 60, -1) == -1);
        assert(totpVerify(hmacSha1, key, 20, c6, t.t - 60, -1) == -1);
        // Replay: Schritt schon verwendet
        assert(totpVerify(hmacSha1, key, 20, c6, t.t, totpStep(t.t)) == -1);
        assert(totpVerify(hmacSha1, key, 20, c6, t.t, totpStep(t.t) - 1) == totpStep(t.t));
    }
    // RFC-4226-HOTP-Testwerte (Zähler 0..2)
    assert(hotp(hmacSha1, key, 20, 0, 6) == 755224);
    assert(hotp(hmacSha1, key, 20, 1, 6) == 287082);
    assert(hotp(hmacSha1, key, 20, 2, 6) == 359152);
    uint32_t c;
    assert(parseTotpCode("081804", c) && c == 81804);
    assert(parseTotpCode("081 804", c) && c == 81804);
    assert(!parseTotpCode("81804", c));
    assert(!parseTotpCode("0818045", c));
    assert(!parseTotpCode("08180a", c));
    assert(!parseTotpCode("", c));
    assert(!parseTotpCode(nullptr, c));
    assert(totpStep(-5) == -1);
    assert(totpVerify(hmacSha1, key, 20, 287082, -1, -1) == -1);
}

static void testRecovery() {
    char code[kRecoveryCodeLen + 1];
    for (int i = 0; i < 200; i++) {
        generateRecoveryCode(fakeRandom, code);
        assert(strlen(code) == 9 && code[4] == '-');
        char norm[kRecoveryCodeLen + 1];
        assert(normalizeRecoveryCode(code, norm) && strcmp(norm, code) == 0);
        for (int j = 0; j < 9; j++) {
            if (j == 4) continue;
            assert(strchr(kRecoveryAlphabet, code[j]) != nullptr);
            assert(code[j] != '0' && code[j] != 'o' && code[j] != '1' && code[j] != 'l' && code[j] != 'i');
        }
    }
    char n[kRecoveryCodeLen + 1];
    assert(normalizeRecoveryCode("ABCD-EFGH", n) && strcmp(n, "abcd-efgh") == 0);
    assert(normalizeRecoveryCode("abcdefgh", n) && strcmp(n, "abcd-efgh") == 0);
    assert(normalizeRecoveryCode(" abcd efgh ", n) && strcmp(n, "abcd-efgh") == 0);
    assert(!normalizeRecoveryCode("abc-defgh", n));
    assert(!normalizeRecoveryCode("abcd--efgh", n));
    assert(!normalizeRecoveryCode("abcd-efg0", n));
    assert(!normalizeRecoveryCode("abcd-efghj", n));
    assert(!normalizeRecoveryCode("abcd-efg", n));
    assert(!normalizeRecoveryCode("", n));
}

static void testValidation() {
    assert(validUsername("admin"));
    assert(validUsername("a.b_c-9"));
    assert(validUsername("abc"));
    assert(!validUsername("ab"));
    assert(validUsername("abcdefghijabcdefghijabcdefghijab"));   // 32
    assert(!validUsername("abcdefghijabcdefghijabcdefghijabc"));  // 33
    assert(!validUsername("ad min"));
    assert(!validUsername("admin!"));
    assert(!validUsername("ädmin"));
    assert(!validUsername(nullptr));
    assert(validPassword("0123456789"));
    assert(!validPassword("012345678"));
    assert(validPassword("ääääääääää"));  // 10 Codepoints, 20 Bytes
    assert(!validPassword("ääääääääa"));  // 9 Codepoints
    std::string p128(128, 'x'), p129(129, 'x');
    assert(validPassword(p128.c_str()));
    assert(!validPassword(p129.c_str()));
    assert(!validPassword("0123456789\n"));
    assert(!validPassword(nullptr));
}

static void testHelpers() {
    char out[80];
    assert(findCookie("linkra_session=abc123", "linkra_session", out, sizeof(out)) && strcmp(out, "abc123") == 0);
    assert(findCookie("a=1; linkra_session=xyz; b=2", "linkra_session", out, sizeof(out)) && strcmp(out, "xyz") == 0);
    assert(findCookie("a=1;linkra_session = \"q\" ;b=2", "linkra_session", out, sizeof(out)) && strcmp(out, "q") == 0);
    assert(!findCookie("xlinkra_session=1; linkra_sessionx=2", "linkra_session", out, sizeof(out)));
    assert(!findCookie("", "linkra_session", out, sizeof(out)));
    assert(!findCookie("linkra_session", "linkra_session", out, sizeof(out)));
    assert(!findCookie("linkra_session=0123456789", "linkra_session", out, 5));  // zu lang
    assert(findCookie("linkra_session=", "linkra_session", out, sizeof(out)) && out[0] == '\0');

    assert(originMatchesHost("http://192.168.1.50", "192.168.1.50"));
    assert(originMatchesHost("http://Linkra-Duo.local", "linkra-duo.local"));
    assert(originMatchesHost("http://linkra-duo.local:80", "linkra-duo.local"));
    assert(originMatchesHost("http://linkra-duo.local", "linkra-duo.local:80"));
    assert(originMatchesHost("http://localhost:5173", "localhost:5173"));
    assert(!originMatchesHost("http://localhost:5173", "localhost"));
    assert(!originMatchesHost("http://evil.example", "192.168.1.50"));
    assert(!originMatchesHost("null", "192.168.1.50"));
    assert(!originMatchesHost("http://192.168.1.50.evil.example", "192.168.1.50"));
    assert(!originMatchesHost("http://a@192.168.1.50", "192.168.1.50"));
    assert(!originMatchesHost("ftp://192.168.1.50", "192.168.1.50"));
    assert(!originMatchesHost("http://", ""));

    uint8_t b[4] = {0x00, 0xab, 0x10, 0xff};
    char h[9];
    hexEncode(b, 4, h);
    assert(strcmp(h, "00ab10ff") == 0);
    uint8_t d[4];
    assert(hexDecode(h, 8, d, 4) && memcmp(b, d, 4) == 0);
    assert(hexDecode("00AB10FF", 8, d, 4) && memcmp(b, d, 4) == 0);
    assert(!hexDecode("00ab10f", 7, d, 4));
    assert(!hexDecode("00ab10fg", 8, d, 4));
    assert(constTimeEq(b, d, 4));
    d[3] = 0;
    assert(!constTimeEq(b, d, 4));

    assert(urlEncode("Linkra Duo:admin", out, sizeof(out)) && strcmp(out, "Linkra%20Duo%3Aadmin") == 0);
    assert(urlEncode("a.b_c-d~", out, sizeof(out)) && strcmp(out, "a.b_c-d~") == 0);
    assert(urlEncode("abc", out, 3) == 0);
}

static void testRateLimiter() {
    assert(lockDurationS(0) == 0);
    assert(lockDurationS(1) == 30);
    assert(lockDurationS(2) == 60);
    assert(lockDurationS(5) == 480);
    assert(lockDurationS(6) == 900);
    assert(lockDurationS(200) == 900);

    RateLimiter rl;
    const uint32_t ip = 0x0A000001, other = 0x0A000002;
    uint32_t t = 1000;
    for (int i = 0; i < 4; i++) { assert(rl.fail(ip, t) == 0); }
    assert(rl.retryAfter(ip, t) == 0);
    assert(rl.fail(ip, t) == 30);           // 5. Fehlversuch -> Sperre 30 s
    assert(rl.retryAfter(ip, t + 10) == 20);
    assert(rl.retryAfter(other, t + 10) == 20);  // global ebenfalls gesperrt
    // Während der Sperre zählen Versuche nicht weiter
    assert(rl.fail(ip, t + 11) == 19);
    t += 30;
    assert(rl.retryAfter(ip, t) == 0);
    for (int i = 0; i < 4; i++) assert(rl.fail(ip, t) == 0);
    assert(rl.fail(ip, t) == 60);           // zweite Sperre: doppelt
    t += 60;
    for (int s = 3; s <= 7; s++) {
        for (int i = 0; i < 4; i++) rl.fail(ip, t);
        const uint32_t r = rl.fail(ip, t);
        assert(r == lockDurationS((uint8_t)s));
        t += r;
    }
    assert(lockDurationS(7) == 900);
    // Erfolg setzt Client zurück; globale Sperrstufe bleibt, offene Zähler weg
    rl.success(ip);
    assert(rl.retryAfter(ip, t) == 0);
    for (int i = 0; i < 4; i++) assert(rl.fail(other, t) == 0);
    // Verfall nach 1 h ohne Fehlversuch
    RateLimiter r2;
    for (int i = 0; i < 5; i++) r2.fail(ip, 100);
    assert(r2.retryAfter(ip, 100) == 30);
    for (int i = 0; i < 5; i++) r2.fail(ip, 100 + 30 + 3600);
    assert(r2.retryAfter(ip, 100 + 30 + 3600) == 30);  // Stufe verfallen -> wieder 30 s
    // Viele Clients: Plätze werden recycelt, kein Absturz
    RateLimiter r3;
    for (uint32_t k = 0; k < 50; k++) { r3.fail(k, 5000 + k); r3.success(k); }
    assert(r3.retryAfter(49, 6000) == 0);
    // Überlauf der monotonen Zeit
    RateLimiter r4;
    for (int i = 0; i < 5; i++) r4.fail(ip, 0xFFFFFFF0u);
    assert(r4.retryAfter(ip, 0xFFFFFFF0u) == 30);
    assert(r4.retryAfter(ip, 0x0000000Eu) == 0);
    rl.reset();
    assert(rl.retryAfter(ip, t) == 0);
}

static void testSessions() {
    SessionTable st;
    uint8_t tok[SessionTable::kMax + 2][32];
    for (int i = 0; i < SessionTable::kMax + 2; i++) { memset(tok[i], 0, 32); tok[i][0] = (uint8_t)(i + 1); tok[i][31] = 0x5A; }
    uint32_t t = 100;
    for (int i = 0; i < SessionTable::kMax; i++) st.create(tok[i], t + (uint32_t)i);
    assert(st.count(t + 10) == SessionTable::kMax);
    // Sitzung 0 benutzen -> Sitzung 1 ist jetzt die älteste
    assert(st.touch(tok[0], t + 20) >= 0);
    st.create(tok[8], t + 21);
    assert(st.find(tok[1], t + 21) == -1);
    assert(st.find(tok[0], t + 21) >= 0);
    assert(st.find(tok[8], t + 21) >= 0);
    assert(st.count(t + 21) == SessionTable::kMax);
    // Unbekanntes Token, Null-Token
    uint8_t zero[32] = {0};
    assert(st.find(tok[9], t + 21) == -1);
    assert(st.find(zero, t + 21) == -1);
    // Gleitende Gültigkeit 7 Tage
    const uint32_t ttl = SessionTable::kTtlS;
    int s0 = st.touch(tok[0], t + 20 + ttl - 1);
    assert(s0 >= 0);
    assert(st.touch(tok[0], t + 20 + 2 * ttl - 2) == s0);
    assert(st.find(tok[8], t + 20 + 2 * ttl - 2) == -1);  // nicht benutzt -> abgelaufen
    assert(st.find(tok[0], t + 21 + 3 * ttl) == -1);
    // removeAllExcept / remove / removeAll
    SessionTable s2;
    int a = s2.create(tok[0], 1), b = s2.create(tok[1], 1), c = s2.create(tok[2], 1);
    (void)a; (void)c;
    s2.removeAllExcept(b);
    assert(s2.count(2) == 1 && s2.find(tok[1], 2) == b);
    s2.remove(b);
    assert(s2.count(2) == 0);
    s2.create(tok[3], 5);
    s2.removeAll();
    assert(s2.find(tok[3], 5) == -1);
    s2.remove(-1);
    s2.remove(99);
}

int main() {
    testHmacReference();
    testBase32();
    testTotp();
    testRecovery();
    testValidation();
    testHelpers();
    testRateLimiter();
    testSessions();
    puts("auth_core_test: OK");
    return 0;
}
