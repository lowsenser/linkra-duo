#include "auth_core.h"

#include <cstring>

namespace auth {

// ---------- Hilfen ----------
bool constTimeEq(const uint8_t* a, const uint8_t* b, size_t len) {
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) diff |= a[i] ^ b[i];
    return diff == 0;
}

void hexEncode(const uint8_t* in, size_t len, char* out) {
    static const char* hex = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[2 * i] = hex[in[i] >> 4];
        out[2 * i + 1] = hex[in[i] & 0x0F];
    }
    out[2 * len] = '\0';
}

static int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool hexDecode(const char* in, size_t inLen, uint8_t* out, size_t outLen) {
    if (!in || inLen != 2 * outLen) return false;
    for (size_t i = 0; i < outLen; i++) {
        int h = hexVal(in[2 * i]), l = hexVal(in[2 * i + 1]);
        if (h < 0 || l < 0) return false;
        out[i] = (uint8_t)((h << 4) | l);
    }
    return true;
}

size_t urlEncode(const char* in, char* out, size_t outCap) {
    static const char* hex = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char* p = (const unsigned char*)in; *p; p++) {
        const unsigned char c = *p;
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
        if (unreserved) {
            if (o + 1 >= outCap) return 0;
            out[o++] = (char)c;
        } else {
            if (o + 3 >= outCap) return 0;
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0x0F];
        }
    }
    if (o >= outCap) return 0;
    out[o] = '\0';
    return o;
}

// ---------- Base32 ----------
static const char kB32[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

size_t base32Encode(const uint8_t* in, size_t len, char* out, size_t outCap, bool pad) {
    const size_t chars = (len * 8 + 4) / 5;
    const size_t total = pad ? ((len + 4) / 5) * 8 : chars;
    if (outCap < total + 1) return 0;
    size_t o = 0;
    uint32_t buffer = 0;
    int bits = 0;
    for (size_t i = 0; i < len; i++) {
        buffer = (buffer << 8) | in[i];
        bits += 8;
        while (bits >= 5) {
            out[o++] = kB32[(buffer >> (bits - 5)) & 0x1F];
            bits -= 5;
        }
    }
    if (bits > 0) out[o++] = kB32[(buffer << (5 - bits)) & 0x1F];
    while (o < total) out[o++] = '=';
    out[o] = '\0';
    return o;
}

int base32Decode(const char* in, uint8_t* out, size_t outCap) {
    if (!in) return -1;
    uint32_t buffer = 0;
    int bits = 0;
    size_t o = 0;
    bool padSeen = false;
    for (const char* p = in; *p; p++) {
        char c = *p;
        if (c == ' ' || c == '-') continue;
        if (c == '=') { padSeen = true; continue; }
        if (padSeen) return -1;  // Daten nach Padding
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= '2' && c <= '7') v = c - '2' + 26;
        else return -1;
        buffer = (buffer << 5) | (uint32_t)v;
        bits += 5;
        if (bits >= 8) {
            if (o >= outCap) return -1;
            out[o++] = (uint8_t)((buffer >> (bits - 8)) & 0xFF);
            bits -= 8;
        }
    }
    return (int)o;
}

// ---------- HOTP/TOTP ----------
uint32_t hotpTruncate(const uint8_t mac[20], int digits) {
    const int off = mac[19] & 0x0F;
    const uint32_t bin = ((uint32_t)(mac[off] & 0x7F) << 24) | ((uint32_t)mac[off + 1] << 16) |
                         ((uint32_t)mac[off + 2] << 8) | (uint32_t)mac[off + 3];
    uint32_t mod = 1;
    for (int i = 0; i < digits; i++) mod *= 10;
    return bin % mod;
}

uint32_t hotp(HmacSha1Fn hmac, const uint8_t* key, size_t keyLen, uint64_t counter, int digits) {
    uint8_t msg[8];
    for (int i = 7; i >= 0; i--) { msg[i] = (uint8_t)(counter & 0xFF); counter >>= 8; }
    uint8_t mac[20];
    hmac(key, keyLen, msg, sizeof(msg), mac);
    const uint32_t r = hotpTruncate(mac, digits);
    memset(mac, 0, sizeof(mac));
    return r;
}

