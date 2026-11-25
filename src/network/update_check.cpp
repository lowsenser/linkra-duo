#include "update_check.h"

#include "../core/update_logic.h"
#include "../core/version.h"
#include "../integration/ha_discovery.h"
#include "../integration/mqtt_bridge.h"
#include "wifi_manager.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <Update.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <esp_ota_ops.h>
#include <esp_timer.h>
#include <mbedtls/sha256.h>
#include <time.h>

namespace {

constexpr uint32_t kFirstCheckMs = 3UL * 60 * 1000;          // after boot
constexpr uint32_t kIntervalMs = 24UL * 60 * 60 * 1000;      // daily
constexpr uint32_t kRetryMs = 2UL * 60 * 60 * 1000;          // after a failed check
constexpr size_t kApiBufMax = 96 * 1024;                     // release list (PSRAM)
constexpr size_t kMinInternalHeap = 40 * 1024;               // task stack 10 KB + lwIP/WiFi; TLS buffers are in PSRAM (main.cpp)
constexpr uint32_t kBitCheck = 1, kBitInstall = 2;

const char* kUserAgent = "Linkra-Duo/" LINKRA_FW_VERSION;

struct Buf {
    char* data = nullptr;
    size_t len = 0, cap = 0;
    bool overflow = false;
};

esp_err_t onHttp(esp_http_client_event_t* e) {
    auto* b = static_cast<Buf*>(e->user_data);
    if (e->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    // with automatic redirects the 302 body arrives first: keep only the final answer
    const int st = esp_http_client_get_status_code(e->client);
    if (st >= 300 && st < 400) return ESP_OK;
    if (b->len + e->data_len > b->cap) { b->overflow = true; return ESP_OK; }
    memcpy(b->data + b->len, e->data, e->data_len);
    b->len += e->data_len;
    return ESP_OK;
}

esp_http_client_config_t httpConfig(const char* url) {
    esp_http_client_config_t c = {};
    c.url = url;
    c.crt_bundle_attach = esp_crt_bundle_attach;
    c.timeout_ms = 20000;
    c.buffer_size = 4096;        // GitHub's redirect "Location" headers are long
    c.buffer_size_tx = 2048;     // ... and so is the request line after the redirect
    c.user_agent = kUserAgent;
    c.max_redirection_count = 5;
    c.keep_alive_enable = false;
    return c;
}

// GET into b (PSRAM). Returns the HTTP status, or -1 with err on transport errors.
int httpGet(const char* url, Buf& b, bool githubApi, String& err) {
    b.len = 0;
    b.overflow = false;
    esp_http_client_config_t c = httpConfig(url);
    c.event_handler = onHttp;
    c.user_data = &b;
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) { err = "HTTP-Client konnte nicht starten"; return -1; }
    if (githubApi) {
        esp_http_client_set_header(h, "Accept", "application/vnd.github+json");
        esp_http_client_set_header(h, "X-GitHub-Api-Version", "2022-11-28");
    }
    const esp_err_t r = esp_http_client_perform(h);
    const int status = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);
    if (r != ESP_OK) { err = String("Verbindung fehlgeschlagen (") + esp_err_to_name(r) + ")"; return -1; }
    if (b.overflow) { err = "Antwort zu groß"; return -1; }
    return status;
}

bool enoughHeap(String& err) {
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >= kMinInternalHeap &&
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >= 12 * 1024)
        return true;
    err = "Zu wenig freier Arbeitsspeicher, später erneut";
    return false;
}

const char* stateName(UpdateCheck::State s) {
    switch (s) {
    case UpdateCheck::State::Idle: return "idle";
    case UpdateCheck::State::Checking: return "checking";
    case UpdateCheck::State::UpToDate: return "up_to_date";
    case UpdateCheck::State::Available: return "available";
    case UpdateCheck::State::Downloading: return "downloading";
    case UpdateCheck::State::Installing: return "installing";
    case UpdateCheck::State::Error: return "error";
    }
    return "idle";
}

}  // namespace

UpdateCheck& UpdateCheck::get() {
    static UpdateCheck inst;
    return inst;
}

