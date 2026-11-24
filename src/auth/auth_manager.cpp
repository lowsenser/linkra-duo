#include "auth_manager.h"
#include "../network/wifi_manager.h"

#include <Preferences.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/sha256.h>
#include <mbedtls/platform_util.h>
#include <sys/time.h>

static const char* kNs = "auth";
// NVS-Schlüssel (max. 15 Zeichen)
static const char* kKeyUser = "user";
static const char* kKeySalt = "salt";
static const char* kKeyHash = "hash";
static const char* kKeyIter = "iter";
static const char* kKeyTotpSec = "totp_sec";
static const char* kKeyTotpOn = "totp_on";
static const char* kKeyTotpLast = "totp_last";
static const char* kKeyRc = "rc";

// ---------- Krypto-Primitive (mbedTLS) ----------
static void hmacSha1(const uint8_t* key, size_t keyLen, const uint8_t* msg, size_t msgLen, uint8_t out[20]) {
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), key, keyLen, msg, msgLen, out);
}

static void fillRandom(uint8_t* buf, size_t len) { esp_fill_random(buf, len); }

static void sha256(const uint8_t* in, size_t len, uint8_t out[32]) {
    mbedtls_sha256(in, len, out, 0);
}

static bool pbkdf2(const char* password, const uint8_t* salt, uint32_t iter, uint8_t out[32]) {
    const int64_t t0 = esp_timer_get_time();
    const int rc = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, (const unsigned char*)password,
                                                 strlen(password), salt, AuthManager::kSaltLen,
                                                 iter, 32, out);
    Serial.printf("[Auth] PBKDF2 %u it: %u ms\n", (unsigned)iter,
                  (unsigned)((esp_timer_get_time() - t0) / 1000));
    return rc == 0;
}

// ---------- Singleton ----------
AuthManager& AuthManager::get() {
    static AuthManager instance;
    return instance;
}

AuthManager::AuthManager() : mtx_(xSemaphoreCreateMutex()) {}

void AuthManager::lock() { xSemaphoreTake(mtx_, portMAX_DELAY); }
void AuthManager::unlock() { xSemaphoreGive(mtx_); }

uint32_t AuthManager::nowS() { return (uint32_t)(esp_timer_get_time() / 1000000LL); }

bool AuthManager::timeSynced() { return WiFiManager::isTimeSynced(); }

void AuthManager::clearRam() {
    exists_ = false;
    mbedtls_platform_zeroize(user_, sizeof(user_));
    mbedtls_platform_zeroize(salt_, sizeof(salt_));
    mbedtls_platform_zeroize(hash_, sizeof(hash_));
    iter_ = 0;
    totpOn_ = false;
    mbedtls_platform_zeroize(totpSecret_, sizeof(totpSecret_));
    totpLast_ = -1;
    mbedtls_platform_zeroize(rc_, sizeof(rc_));
    pendingValid_ = false;
    mbedtls_platform_zeroize(pending_, sizeof(pending_));
}

void AuthManager::begin() {
    lock();
    clearRam();
    Preferences p;
    if (p.begin(kNs, true)) {
        const String u = p.getString(kKeyUser, "");
        const bool ok = u.length() >= 3 && u.length() <= 32 &&
                        p.getBytes(kKeySalt, salt_, kSaltLen) == kSaltLen &&
                        p.getBytes(kKeyHash, hash_, kHashLen) == kHashLen;
        iter_ = p.getUInt(kKeyIter, 0);
        if (ok && iter_ > 0) {
            strlcpy(user_, u.c_str(), sizeof(user_));
            exists_ = true;
            totpOn_ = p.getUChar(kKeyTotpOn, 0) == 1 &&
                      p.getBytes(kKeyTotpSec, totpSecret_, kTotpSecretLen) == kTotpSecretLen;
            totpLast_ = p.getLong64(kKeyTotpLast, -1);
            if (p.getBytes(kKeyRc, rc_, sizeof(rc_)) != sizeof(rc_)) memset(rc_, 0, sizeof(rc_));
        } else {
            clearRam();
        }
        p.end();
    }
    loaded_ = true;
    Serial.printf("[Auth] %s, 2FA %s\n", exists_ ? "Konto vorhanden" : "Einrichtung erforderlich",
                  totpOn_ ? "aktiv" : "aus");
    unlock();
}

