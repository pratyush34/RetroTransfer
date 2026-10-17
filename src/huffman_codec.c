#include "huffman_codec_c.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RT_MAX_CHUNK_SIZE_BYTES (UINT64_C(1024) * UINT64_C(1024) * UINT64_C(1024))
#define RT_SYMBOL_COUNT 256
#define RT_MAX_NODES (RT_SYMBOL_COUNT * 2 - 1)
#define RT_MAX_CODE_LENGTH 255

typedef struct {
    uint64_t frequency;
    unsigned min_symbol;
    int left;
    int right;
    int symbol;
} HuffmanNode;

typedef struct {
    uint64_t frequency;
    unsigned min_symbol;
    uint64_t order;
    int node;
} QueueEntry;

typedef struct {
    int child[2];
    int symbol;
} TrieNode;

static void set_error(char *error, size_t capacity, const char *message) {
    if (error != NULL && capacity != 0) {
        (void)snprintf(error, capacity, "%s", message);
    }
}

static int queue_entry_less(const QueueEntry *left, const QueueEntry *right) {
    if (left->frequency != right->frequency) {
        return left->frequency < right->frequency;
    }
    if (left->min_symbol != right->min_symbol) {
        return left->min_symbol < right->min_symbol;
    }
    return left->order < right->order;
}

static void heap_push(QueueEntry *heap, size_t *size, QueueEntry value) {
    size_t index = (*size)++;
    while (index > 0) {
        const size_t parent = (index - 1) / 2;
        if (!queue_entry_less(&value, &heap[parent])) {
            break;
        }
        heap[index] = heap[parent];
        index = parent;
    }
    heap[index] = value;
}

static QueueEntry heap_pop(QueueEntry *heap, size_t *size) {
    const QueueEntry result = heap[0];
    const QueueEntry last = heap[--(*size)];
    size_t index = 0;
    while (index * 2 + 1 < *size) {
        size_t child = index * 2 + 1;
        if (child + 1 < *size && queue_entry_less(&heap[child + 1], &heap[child])) {
            ++child;
        }
        if (!queue_entry_less(&heap[child], &last)) {
            break;
        }
        heap[index] = heap[child];
        index = child;
    }
    if (*size != 0) {
        heap[index] = last;
    }
    return result;
}

static int set_code_lengths(const uint8_t *input,
                            size_t input_size,
                            uint8_t lengths[RT_SYMBOL_COUNT],
                            char *error,
                            size_t error_capacity) {
    uint64_t frequencies[RT_SYMBOL_COUNT] = {0};
    HuffmanNode nodes[RT_MAX_NODES];
    QueueEntry heap[RT_SYMBOL_COUNT];
    size_t node_count = 0;
    size_t heap_size = 0;
    uint64_t order = 0;

    for (size_t i = 0; i < input_size; ++i) {
        ++frequencies[input[i]];
    }

    for (unsigned symbol = 0; symbol < RT_SYMBOL_COUNT; ++symbol) {
        if (frequencies[symbol] == 0) {
            continue;
        }
        const int node = (int)node_count++;
        nodes[node] = (HuffmanNode){frequencies[symbol], symbol, -1, -1, (int)symbol};
        heap_push(heap, &heap_size,
                  (QueueEntry){frequencies[symbol], symbol, order++, node});
    }

    if (heap_size == 0) {
        return 1;
    }
    if (heap_size == 1) {
        lengths[nodes[heap[0].node].symbol] = 1;
        return 1;
    }

    while (heap_size > 1) {
        const QueueEntry left = heap_pop(heap, &heap_size);
        const QueueEntry right = heap_pop(heap, &heap_size);
        const uint64_t combined = left.frequency + right.frequency;
        if (combined < left.frequency) {
            set_error(error, error_capacity, "Huffman frequency overflow.");
            return 0;
        }
        const unsigned min_symbol =
            left.min_symbol < right.min_symbol ? left.min_symbol : right.min_symbol;
        const int parent = (int)node_count++;
        nodes[parent] = (HuffmanNode){combined, min_symbol, left.node, right.node, -1};
        heap_push(heap, &heap_size,
                  (QueueEntry){combined, min_symbol, order++, parent});
    }

    int stack_nodes[RT_MAX_NODES];
    unsigned stack_depths[RT_MAX_NODES];
    size_t stack_size = 0;
    stack_nodes[stack_size] = heap[0].node;
    stack_depths[stack_size++] = 0;
    while (stack_size != 0) {
        const int node_index = stack_nodes[--stack_size];
        const unsigned depth = stack_depths[stack_size];
        const HuffmanNode *node = &nodes[node_index];
        if (node->symbol >= 0) {
            if (depth == 0 || depth > RT_MAX_CODE_LENGTH) {
                set_error(error, error_capacity,
                          "Huffman code length is outside the supported range.");
                return 0;
            }
            lengths[node->symbol] = (uint8_t)depth;
            continue;
        }
        stack_nodes[stack_size] = node->right;
        stack_depths[stack_size++] = depth + 1;
        stack_nodes[stack_size] = node->left;
        stack_depths[stack_size++] = depth + 1;
    }
    return 1;
}

