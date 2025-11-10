#ifndef FREERTOS_FREERTOS_H
#define FREERTOS_FREERTOS_H

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// Mock FreeRTOS types
typedef void* QueueHandle_t;
typedef void* TaskHandle_t;
typedef void* SemaphoreHandle_t;
typedef uint32_t TickType_t;
typedef int BaseType_t;

// FreeRTOS constants
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY UINT32_MAX

// FreeRTOS task priorities
#define tskIDLE_PRIORITY 0
#define configMAX_PRIORITIES 25

// FreeRTOS queue functions
QueueHandle_t xQueueCreate(uint32_t uxQueueLength, uint32_t uxItemSize);
BaseType_t xQueueSend(QueueHandle_t xQueue, const void* pvItemToQueue, TickType_t xTicksToWait);
BaseType_t xQueueSendFromISR(QueueHandle_t xQueue,
                             const void* pvItemToQueue,
                             BaseType_t* pxHigherPriorityTaskWoken);
BaseType_t xQueueReceive(QueueHandle_t xQueue, void* pvBuffer, TickType_t xTicksToWait);
BaseType_t xQueueReceiveFromISR(QueueHandle_t xQueue,
                                void* pvBuffer,
                                BaseType_t* pxHigherPriorityTaskWoken);
uint32_t uxQueueMessagesWaiting(QueueHandle_t xQueue);
void vQueueDelete(QueueHandle_t xQueue);

// FreeRTOS task functions
TaskHandle_t xTaskCreate(void (*pxTaskCode)(void*),
                         const char* pcName,
                         uint32_t usStackDepth,
                         void* pvParameters,
                         uint32_t uxPriority,
                         TaskHandle_t* pxCreatedTask);
void vTaskDelete(TaskHandle_t xTask);
void vTaskDelay(TickType_t xTicksToDelay);
TickType_t xTaskGetTickCount(void);

// FreeRTOS semaphore functions
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t xSemaphore, TickType_t xBlockTime);
BaseType_t xSemaphoreGive(SemaphoreHandle_t xSemaphore);
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t xSemaphore,
                                 BaseType_t* pxHigherPriorityTaskWoken);

#ifdef __cplusplus
}
#endif

#endif  // FREERTOS_FREERTOS_H
