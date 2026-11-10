#include "network/transfer_queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

static int g_tests_run = 0;
static int g_tests_failed = 0;

#define TEST_ASSERT(cond, msg) \
    do { \
        g_tests_run++; \
        if (!(cond)) { \
            fprintf(stderr, "FAIL: %s (line %d): %s\n", __func__, __LINE__, msg); \
            g_tests_failed++; \
        } else { \
            printf("  [PASS] %s: %s\n", __func__, msg); \
        } \
    } while (0)

static void test_initialization(void) {
    TransferQueue q;
    TEST_ASSERT(transfer_queue_init(NULL, 10) == 0, "NULL queue rejected");
    TEST_ASSERT(transfer_queue_init(&q, 0) == 0, "Zero capacity rejected");
    
    TEST_ASSERT(transfer_queue_init(&q, 8) == 1, "Capacity 8 initialized");
    TEST_ASSERT(transfer_queue_is_empty(&q) == 1, "Initially empty");
    TEST_ASSERT(transfer_queue_is_full(&q) == 0, "Initially not full");
    TEST_ASSERT(transfer_queue_size(&q) == 0, "Initial size is 0");
    TEST_ASSERT(transfer_queue_capacity(&q) == 8, "Capacity is 8");
    
    transfer_queue_destroy(&q);
    TEST_ASSERT(transfer_queue_is_empty(&q) == 1, "Empty after destroy");
    TEST_ASSERT(transfer_queue_capacity(&q) == 0, "Capacity is 0 after destroy");
}

static void test_enqueue_dequeue_fifo(void) {
    TransferQueue q;
    transfer_queue_init(&q, 4);

    TransferBlockDesc d1 = {0, 1024, 0, 0};
    TransferBlockDesc d2 = {1024, 2048, 1, 0};
    TransferBlockDesc d3 = {3072, 512, 2, 1};

    TEST_ASSERT(transfer_queue_enqueue(&q, &d1) == 1, "Enqueue d1");
    TEST_ASSERT(transfer_queue_enqueue(&q, &d2) == 1, "Enqueue d2");
    TEST_ASSERT(transfer_queue_enqueue(&q, &d3) == 1, "Enqueue d3");
    TEST_ASSERT(transfer_queue_size(&q) == 3, "Size is 3");
    TEST_ASSERT(transfer_queue_is_empty(&q) == 0, "Not empty");
    TEST_ASSERT(transfer_queue_is_full(&q) == 0, "Not full");

    TransferBlockDesc peek_out;
    TEST_ASSERT(transfer_queue_peek(&q, &peek_out) == 1, "Peek succeeds");
    TEST_ASSERT(peek_out.file_offset == 0 && peek_out.block_size == 1024, "Peek matches d1");
    TEST_ASSERT(transfer_queue_size(&q) == 3, "Peek did not remove element");

    TransferBlockDesc out;
    TEST_ASSERT(transfer_queue_dequeue(&q, &out) == 1, "Dequeue d1");
    TEST_ASSERT(out.file_offset == 0 && out.block_size == 1024 && out.block_index == 0, "d1 matches");

    TEST_ASSERT(transfer_queue_dequeue(&q, &out) == 1, "Dequeue d2");
    TEST_ASSERT(out.file_offset == 1024 && out.block_size == 2048 && out.block_index == 1, "d2 matches");

    TEST_ASSERT(transfer_queue_dequeue(&q, &out) == 1, "Dequeue d3");
    TEST_ASSERT(out.file_offset == 3072 && out.block_size == 512 && out.block_index == 2 && out.is_last == 1, "d3 matches");

    TEST_ASSERT(transfer_queue_is_empty(&q) == 1, "Queue is empty after 3 dequeues");

    transfer_queue_destroy(&q);
}

static void test_overflow_and_underflow(void) {
    TransferQueue q;
    transfer_queue_init(&q, 2);

    TransferBlockDesc dummy = {0, 100, 0, 0};
    TransferBlockDesc out;

    /* Underflow test on empty queue */
    TEST_ASSERT(transfer_queue_dequeue(&q, &out) == 0, "Dequeue from empty returns 0 (underflow)");
    TEST_ASSERT(transfer_queue_peek(&q, &out) == 0, "Peek on empty returns 0");

    /* Fill to capacity */
    TEST_ASSERT(transfer_queue_enqueue(&q, &dummy) == 1, "Enqueue item 1");
    TEST_ASSERT(transfer_queue_enqueue(&q, &dummy) == 1, "Enqueue item 2");
    TEST_ASSERT(transfer_queue_is_full(&q) == 1, "Queue is full");

    /* Overflow test */
    TEST_ASSERT(transfer_queue_enqueue(&q, &dummy) == 0, "Enqueue to full returns 0 (overflow)");
    TEST_ASSERT(transfer_queue_size(&q) == 2, "Size remains 2 after failed overflow enqueue");

    transfer_queue_destroy(&q);
}

