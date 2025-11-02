#ifndef WAVEX_UART_DEBUG_CONFIG_H
#define WAVEX_UART_DEBUG_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#include <cstdio>
#include <cstdarg>
#else
#include <stdio.h>
#include <stdarg.h>
#endif

#ifndef WAVEX_UART_DEBUG_LEVEL
#define WAVEX_UART_DEBUG_LEVEL 3  // Enable INFO level for UART debugging
#endif

#define UART_LOG_ERROR   1
#define UART_LOG_WARN    2
#define UART_LOG_INFO    3
#define UART_LOG_VERBOSE 4
#define UART_LOG_DUMP    5

#if defined(ESP_PLATFORM)

#include "esp_log.h"

#if WAVEX_UART_DEBUG_LEVEL >= UART_LOG_ERROR
#define UART_LOGE(tag, fmt, ...) ESP_LOGE(tag ? tag : "uart", fmt, ##__VA_ARGS__)
#else
#define UART_LOGE(tag, fmt, ...) ((void)0)
#endif

#if WAVEX_UART_DEBUG_LEVEL >= UART_LOG_WARN
#define UART_LOGW(tag, fmt, ...) ESP_LOGW(tag ? tag : "uart", fmt, ##__VA_ARGS__)
#else
#define UART_LOGW(tag, fmt, ...) ((void)0)
#endif

#if WAVEX_UART_DEBUG_LEVEL >= UART_LOG_INFO
#define UART_LOGI(tag, fmt, ...) ESP_LOGI(tag ? tag : "uart", fmt, ##__VA_ARGS__)
#else
#define UART_LOGI(tag, fmt, ...) ((void)0)
#endif

#if WAVEX_UART_DEBUG_LEVEL >= UART_LOG_VERBOSE
#define UART_LOGV(tag, fmt, ...) ESP_LOGD(tag ? tag : "uart", fmt, ##__VA_ARGS__)
#else
#define UART_LOGV(tag, fmt, ...) ((void)0)
#endif

#elif defined(DAISY_PLATFORM)

#ifdef __cplusplus
namespace WaveX {
namespace Debug {
inline void Printf(const char* level, const char* tag, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    std::printf("[UART][%s]%s%s: ", level, tag ? " " : "", tag ? tag : "daisy");
    std::vprintf(fmt, args);
    std::printf("\n");
    va_end(args);
}
} // namespace Debug
} // namespace WaveX
#endif

#if WAVEX_UART_DEBUG_LEVEL >= UART_LOG_ERROR
#define UART_LOGE(tag, fmt, ...) WaveX::Debug::Printf("E", tag, fmt, ##__VA_ARGS__)
#else
#define UART_LOGE(tag, fmt, ...) ((void)0)
#endif

#if WAVEX_UART_DEBUG_LEVEL >= UART_LOG_INFO
#define UART_LOGI(tag, fmt, ...) WaveX::Debug::Printf("I", tag, fmt, ##__VA_ARGS__)
#else
#define UART_LOGI(tag, fmt, ...) ((void)0)
#endif

#if WAVEX_UART_DEBUG_LEVEL >= UART_LOG_WARN
#define UART_LOGW(tag, fmt, ...) WaveX::Debug::Printf("W", tag, fmt, ##__VA_ARGS__)
#else
#define UART_LOGW(tag, fmt, ...) ((void)0)
#endif

#if WAVEX_UART_DEBUG_LEVEL >= UART_LOG_VERBOSE
#define UART_LOGV(tag, fmt, ...) WaveX::Debug::Printf("V", tag, fmt, ##__VA_ARGS__)
#else
#define UART_LOGV(tag, fmt, ...) ((void)0)
#endif

#else

#if WAVEX_UART_DEBUG_LEVEL >= UART_LOG_ERROR
#define UART_LOGE(tag, fmt, ...) do { printf("[UART][E]%s%s: " fmt "\n", tag ? " " : "", tag ? tag : "uart", ##__VA_ARGS__); } while (0)
#else
#define UART_LOGE(tag, fmt, ...) ((void)0)
#endif

#if WAVEX_UART_DEBUG_LEVEL >= UART_LOG_WARN
#define UART_LOGW(tag, fmt, ...) do { printf("[UART][W]%s%s: " fmt "\n", tag ? " " : "", tag ? tag : "uart", ##__VA_ARGS__); } while (0)
#else
#define UART_LOGW(tag, fmt, ...) ((void)0)
#endif

#if WAVEX_UART_DEBUG_LEVEL >= UART_LOG_INFO
#define UART_LOGI(tag, fmt, ...) do { printf("[UART][I]%s%s: " fmt "\n", tag ? " " : "", tag ? tag : "uart", ##__VA_ARGS__); } while (0)
#else
#define UART_LOGI(tag, fmt, ...) ((void)0)
#endif

#if WAVEX_UART_DEBUG_LEVEL >= UART_LOG_VERBOSE
#define UART_LOGV(tag, fmt, ...) do { printf("[UART][V]%s%s: " fmt "\n", tag ? " " : "", tag ? tag : "uart", ##__VA_ARGS__); } while (0)
#else
#define UART_LOGV(tag, fmt, ...) ((void)0)
#endif

#endif // platform selection

#ifdef __cplusplus
namespace WaveX {
namespace UartProtocol {
void DumpPacket(const char* tag, const uint8_t* data, size_t length);
} // namespace UartProtocol
} // namespace WaveX
#endif

#if WAVEX_UART_DEBUG_LEVEL >= UART_LOG_DUMP
#define UART_LOG_DUMP_PACKET(tag, data, len) WaveX::UartProtocol::DumpPacket(tag, data, len)
#else
#define UART_LOG_DUMP_PACKET(tag, data, len) do { (void)(tag); (void)(data); (void)(len); } while (0)
#endif

#endif // WAVEX_UART_DEBUG_CONFIG_H