bool parseTotpCode(const char* s, uint32_t& code) {
    if (!s) return false;
    uint32_t v = 0;
    int n = 0;
    for (const char* p = s; *p; p++) {
        if (*p == ' ') continue;
        if (*p < '0' || *p > '9') return false;
        if (++n > kTotpDigits) return false;
        v = v * 10 + (uint32_t)(*p - '0');
    }
    if (n != kTotpDigits) return false;
    code = v;
    return true;
}

int64_t totpVerify(HmacSha1Fn hmac, const uint8_t* key, size_t keyLen, uint32_t code,
                   int64_t unixTime, int64_t lastUsedStep, int window) {
    const int64_t step = totpStep(unixTime);
    if (step < 0) return -1;
    int64_t matched = -1;
    for (int64_t s = step - window; s <= step + window; s++) {
        if (s < 0) continue;
        const uint32_t c = hotp(hmac, key, keyLen, (uint64_t)s, kTotpDigits);
        const bool eq = constTimeEq((const uint8_t*)&c, (const uint8_t*)&code, sizeof(c));
        if (eq && s > lastUsedStep && matched < 0) matched = s;
    }
    return matched;
}

// ---------- Wiederherstellungscodes ----------
const char kRecoveryAlphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
static constexpr unsigned kAlphaLen = sizeof(kRecoveryAlphabet) - 1;  // 31

static bool inAlphabet(char c) {
    for (unsigned i = 0; i < kAlphaLen; i++) if (kRecoveryAlphabet[i] == c) return true;
    return false;
}

void generateRecoveryCode(RandomFn rnd, char out[kRecoveryCodeLen + 1]) {
    // Rejection sampling: nur Bytes < 248 (= 8*31) verwenden, damit gleichverteilt.
    const uint8_t limit = (uint8_t)(256 - (256 % kAlphaLen));
    size_t pos = 0;
    uint8_t buf[16];
    while (pos < 8) {
        rnd(buf, sizeof(buf));
        for (size_t i = 0; i < sizeof(buf) && pos < 8; i++) {
            if (buf[i] >= limit) continue;
            const char c = kRecoveryAlphabet[buf[i] % kAlphaLen];
            out[pos < 4 ? pos : pos + 1] = c;
            pos++;
        }
    }
    out[4] = '-';
    out[kRecoveryCodeLen] = '\0';
    memset(buf, 0, sizeof(buf));
}

bool normalizeRecoveryCode(const char* in, char out[kRecoveryCodeLen + 1]) {
    if (!in) return false;
    char raw[8];
    size_t n = 0;
    int hyphens = 0;
    for (const char* p = in; *p; p++) {
        char c = *p;
        if (c == ' ') continue;
        if (c == '-') {
            if (n != 4 || ++hyphens > 1) return false;
            continue;
        }
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (!inAlphabet(c) || n >= 8) return false;
        raw[n++] = c;
    }
    if (n != 8) return false;
    memcpy(out, raw, 4);
    out[4] = '-';
    memcpy(out + 5, raw + 4, 4);
    out[kRecoveryCodeLen] = '\0';
    return true;
}

// ---------- Eingabeprüfung ----------
bool validUsername(const char* s) {
    if (!s) return false;
    size_t n = 0;
    for (const char* p = s; *p; p++, n++) {
        const char c = *p;
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok || n >= 32) return false;
    }
    return n >= 3 && n <= 32;
}

bool validPassword(const char* s) {
    if (!s) return false;
    size_t cps = 0, bytes = 0;
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        if (++bytes > 512) return false;
        if (*p < 0x20 || *p == 0x7F) return false;   // Steuerzeichen
        if ((*p & 0xC0) != 0x80) cps++;              // kein UTF-8-Folgebyte
    }
    return cps >= 10 && cps <= 128;
}

