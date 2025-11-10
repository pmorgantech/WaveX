#ifndef FREERTOS_PORTMACRO_H
#define FREERTOS_PORTMACRO_H

#include <cstdint>

// Mock portMUX_TYPE for FreeRTOS
typedef struct {
    volatile uint32_t owner;
    volatile uint32_t count;
} portMUX_TYPE;

#define portMUX_INITIALIZER_UNLOCKED {0, 0}

// Mock critical section macros
#define taskENTER_CRITICAL(mux) \
    do {                        \
        (void)(mux);            \
    } while (0)
#define taskEXIT_CRITICAL(mux) \
    do {                       \
        (void)(mux);           \
    } while (0)

// Mock port macros
#define portMAX_DELAY UINT32_MAX

#endif  // FREERTOS_PORTMACRO_H
