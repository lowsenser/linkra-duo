#pragma once
// Firmware version (semantic). Hardware revision is reported separately (A0.1 = breadboard).
// 0.5.0-beta (2026-11-22): all core functions work end to end – 433/868 reception with
// 377 rtl_433 decoders and receive-profile rotation, wM-Bus T1/C1 incl. Techem, device
// database, Matter-over-Thread bridge to Home Assistant, new web UI. Open for 1.0:
// OTA (partition table), long-term stability (web server stall 2026-11-22 unexplained),
// PCB hardware.
// 0.6.0-beta (2026-11-24): ESP32-C6 / Matter removed; Home Assistant via MQTT with
// auto-discovery (only devices released by the user).
// 0.7.0-beta (2026-11-25): OTA partitions with rollback, WPA2 setup hotspot, P1 robustness
// (pipeline task, supervisor, RMT watchdog), P3 wM-Bus layers/statistics, rtl_433 tuning.
#define LINKRA_FW_VERSION "0.7.0-beta"