void UpdateCheck::begin() {
    Preferences p;
    if (p.begin("update", true)) {
        autoCheck_ = p.getBool("auto", true);
        haInstall_ = p.getBool("ha_inst", false);
        p.end();
    }
    // The worker task (10 KB internal stack for TLS) exists only while it works;
    // a 60-s timer decides when the daily check is due.
    if (!timer_) {
        esp_timer_create_args_t a = {};
        a.callback = [](void* arg) { static_cast<UpdateCheck*>(arg)->onTimer(); };
        a.arg = this;
        a.name = "updTimer";
        if (esp_timer_create(&a, &timer_) == ESP_OK) esp_timer_start_periodic(timer_, 60ULL * 1000 * 1000);
    }
    Serial.printf("[Update] Prüfung %s, Quelle github.com/%s\n", autoCheck_ ? "täglich" : "nur manuell",
                  LINKRA_UPDATE_REPO);
}

void UpdateCheck::setSettings(bool autoCheck, bool haInstall) {
    const bool haChanged = haInstall != haInstall_;
    autoCheck_ = autoCheck;
    haInstall_ = haInstall;
    Preferences p;
    if (p.begin("update", false)) {
        p.putBool("auto", autoCheck);
        p.putBool("ha_inst", haInstall);
        p.end();
    }
    if (haChanged) MqttBridge::getInstance().republishDiscovery();   // install button on/off
}

bool UpdateCheck::busy() const {
    return state_ == State::Downloading || state_ == State::Installing;
}

bool UpdateCheck::spawn(uint32_t job) {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return false;
    if (xTaskCreatePinnedToCore(&UpdateCheck::taskEntry, "updCheck", 10240, (void*)(uintptr_t)job, 1, nullptr, 0) != pdPASS) {
        running_ = false;
        return false;
    }
    return true;
}

void UpdateCheck::onTimer() {
    if (!autoCheck_ || running_ || !WiFiManager::getInstance().isConnected()) return;
    const uint32_t now = millis();
    const bool due = !checkedOnce_ ? now >= kFirstCheckMs
                                   : now - checkedMs_ >= (state_ == State::Error ? kRetryMs : kIntervalMs);
    if (due) spawn(kBitCheck);
}

bool UpdateCheck::requestCheck(String& err) {
    if (busy() || running_ || state_ == State::Checking) { err = "Läuft bereits"; return false; }
    if (!WiFiManager::getInstance().isConnected()) { err = "Keine WLAN-Verbindung"; return false; }
    if (!spawn(kBitCheck)) { err = "Konnte nicht starten (Speicher)"; return false; }
    return true;
}

bool UpdateCheck::requestInstall(String& err) {
    if (busy() || running_) { err = "Es läuft bereits ein Update oder eine Prüfung"; return false; }
    if (state_ != State::Available) { err = "Kein Update verfügbar"; return false; }
    if (Update.isRunning()) { err = "Es läuft bereits ein Update (Upload)"; return false; }
    if (!spawn(kBitInstall)) { err = "Konnte nicht starten (Speicher)"; return false; }
    return true;
}

void UpdateCheck::setState(State s, const char* error) {
    xSemaphoreTake(lock_, portMAX_DELAY);
    state_ = s;
    if (s != State::Downloading && s != State::Installing) progress_ = -1;
    error_ = error ? error : "";
    xSemaphoreGive(lock_);
    MqttBridge::getInstance().publishUpdateState();
}

void UpdateCheck::setProgress(int pct) {
    if (pct == progress_) return;
    const bool publish = progress_ < 0 || pct / 10 != progress_ / 10;
    progress_ = pct;
    if (publish) MqttBridge::getInstance().publishUpdateState();
}

String UpdateCheck::statusJson() {
    JsonDocument d;
    xSemaphoreTake(lock_, portMAX_DELAY);
    d["state"] = stateName(state_);
    d["installed"] = LINKRA_FW_VERSION;
    d["repo"] = LINKRA_UPDATE_REPO;
    d["auto_check"] = autoCheck_;
    d["ha_install"] = haInstall_;
    if (checkedAt_) d["checked_at"] = checkedAt_;
    if (checkedOnce_) d["checked_ago_s"] = (millis() - checkedMs_) / 1000;
    if (progress_ >= 0) d["progress"] = progress_;
    if (error_.length()) d["error"] = error_;
    if (tag_.length()) {
        JsonObject r = d["latest"].to<JsonObject>();
        r["version"] = version_;
        r["name"] = name_;
        r["notes"] = notes_;
        r["url"] = htmlUrl_;
        r["size"] = binSize_;
    }
    xSemaphoreGive(lock_);
    String out;
    serializeJson(d, out);
    return out;
}