bool AuthManager::accountExists() { lock(); bool r = exists_; unlock(); return r; }
bool AuthManager::totpEnabled() { lock(); bool r = exists_ && totpOn_; unlock(); return r; }
String AuthManager::username() { lock(); String r(user_); unlock(); return r; }

// ---------- Sitzungen ----------
void AuthManager::createSession(char tokenHex[kTokenHexLen + 1]) {
    uint8_t tok[auth::SessionTable::kTokenLen];
    esp_fill_random(tok, sizeof(tok));
    lock();
    sessions_.create(tok, nowS());
    unlock();
    auth::hexEncode(tok, sizeof(tok), tokenHex);
    mbedtls_platform_zeroize(tok, sizeof(tok));
}

int AuthManager::sessionFromCookie(const char* cookieHeader, bool touch, bool* needsCookieRefresh) {
    if (needsCookieRefresh) *needsCookieRefresh = false;
    if (!cookieHeader) return -1;
    char hex[kTokenHexLen + 2];
    if (!auth::findCookie(cookieHeader, "linkra_session", hex, sizeof(hex))) return -1;
    uint8_t tok[auth::SessionTable::kTokenLen];
    if (!auth::hexDecode(hex, strlen(hex), tok, sizeof(tok))) return -1;
    lock();
    const uint32_t now = nowS();
    int slot = -1;
    if (exists_) {
        slot = touch ? sessions_.touch(tok, now) : sessions_.find(tok, now);
        if (slot >= 0 && touch && needsCookieRefresh && now - sessions_.issuedAt(slot) >= kCookieRefreshS) {
            sessions_.setIssued(slot, now);
            *needsCookieRefresh = true;
        }
    }
    unlock();
    mbedtls_platform_zeroize(tok, sizeof(tok));
    return slot;
}

void AuthManager::endSession(int slot) { lock(); sessions_.remove(slot); unlock(); }
void AuthManager::endAllSessions() { lock(); sessions_.removeAll(); unlock(); }
void AuthManager::endAllSessionsExcept(int slot) { lock(); sessions_.removeAllExcept(slot); unlock(); }

// ---------- Passwort ----------
bool AuthManager::verifyPasswordLocked(const char* password) {
    if (!password) password = "";
    uint8_t calc[kHashLen];
    // Ohne Konto trotzdem rechnen (gleiche Laufzeit), Ergebnis verwerfen.
    const bool ok = pbkdf2(password, salt_, iter_ ? iter_ : kPbkdf2Iterations, calc);
    const bool eq = auth::constTimeEq(calc, hash_, kHashLen);
    mbedtls_platform_zeroize(calc, sizeof(calc));
    return ok && exists_ && eq;
}

bool AuthManager::storePasswordLocked(const char* password) {
    uint8_t salt[kSaltLen], hash[kHashLen];
    esp_fill_random(salt, sizeof(salt));
    if (!pbkdf2(password, salt, kPbkdf2Iterations, hash)) return false;
    Preferences p;
    bool ok = p.begin(kNs, false);
    if (ok) {
        ok = p.putBytes(kKeySalt, salt, kSaltLen) == kSaltLen &&
             p.putBytes(kKeyHash, hash, kHashLen) == kHashLen &&
             p.putUInt(kKeyIter, kPbkdf2Iterations) == sizeof(uint32_t);
        p.end();
    }
    if (ok) {
        memcpy(salt_, salt, kSaltLen);
        memcpy(hash_, hash, kHashLen);
        iter_ = kPbkdf2Iterations;
    }
    mbedtls_platform_zeroize(hash, sizeof(hash));
    return ok;
}

