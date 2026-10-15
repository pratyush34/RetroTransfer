#ifndef HUFFMAN_CODEC_H
#define HUFFMAN_CODEC_H

#include <cstdint>
#include <string>
#include <vector>

class HuffmanCodec {
public:
    static bool compress(const std::vector<uint8_t>& input,
                         std::vector<uint8_t>& output,
                         std::string& error);

    static bool decompress(const std::vector<uint8_t>& input,
                           uint64_t originalSize,
                           std::vector<uint8_t>& output,
                           std::string& error);
};

#endif
