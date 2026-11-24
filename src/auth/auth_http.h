#pragma once
// HTTP-Seite der Anmeldung: Routen /api/auth/*, Middleware (Sitzung + CSRF)
// und Prüfhilfen für Upload-Callbacks. Vertrag: docs/auth-api.md.

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <functional>

namespace AuthHttp {

// Registriert alle /api/auth/*-Routen. onSessionsRevoked wird nach Logout,
// Logout-all und Passwortwechsel aufgerufen (z. B. SSE-Verbindungen schließen).
void registerRoutes(AsyncWebServer& server, std::function<void()> onSessionsRevoked);

// Server-Middleware für ALLE Anfragen (statische UI, /api/*, SSE):
//  - Pfade mit ".." / "//" / "\" -> 400
//  - Nicht GET/HEAD ohne "X-Linkra: 1" -> 403 csrf; Origin gesetzt und != Host -> 403 csrf
//  - Öffentlich: GET/HEAD /, /index.html, /assets/*; /api/auth/status|setup|login
//  - Übrige /api/* ohne gültige Sitzung -> 401 unauthorized; sonstige Pfade -> 404
void middleware(AsyncWebServerRequest* request, ArMiddlewareNext next);

// Für Body-/Upload-Callbacks, die VOR der Middleware laufen (ESPAsyncWebServer
// führt die Middleware erst nach dem vollständigen Body aus): true nur bei
// gültiger Sitzung und bestandener CSRF-Prüfung.
bool requestAuthorized(AsyncWebServerRequest* request);

void sendError(AsyncWebServerRequest* request, int status, const char* code, const char* message,
               uint32_t retryAfterS = 0);

}  // namespace AuthHttp
