#ifndef FREERTOS_TASK_H
#define FREERTOS_TASK_H

#include "freertos/FreeRTOS.h"

#include <cstdint>

// Mock FreeRTOS task.h - includes task-related functions
// Most functions are already declared in FreeRTOS.h

// Additional task macros
#define taskYIELD() \
    do {            \
    } while (0)
#define taskENTER_CRITICAL_ISR(mux) taskENTER_CRITICAL(mux)
#define taskEXIT_CRITICAL_ISR(mux) taskEXIT_CRITICAL(mux)

// Task states
typedef enum { eRunning = 0, eReady, eBlocked, eSuspended, eDeleted, eInvalid } eTaskState;

// Task function prototype
typedef void (*TaskFunction_t)(void*);

// Base type for FreeRTOS
typedef uint32_t UBaseType_t;

// Task handle functions
eTaskState eTaskGetState(TaskHandle_t xTask);
const char* pcTaskGetName(TaskHandle_t xTask);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t xTask);

#endif  // FREERTOS_TASK_H
