#ifndef FREERTOS_QUEUE_H
#define FREERTOS_QUEUE_H

#include "freertos/FreeRTOS.h"

// Mock FreeRTOS queue.h - queue functions are already declared in FreeRTOS.h
// This header just provides the declarations in the expected location

// Queue functions are already in FreeRTOS.h mock
// Additional queue macros if needed
#define queueQUEUE_TYPE_BASE 0
#define queueQUEUE_TYPE_SET 1
#define queueQUEUE_TYPE_MUTEX 2
#define queueQUEUE_TYPE_COUNTING_SEMAPHORE 3
#define queueQUEUE_TYPE_BINARY_SEMAPHORE 4

#endif  // FREERTOS_QUEUE_H
