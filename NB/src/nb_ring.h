#ifndef NB_RING_H
#define NB_RING_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t* data;
    size_t cap;
    size_t head;
    size_t len;
} nb_ring_t;

void nb_ring_dispose(nb_ring_t* q);
int nb_ring_append(nb_ring_t* q, const uint8_t* data, size_t len, size_t limit);
size_t nb_ring_peek(const nb_ring_t* q, const uint8_t** data);
void nb_ring_consume(nb_ring_t* q, size_t len);
size_t nb_ring_copyout(nb_ring_t* q, uint8_t* dst, size_t len);
void nb_ring_clear(nb_ring_t* q);

#endif
