#pragma once
// Reine Logik für Anmeldung und 2FA (ohne Arduino/ESP-IDF/mbedTLS).
// Wird auf dem Gerät und in den Host-Tests (tests/auth_core_test.cpp) gebaut.
// Kryptografische Primitive (HMAC-SHA1, Zufall) werden als Funktionszeiger
// hereingereicht: auf dem Gerät mbedTLS/esp_fill_random, im Test eine kleine
// Referenzimplementierung bzw. ein deterministischer Generator.

#include <cstddef>
#include <cstdint>

namespace auth {

using HmacSha1Fn = void (*)(const uint8_t* key, size_t keyLen,
                            const uint8_t* msg, size_t msgLen, uint8_t out[20]);
using RandomFn = void (*)(uint8_t* buf, size_t len);

// ---------- Hilfen ----------
// Vergleich in konstanter Zeit (Laufzeit hängt nur von len ab).
bool constTimeEq(const uint8_t* a, const uint8_t* b, size_t len);
// Hex in Kleinbuchstaben; out braucht 2*len+1 Bytes.
void hexEncode(const uint8_t* in, size_t len, char* out);
// Strikte Hex-Dekodierung genau outLen Bytes (2*outLen Zeichen). false bei Fehler.
bool hexDecode(const char* in, size_t inLen, uint8_t* out, size_t outLen);
// Prozent-Kodierung (RFC 3986, unreserved bleibt). Rückgabe: Länge oder 0 wenn zu klein.
size_t urlEncode(const char* in, char* out, size_t outCap);

// ---------- Base32 (RFC 4648) ----------
// Rückgabe: Anzahl geschriebener Zeichen (ohne NUL) oder 0, wenn outCap zu klein.
size_t base32Encode(const uint8_t* in, size_t len, char* out, size_t outCap, bool pad);
// Toleriert Kleinbuchstaben, '=' am Ende sowie Leerzeichen/Bindestriche.
// Rückgabe: Anzahl Bytes oder -1 bei ungültiger Eingabe/zu kleinem Puffer.
int base32Decode(const char* in, uint8_t* out, size_t outCap);

// ---------- HOTP/TOTP (RFC 4226 / RFC 6238, SHA1) ----------
constexpr int kTotpPeriod = 30;
constexpr int kTotpDigits = 6;
uint32_t hotpTruncate(const uint8_t mac[20], int digits);
uint32_t hotp(HmacSha1Fn hmac, const uint8_t* key, size_t keyLen, uint64_t counter, int digits);
inline int64_t totpStep(int64_t unixTime) { return unixTime < 0 ? -1 : unixTime / kTotpPeriod; }
// Genau 6 Ziffern; Leerzeichen werden ignoriert.
bool parseTotpCode(const char* s, uint32_t& code);
// Prüft code gegen Schritte step-window .. step+window. Schritte <= lastUsedStep
// werden abgelehnt (Replay-Schutz). Prüft immer alle Fenster (keine frühe Rückkehr).
// Rückgabe: akzeptierter Zeitschritt oder -1.
int64_t totpVerify(HmacSha1Fn hmac, const uint8_t* key, size_t keyLen, uint32_t code,
                   int64_t unixTime, int64_t lastUsedStep, int window = 1);

// ---------- Wiederherstellungscodes ----------
// Format "xxxx-xxxx", Alphabet ohne verwechselbare Zeichen (0 o 1 l i).
extern const char kRecoveryAlphabet[];      // 31 Zeichen
constexpr size_t kRecoveryCodeLen = 9;      // ohne NUL
void generateRecoveryCode(RandomFn rnd, char out[kRecoveryCodeLen + 1]);
// Kleinschreibung, Leerzeichen und Bindestrich optional. Ergebnis kanonisch "xxxx-xxxx".
bool normalizeRecoveryCode(const char* in, char out[kRecoveryCodeLen + 1]);

// ---------- Eingabeprüfung ----------
bool validUsername(const char* s);   // 3–32 Zeichen [A-Za-z0-9._-]
bool validPassword(const char* s);   // 10–128 Zeichen (UTF-8-Codepoints), keine Steuerzeichen

// ---------- HTTP-Hilfen ----------
// Sucht name=wert im Cookie-Header. false, wenn nicht vorhanden oder zu lang.
bool findCookie(const char* header, const char* name, char* out, size_t outCap);
// Origin "http://host[:port]" gegen Host-Header (Standardport 80/443 tolerant).
bool originMatchesHost(const char* origin, const char* host);

// ---------- Rate-Limit / Sperre ----------
// 5 Fehlversuche -> Sperre 30 s, jede weitere Sperre verdoppelt (max 900 s).
// Pro Client (Schlüssel = Hash der IP, 8 Plätze, ältester fliegt raus) und global.
// Zeit: monotone Sekunden. Nach 1 h ohne Fehlversuch verfällt der Zähler.
class RateLimiter {
public:
    static constexpr int kMaxFailures = 5;
    static constexpr uint32_t kBaseLockS = 30;
    static constexpr uint32_t kMaxLockS = 900;
    static constexpr uint32_t kDecayS = 3600;
    static constexpr int kSlots = 8;

