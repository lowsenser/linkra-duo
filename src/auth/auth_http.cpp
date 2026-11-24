#include "auth_http.h"
#include "auth_manager.h"

#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <mbedtls/platform_util.h>

namespace AuthHttp {

static const char* kCookieAttrs = "; Path=/; HttpOnly; SameSite=Strict; Max-Age=604800";
static const char* kCookieClear = "linkra_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0";

// ---------- Hilfen ----------
static uint32_t clientKey(AsyncWebServerRequest* r) {
    // FNV-1a über die Textform der Client-IP (IPv4 und IPv6).
    const String ip = r->client() ? r->client()->remoteIP().toString() : String("?");
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < ip.length(); i++) { h ^= (uint8_t)ip[i]; h *= 16777619u; }
    return h;
}

static const char* header(AsyncWebServerRequest* r, const char* name) {
    const AsyncWebHeader* h = r->getHeader(name);
    return h ? h->value().c_str() : nullptr;
}

static bool isSafeMethod(AsyncWebServerRequest* r) {
    return r->method() == HTTP_GET || r->method() == HTTP_HEAD;
}

static bool csrfOk(AsyncWebServerRequest* r) {
    if (!isSafeMethod(r)) {
        const char* x = header(r, "X-Linkra");
        if (!x || strcmp(x, "1") != 0) return false;
    }
    const char* origin = header(r, "Origin");
    if (origin) {
        const char* host = header(r, "Host");
        if (!host || !auth::originMatchesHost(origin, host)) return false;
    }
    return true;
}

static int sessionSlot(AsyncWebServerRequest* r, bool touch, bool* refresh = nullptr) {
    return AuthManager::get().sessionFromCookie(header(r, "Cookie"), touch, refresh);
}

void sendError(AsyncWebServerRequest* request, int status, const char* code, const char* message,
               uint32_t retryAfterS) {
    JsonDocument d;
    JsonObject e = d["error"].to<JsonObject>();
    e["code"] = code;
    e["message"] = message;
    if (retryAfterS) e["retry_after_s"] = retryAfterS;
    String out;
    serializeJson(d, out);
    AsyncWebServerResponse* resp = request->beginResponse(status, "application/json", out);
    if (retryAfterS) resp->addHeader("Retry-After", String(retryAfterS));
    resp->addHeader("Cache-Control", "no-store");
    request->send(resp);
}

static void sendJson(AsyncWebServerRequest* r, JsonDocument& d, const char* setCookie = nullptr) {
    String out;
    serializeJson(d, out);
    AsyncWebServerResponse* resp = r->beginResponse(200, "application/json", out);
    resp->addHeader("Cache-Control", "no-store");
    if (setCookie) resp->addHeader("Set-Cookie", setCookie);
    r->send(resp);
}

static void sendOk(AsyncWebServerRequest* r, const char* setCookie = nullptr) {
    JsonDocument d;
    d["ok"] = true;
    sendJson(r, d, setCookie);
}

static void fillStatus(JsonDocument& d, bool authenticated) {
    AuthManager& am = AuthManager::get();
    d["setup_required"] = !am.accountExists();
    d["authenticated"] = authenticated;
    if (authenticated) d["user"] = am.username();
    d["totp_enabled"] = am.totpEnabled();
    d["time_synced"] = AuthManager::timeSynced();
}

// Neue Sitzung + Cookie + Status-JSON.
static void sendLoggedIn(AsyncWebServerRequest* r) {
    char tok[AuthManager::kTokenHexLen + 1];
    AuthManager::get().createSession(tok);
    String cookie = String("linkra_session=") + tok + kCookieAttrs;
    mbedtls_platform_zeroize(tok, sizeof(tok));
    JsonDocument d;
    fillStatus(d, true);
    sendJson(r, d, cookie.c_str());
}

