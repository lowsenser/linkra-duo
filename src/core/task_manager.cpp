#include "task_manager.h"

void TaskManager::initSystemTasks() {
    // Tasks werden später in system_init.cpp angelegt
    // Dies ist nur die Verwaltungsstruktur
}

void TaskManager::suspendTask(const char* name) {
    if (tasks.find(name) != tasks.end()) {
        vTaskSuspend(tasks[name]);
    }
}

void TaskManager::resumeTask(const char* name) {
    if (tasks.find(name) != tasks.end()) {
        vTaskResume(tasks[name]);
    }
}

void TaskManager::deleteTask(const char* name) {
    if (tasks.find(name) != tasks.end()) {
        vTaskDelete(tasks[name]);
        tasks.erase(name);
    }
}