// ---------- TOTP / Wiederherstellungscodes ----------
bool AuthManager::checkTotpLocked(const char* code, bool& timeErr) {
    timeErr = false;
    uint32_t c;
    if (!auth::parseTotpCode(code, c)) return false;
    if (!timeSynced()) { timeErr = true; return false; }
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    const int64_t step = auth::totpVerify(hmacSha1, totpSecret_, kTotpSecretLen, c, tv.tv_sec, totpLast_);
    if (step < 0) return false;
    totpLast_ = step;
    Preferences p;
    if (p.begin(kNs, false)) { p.putLong64(kKeyTotpLast, totpLast_); p.end(); }
    return true;
}

bool AuthManager::consumeRecoveryLocked(const char* code) {
    char norm[auth::kRecoveryCodeLen + 1];
    if (!auth::normalizeRecoveryCode(code, norm)) return false;
    uint8_t h[32];
    sha256((const uint8_t*)norm, auth::kRecoveryCodeLen, h);
    static const uint8_t zero[32] = {0};
    int hit = -1;
    for (int i = 0; i < kRecoveryCount; i++) {
        const bool eq = auth::constTimeEq(rc_[i], h, 32);
        const bool used = auth::constTimeEq(rc_[i], zero, 32);
        if (eq && !used && hit < 0) hit = i;
    }
    mbedtls_platform_zeroize(h, sizeof(h));
    if (hit < 0) return false;
    memset(rc_[hit], 0, 32);
    Preferences p;
    if (p.begin(kNs, false)) { p.putBytes(kKeyRc, rc_, sizeof(rc_)); p.end(); }
    int left = 0;
    for (int i = 0; i < kRecoveryCount; i++) if (!auth::constTimeEq(rc_[i], zero, 32)) left++;
    Serial.printf("[Auth] Wiederherstellungscode verwendet (%d übrig)\n", left);
    return true;
}

bool AuthManager::saveTotpLocked() {
    Preferences p;
    if (!p.begin(kNs, false)) return false;
    bool ok = p.putUChar(kKeyTotpOn, totpOn_ ? 1 : 0) == 1;
    if (totpOn_) {
        ok = ok && p.putBytes(kKeyTotpSec, totpSecret_, kTotpSecretLen) == kTotpSecretLen;
        ok = ok && p.putLong64(kKeyTotpLast, totpLast_) == sizeof(int64_t);
        ok = ok && p.putBytes(kKeyRc, rc_, sizeof(rc_)) == sizeof(rc_);
    } else {
        p.remove(kKeyTotpSec);
        p.remove(kKeyTotpLast);
        p.remove(kKeyRc);
    }
    p.end();
    return ok;
}

// ---------- Abläufe ----------
AuthManager::Result AuthManager::setup(const char* user, const char* password) {
    if (!auth::validUsername(user) || !auth::validPassword(password)) {
        lock();
        const bool e = exists_;
        unlock();
        return e ? Result::Exists : Result::Invalid;
    }
    lock();
    if (exists_) { unlock(); return Result::Exists; }
    Preferences p;
    bool ok = p.begin(kNs, false);
    if (ok) {
        p.clear();
        ok = p.putString(kKeyUser, user) == strlen(user);
        p.end();
    }
    ok = ok && storePasswordLocked(password);
    if (ok) {
        strlcpy(user_, user, sizeof(user_));
        exists_ = true;
        totpOn_ = false;
        totpLast_ = -1;
        memset(rc_, 0, sizeof(rc_));
        sessions_.removeAll();
        limiter_.reset();
        Serial.printf("[Auth] Konto '%s' eingerichtet\n", user_);
    } else {
        Preferences q;
        if (q.begin(kNs, false)) { q.clear(); q.end(); }
        clearRam();
    }
    unlock();
    return ok ? Result::Ok : Result::StorageError;
}