static int make_canonical_codes(
    const uint8_t lengths[RT_SYMBOL_COUNT],
    uint8_t codes[RT_SYMBOL_COUNT][RT_MAX_CODE_LENGTH],
    char *error,
    size_t error_capacity) {
    unsigned symbols[RT_SYMBOL_COUNT];
    size_t symbol_count = 0;
    for (unsigned symbol = 0; symbol < RT_SYMBOL_COUNT; ++symbol) {
        if (lengths[symbol] != 0) {
            symbols[symbol_count++] = symbol;
        }
    }
    for (size_t i = 1; i < symbol_count; ++i) {
        const unsigned symbol = symbols[i];
        size_t j = i;
        while (j > 0 &&
               (lengths[symbols[j - 1]] > lengths[symbol] ||
                (lengths[symbols[j - 1]] == lengths[symbol] &&
                 symbols[j - 1] > symbol))) {
            symbols[j] = symbols[j - 1];
            --j;
        }
        symbols[j] = symbol;
    }
    if (symbol_count == 0) {
        return 1;
    }

    uint8_t code[RT_MAX_CODE_LENGTH] = {0};
    size_t code_length = lengths[symbols[0]];
    memcpy(codes[symbols[0]], code, code_length);
    for (size_t index = 1; index < symbol_count; ++index) {
        size_t bit = code_length;
        while (bit > 0 && code[bit - 1] != 0) {
            code[--bit] = 0;
        }
        if (bit == 0) {
            set_error(error, error_capacity,
                      "Invalid Huffman code lengths: canonical code overflow.");
            return 0;
        }
        code[bit - 1] = 1;

        const size_t next_length = lengths[symbols[index]];
        if (next_length < code_length) {
            set_error(error, error_capacity,
                      "Invalid Huffman code lengths: lengths are not ordered.");
            return 0;
        }
        memset(code + code_length, 0, next_length - code_length);
        code_length = next_length;
        memcpy(codes[symbols[index]], code, code_length);
    }
    return 1;
}

static int make_decode_trie(
    const uint8_t lengths[RT_SYMBOL_COUNT],
    const uint8_t *codes,
    TrieNode *trie,
    size_t trie_capacity,
    size_t *trie_size,
    char *error,
    size_t error_capacity) {
    *trie_size = 1;
    trie[0] = (TrieNode){{-1, -1}, -1};
    for (unsigned symbol = 0; symbol < RT_SYMBOL_COUNT; ++symbol) {
        const size_t code_length = lengths[symbol];
        if (code_length == 0) {
            continue;
        }
        int node = 0;
        for (size_t bit_index = 0; bit_index < code_length; ++bit_index) {
            if (trie[node].symbol != -1) {
                set_error(error, error_capacity,
                          "Invalid Huffman code table: a code is a prefix of another.");
                return 0;
            }
            const unsigned direction =
                codes[symbol * RT_MAX_CODE_LENGTH + bit_index];
            if (trie[node].child[direction] == -1) {
                if (*trie_size == trie_capacity) {
                    set_error(error, error_capacity, "Invalid Huffman code table.");
                    return 0;
                }
                trie[node].child[direction] = (int)*trie_size;
                trie[(*trie_size)++] = (TrieNode){{-1, -1}, -1};
            }
            node = trie[node].child[direction];
        }
        if (trie[node].symbol != -1 ||
            trie[node].child[0] != -1 ||
            trie[node].child[1] != -1) {
            set_error(error, error_capacity,
                      "Invalid Huffman code table: duplicate or prefix code.");
            return 0;
        }
        trie[node].symbol = (int)symbol;
    }
    return 1;
}

