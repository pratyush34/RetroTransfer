#include "huffman_codec.h"

#include "chunk_config.h"

#include <algorithm>
#include <array>
#include <functional>
#include <queue>
#include <utility>

namespace {

struct HuffmanNode {
    uint64_t frequency;
    unsigned minSymbol;
    int left;
    int right;
    int symbol;
};

struct QueueEntry {
    uint64_t frequency;
    unsigned minSymbol;
    uint64_t order;
    int node;
};

struct QueueEntryGreater {
    bool operator()(const QueueEntry& left, const QueueEntry& right) const {
        if (left.frequency != right.frequency) {
            return left.frequency > right.frequency;
        }
        if (left.minSymbol != right.minSymbol) {
            return left.minSymbol > right.minSymbol;
        }
        return left.order > right.order;
    }
};

struct Code {
    std::vector<uint8_t> bits;
};

struct TrieNode {
    int child[2] = {-1, -1};
    int symbol = -1;
};

bool makeCanonicalCodes(const std::array<uint8_t, 256>& lengths,
                        std::array<Code, 256>& codes,
                        std::string& error) {
    std::vector<unsigned> symbols;
    for (unsigned symbol = 0; symbol < lengths.size(); ++symbol) {
        if (lengths[symbol] != 0) symbols.push_back(symbol);
    }
    std::sort(symbols.begin(), symbols.end(), [&](unsigned left, unsigned right) {
        if (lengths[left] != lengths[right]) return lengths[left] < lengths[right];
        return left < right;
    });

    if (symbols.empty()) return true;

    std::vector<uint8_t> code(lengths[symbols.front()], 0);
    codes[symbols.front()].bits = code;
    for (size_t index = 1; index < symbols.size(); ++index) {
        size_t bit = code.size();
        while (bit > 0 && code[bit - 1] != 0) {
            code[--bit] = 0;
        }
        if (bit == 0) {
            error = "Invalid Huffman code lengths: canonical code overflow.";
            return false;
        }
        code[bit - 1] = 1;

        const size_t nextLength = lengths[symbols[index]];
        if (nextLength < code.size()) {
            error = "Invalid Huffman code lengths: lengths are not ordered.";
            return false;
        }
        code.resize(nextLength, 0);
        codes[symbols[index]].bits = code;
    }
    return true;
}

bool makeDecodeTrie(const std::array<Code, 256>& codes,
                    std::vector<TrieNode>& trie,
                    std::string& error) {
    trie.clear();
    trie.emplace_back();

    for (unsigned symbol = 0; symbol < codes.size(); ++symbol) {
        const auto& bits = codes[symbol].bits;
        if (bits.empty()) continue;

        int node = 0;
        for (size_t index = 0; index < bits.size(); ++index) {
            if (trie[node].symbol != -1) {
                error = "Invalid Huffman code table: a code is a prefix of another.";
                return false;
            }
            const uint8_t direction = bits[index];
            if (trie[node].child[direction] == -1) {
                trie[node].child[direction] = static_cast<int>(trie.size());
                trie.emplace_back();
            }
            node = trie[node].child[direction];
        }
        if (trie[node].symbol != -1 ||
            trie[node].child[0] != -1 ||
            trie[node].child[1] != -1) {
            error = "Invalid Huffman code table: duplicate or prefix code.";
            return false;
        }
        trie[node].symbol = static_cast<int>(symbol);
    }
    return true;
}

bool makeCodeLengths(const std::vector<uint8_t>& input,
                     std::array<uint8_t, 256>& lengths,
                     std::string& error) {
    std::array<uint64_t, 256> frequencies{};
    for (uint8_t byte : input) ++frequencies[byte];

    std::vector<HuffmanNode> nodes;
    nodes.reserve(511);
    std::priority_queue<QueueEntry, std::vector<QueueEntry>, QueueEntryGreater> queue;
    uint64_t order = 0;
    for (unsigned symbol = 0; symbol < frequencies.size(); ++symbol) {
        if (frequencies[symbol] == 0) continue;
        const int node = static_cast<int>(nodes.size());
        nodes.push_back({frequencies[symbol], symbol, -1, -1, static_cast<int>(symbol)});
        queue.push({frequencies[symbol], symbol, order++, node});
    }

    if (queue.empty()) return true;
    if (queue.size() == 1) {
        lengths[static_cast<size_t>(nodes[queue.top().node].symbol)] = 1;
        return true;
    }

    while (queue.size() > 1) {
        const QueueEntry left = queue.top();
        queue.pop();
        const QueueEntry right = queue.top();
        queue.pop();
        const uint64_t combined = left.frequency + right.frequency;
        if (combined < left.frequency) {
            error = "Huffman frequency overflow.";
            return false;
        }

        const int parent = static_cast<int>(nodes.size());
        const unsigned minSymbol = std::min(left.minSymbol, right.minSymbol);
        nodes.push_back({combined, minSymbol, left.node, right.node, -1});
        queue.push({combined, minSymbol, order++, parent});
    }

    std::function<bool(int, unsigned)> visit = [&](int nodeIndex, unsigned depth) {
        const HuffmanNode& node = nodes[static_cast<size_t>(nodeIndex)];
        if (node.symbol >= 0) {
            if (depth == 0 || depth > 255) {
                error = "Huffman code length is outside the supported range.";
                return false;
            }
            lengths[static_cast<size_t>(node.symbol)] = static_cast<uint8_t>(depth);
            return true;
        }
        return visit(node.left, depth + 1) && visit(node.right, depth + 1);
    };
    return visit(queue.top().node, 0);
}

} // namespace

