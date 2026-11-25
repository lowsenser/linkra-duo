#pragma once
// Firmware update check against GitHub Releases and one-click install.
//
// - Checks LINKRA_UPDATE_REPO (src/core/version.h) once a day, first ~3 min
//   after boot. Opt-out: setting "auto_check" (NVS namespace "update").
//   Pre-releases are offered only while a pre-release is installed.
// - Install: downloads firmware.bin over HTTPS (certificate bundle) into the
//   free OTA slot, compares its SHA-256 with firmware.bin.sha256 from the same
//   release, then switches the boot slot and restarts. The new image runs "on
//   probation" (Supervisor confirms after 90 s, otherwise bootloader rollback).
// - Home Assistant: state is published as an MQTT update entity; installing
//   from HA is possible only with the setting "ha_install".
// All network work runs in one low-priority task; only one TLS connection at a time.

#include <Arduino.h>
#include <atomic>
#include <esp_timer.h>

namespace ha { struct UpdateInfo; }

class UpdateCheck {
public:
    enum class State : uint8_t { Idle, Checking, UpToDate, Available, Downloading, Installing, Error };

    static UpdateCheck& get();
    void begin();

    bool requestCheck(String& err);       // manual check (also when auto_check is off)
    bool requestInstall(String& err);     // only in state Available
    bool busy() const;                    // download or install running
    String statusJson();

    bool autoCheck() const { return autoCheck_; }
    bool haInstall() const { return haInstall_; }
    void setSettings(bool autoCheck, bool haInstall);
    void fillHaInfo(ha::UpdateInfo& u);

private:
    UpdateCheck() { lock_ = xSemaphoreCreateMutex(); }
    static void taskEntry(void* arg);       // one job, then the task deletes itself
    bool spawn(uint32_t job);
    void onTimer();
    void runCheck();
    void runInstall();
    void setState(State s, const char* error = nullptr);
    void setProgress(int pct);

    SemaphoreHandle_t lock_ = nullptr;
    esp_timer_handle_t timer_ = nullptr;
    std::atomic<bool> running_{false};
    volatile State state_ = State::Idle;
    volatile int progress_ = -1;
    bool autoCheck_ = true;
    bool haInstall_ = false;
    uint32_t checkedAt_ = 0;              // epoch of the last successful check (0 = clock unknown)
    uint32_t checkedMs_ = 0;              // millis() of the last attempt
    bool checkedOnce_ = false;
    String error_;
    // newest release found (empty tag = none newer)
    String tag_, version_, name_, notes_, htmlUrl_, binUrl_, shaUrl_;
    uint32_t binSize_ = 0;
};