static int append_byte(uint8_t **buffer,
                       size_t *size,
                       size_t *capacity,
                       uint8_t byte) {
    if (*size == *capacity) {
        size_t next_capacity = *capacity == 0 ? 512 : *capacity * 2;
        if (next_capacity < *capacity) {
            return 0;
        }
        uint8_t *next = (uint8_t *)realloc(*buffer, next_capacity);
        if (next == NULL) {
            return 0;
        }
        *buffer = next;
        *capacity = next_capacity;
    }
    (*buffer)[(*size)++] = byte;
    return 1;
}

int rt_huffman_compress(const uint8_t *input,
                        size_t input_size,
                        uint8_t **output,
                        size_t *output_size,
                        char *error,
                        size_t error_capacity) {
    uint8_t lengths[RT_SYMBOL_COUNT] = {0};
    uint8_t codes[RT_SYMBOL_COUNT][RT_MAX_CODE_LENGTH] = {{0}};
    uint8_t *encoded = NULL;
    size_t encoded_size = 0;
    size_t encoded_capacity = 0;
    uint8_t packed_byte = 0;
    unsigned bits_in_byte = 0;

    if (output == NULL || output_size == NULL ||
        (input == NULL && input_size != 0)) {
        set_error(error, error_capacity, "Invalid Huffman compressor arguments.");
        return 0;
    }
    *output = NULL;
    *output_size = 0;
    if (error != NULL && error_capacity != 0) {
        error[0] = '\0';
    }
    if ((uint64_t)input_size > RT_MAX_CHUNK_SIZE_BYTES) {
        set_error(error, error_capacity,
                  "Input exceeds the configured maximum chunk size.");
        return 0;
    }
    if (!set_code_lengths(input, input_size, lengths, error, error_capacity) ||
        !make_canonical_codes(lengths, codes, error, error_capacity)) {
        return 0;
    }
    encoded = (uint8_t *)malloc(RT_SYMBOL_COUNT);
    if (encoded == NULL) {
        set_error(error, error_capacity, "Could not allocate Huffman output.");
        return 0;
    }
    memcpy(encoded, lengths, RT_SYMBOL_COUNT);
    encoded_size = RT_SYMBOL_COUNT;
    encoded_capacity = RT_SYMBOL_COUNT;

    for (size_t i = 0; i < input_size; ++i) {
        const unsigned symbol = input[i];
        for (size_t bit = 0; bit < lengths[symbol]; ++bit) {
            packed_byte = (uint8_t)((packed_byte << 1) | codes[symbol][bit]);
            if (++bits_in_byte == 8) {
                if (!append_byte(&encoded, &encoded_size, &encoded_capacity,
                                 packed_byte)) {
                    free(encoded);
                    set_error(error, error_capacity, "Could not allocate Huffman output.");
                    return 0;
                }
                packed_byte = 0;
                bits_in_byte = 0;
            }
        }
    }
    if (bits_in_byte != 0) {
        packed_byte = (uint8_t)(packed_byte << (8 - bits_in_byte));
        if (!append_byte(&encoded, &encoded_size, &encoded_capacity, packed_byte)) {
            free(encoded);
            set_error(error, error_capacity, "Could not allocate Huffman output.");
            return 0;
        }
    }
    *output = encoded;
    *output_size = encoded_size;
    return 1;
}