// ---------- HTTP-Hilfen ----------
bool findCookie(const char* header, const char* name, char* out, size_t outCap) {
    if (!header || !name || !out || outCap == 0) return false;
    const size_t nameLen = strlen(name);
    const char* p = header;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ';') p++;
        if (!*p) break;
        const char* start = p;
        while (*p && *p != ';') p++;
        const char* end = p;  // [start, end) = "name=value"
        const char* eq = (const char*)memchr(start, '=', (size_t)(end - start));
        if (eq) {
            const char* ne = eq;
            while (ne > start && (ne[-1] == ' ' || ne[-1] == '\t')) ne--;
            if ((size_t)(ne - start) == nameLen && memcmp(start, name, nameLen) == 0) {
                const char* v = eq + 1;
                while (v < end && *v == ' ') v++;
                const char* ve = end;
                while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
                if (ve - v >= 2 && *v == '"' && ve[-1] == '"') { v++; ve--; }
                const size_t len = (size_t)(ve - v);
                if (len + 1 > outCap) return false;
                memcpy(out, v, len);
                out[len] = '\0';
                return true;
            }
        }
    }
    return false;
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

// Vergleicht Authority a (Länge al) mit b (bl), Groß/klein egal, ":<defaultPort>" optional.
static bool authorityEq(const char* a, size_t al, const char* b, size_t bl, const char* defPort) {
    auto strip = [defPort](const char* s, size_t& l) {
        const size_t dl = strlen(defPort);
        if (l > dl + 1 && s[l - dl - 1] == ':' && memcmp(s + l - dl, defPort, dl) == 0) l -= dl + 1;
    };
    strip(a, al);
    strip(b, bl);
    if (al != bl || al == 0) return false;
    for (size_t i = 0; i < al; i++) if (lower(a[i]) != lower(b[i])) return false;
    return true;
}

bool originMatchesHost(const char* origin, const char* host) {
    if (!origin || !host) return false;
    const char* defPort;
    const char* rest;
    if (strncmp(origin, "http://", 7) == 0) { rest = origin + 7; defPort = "80"; }
    else if (strncmp(origin, "https://", 8) == 0) { rest = origin + 8; defPort = "443"; }
    else return false;  // u. a. "null"
    size_t rl = strlen(rest);
    if (rl && rest[rl - 1] == '/') rl--;
    for (size_t i = 0; i < rl; i++) if (rest[i] == '/' || rest[i] == '@') return false;
    size_t hl = strlen(host);
    while (hl && host[hl - 1] == ' ') hl--;
    while (*host == ' ') { host++; if (hl) hl--; }
    return authorityEq(rest, rl, host, hl, defPort);
}

// ---------- Rate-Limit ----------
uint32_t lockDurationS(uint8_t lockNumber) {
    if (lockNumber == 0) return 0;
    uint32_t d = RateLimiter::kBaseLockS;
    for (uint8_t i = 1; i < lockNumber && d < RateLimiter::kMaxLockS; i++) d *= 2;
    return d > RateLimiter::kMaxLockS ? RateLimiter::kMaxLockS : d;
}

void RateLimiter::decay(Entry& e, uint32_t now) {
    if (!e.used) return;
    if (remaining(e, now) == 0 && now - e.lastFail >= kDecayS) {
        e.failures = 0;
        e.locks = 0;
    }
}

uint32_t RateLimiter::remaining(const Entry& e, uint32_t now) {
    if (!e.used || e.lockedUntil == 0) return 0;
    const int32_t d = (int32_t)(e.lockedUntil - now);
    return d > 0 ? (uint32_t)d : 0;
}

void RateLimiter::registerFail(Entry& e, uint32_t now) {
    decay(e, now);
    e.used = true;
    e.lastFail = now;
    if (remaining(e, now) > 0) return;  // während Sperre zählt nichts zusätzlich
    if (++e.failures >= kMaxFailures) {
        e.failures = 0;
        if (e.locks < 255) e.locks++;
        e.lockedUntil = now + lockDurationS(e.locks);
        if (e.lockedUntil == 0) e.lockedUntil = 1;
    }
}