// Ergebnis eines Ablaufs auf HTTP abbilden (nur Fehlerfälle).
static void sendResult(AsyncWebServerRequest* r, AuthManager::Result res, uint32_t retryAfter,
                       int totpInvalidStatus = 401) {
    using R = AuthManager::Result;
    switch (res) {
        case R::Exists:             sendError(r, 409, "exists", "Es existiert bereits ein Konto"); break;
        case R::Invalid:            sendError(r, 400, "invalid", "Ungültige Eingabe (Benutzername 3–32 Zeichen A-Z a-z 0-9 . _ -, Passwort 10–128 Zeichen)"); break;
        case R::InvalidCredentials: sendError(r, 401, "invalid_credentials", "Benutzername oder Passwort falsch"); break;
        case R::TotpRequired:       sendError(r, 401, "totp_required", "Bestätigungscode erforderlich"); break;
        case R::TotpInvalid:        sendError(r, totpInvalidStatus, "totp_invalid", "Bestätigungscode ungültig"); break;
        case R::TimeNotSynced:      sendError(r, 503, "time_not_synced", "Gerätezeit nicht synchronisiert – Wiederherstellungscode verwenden oder später erneut versuchen"); break;
        case R::Locked:             sendError(r, 429, "locked", "Zu viele Fehlversuche – bitte warten", retryAfter ? retryAfter : 1); break;
        case R::NoPending:          sendError(r, 400, "totp_not_pending", "Zuerst ein neues Geheimnis erzeugen (totp/setup)"); break;
        case R::SetupRequired:      sendError(r, 409, "setup_required", "Noch kein Konto eingerichtet"); break;
        case R::StorageError:       sendError(r, 500, "storage", "Speichern fehlgeschlagen"); break;
        case R::Ok:                 sendOk(r); break;
    }
}

static const char* str(JsonVariant& json, const char* key) {
    JsonVariant v = json[key];
    return v.is<const char*>() ? v.as<const char*>() : nullptr;
}

// ---------- Middleware ----------
static bool isPublic(AsyncWebServerRequest* r, const String& url) {
    if (isSafeMethod(r) && (url == "/" || url == "/index.html" || url.startsWith("/assets/"))) return true;
    return url == "/api/auth/status" || url == "/api/auth/setup" || url == "/api/auth/login";
}

void middleware(AsyncWebServerRequest* request, ArMiddlewareNext next) {
    const String& url = request->url();
    if (url.indexOf("..") >= 0 || url.indexOf("//") >= 0 || url.indexOf('\\') >= 0) {
        request->send(400, "text/plain", "Bad Request");
        return;
    }
    if (!csrfOk(request)) {
        sendError(request, 403, "csrf", "Ungültige Anfrage");
        return;
    }
    if (isPublic(request, url)) {
        next();
        return;
    }
    if (!url.startsWith("/api/")) {
        request->send(404, "text/plain", "Not Found");
        return;
    }
    bool refresh = false;
    if (sessionSlot(request, true, &refresh) < 0) {
        sendError(request, 401, "unauthorized", "Anmeldung erforderlich");
        return;
    }
    next();
    // Gleitende Gültigkeit auch im Browser: Cookie höchstens einmal pro Tag erneuern.
    if (refresh && url != "/api/events" && !url.startsWith("/api/auth/")) {
        AsyncWebServerResponse* resp = request->getResponse();
        char tok[AuthManager::kTokenHexLen + 2];
        const char* c = header(request, "Cookie");
        if (resp && c && auth::findCookie(c, "linkra_session", tok, sizeof(tok))) {
            resp->addHeader("Set-Cookie", String("linkra_session=") + tok + kCookieAttrs);
        }
    }
}

bool requestAuthorized(AsyncWebServerRequest* request) {
    return csrfOk(request) && sessionSlot(request, false) >= 0;
}