AuthManager::Result AuthManager::login(uint32_t clientKey, const char* user, const char* password,
                                       const char* totp, const char* recoveryCode, uint32_t& retryAfter) {
    retryAfter = 0;
    lock();
    const uint32_t now = nowS();
    if ((retryAfter = limiter_.retryAfter(clientKey, now)) > 0) { unlock(); return Result::Locked; }
    if (!exists_) { unlock(); return Result::SetupRequired; }

    // Benutzername in konstanter Zeit vergleichen (auf 33 Bytes aufgefüllt).
    char given[33] = {};
    if (user) strncpy(given, user, sizeof(given) - 1);
    const bool userOk = auth::constTimeEq((const uint8_t*)given, (const uint8_t*)user_, sizeof(given)) &&
                        user && strlen(user) < sizeof(given);
    const bool pwOk = verifyPasswordLocked(password);  // immer rechnen
    if (!(userOk && pwOk)) {
        limiter_.fail(clientKey, now);
        unlock();
        Serial.println("[Auth] Login fehlgeschlagen");
        return Result::InvalidCredentials;
    }

    if (totpOn_) {
        if (recoveryCode && *recoveryCode) {
            if (!consumeRecoveryLocked(recoveryCode)) {
                limiter_.fail(clientKey, now);
                unlock();
                return Result::TotpInvalid;
            }
        } else if (!totp || !*totp) {
            unlock();
            return Result::TotpRequired;
        } else {
            bool timeErr;
            if (!checkTotpLocked(totp, timeErr)) {
                if (timeErr) { unlock(); return Result::TimeNotSynced; }
                limiter_.fail(clientKey, now);
                unlock();
                return Result::TotpInvalid;
            }
        }
    }

    limiter_.success(clientKey);
    if (iter_ != kPbkdf2Iterations) {
        Serial.printf("[Auth] Rehash %u -> %u Iterationen\n", (unsigned)iter_, (unsigned)kPbkdf2Iterations);
        storePasswordLocked(password);
    }
    unlock();
    Serial.println("[Auth] Login erfolgreich");
    return Result::Ok;
}

AuthManager::Result AuthManager::changePassword(uint32_t clientKey, const char* current, const char* next,
                                                uint32_t& retryAfter) {
    retryAfter = 0;
    if (!auth::validPassword(next)) return Result::Invalid;
    lock();
    const uint32_t now = nowS();
    if ((retryAfter = limiter_.retryAfter(clientKey, now)) > 0) { unlock(); return Result::Locked; }
    if (!verifyPasswordLocked(current)) {
        limiter_.fail(clientKey, now);
        unlock();
        return Result::InvalidCredentials;
    }
    limiter_.success(clientKey);
    const bool ok = storePasswordLocked(next);
    unlock();
    if (ok) Serial.println("[Auth] Passwort geändert");
    return ok ? Result::Ok : Result::StorageError;
}

AuthManager::Result AuthManager::totpSetup(char* secretB32, size_t cap) {
    lock();
    esp_fill_random(pending_, kTotpSecretLen);
    pendingValid_ = true;
    const size_t n = auth::base32Encode(pending_, kTotpSecretLen, secretB32, cap, false);
    unlock();
    return n ? Result::Ok : Result::Invalid;
}

AuthManager::Result AuthManager::totpEnable(const char* code,
                                            char codes[kRecoveryCount][auth::kRecoveryCodeLen + 1]) {
    uint32_t c;
    lock();
    if (!pendingValid_) { unlock(); return Result::NoPending; }
    if (!auth::parseTotpCode(code, c)) { unlock(); return Result::TotpInvalid; }
    if (!timeSynced()) { unlock(); return Result::TimeNotSynced; }
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    const int64_t step = auth::totpVerify(hmacSha1, pending_, kTotpSecretLen, c, tv.tv_sec, -1);
    if (step < 0) { unlock(); return Result::TotpInvalid; }

    memcpy(totpSecret_, pending_, kTotpSecretLen);
    mbedtls_platform_zeroize(pending_, sizeof(pending_));
    pendingValid_ = false;
    totpOn_ = true;
    totpLast_ = step;
    for (int i = 0; i < kRecoveryCount; i++) {
        auth::generateRecoveryCode(fillRandom, codes[i]);
        sha256((const uint8_t*)codes[i], auth::kRecoveryCodeLen, rc_[i]);
    }
    const bool ok = saveTotpLocked();
    unlock();
    Serial.println(ok ? "[Auth] 2FA aktiviert" : "[Auth] 2FA: Speichern fehlgeschlagen");
    return ok ? Result::Ok : Result::StorageError;
}