RateLimiter::Entry* RateLimiter::find(uint32_t key) {
    for (auto& s : slots_) if (s.used && s.key == key) return &s;
    return nullptr;
}
const RateLimiter::Entry* RateLimiter::find(uint32_t key) const {
    for (auto& s : slots_) if (s.used && s.key == key) return &s;
    return nullptr;
}

uint32_t RateLimiter::retryAfter(uint32_t clientKey, uint32_t now) const {
    uint32_t r = remaining(global_, now);
    if (const Entry* e = find(clientKey)) {
        const uint32_t c = remaining(*e, now);
        if (c > r) r = c;
    }
    return r;
}

uint32_t RateLimiter::fail(uint32_t clientKey, uint32_t now) {
    Entry* e = find(clientKey);
    if (!e) {
        // Freien Platz oder den am längsten ungenutzten, nicht gesperrten nehmen.
        Entry* victim = nullptr;
        for (auto& s : slots_) if (!s.used) { victim = &s; break; }
        if (!victim) {
            for (auto& s : slots_) {
                if (remaining(s, now) > 0) continue;
                if (!victim || (int32_t)(s.lastFail - victim->lastFail) < 0) victim = &s;
            }
        }
        if (!victim) {  // alle gesperrt: ältesten nehmen
            victim = &slots_[0];
            for (auto& s : slots_) if ((int32_t)(s.lastFail - victim->lastFail) < 0) victim = &s;
        }
        *victim = Entry{};
        victim->key = clientKey;
        victim->used = true;
        e = victim;
    }
    registerFail(*e, now);
    global_.used = true;
    registerFail(global_, now);
    return retryAfter(clientKey, now);
}

void RateLimiter::success(uint32_t clientKey) {
    if (Entry* e = find(clientKey)) *e = Entry{};
    // Globaler Zähler: offene Fehlversuche zurücksetzen, Sperrstufe bleibt bis Verfall.
    global_.failures = 0;
}

void RateLimiter::reset() {
    for (auto& s : slots_) s = Entry{};
    global_ = Entry{};
}

// ---------- Sitzungen ----------
int SessionTable::create(const uint8_t token[kTokenLen], uint32_t now) {
    int slot = -1;
    for (int i = 0; i < kMax; i++) {
        if (!valid(s_[i], now)) { slot = i; break; }
    }
    if (slot < 0) {
        slot = 0;
        for (int i = 1; i < kMax; i++)
            if ((int32_t)(s_[i].lastSeen - s_[slot].lastSeen) < 0) slot = i;
    }
    memcpy(s_[slot].token, token, kTokenLen);
    s_[slot].lastSeen = now;
    s_[slot].issued = now;
    s_[slot].used = true;
    return slot;
}

int SessionTable::find(const uint8_t token[kTokenLen], uint32_t now) const {
    int found = -1;
    for (int i = 0; i < kMax; i++) {
        const bool eq = constTimeEq(s_[i].token, token, kTokenLen);
        if (eq && valid(s_[i], now) && found < 0) found = i;
    }
    return found;
}

int SessionTable::touch(const uint8_t token[kTokenLen], uint32_t now) {
    for (auto& s : s_) if (s.used && !valid(s, now)) { memset(&s, 0, sizeof(s)); }
    const int i = find(token, now);
    if (i >= 0) s_[i].lastSeen = now;
    return i;
}

void SessionTable::remove(int slot) {
    if (slot >= 0 && slot < kMax) memset(&s_[slot], 0, sizeof(Slot));
}

void SessionTable::removeAll() {
    memset(s_, 0, sizeof(s_));
}

void SessionTable::removeAllExcept(int slot) {
    for (int i = 0; i < kMax; i++) if (i != slot) remove(i);
}

int SessionTable::count(uint32_t now) const {
    int n = 0;
    for (auto& s : s_) if (valid(s, now)) n++;
    return n;
}

}  // namespace auth