void UpdateCheck::fillHaInfo(ha::UpdateInfo& u) {
    xSemaphoreTake(lock_, portMAX_DELAY);
    u.installed = LINKRA_FW_VERSION;
    u.latest = tag_.length() ? version_.c_str() : LINKRA_FW_VERSION;
    u.title = name_.c_str();
    u.summary = notes_.c_str();
    u.releaseUrl = htmlUrl_.c_str();
    u.progress = busy() ? (progress_ < 0 ? 0 : progress_) : -1;
    xSemaphoreGive(lock_);
}

void UpdateCheck::taskEntry(void* arg) {
    UpdateCheck& u = get();
    if ((uint32_t)(uintptr_t)arg == kBitInstall) u.runInstall();
    else u.runCheck();
    u.running_ = false;
    vTaskDelete(nullptr);
}

void UpdateCheck::runCheck() {
    checkedOnce_ = true;
    checkedMs_ = millis();
    const State before = state_;
    setState(State::Checking);
    // a failed re-check keeps an already found release installable
    auto failCheck = [&](const String& msg) {
        Serial.printf("[Update] Prüfung fehlgeschlagen: %s\n", msg.c_str());
        setState(tag_.length() ? State::Available : State::Error, msg.c_str());
    };
    String err;
    if (!enoughHeap(err)) return failCheck(err);

    Buf b;
    b.cap = kApiBufMax;
    b.data = (char*)heap_caps_malloc(b.cap, MALLOC_CAP_SPIRAM);
    if (!b.data) return failCheck("Kein Speicher für die Antwort");
    const String url = "https://api.github.com/repos/" LINKRA_UPDATE_REPO "/releases?per_page=8";
    const int status = httpGet(url.c_str(), b, true, err);
    if (status != 200) {
        heap_caps_free(b.data);
        if (status == 403 || status == 429) err = "GitHub-Abfragelimit erreicht, später erneut";
        else if (status == 404) err = "Repository nicht gefunden";
        else if (status > 0) err = "GitHub antwortet mit HTTP " + String(status);
        return failCheck(err);
    }
    const upd::Version installed = upd::parseVersion(LINKRA_FW_VERSION);
    upd::Release rel;
    std::string perr;
    const bool ok = upd::pickRelease(b.data, b.len, !installed.pre.empty(), rel, perr);
    heap_caps_free(b.data);
    if (!ok) return failCheck(perr.c_str());

    const bool newer = !rel.tag.empty() && upd::compareVersions(upd::parseVersion(rel.tag), installed) > 0;
    xSemaphoreTake(lock_, portMAX_DELAY);
    const time_t now = time(nullptr);
    checkedAt_ = now > 1700000000 ? (uint32_t)now : 0;
    if (newer) {
        tag_ = rel.tag.c_str();
        version_ = rel.version.c_str();
        name_ = rel.name.c_str();
        notes_ = rel.notes.c_str();
        htmlUrl_ = rel.htmlUrl.c_str();
        binUrl_ = rel.binUrl.c_str();
        shaUrl_ = rel.shaUrl.c_str();
        binSize_ = rel.binSize;
    } else {
        tag_ = version_ = name_ = notes_ = htmlUrl_ = binUrl_ = shaUrl_ = "";
        binSize_ = 0;
    }
    xSemaphoreGive(lock_);
    if (newer && before != State::Available)
        Serial.printf("[Update] Neue Version verfügbar: %s (installiert %s)\n", rel.version.c_str(), LINKRA_FW_VERSION);
    else if (!newer)
        Serial.printf("[Update] Aktuell (%s)\n", LINKRA_FW_VERSION);
    setState(newer ? State::Available : State::UpToDate);
}