// ---------- Routen ----------
void registerRoutes(AsyncWebServer& server, std::function<void()> onSessionsRevoked) {
    // Spezifische Pfade zuerst (Matcher: "/x" passt auch auf "/x/...").
    server.on("/api/auth/status", HTTP_GET, [](AsyncWebServerRequest* r) {
        JsonDocument d;
        fillStatus(d, sessionSlot(r, false) >= 0);
        sendJson(r, d);
    });

    server.on("/api/auth/setup", HTTP_POST, [](AsyncWebServerRequest* r, JsonVariant& json) {
        const auto res = AuthManager::get().setup(str(json, "username"), str(json, "password"));
        if (res != AuthManager::Result::Ok) { sendResult(r, res, 0); return; }
        sendLoggedIn(r);
    });

    server.on("/api/auth/login", HTTP_POST, [](AsyncWebServerRequest* r, JsonVariant& json) {
        uint32_t retry = 0;
        const auto res = AuthManager::get().login(clientKey(r), str(json, "username"), str(json, "password"),
                                                  str(json, "totp"), str(json, "recovery_code"), retry);
        if (res != AuthManager::Result::Ok) { sendResult(r, res, retry); return; }
        sendLoggedIn(r);
    });

    server.on("/api/auth/logout-all", HTTP_POST, [onSessionsRevoked](AsyncWebServerRequest* r) {
        AuthManager::get().endAllSessions();
        sendOk(r, kCookieClear);
        if (onSessionsRevoked) onSessionsRevoked();
    });

    server.on("/api/auth/logout", HTTP_POST, [onSessionsRevoked](AsyncWebServerRequest* r) {
        AuthManager::get().endSession(sessionSlot(r, false));
        sendOk(r, kCookieClear);
        if (onSessionsRevoked) onSessionsRevoked();
    });

    server.on("/api/auth/password", HTTP_POST, [onSessionsRevoked](AsyncWebServerRequest* r, JsonVariant& json) {
        uint32_t retry = 0;
        const auto res = AuthManager::get().changePassword(clientKey(r), str(json, "current"), str(json, "new"), retry);
        if (res != AuthManager::Result::Ok) { sendResult(r, res, retry); return; }
        AuthManager::get().endAllSessionsExcept(sessionSlot(r, false));
        sendOk(r);
        if (onSessionsRevoked) onSessionsRevoked();
    });

    server.on("/api/auth/totp/setup", HTTP_POST, [](AsyncWebServerRequest* r) {
        char secret[40];
        if (AuthManager::get().totpSetup(secret, sizeof(secret)) != AuthManager::Result::Ok) {
            sendError(r, 500, "internal", "Geheimnis konnte nicht erzeugt werden");
            return;
        }
        char label[100];
        const String user = AuthManager::get().username();
        if (!auth::urlEncode(user.c_str(), label, sizeof(label))) label[0] = '\0';
        JsonDocument d;
        d["secret"] = secret;
        d["otpauth_uri"] = String("otpauth://totp/Linkra%20Duo:") + label + "?secret=" + secret +
                           "&issuer=Linkra%20Duo&algorithm=SHA1&digits=6&period=30";
        mbedtls_platform_zeroize(secret, sizeof(secret));
        sendJson(r, d);
    });

    server.on("/api/auth/totp/enable", HTTP_POST, [](AsyncWebServerRequest* r, JsonVariant& json) {
        char codes[AuthManager::kRecoveryCount][auth::kRecoveryCodeLen + 1];
        const auto res = AuthManager::get().totpEnable(str(json, "code"), codes);
        if (res != AuthManager::Result::Ok) { sendResult(r, res, 0, 400); return; }
        JsonDocument d;
        JsonArray a = d["recovery_codes"].to<JsonArray>();
        for (auto& c : codes) a.add(c);
        mbedtls_platform_zeroize(codes, sizeof(codes));
        sendJson(r, d);
    });

    server.on("/api/auth/totp/disable", HTTP_POST, [](AsyncWebServerRequest* r, JsonVariant& json) {
        uint32_t retry = 0;
        const auto res = AuthManager::get().totpDisable(clientKey(r), str(json, "password"), str(json, "code"),
                                                        str(json, "recovery_code"), retry);
        sendResult(r, res, retry);
    });
}

}  // namespace AuthHttp
