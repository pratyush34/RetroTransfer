#include "network/transfer_queue.h"

#include <stdlib.h>
#include <string.h>

int transfer_queue_init(TransferQueue* q, size_t capacity) {
    if (!q || capacity == 0 || capacity > ((size_t)-1) / sizeof(TransferBlockDesc)) {
        return 0;
    }

    q->buffer = (TransferBlockDesc*)malloc(capacity * sizeof(TransferBlockDesc));
    if (!q->buffer) {
        q->capacity = 0;
        q->head = 0;
        q->tail = 0;
        q->count = 0;
        return 0;
    }

    q->capacity = capacity;
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    return 1;
}

void transfer_queue_destroy(TransferQueue* q) {
    if (!q) {
        return;
    }

    if (q->buffer) {
        free(q->buffer);
        q->buffer = NULL;
    }
    q->capacity = 0;
    q->head = 0;
    q->tail = 0;
    q->count = 0;
}

int transfer_queue_is_empty(const TransferQueue* q) {
    if (!q) {
        return 1;
    }
    return (q->count == 0) ? 1 : 0;
}

int transfer_queue_is_full(const TransferQueue* q) {
    if (!q) {
        return 0;
    }
    return (q->count == q->capacity) ? 1 : 0;
}

size_t transfer_queue_size(const TransferQueue* q) {
    if (!q) {
        return 0;
    }
    return q->count;
}

size_t transfer_queue_capacity(const TransferQueue* q) {
    if (!q) {
        return 0;
    }
    return q->capacity;
}

int transfer_queue_enqueue(TransferQueue* q, const TransferBlockDesc* desc) {
    /* Guard against NULL pointer and check for queue overflow */
    if (!q || !desc || !q->buffer) {
        return 0;
    }

    if (q->count == q->capacity) {
        /* Queue is full - overflow handling: reject new element */
        return 0;
    }

    /* Store descriptor at current tail position */
    q->buffer[q->tail] = *desc;

    /* Advance tail with circular wrap-around */
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;

    return 1;
}

int transfer_queue_dequeue(TransferQueue* q, TransferBlockDesc* out_desc) {
    /* Guard against NULL pointer and check for queue underflow */
    if (!q || !q->buffer) {
        return 0;
    }

    if (q->count == 0) {
        /* Queue is empty - underflow handling: cannot dequeue */
        return 0;
    }

    /* Copy descriptor from head if caller provided destination */
    if (out_desc) {
        *out_desc = q->buffer[q->head];
    }

    /* Advance head with circular wrap-around */
    q->head = (q->head + 1) % q->capacity;
    q->count--;

    return 1;
}

int transfer_queue_peek(const TransferQueue* q, TransferBlockDesc* out_desc) {
    if (!q || !q->buffer || !out_desc) {
        return 0;
    }

    if (q->count == 0) {
        return 0;
    }

    *out_desc = q->buffer[q->head];
    return 1;
}

void transfer_queue_clear(TransferQueue* q) {
    if (!q) {
        return;
    }
    q->head = 0;
    q->tail = 0;
    q->count = 0;
}