static void test_circular_wrap_around(void) {
    TransferQueue q;
    /* Capacity 3 */
    transfer_queue_init(&q, 3);

    for (uint32_t round = 0; round < 100; round++) {
        TransferBlockDesc in1 = {round * 3, 10, round * 3, 0};
        TransferBlockDesc in2 = {round * 3 + 1, 20, round * 3 + 1, 0};
        TransferBlockDesc out;

        TEST_ASSERT(transfer_queue_enqueue(&q, &in1) == 1, "Enqueue round item 1");
        TEST_ASSERT(transfer_queue_enqueue(&q, &in2) == 1, "Enqueue round item 2");

        TEST_ASSERT(transfer_queue_dequeue(&q, &out) == 1, "Dequeue round item 1");
        if (out.block_index != in1.block_index) {
            TEST_ASSERT(0, "Wrapped index mismatch item 1");
        }

        TEST_ASSERT(transfer_queue_dequeue(&q, &out) == 1, "Dequeue round item 2");
        if (out.block_index != in2.block_index) {
            TEST_ASSERT(0, "Wrapped index mismatch item 2");
        }
    }

    TEST_ASSERT(transfer_queue_is_empty(&q) == 1, "Queue empty after 100 wrap-around rounds");
    transfer_queue_destroy(&q);
}

static void test_repeated_operations_10000(void) {
    TransferQueue q;
    const size_t cap = 16; /* Same capacity used by TransferSender */
    TEST_ASSERT(transfer_queue_init(&q, cap) == 1, "Queue init with capacity 16");

    const uint32_t total_ops = 20000;
    uint32_t enqueued_count = 0;
    uint32_t dequeued_count = 0;
    int ordering_ok = 1;

    /* Execute 20,000 operations with dynamic batch sizes */
    while (dequeued_count < total_ops) {
        /* Enqueue a batch up to capacity */
        uint32_t batch = (rand() % cap) + 1;
        for (uint32_t i = 0; i < batch && enqueued_count < total_ops; i++) {
            if (transfer_queue_is_full(&q)) {
                break;
            }
            TransferBlockDesc in;
            in.file_offset = (uint64_t)enqueued_count * 65536;
            in.block_size = 65536;
            in.block_index = enqueued_count;
            in.is_last = (enqueued_count == total_ops - 1) ? 1 : 0;

            if (!transfer_queue_enqueue(&q, &in)) {
                ordering_ok = 0;
                break;
            }
            enqueued_count++;
        }

        /* Dequeue elements and verify FIFO ordering and data integrity */
        uint32_t drain = (rand() % (transfer_queue_size(&q) + 1));
        if (drain == 0 && !transfer_queue_is_empty(&q)) {
            drain = 1;
        }
        for (uint32_t i = 0; i < drain; i++) {
            TransferBlockDesc out;
            if (!transfer_queue_dequeue(&q, &out)) {
                break;
            }
            if (out.block_index != dequeued_count ||
                out.file_offset != (uint64_t)dequeued_count * 65536 ||
                out.block_size != 65536 ||
                out.is_last != ((dequeued_count == total_ops - 1) ? 1 : 0)) {
                ordering_ok = 0;
            }
            dequeued_count++;
        }
    }

    /* Drain any remaining items */
    while (!transfer_queue_is_empty(&q)) {
        TransferBlockDesc out;
        if (transfer_queue_dequeue(&q, &out)) {
            if (out.block_index != dequeued_count) {
                ordering_ok = 0;
            }
            dequeued_count++;
        }
    }

    TEST_ASSERT(ordering_ok == 1, "20,000 operations preserved strict FIFO order & integrity");
    TEST_ASSERT(dequeued_count == total_ops, "Dequeued count reached total operations (20,000)");
    TEST_ASSERT(transfer_queue_is_empty(&q) == 1, "Queue is empty after 20,000 operations");

    /* Clear and test reuse */
    transfer_queue_clear(&q);
    TEST_ASSERT(transfer_queue_size(&q) == 0, "Queue cleared");
    TransferBlockDesc d = {0, 512, 1, 1};
    TEST_ASSERT(transfer_queue_enqueue(&q, &d) == 1, "Enqueue after clear");
    TEST_ASSERT(transfer_queue_size(&q) == 1, "Size is 1 after clear + enqueue");

    transfer_queue_destroy(&q);
    TEST_ASSERT(transfer_queue_is_empty(&q) == 1, "Queue destroyed successfully");
}

int main(void) {
    printf("========================================\n");
    printf("     RUNNING C TRANSFER QUEUE TESTS     \n");
    printf("========================================\n");

    test_initialization();
    test_enqueue_dequeue_fifo();
    test_overflow_and_underflow();
    test_circular_wrap_around();
    test_repeated_operations_10000();

    printf("========================================\n");
    printf("Results: %d run, %d failed\n", g_tests_run, g_tests_failed);
    printf("========================================\n");

    return (g_tests_failed == 0) ? 0 : 1;
}