int rt_huffman_decompress(const uint8_t *input,
                          size_t input_size,
                          uint64_t original_size,
                          uint8_t **output,
                          size_t *output_size,
                          char *error,
                          size_t error_capacity) {
    uint8_t lengths[RT_SYMBOL_COUNT];
    uint8_t codes[RT_SYMBOL_COUNT][RT_MAX_CODE_LENGTH] = {{0}};
    TrieNode *trie = NULL;
    size_t trie_size = 0;
    uint8_t *decoded = NULL;
    size_t bit_position = 0;
    size_t payload_bits = 0;

    if (output == NULL || output_size == NULL ||
        (input == NULL && input_size != 0)) {
        set_error(error, error_capacity, "Invalid Huffman decompressor arguments.");
        return 0;
    }
    *output = NULL;
    *output_size = 0;
    if (error != NULL && error_capacity != 0) {
        error[0] = '\0';
    }
    if (original_size > RT_MAX_CHUNK_SIZE_BYTES || original_size > SIZE_MAX) {
        set_error(error, error_capacity,
                  "Requested output exceeds the configured maximum chunk size.");
        return 0;
    }
    if (input_size < RT_SYMBOL_COUNT) {
        set_error(error, error_capacity, "Truncated Huffman header.");
        return 0;
    }
    memcpy(lengths, input, RT_SYMBOL_COUNT);
    if (!make_canonical_codes(lengths, codes, error, error_capacity)) {
        return 0;
    }

    const size_t trie_capacity =
        1 + (size_t)RT_SYMBOL_COUNT * (size_t)RT_MAX_CODE_LENGTH;
    trie = (TrieNode *)malloc(trie_capacity * sizeof(*trie));
    if (trie == NULL) {
        set_error(error, error_capacity, "Could not allocate Huffman decode table.");
        return 0;
    }
    if (!make_decode_trie(lengths, &codes[0][0], trie, trie_capacity, &trie_size,
                          error, error_capacity)) {
        free(trie);
        return 0;
    }

    if (original_size == 0) {
        for (size_t i = 0; i < RT_SYMBOL_COUNT; ++i) {
            if (lengths[i] != 0) {
                free(trie);
                set_error(error, error_capacity,
                          "Invalid Huffman stream for empty input.");
                return 0;
            }
        }
        free(trie);
        if (input_size != RT_SYMBOL_COUNT) {
            set_error(error, error_capacity,
                      "Invalid Huffman stream for empty input.");
            return 0;
        }
        return 1;
    }
    if (trie_size == 1) {
        free(trie);
        set_error(error, error_capacity,
                  "Huffman stream has no symbols for non-empty output.");
        return 0;
    }

    const size_t payload_size = input_size - RT_SYMBOL_COUNT;
    if (payload_size > SIZE_MAX / 8) {
        free(trie);
        set_error(error, error_capacity, "Huffman bitstream is too large.");
        return 0;
    }
    payload_bits = payload_size * 8;
    decoded = original_size == 0 ? NULL : (uint8_t *)malloc((size_t)original_size);
    if (original_size != 0 && decoded == NULL) {
        free(trie);
        set_error(error, error_capacity, "Could not allocate Huffman output.");
        return 0;
    }

    size_t decoded_size = 0;
    while ((uint64_t)decoded_size < original_size) {
        int node = 0;
        while (trie[node].symbol == -1) {
            if (bit_position >= payload_bits) {
                free(trie);
                free(decoded);
                set_error(error, error_capacity, "Truncated Huffman bitstream.");
                return 0;
            }
            const uint8_t byte = input[RT_SYMBOL_COUNT + bit_position / 8];
            const unsigned bit = (byte >> (7 - (bit_position % 8))) & 1U;
            ++bit_position;
            node = trie[node].child[bit];
            if (node == -1) {
                free(trie);
                free(decoded);
                set_error(error, error_capacity, "Invalid Huffman bitstream.");
                return 0;
            }
        }
        decoded[decoded_size++] = (uint8_t)trie[node].symbol;
    }
    free(trie);

    if (payload_bits - bit_position > 7) {
        free(decoded);
        set_error(error, error_capacity, "Huffman stream contains trailing data.");
        return 0;
    }
    while (bit_position < payload_bits) {
        const uint8_t byte = input[RT_SYMBOL_COUNT + bit_position / 8];
        const unsigned bit = (byte >> (7 - (bit_position % 8))) & 1U;
        if (bit != 0) {
            free(decoded);
            set_error(error, error_capacity, "Huffman stream has nonzero padding bits.");
            return 0;
        }
        ++bit_position;
    }

    *output = decoded;
    *output_size = decoded_size;
    return 1;
}