bool HuffmanCodec::compress(const std::vector<uint8_t>& input,
                            std::vector<uint8_t>& output,
                            std::string& error) {
    output.clear();
    error.clear();
    if (input.size() > kMaxChunkSizeBytes) {
        error = "Input exceeds the configured maximum chunk size.";
        return false;
    }

    std::array<uint8_t, 256> lengths{};
    if (!makeCodeLengths(input, lengths, error)) return false;

    std::array<Code, 256> codes;
    if (!makeCanonicalCodes(lengths, codes, error)) return false;

    output.insert(output.end(), lengths.begin(), lengths.end());
    uint8_t byte = 0;
    unsigned bitsInByte = 0;
    for (uint8_t symbol : input) {
        for (uint8_t bit : codes[symbol].bits) {
            byte = static_cast<uint8_t>((byte << 1) | bit);
            if (++bitsInByte == 8) {
                output.push_back(byte);
                byte = 0;
                bitsInByte = 0;
            }
        }
    }
    if (bitsInByte != 0) {
        byte = static_cast<uint8_t>(byte << (8 - bitsInByte));
        output.push_back(byte);
    }
    return true;
}

bool HuffmanCodec::decompress(const std::vector<uint8_t>& input,
                              uint64_t originalSize,
                              std::vector<uint8_t>& output,
                              std::string& error) {
    output.clear();
    error.clear();
    if (originalSize > kMaxChunkSizeBytes) {
        error = "Requested output exceeds the configured maximum chunk size.";
        return false;
    }
    if (input.size() < 256) {
        error = "Truncated Huffman header.";
        return false;
    }

    std::array<uint8_t, 256> lengths{};
    std::copy_n(input.begin(), lengths.size(), lengths.begin());
    std::array<Code, 256> codes;
    if (!makeCanonicalCodes(lengths, codes, error)) return false;

    std::vector<TrieNode> trie;
    if (!makeDecodeTrie(codes, trie, error)) return false;
    if (originalSize == 0) {
        if (std::any_of(lengths.begin(), lengths.end(), [](uint8_t length) {
                return length != 0;
            }) || input.size() != 256) {
            error = "Invalid Huffman stream for empty input.";
            return false;
        }
        return true;
    }
    if (trie.size() == 1) {
        error = "Huffman stream has no symbols for non-empty output.";
        return false;
    }

    output.reserve(static_cast<size_t>(originalSize));
    size_t bitPosition = 0;
    const size_t payloadSize = input.size() - 256;
    const size_t payloadBits = payloadSize * 8;
    while (output.size() < originalSize) {
        int node = 0;
        while (trie[static_cast<size_t>(node)].symbol == -1) {
            if (bitPosition >= payloadBits) {
                output.clear();
                error = "Truncated Huffman bitstream.";
                return false;
            }
            const uint8_t byte = input[256 + bitPosition / 8];
            const uint8_t bit = static_cast<uint8_t>(
                (byte >> (7 - (bitPosition % 8))) & 1);
            ++bitPosition;
            node = trie[static_cast<size_t>(node)].child[bit];
            if (node == -1) {
                output.clear();
                error = "Invalid Huffman bitstream.";
                return false;
            }
        }
        output.push_back(static_cast<uint8_t>(
            trie[static_cast<size_t>(node)].symbol));
    }

    if (payloadBits - bitPosition > 7) {
        output.clear();
        error = "Huffman stream contains trailing data.";
        return false;
    }
    while (bitPosition < payloadBits) {
        const uint8_t byte = input[256 + bitPosition / 8];
        const uint8_t bit = static_cast<uint8_t>(
            (byte >> (7 - (bitPosition % 8))) & 1);
        if (bit != 0) {
            output.clear();
            error = "Huffman stream has nonzero padding bits.";
            return false;
        }
        ++bitPosition;
    }
    return true;
}