void UpdateCheck::runInstall() {
    if (state_ != State::Available) return;
    xSemaphoreTake(lock_, portMAX_DELAY);
    const String binUrl = binUrl_, shaUrl = shaUrl_, version = version_;
    const uint32_t size = binSize_;
    xSemaphoreGive(lock_);

    auto fail = [&](const String& msg) {
        Serial.printf("[Update] Installation abgebrochen: %s\n", msg.c_str());
        // back to "available" so the user can retry; error is shown alongside
        xSemaphoreTake(lock_, portMAX_DELAY);
        state_ = State::Available;
        progress_ = -1;
        error_ = msg;
        xSemaphoreGive(lock_);
        MqttBridge::getInstance().publishUpdateState();
    };

    const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
    if (!target) return fail("Kein OTA-Slot vorhanden (alte Partitionstabelle)");
    if (size == 0 || size > target->size) return fail("Firmware passt nicht in den OTA-Slot");
    if (Update.isRunning()) return fail("Es läuft bereits ein Update (Upload)");
    String err;
    if (!enoughHeap(err)) return fail(err);

    setState(State::Downloading);
    setProgress(0);
    Serial.printf("[Update] Lade %s (%u Bytes) nach %s\n", version.c_str(), (unsigned)size, target->label);

    // 1. expected checksum
    Buf sb;
    sb.cap = 512;
    sb.data = (char*)heap_caps_malloc(sb.cap + 1, MALLOC_CAP_SPIRAM);
    if (!sb.data) return fail("Kein Speicher");
    const int st = httpGet(shaUrl.c_str(), sb, false, err);
    uint8_t expected[32];
    bool shaOk = false;
    if (st == 200) {
        sb.data[sb.len] = '\0';
        shaOk = upd::parseSha256(std::string(sb.data, sb.len), expected);
    }
    heap_caps_free(sb.data);
    if (st != 200) return fail(st > 0 ? "Prüfsumme nicht abrufbar (HTTP " + String(st) + ")" : err);
    if (!shaOk) return fail("Prüfsummendatei ungültig");

    // 2. image into the free slot
    esp_http_client_config_t hc = httpConfig(binUrl.c_str());
    esp_https_ota_config_t oc = {};
    oc.http_config = &hc;
    esp_https_ota_handle_t h = nullptr;
    esp_err_t r = esp_https_ota_begin(&oc, &h);
    if (r != ESP_OK) return fail(String("Download fehlgeschlagen (") + esp_err_to_name(r) + ")");
    for (;;) {
        r = esp_https_ota_perform(h);
        if (r != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;
        const int total = esp_https_ota_get_image_size(h) > 0 ? esp_https_ota_get_image_size(h) : (int)size;
        const int done = esp_https_ota_get_image_len_read(h);
        if (total > 0) setProgress(min(99, done * 100 / total));
    }
    if (r != ESP_OK || !esp_https_ota_is_complete_data_received(h)) {
        esp_https_ota_abort(h);
        return fail(String("Download unvollständig (") + esp_err_to_name(r) + ")");
    }
    const int len = esp_https_ota_get_image_len_read(h);

    // 3. SHA-256 of what is now in flash must match the release's checksum file
    uint8_t* chunk = (uint8_t*)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (!chunk) { esp_https_ota_abort(h); return fail("Kein Speicher"); }
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    bool readOk = true;
    for (int off = 0; off < len && readOk; off += 4096) {
        const size_t n = min(4096, len - off);
        readOk = esp_partition_read(target, off, chunk, n) == ESP_OK;
        if (readOk) mbedtls_sha256_update(&ctx, chunk, n);
    }
    uint8_t actual[32];
    mbedtls_sha256_finish(&ctx, actual);
    mbedtls_sha256_free(&ctx);
    heap_caps_free(chunk);
    if (!readOk || memcmp(actual, expected, 32) != 0) {
        esp_https_ota_abort(h);
        return fail(readOk ? "Prüfsumme stimmt nicht – Update verworfen" : "Flash lesen fehlgeschlagen");
    }

    // 4. validate image, switch boot slot, restart (runs on probation, see Supervisor)
    setState(State::Installing);
    r = esp_https_ota_finish(h);
    if (r != ESP_OK) return fail(String("Image ungültig (") + esp_err_to_name(r) + ")");
    Serial.printf("[Update] %s installiert (Prüfsumme ok), Neustart\n", version.c_str());
    setProgress(100);
    vTaskDelay(pdMS_TO_TICKS(1500));   // let MQTT/HTTP clients see the final state
    esp_restart();
}