    // 0 = erlaubt, sonst verbleibende Sperrzeit in Sekunden.
    uint32_t retryAfter(uint32_t clientKey, uint32_t now) const;
    // Fehlversuch zählen; Rückgabe wie retryAfter() danach.
    uint32_t fail(uint32_t clientKey, uint32_t now);
    void success(uint32_t clientKey);
    void reset();

    struct Entry {
        uint32_t key = 0;
        uint32_t lastFail = 0;
        uint32_t lockedUntil = 0;
        uint8_t failures = 0;
        uint8_t locks = 0;
        bool used = false;
    };
private:
    Entry slots_[kSlots];
    Entry global_;
    static void decay(Entry& e, uint32_t now);
    static void registerFail(Entry& e, uint32_t now);
    static uint32_t remaining(const Entry& e, uint32_t now);
    Entry* find(uint32_t key);
    const Entry* find(uint32_t key) const;
};
uint32_t lockDurationS(uint8_t lockNumber);  // 1 -> 30, 2 -> 60, ... max 900

// ---------- Sitzungen ----------
// Max. 8 Sitzungen im RAM, gleitend 7 Tage. Volle Tabelle: älteste (lastSeen) fliegt raus.
class SessionTable {
public:
    static constexpr int kMax = 8;
    static constexpr uint32_t kTtlS = 7u * 24u * 3600u;
    static constexpr size_t kTokenLen = 32;

    // Legt eine Sitzung an. Rückgabe: Slot-Index.
    int create(const uint8_t token[kTokenLen], uint32_t now);
    // Sucht gültige Sitzung (konstanter Vergleich über alle Slots) und verlängert sie.
    // Rückgabe: Slot oder -1. Abgelaufene werden entfernt.
    int touch(const uint8_t token[kTokenLen], uint32_t now);
    // Sucht ohne zu verlängern (für Status-Abfrage). -1 wenn ungültig.
    int find(const uint8_t token[kTokenLen], uint32_t now) const;
    void remove(int slot);
    void removeAll();
    void removeAllExcept(int slot);
    int count(uint32_t now) const;
    uint32_t issuedAt(int slot) const { return (slot >= 0 && slot < kMax) ? s_[slot].issued : 0; }
    void setIssued(int slot, uint32_t now) { if (slot >= 0 && slot < kMax) s_[slot].issued = now; }

private:
    struct Slot {
        uint8_t token[kTokenLen];
        uint32_t lastSeen;
        uint32_t issued;   // letzter Set-Cookie (für Erneuerung des Cookies)
        bool used;
    };
    Slot s_[kMax] = {};
    bool valid(const Slot& s, uint32_t now) const { return s.used && now - s.lastSeen < kTtlS; }
};

}  // namespace auth
