#ifndef RETROTRANSFER_TRANSFER_QUEUE_H
#define RETROTRANSFER_TRANSFER_QUEUE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * TransferBlockDesc:
 * Descriptor representing a block of data to be transmitted.
 * This stores metadata (offset, length, index, last block flag),
 * NOT the actual large payload data itself, keeping memory bounded.
 */
typedef struct {
    uint64_t file_offset;   /* Byte offset in the source file */
    uint32_t block_size;    /* Number of bytes in this block (e.g. up to 64 KiB) */
    uint32_t block_index;   /* Sequential index of this block */
    int is_last;            /* 1 if this is the final block of the transfer, 0 otherwise */
} TransferBlockDesc;

/*
 * TransferQueue:
 * A bounded circular queue (ring buffer) for transfer-block descriptors.
 * Implemented in pure C as a foundational Data Structure & Algorithm (DSA) component.
 */
typedef struct {
    TransferBlockDesc* buffer; /* Dynamically allocated array of descriptors */
    size_t capacity;           /* Maximum capacity of the ring buffer */
    size_t head;               /* Read index (dequeue position) */
    size_t tail;               /* Write index (enqueue position) */
    size_t count;              /* Current number of items in the queue */
} TransferQueue;

/*
 * Initialize the circular queue with the specified capacity.
 * Returns 1 on success, 0 on failure (e.g. capacity == 0 or malloc failed).
 */
int transfer_queue_init(TransferQueue* q, size_t capacity);

/*
 * Free all resources allocated for the circular queue and reset fields.
 */
void transfer_queue_destroy(TransferQueue* q);

/*
 * Check if the queue is empty.
 * Returns 1 if empty, 0 otherwise (or if q is NULL).
 */
int transfer_queue_is_empty(const TransferQueue* q);

/*
 * Check if the queue is full.
 * Returns 1 if full, 0 otherwise (or if q is NULL).
 */
int transfer_queue_is_full(const TransferQueue* q);

/*
 * Get the current number of elements in the queue.
 */
size_t transfer_queue_size(const TransferQueue* q);

/*
 * Get the maximum capacity of the queue.
 */
size_t transfer_queue_capacity(const TransferQueue* q);

/*
 * Enqueue a transfer-block descriptor at the tail of the circular queue.
 * Overflow handling: If the queue is full, the item is rejected and 0 is returned.
 * Returns 1 on success, 0 on failure/overflow.
 */
int transfer_queue_enqueue(TransferQueue* q, const TransferBlockDesc* desc);

/*
 * Dequeue a transfer-block descriptor from the head of the circular queue.
 * Underflow handling: If the queue is empty, 0 is returned and out_desc is unmodified.
 * Returns 1 on success, 0 on failure/underflow.
 */
int transfer_queue_dequeue(TransferQueue* q, TransferBlockDesc* out_desc);

/*
 * Peek at the item at the head of the circular queue without removing it.
 * Returns 1 on success, 0 on underflow / invalid queue.
 */
int transfer_queue_peek(const TransferQueue* q, TransferBlockDesc* out_desc);

/*
 * Clear all items in the queue without releasing the allocated buffer.
 */
void transfer_queue_clear(TransferQueue* q);

#ifdef __cplusplus
}
#endif

#endif /* RETROTRANSFER_TRANSFER_QUEUE_H */
