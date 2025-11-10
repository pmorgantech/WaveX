#ifndef WAVEX_SPI_PROTOCOL_H
#define WAVEX_SPI_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- Framing ---------------------------------------------------------------
typedef struct __attribute__((packed)) {
    uint16_t len;   // bytes in payload (0..244)
    uint16_t type;  // app-defined (0=ping,1=state,2=param,...)
} pkt_hdr_t;

typedef struct __attribute__((packed)) {
    pkt_hdr_t h;
    uint8_t payload[240];  // tune for your MTU (keep <= 240 for easy DMA)
    uint16_t crc;          // CRC-16/CCITT-FALSE over hdr+payload
} pkt_t;

// CRC-16/CCITT-FALSE calculation
static inline uint16_t crc16_ccitt_false(const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint16_t crc = 0xFFFF;  // CCITT-FALSE init
    while (len--) {
        crc ^= (uint16_t)(*p++) << 8;
        for (int i = 0; i < 8; ++i)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
    }
    return crc;
}

static inline uint16_t pkt_crc(const pkt_t *pkt) {
    return crc16_ccitt_false(&pkt->h, sizeof(pkt_hdr_t) + pkt->h.len);
}

// ---- Lock-free ring (single-producer/single-consumer) ----------------------
typedef struct {
    volatile uint16_t head;  // producer writes head
    volatile uint16_t tail;  // consumer writes tail
    uint16_t cap;            // number of elements
    void **buf;              // array of void* (pkt_t*)
} ring_t;

// Simple ring buffer for pointer-sized data
static inline void ring_init(ring_t *r, void **backing, uint16_t cap) {
    r->tail = 0;
    r->head = 0;
    r->cap = cap;
    r->buf = backing;
}

static inline int ring_push(ring_t *r, void *ptr) {
    uint16_t h = r->head, n = (h + 1u) % r->cap;
    if (n == r->tail)
        return 0;  // full
    r->buf[h] = ptr;
    r->head = n;
    return 1;
}

static inline void *ring_pop(ring_t *r) {
    uint16_t t = r->tail;
    if (t == r->head)
        return 0;  // empty
    void *v = r->buf[t];
    r->tail = (t + 1u) % r->cap;
    return v;
}

static inline int ring_empty(const ring_t *r) {
    return r->head == r->tail;
}
static inline int ring_full(const ring_t *r) {
    return ((r->head + 1u) % r->cap) == r->tail;
}

// ---- App packet helpers ----------------------------------------------------
static inline void pkt_fill(pkt_t *p, uint16_t type, const void *payload, uint16_t len) {
    p->h.type = type;
    p->h.len = len;
    if (len && payload)
        memcpy(p->payload, payload, len);
    p->crc = pkt_crc(p);
}

#ifdef __cplusplus
}
#endif

#endif  // WAVEX_SPI_PROTOCOL_H
