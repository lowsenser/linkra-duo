#pragma once

#include <Arduino.h>
#include <map>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/**
 * Task Management für FreeRTOS
 * Zentrale Verwaltung aller System-Tasks
 */
class TaskManager {
public:
    static TaskManager& getInstance() {
        static TaskManager instance;
        return instance;
    }

    void initSystemTasks();
    void suspendTask(const char* name);
    void resumeTask(const char* name);
    void deleteTask(const char* name);

private:
    TaskManager() {}
    std::map<String, TaskHandle_t> tasks;
};

// Task Priorities (korrekt nach Context7):
// - Low Priority (1): Web Server, Idle Tasks
// - Medium Priority (2): RF Decoder, WebSocket
// - High Priority (3): Time-Critical (nicht verwendet bisher)

#define TASK_PRIORITY_WEB      1
#define TASK_PRIORITY_RF       2
#define TASK_PRIORITY_MATTER   1