AuthManager::Result AuthManager::totpDisable(uint32_t clientKey, const char* password, const char* code,
                                             const char* recoveryCode, uint32_t& retryAfter) {
    retryAfter = 0;
    lock();
    const uint32_t now = nowS();
    if ((retryAfter = limiter_.retryAfter(clientKey, now)) > 0) { unlock(); return Result::Locked; }
    if (!verifyPasswordLocked(password)) {
        limiter_.fail(clientKey, now);
        unlock();
        return Result::InvalidCredentials;
    }
    if (totpOn_) {
        if (recoveryCode && *recoveryCode) {
            if (!consumeRecoveryLocked(recoveryCode)) {
                limiter_.fail(clientKey, now);
                unlock();
                return Result::TotpInvalid;
            }
        } else if (!code || !*code) {
            unlock();
            return Result::TotpRequired;
        } else {
            bool timeErr;
            if (!checkTotpLocked(code, timeErr)) {
                if (timeErr) { unlock(); return Result::TimeNotSynced; }
                limiter_.fail(clientKey, now);
                unlock();
                return Result::TotpInvalid;
            }
        }
    }
    limiter_.success(clientKey);
    totpOn_ = false;
    totpLast_ = -1;
    mbedtls_platform_zeroize(totpSecret_, sizeof(totpSecret_));
    mbedtls_platform_zeroize(rc_, sizeof(rc_));
    pendingValid_ = false;
    mbedtls_platform_zeroize(pending_, sizeof(pending_));
    const bool ok = saveTotpLocked();
    unlock();
    Serial.println("[Auth] 2FA deaktiviert");
    return ok ? Result::Ok : Result::StorageError;
}

void AuthManager::wipe() {
    lock();
    Preferences p;
    if (p.begin(kNs, false)) { p.clear(); p.end(); }
    clearRam();
    sessions_.removeAll();
    limiter_.reset();
    unlock();
    Serial.println("[Auth] NVS 'auth' gelöscht – Konto neu einrichten (setup_required)");
}

// ---------- BOOT-Taste ----------
// GPIO0 ist ein Strapping-Pin: beim Einschalten gedrückt -> ROM-Download-Modus,
// die Firmware startet dann gar nicht. Deshalb wird die Taste NACH dem Start
// ausgewertet: innerhalb der ersten 60 s drücken und 10 s halten.
static void recoveryButtonTask(void*) {
    constexpr int kPin = 0;
    constexpr uint32_t kWindowMs = 60000, kHoldMs = 10000;
    pinMode(kPin, INPUT_PULLUP);
    const uint32_t start = millis();
    uint32_t pressedSince = 0;
    uint32_t lastLog = 0;
    while (pressedSince || millis() - start < kWindowMs) {
        const bool pressed = digitalRead(kPin) == LOW;
        const uint32_t now = millis();
        if (pressed) {
            if (!pressedSince) {
                pressedSince = now ? now : 1;
                lastLog = now;
                Serial.println("[Auth] BOOT-Taste gedrückt – 10 s halten, um die Anmeldedaten zu löschen");
            } else if (now - pressedSince >= kHoldMs) {
                Serial.println("[Auth] BOOT-Taste 10 s gehalten: Zugang wird zurückgesetzt");
                AuthManager::get().wipe();
                while (digitalRead(kPin) == LOW) vTaskDelay(pdMS_TO_TICKS(100));
                break;
            } else if (now - lastLog >= 1000) {
                lastLog = now;
                Serial.printf("[Auth] ... noch %u s\n", (unsigned)((kHoldMs - (now - pressedSince) + 999) / 1000));
            }
        } else if (pressedSince) {
            Serial.println("[Auth] BOOT-Taste losgelassen – abgebrochen");
            pressedSince = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelete(nullptr);
}

void AuthManager::startRecoveryButtonWatch() {
    xTaskCreate(recoveryButtonTask, "authBtn", 3072, nullptr, 1, nullptr);
}
