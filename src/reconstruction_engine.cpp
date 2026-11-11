#include "reconstruction_engine.h"

#include "chunk_engine.h"
#include "fingerprint_c.h"
#include "huffman_codec_c.h"
#include "storage/chunk_storage.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct JsonValue {
    enum class Type { Null, Boolean, Number, String, Array, Object };
    Type type = Type::Null;
    std::string text;
    std::vector<JsonValue> array;
    std::map<std::string, JsonValue> object;
};

class JsonParser {
public:
    explicit JsonParser(const std::string& input) : input_(input) {}

    JsonValue parse() {
        JsonValue value = parseValue();
        skipWhitespace();
        if (position_ != input_.size()) fail("Unexpected trailing JSON data");
        return value;
    }

private:
    const std::string& input_;
    size_t position_ = 0;

    [[noreturn]] void fail(const char* message) const {
        throw std::runtime_error(std::string(message) + " at byte " +
                                 std::to_string(position_));
    }

    void skipWhitespace() {
        while (position_ < input_.size() &&
               (input_[position_] == ' ' || input_[position_] == '\t' ||
                input_[position_] == '\r' || input_[position_] == '\n')) {
            ++position_;
        }
    }

    bool consume(char character) {
        skipWhitespace();
        if (position_ < input_.size() && input_[position_] == character) {
            ++position_;
            return true;
        }
        return false;
    }

    static void appendUtf8(std::string& output, uint32_t codePoint) {
        if (codePoint <= 0x7f) {
            output.push_back(static_cast<char>(codePoint));
        } else if (codePoint <= 0x7ff) {
            output.push_back(static_cast<char>(0xc0 | (codePoint >> 6)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
        } else if (codePoint <= 0xffff) {
            output.push_back(static_cast<char>(0xe0 | (codePoint >> 12)));
            output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
        } else {
            output.push_back(static_cast<char>(0xf0 | (codePoint >> 18)));
            output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
        }
    }

    uint32_t parseHexQuad() {
        if (input_.size() - position_ < 4) fail("Incomplete Unicode escape");
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char character = input_[position_++];
            value <<= 4;
            if (character >= '0' && character <= '9') value |= character - '0';
            else if (character >= 'a' && character <= 'f') value |= character - 'a' + 10;
            else if (character >= 'A' && character <= 'F') value |= character - 'A' + 10;
            else fail("Invalid Unicode escape");
        }
        return value;
    }

    std::string parseString() {
        if (!consume('"')) fail("Expected JSON string");
        std::string value;
        while (position_ < input_.size()) {
            const unsigned char character =
                static_cast<unsigned char>(input_[position_++]);
            if (character == '"') return value;
            if (character < 0x20) fail("Unescaped control character in JSON string");
            if (character != '\\') {
                value.push_back(static_cast<char>(character));
                continue;
            }
            if (position_ == input_.size()) fail("Incomplete JSON escape");
            switch (input_[position_++]) {
                case '"': value.push_back('"'); break;
                case '\\': value.push_back('\\'); break;
                case '/': value.push_back('/'); break;
                case 'b': value.push_back('\b'); break;
                case 'f': value.push_back('\f'); break;
                case 'n': value.push_back('\n'); break;
                case 'r': value.push_back('\r'); break;
                case 't': value.push_back('\t'); break;
                case 'u': {
                    uint32_t codePoint = parseHexQuad();
                    if (codePoint >= 0xd800 && codePoint <= 0xdbff) {
                        if (input_.size() - position_ < 6 ||
                            input_[position_] != '\\' ||
                            input_[position_ + 1] != 'u') {
                            fail("Unpaired high surrogate");
                        }
                        position_ += 2;
                        const uint32_t low = parseHexQuad();
                        if (low < 0xdc00 || low > 0xdfff) {
                            fail("Invalid low surrogate");
                        }
                        codePoint = 0x10000 + ((codePoint - 0xd800) << 10) +
                                    (low - 0xdc00);
                    } else if (codePoint >= 0xdc00 && codePoint <= 0xdfff) {
                        fail("Unpaired low surrogate");
                    }
                    appendUtf8(value, codePoint);
                    break;
                }
                default: fail("Invalid JSON escape");
            }
        }
        fail("Unterminated JSON string");
    }

    JsonValue parseNumber() {
        skipWhitespace();
        const size_t start = position_;
        if (position_ < input_.size() && input_[position_] == '-') ++position_;
        if (position_ == input_.size()) fail("Invalid JSON number");
        if (input_[position_] == '0') {
            ++position_;
        } else if (input_[position_] >= '1' && input_[position_] <= '9') {
            while (position_ < input_.size() && input_[position_] >= '0' &&
                   input_[position_] <= '9') ++position_;
        } else {
            fail("Invalid JSON number");
        }
        if (position_ < input_.size() && input_[position_] == '.') {
            ++position_;
            const size_t fraction = position_;
            while (position_ < input_.size() && input_[position_] >= '0' &&
                   input_[position_] <= '9') ++position_;
            if (fraction == position_) fail("Invalid JSON number");
        }
        if (position_ < input_.size() &&
            (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() &&
                (input_[position_] == '+' || input_[position_] == '-')) ++position_;
            const size_t exponent = position_;
            while (position_ < input_.size() && input_[position_] >= '0' &&
                   input_[position_] <= '9') ++position_;
            if (exponent == position_) fail("Invalid JSON number");
        }
        JsonValue value;
        value.type = JsonValue::Type::Number;
        value.text = input_.substr(start, position_ - start);
        return value;
    }

    JsonValue parseValue() {
        skipWhitespace();
        if (position_ == input_.size()) fail("Unexpected end of JSON");
        if (input_[position_] == '"') {
            JsonValue value;
            value.type = JsonValue::Type::String;
            value.text = parseString();
            return value;
        }
        if (input_[position_] == '{') {
            ++position_;
            JsonValue value;
            value.type = JsonValue::Type::Object;
            if (consume('}')) return value;
            do {
                const std::string key = parseString();
                if (!consume(':')) fail("Expected ':' after JSON object key");
                if (!value.object.emplace(key, parseValue()).second) {
                    fail("Duplicate JSON object key");
                }
            } while (consume(','));
            if (!consume('}')) fail("Expected '}' in JSON object");
            return value;
        }
        if (input_[position_] == '[') {
            ++position_;
            JsonValue value;
            value.type = JsonValue::Type::Array;
            if (consume(']')) return value;
            do {
                value.array.push_back(parseValue());
            } while (consume(','));
            if (!consume(']')) fail("Expected ']' in JSON array");
            return value;
        }
        if (input_.compare(position_, 4, "true") == 0) {
            position_ += 4;
            JsonValue value;
            value.type = JsonValue::Type::Boolean;
            return value;
        }
        if (input_.compare(position_, 5, "false") == 0) {
            position_ += 5;
            JsonValue value;
            value.type = JsonValue::Type::Boolean;
            value.text = "false";
            return value;
        }
        if (input_.compare(position_, 4, "null") == 0) {
            position_ += 4;
            return {};
        }
        return parseNumber();
    }
};

const JsonValue& requiredField(const JsonValue& value, const char* name,
                               JsonValue::Type type) {
    if (value.type != JsonValue::Type::Object) {
        throw std::runtime_error("Expected a JSON object while reading " + std::string(name));
    }
    const auto found = value.object.find(name);
    if (found == value.object.end() || found->second.type != type) {
        throw std::runtime_error(std::string("Manifest is missing a valid '") +
                                 name + "' field");
    }
    return found->second;
}

bool optionalUnsigned(const JsonValue& value, const char* name, uint64_t& output) {
    if (value.type != JsonValue::Type::Object) return false;
    const auto found = value.object.find(name);
    if (found == value.object.end()) return false;
    if (found->second.type != JsonValue::Type::Number ||
        found->second.text.empty() || found->second.text[0] == '-') {
        throw std::runtime_error("Manifest field '" + std::string(name) +
                                 "' must be an unsigned integer");
    }
    size_t parsed = 0;
    try {
        output = std::stoull(found->second.text, &parsed);
    } catch (const std::exception&) {
        throw std::runtime_error("Manifest field '" + std::string(name) +
                                 "' is out of range");
    }
    if (parsed != found->second.text.size()) {
        throw std::runtime_error("Manifest field '" + std::string(name) +
                                 "' must be an unsigned integer");
    }
    return true;
}

uint64_t requiredUnsigned(const JsonValue& value, const char* name) {
    uint64_t output = 0;
    if (!optionalUnsigned(value, name, output)) {
        throw std::runtime_error("Manifest is missing '" + std::string(name) + "'");
    }
    return output;
}

std::string requiredString(const JsonValue& value, const char* name) {
    return requiredField(value, name, JsonValue::Type::String).text;
}

struct RecipeChunk {
    uint64_t offset = 0;
    uint64_t length = 0;
    uint32_t sequence = 0;
    uint32_t crc32 = 0;
    std::string fingerprint;
};

struct RecipeFile {
    std::string relativePath;
    uint64_t size = 0;
    std::vector<RecipeChunk> chunks;
};

struct PayloadChunk {
    uint64_t originalSize = 0;
    uint64_t offset = 0;
    uint64_t storedSize = 0;
    uint32_t crc32 = 0;
    std::string fingerprint;
    std::string codec;
};

struct SourceChunk {
    std::string fullPath;
    uint64_t offset = 0;
    uint64_t length = 0;
    uint32_t crc32 = 0;
};

std::string chunkKey(const std::string& fingerprint, uint64_t length, uint32_t crc32) {
    return fingerprint + ":" + std::to_string(length) + ":" +
           std::to_string(crc32);
}

bool parseFingerprint(const std::string& fingerprint) {
    return fingerprint.size() == 32 &&
           std::all_of(fingerprint.begin(), fingerprint.end(), [](unsigned char ch) {
               return std::isxdigit(ch) != 0;
           });
}

std::string lowercaseAscii(std::string value) {
    for (char& character : value) {
        character = static_cast<char>(
            std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}

fs::path validatedRelativePath(const std::string& pathText) {
    if (pathText.empty()) throw std::runtime_error("Manifest contains an empty file path");
    const fs::path path(pathText);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
        throw std::runtime_error("Manifest contains an absolute file path: " + pathText);
    }
    for (const fs::path& component : path) {
        if (component.empty() || component == "." || component == "..") {
            throw std::runtime_error("Manifest contains an unsafe file path: " + pathText);
        }
    }
    if (path.lexically_normal().generic_string() != path.generic_string()) {
        throw std::runtime_error("Manifest contains a non-normalized file path: " + pathText);
    }
    return path;
}

uint32_t parseCrc32(const JsonValue& value, const char* field) {
    const uint64_t crc = requiredUnsigned(value, field);
    if (crc > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("Manifest field '" + std::string(field) +
                                 "' is out of range");
    }
    return static_cast<uint32_t>(crc);
}

bool readFileBytes(const fs::path& path, std::string& contents, std::string& error) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(path, ec);
    constexpr uintmax_t kMaxManifestBytes = 256ULL * 1024ULL * 1024ULL;
    if (ec || size > kMaxManifestBytes ||
        size > static_cast<uintmax_t>(std::numeric_limits<size_t>::max())) {
        error = ec ? "Could not read manifest size: " + ec.message()
                   : "Manifest is too large to safely parse.";
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "Could not open manifest: " + path.string();
        return false;
    }
    contents.resize(static_cast<size_t>(size));
    if (size != 0) {
        input.read(contents.data(), static_cast<std::streamsize>(size));
        if (input.gcount() != static_cast<std::streamsize>(size) || input.bad()) {
            error = "Could not read manifest: " + path.string();
            return false;
        }
    }
    return true;
}

bool readStoredChunk(std::ifstream& payload, const PayloadChunk& chunk,
                     std::vector<uint8_t>& decoded, std::string& error) {
    if (chunk.originalSize > kMaxChunkSizeBytes ||
        chunk.originalSize > std::numeric_limits<size_t>::max() ||
        chunk.storedSize > kMaxChunkSizeBytes + 1024ULL * 1024ULL ||
        chunk.storedSize > std::numeric_limits<size_t>::max() ||
        chunk.offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
        chunk.storedSize > static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max())) {
        error = "Payload chunk has an unsupported size or offset.";
        return false;
    }
    std::vector<uint8_t> stored(static_cast<size_t>(chunk.storedSize));
    payload.clear();
    payload.seekg(static_cast<std::streamoff>(chunk.offset), std::ios::beg);
    if (!payload || (!stored.empty() &&
        (!payload.read(reinterpret_cast<char*>(stored.data()),
                       static_cast<std::streamsize>(stored.size())) ||
         payload.gcount() != static_cast<std::streamsize>(stored.size())))) {
        error = "Could not read a chunk from the payload bundle.";
        return false;
    }
    if (chunk.codec == "raw") {
        if (chunk.storedSize != chunk.originalSize) {
            error = "Raw payload chunk size does not match the manifest.";
            return false;
        }
        decoded = std::move(stored);
    } else if (chunk.codec == "huffman") {
        uint8_t* output = nullptr;
        size_t outputSize = 0;
        char errorBuffer[512] = {};
        if (!rt_huffman_decompress(stored.data(), stored.size(), chunk.originalSize,
                                   &output, &outputSize, errorBuffer,
                                   sizeof(errorBuffer))) {
            error = std::string("Huffman decompression failed: ") + errorBuffer;
            return false;
        }
        if (outputSize != chunk.originalSize) {
            std::free(output);
            error = "Huffman decoder returned an unexpected chunk size.";
            return false;
        }
        if (outputSize != 0) decoded.assign(output, output + outputSize);
        std::free(output);
    } else {
        error = "Manifest specifies an unsupported payload codec: " + chunk.codec;
        return false;
    }
    char fingerprint[33] = {};
    rt_compute_fingerprint_hex(decoded.data(), decoded.size(), 42, fingerprint);
    if (chunk.fingerprint != fingerprint ||
        rt_compute_crc32(decoded.data(), decoded.size()) != chunk.crc32) {
        error = "Payload chunk failed fingerprint or CRC32 verification.";
        return false;
    }
    return true;
}

bool readSourceChunk(const SourceChunk& source, std::vector<uint8_t>& bytes,
                     std::string& error) {
    if (!ChunkEngine::readChunkPayload(source.fullPath, source.offset,
                                       source.length, bytes)) {
        error = "Could not read a reusable chunk from " + source.fullPath;
        return false;
    }
    if (rt_compute_crc32(bytes.data(), bytes.size()) != source.crc32 ||
        source.length != bytes.size()) {
        error = "Existing project chunk failed CRC32 verification: " + source.fullPath;
        return false;
    }
    return true;
}

bool validateOutputDirectory(const fs::path& output, std::string& error) {
    std::error_code ec;
    fs::create_directories(output, ec);
    if (ec) {
        error = "Could not create reconstruction directory: " + ec.message();
        return false;
    }
    const fs::file_status status = fs::symlink_status(output, ec);
    if (ec || fs::is_symlink(status) || !fs::is_directory(status)) {
        error = "Reconstruction output must be a real directory, not a symlink.";
        return false;
    }
    return true;
}

bool ensureSafeParentDirectories(const fs::path& root, const fs::path& relative,
                                 std::string& error) {
    fs::path current = root;
    auto end = relative.end();
    if (end != relative.begin()) --end;
    for (auto component = relative.begin(); component != end; ++component) {
        current /= *component;
        std::error_code ec;
        const fs::file_status status = fs::symlink_status(current, ec);
        if (!ec && fs::exists(status)) {
            if (fs::is_symlink(status) || !fs::is_directory(status)) {
                error = "Output path has a non-directory or symlink parent: " +
                        current.string();
                return false;
            }
        } else {
            ec.clear();
            if (!fs::create_directory(current, ec) && ec) {
                error = "Could not create output directory " + current.string() +
                        ": " + ec.message();
                return false;
            }
        }
    }
    return true;
}

} // namespace

bool ReconstructionEngine::reconstruct(const std::string& manifestPath,
                                       const std::string& payloadPath,
                                       const std::string& outputDirectory,
                                       ReconstructionResult& result,
                                       std::string& error,
                                       const std::string& persistentStorageDir) const {
    result = {};
    error.clear();

    fs::path stagingDirectory;
    std::vector<std::pair<fs::path, fs::path>> committed;
    std::vector<fs::path> createdTargets;
    bool rollbackIncomplete = false;
    try {
        std::string manifestText;
        if (!readFileBytes(manifestPath, manifestText, error)) return false;
        const JsonValue manifest = JsonParser(manifestText).parse();
        const JsonValue& filesJson =
            requiredField(manifest, "files", JsonValue::Type::Array);
        const JsonValue& compression =
            requiredField(manifest, "selectiveCompression", JsonValue::Type::Object);
        const JsonValue& payloadChunksJson =
            requiredField(compression, "payloadChunks", JsonValue::Type::Array);

        fs::path bundlePath;
        if (!payloadPath.empty()) {
            bundlePath = payloadPath;
        } else {
            const std::string payloadName = requiredString(compression, "payloadFile");
            const fs::path relativePayload(payloadName);
            if (relativePayload.is_absolute() || relativePayload.has_root_name() ||
                relativePayload.has_root_directory()) {
                throw std::runtime_error("Manifest payload filename must be relative.");
            }
            for (const fs::path& component : relativePayload) {
                if (component == "." || component == "..") {
                    throw std::runtime_error("Manifest payload filename is unsafe.");
                }
            }
            bundlePath = fs::path(manifestPath).parent_path() / relativePayload;
        }

        std::unordered_map<std::string, PayloadChunk> payloadChunks;
        for (const JsonValue& entry : payloadChunksJson.array) {
            PayloadChunk chunk;
            chunk.fingerprint = requiredString(entry, "fingerprint");
            chunk.originalSize = requiredUnsigned(entry, "originalSize");
            chunk.offset = requiredUnsigned(entry, "payloadOffset");
            chunk.storedSize = requiredUnsigned(entry, "storedSize");
            chunk.crc32 = parseCrc32(entry, "crc32");
            chunk.codec = requiredString(entry, "codec");
            if (!parseFingerprint(chunk.fingerprint) ||
                (chunk.codec != "raw" && chunk.codec != "huffman")) {
                throw std::runtime_error("Manifest contains invalid payload chunk metadata.");
            }
            const std::string key =
                chunkKey(chunk.fingerprint, chunk.originalSize, chunk.crc32);
            if (!payloadChunks.emplace(key, std::move(chunk)).second) {
                throw std::runtime_error("Manifest contains duplicate payload chunk entries.");
            }
        }

        std::vector<RecipeFile> files;
        files.reserve(filesJson.array.size());
        std::unordered_set<std::string> paths;
        for (const JsonValue& entry : filesJson.array) {
            RecipeFile file;
            file.relativePath = requiredString(entry, "path");
            const fs::path safePath = validatedRelativePath(file.relativePath);
            if (!paths.insert(lowercaseAscii(safePath.generic_string())).second) {
                throw std::runtime_error("Manifest contains duplicate file paths.");
            }
            file.size = requiredUnsigned(entry, "size");
            const JsonValue& chunks =
                requiredField(entry, "chunks", JsonValue::Type::Array);
            uint64_t expectedOffset = 0;
            for (size_t index = 0; index < chunks.array.size(); ++index) {
                const JsonValue& chunkJson = chunks.array[index];
                RecipeChunk chunk;
                const uint64_t sequence = requiredUnsigned(chunkJson, "seq");
                if (sequence > std::numeric_limits<uint32_t>::max()) {
                    throw std::runtime_error("Manifest chunk sequence number is out of range.");
                }
                chunk.sequence = static_cast<uint32_t>(sequence);
                chunk.offset = requiredUnsigned(chunkJson, "offset");
                chunk.length = requiredUnsigned(chunkJson, "length");
                chunk.fingerprint = requiredString(chunkJson, "fingerprint");
                chunk.crc32 = parseCrc32(chunkJson, "crc32");
                if (index > std::numeric_limits<uint32_t>::max() ||
                    chunk.sequence != index || chunk.offset != expectedOffset ||
                    !parseFingerprint(chunk.fingerprint) ||
                    chunk.length > kMaxChunkSizeBytes ||
                    chunk.length > std::numeric_limits<uint64_t>::max() - expectedOffset) {
                    throw std::runtime_error("Manifest contains invalid file chunk ordering or size.");
                }
                expectedOffset += chunk.length;
                file.chunks.push_back(std::move(chunk));
            }
            if (expectedOffset != file.size) {
                throw std::runtime_error("Manifest chunk lengths do not match file size: " +
                                         file.relativePath);
            }
            files.push_back(std::move(file));
        }

        uint64_t fixedChunkSize = 64 * 1024;
        uint64_t cdcMin = 16 * 1024;
        uint64_t cdcTarget = 64 * 1024;
        uint64_t cdcMax = 256 * 1024;
        const auto chunkingField = manifest.object.find("chunking");
        if (chunkingField != manifest.object.end()) {
            optionalUnsigned(chunkingField->second, "fixedChunkSize", fixedChunkSize);
            optionalUnsigned(chunkingField->second, "cdcMinChunkSize", cdcMin);
            optionalUnsigned(chunkingField->second, "cdcTargetChunkSize", cdcTarget);
            optionalUnsigned(chunkingField->second, "cdcMaxChunkSize", cdcMax);
        }
        if (fixedChunkSize == 0 || fixedChunkSize > kMaxChunkSizeBytes ||
            cdcMin == 0 || cdcMin > cdcTarget || cdcTarget > cdcMax ||
            cdcMax > kMaxChunkSizeBytes ||
            fixedChunkSize > std::numeric_limits<size_t>::max() ||
            cdcMin > std::numeric_limits<size_t>::max() ||
            cdcTarget > std::numeric_limits<size_t>::max() ||
            cdcMax > std::numeric_limits<size_t>::max()) {
            throw std::runtime_error("Manifest contains invalid chunking configuration.");
        }
        EngineConfig config;
        const std::string strategy =
            requiredString(manifest, "selectedStrategy");
        if (!string_to_strategy(strategy, config.strategy)) {
            throw std::runtime_error("Manifest specifies an unsupported chunking strategy.");
        }
        config.fixedConfig.chunkSize = static_cast<size_t>(fixedChunkSize);
        config.cdcConfig = CDCConfig(static_cast<size_t>(cdcMin),
                                     static_cast<size_t>(cdcTarget),
                                     static_cast<size_t>(cdcMax));

        std::error_code ec;
        const uintmax_t bundleSize = fs::file_size(bundlePath, ec);
        if (ec || bundleSize < 8) {
            throw std::runtime_error("Could not read a valid RTPK payload bundle: " +
                                     bundlePath.string());
        }
        std::ifstream payload(bundlePath, std::ios::binary);
        char header[8] = {};
        payload.read(header, sizeof(header));
        if (!payload || std::string(header, header + 4) != "RTPK" ||
            static_cast<unsigned char>(header[4]) != 1 ||
            header[5] != 0 || header[6] != 0 || header[7] != 0) {
            throw std::runtime_error("Payload bundle has an invalid or unsupported RTPK header.");
        }
        for (const auto& entry : payloadChunks) {
            const PayloadChunk& chunk = entry.second;
            if (chunk.offset < 8 || chunk.offset > bundleSize ||
                chunk.storedSize > bundleSize - chunk.offset) {
                throw std::runtime_error("Payload chunk points outside the bundle file.");
            }
        }

        const fs::path outputRoot = fs::absolute(outputDirectory).lexically_normal();
        if (!validateOutputDirectory(outputRoot, error)) return false;

        std::unordered_map<std::string, SourceChunk> availableChunks;
        ProjectScanner scanner(outputRoot.string());
        ChunkEngine engine(config);
        const DirectoryNode existingTree = scanner.scan();
        const ProjectChunkResult existing =
            engine.processScannedProject(existingTree, outputRoot.string());
        for (const FileChunkResult& file : existing.files) {
            for (const ChunkMetadata& chunk : file.chunks) {
                const std::string key =
                    chunkKey(chunk.fingerprint, chunk.length, chunk.crc32);
                availableChunks.emplace(key, SourceChunk{
                    file.fileInfo.filePath, chunk.offset, chunk.length, chunk.crc32});
            }
        }

        static uint64_t stagingSequence = 0;
        do {
            stagingDirectory = outputRoot /
                (".retrotransfer-staging-" +
                 std::to_string(++stagingSequence));
            ec.clear();
        } while (fs::exists(stagingDirectory, ec));
        if (!fs::create_directory(stagingDirectory, ec) || ec) {
            throw std::runtime_error("Could not create reconstruction staging directory: " +
                                     ec.message());
        }

        size_t reusableCount = 0;
        size_t writtenChunkCount = 0;
        for (const RecipeFile& file : files) {
            const fs::path relative = validatedRelativePath(file.relativePath);
            const fs::path stagedFile = stagingDirectory / relative;
            fs::create_directories(stagedFile.parent_path(), ec);
            if (ec) throw std::runtime_error("Could not create staging directory: " + ec.message());
            std::ofstream output(stagedFile, std::ios::binary | std::ios::trunc);
            if (!output) throw std::runtime_error("Could not create staged file: " + stagedFile.string());

            for (const RecipeChunk& recipe : file.chunks) {
                const std::string key =
                    chunkKey(recipe.fingerprint, recipe.length, recipe.crc32);
                std::vector<uint8_t> bytes;
                const auto bundled = payloadChunks.find(key);
                if (bundled != payloadChunks.end()) {
                    const PayloadChunk& packed = bundled->second;
                    if (packed.crc32 != recipe.crc32) {
                        throw std::runtime_error("Manifest chunk CRC does not match payload metadata.");
                    }
                    std::string readError;
                    if (!readStoredChunk(payload, packed, bytes, readError)) {
                        throw std::runtime_error(readError);
                    }
                    ++writtenChunkCount;
                } else {
                    bool retrievedFromPersistent = false;
                    if (!persistentStorageDir.empty()) {
                        retro::storage::ChunkStorage pStorage(persistentStorageDir);
                        std::string pErr;
                        if (pStorage.retrieveChunk(recipe.fingerprint, bytes, pErr)) {
                            if (bytes.size() == recipe.length &&
                                rt_compute_crc32(bytes.data(), bytes.size()) == recipe.crc32) {
                                ++reusableCount;
                                retrievedFromPersistent = true;
                            }
                        }
                    }
                    if (!retrievedFromPersistent) {
                        const auto source = availableChunks.find(key);
                        if (source == availableChunks.end() ||
                            source->second.crc32 != recipe.crc32) {
                            throw std::runtime_error(
                                "Missing chunk is neither in the payload, destination, nor persistent storage: " +
                                recipe.fingerprint);
                        }
                        std::string readError;
                        if (!readSourceChunk(source->second, bytes, readError)) {
                            throw std::runtime_error(readError);
                        }
                        char fingerprint[33] = {};
                        rt_compute_fingerprint_hex(bytes.data(), bytes.size(), 42, fingerprint);
                        if (recipe.fingerprint != fingerprint) {
                            throw std::runtime_error("Reusable chunk fingerprint verification failed.");
                        }
                        ++reusableCount;
                    }
                }
                if (bytes.size() != recipe.length ||
                    rt_compute_crc32(bytes.data(), bytes.size()) != recipe.crc32) {
                    throw std::runtime_error("Reconstructed chunk failed size or CRC32 verification.");
                }
                char fingerprint[33] = {};
                rt_compute_fingerprint_hex(bytes.data(), bytes.size(), 42, fingerprint);
                if (recipe.fingerprint != fingerprint) {
                    throw std::runtime_error("Reconstructed chunk failed fingerprint verification.");
                }
                if (!bytes.empty()) {
                    output.write(reinterpret_cast<const char*>(bytes.data()),
                                 static_cast<std::streamsize>(bytes.size()));
                    if (!output) throw std::runtime_error("Failed writing staged file: " +
                                                          stagedFile.string());
                }
            }
            output.close();
            if (!output) throw std::runtime_error("Failed closing staged file: " +
                                                  stagedFile.string());
        }

        for (size_t index = 0; index < files.size(); ++index) {
            const fs::path relative = validatedRelativePath(files[index].relativePath);
            if (!ensureSafeParentDirectories(outputRoot, relative, error)) {
                throw std::runtime_error(error);
            }
            const fs::path target = outputRoot / relative;
            const fs::path stagedFile = stagingDirectory / relative;
            const fs::path backup = stagingDirectory /
                (".backup-" + std::to_string(index));
            const fs::file_status targetStatus = fs::symlink_status(target, ec);
            const bool targetExists = !ec && fs::exists(targetStatus);
            ec.clear();
            if (targetExists && (fs::is_symlink(targetStatus) ||
                                 !fs::is_regular_file(targetStatus))) {
                throw std::runtime_error("Refusing to overwrite a non-regular file: " +
                                         target.string());
            }
            if (targetExists) {
                fs::rename(target, backup, ec);
                if (ec) throw std::runtime_error("Could not stage existing file " +
                                                 target.string() + ": " + ec.message());
            }
            fs::rename(stagedFile, target, ec);
            if (ec) {
                const std::string commitError = "Could not write reconstructed file " +
                                                target.string() + ": " + ec.message();
                if (targetExists) {
                    std::error_code restoreError;
                    fs::rename(backup, target, restoreError);
                    if (restoreError) {
                        rollbackIncomplete = true;
                        throw std::runtime_error(commitError +
                            "; restoring the original also failed: " +
                            restoreError.message());
                    }
                }
                throw std::runtime_error(commitError);
            }
            if (targetExists) committed.emplace_back(target, backup);
            else createdTargets.push_back(target);
        }

        result.filesWritten = files.size();
        result.chunksWritten = writtenChunkCount;
        result.chunksReused = reusableCount;
        fs::remove_all(stagingDirectory, ec);
        if (ec) {
            error = "Reconstruction succeeded, but could not remove staging directory: " +
                    ec.message();
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        for (auto target = createdTargets.rbegin(); target != createdTargets.rend(); ++target) {
            std::error_code rollbackError;
            fs::remove(*target, rollbackError);
            if (rollbackError) {
                rollbackIncomplete = true;
                if (error.empty()) {
                    error = "Reconstruction failed and rollback could not remove " +
                            target->string() + ": " + rollbackError.message();
                }
            }
        }
        for (auto entry = committed.rbegin(); entry != committed.rend(); ++entry) {
            std::error_code rollbackError;
            fs::remove(entry->first, rollbackError);
            if (!rollbackError) fs::rename(entry->second, entry->first, rollbackError);
            if (rollbackError) {
                rollbackIncomplete = true;
                if (error.empty()) {
                    error = "Reconstruction failed and rollback could not restore " +
                            entry->first.string() + ": " + rollbackError.message();
                }
            }
        }
        if (!stagingDirectory.empty() && !rollbackIncomplete) {
            std::error_code cleanupError;
            fs::remove_all(stagingDirectory, cleanupError);
            if (cleanupError && error.empty()) {
                error = "Reconstruction failed and staging cleanup also failed: " +
                        cleanupError.message();
            }
        }
        if (rollbackIncomplete && error.empty()) {
            error = "Reconstruction failed; recovery files were preserved in " +
                    stagingDirectory.string();
        }
        if (rollbackIncomplete) {
            error += " Recovery files are preserved in " + stagingDirectory.string();
        }
        if (error.empty()) error = exception.what();
        return false;
    }
}

bool ReconstructionEngine::extractPayloadChunks(const std::string& manifestPath,
                                                const std::string& payloadPath,
                                                std::vector<ExtractedChunk>& outChunks,
                                                std::string& error) {
    outChunks.clear();
    error.clear();

    try {
        std::string manifestText;
        if (!readFileBytes(manifestPath, manifestText, error)) return false;
        const JsonValue manifest = JsonParser(manifestText).parse();

        const auto selectiveIt = manifest.object.find("selectiveCompression");
        if (selectiveIt == manifest.object.end() || selectiveIt->second.type != JsonValue::Type::Object) {
            return true;
        }
        const JsonValue& compression = selectiveIt->second;
        const auto payloadChunksIt = compression.object.find("payloadChunks");
        if (payloadChunksIt == compression.object.end() || payloadChunksIt->second.type != JsonValue::Type::Array) {
            return true;
        }

        fs::path bundlePath;
        if (!payloadPath.empty()) {
            bundlePath = payloadPath;
        } else {
            const std::string payloadName = requiredString(compression, "payloadFile");
            bundlePath = fs::path(manifestPath).parent_path() / payloadName;
        }

        std::error_code ec;
        const uintmax_t bundleSize = fs::file_size(bundlePath, ec);
        if (ec || bundleSize < 8) {
            error = "Could not read valid RTPK payload bundle: " + bundlePath.string();
            return false;
        }

        std::ifstream payload(bundlePath, std::ios::binary);
        char header[8] = {};
        payload.read(header, sizeof(header));
        if (!payload || std::string(header, header + 4) != "RTPK" ||
            static_cast<unsigned char>(header[4]) != 1 ||
            header[5] != 0 || header[6] != 0 || header[7] != 0) {
            error = "Payload bundle has an invalid or unsupported RTPK header.";
            return false;
        }

        for (const JsonValue& entry : payloadChunksIt->second.array) {
            PayloadChunk chunk;
            chunk.fingerprint = requiredString(entry, "fingerprint");
            chunk.originalSize = requiredUnsigned(entry, "originalSize");
            chunk.offset = requiredUnsigned(entry, "payloadOffset");
            chunk.storedSize = requiredUnsigned(entry, "storedSize");
            chunk.crc32 = parseCrc32(entry, "crc32");
            chunk.codec = requiredString(entry, "codec");

            if (!parseFingerprint(chunk.fingerprint) ||
                (chunk.codec != "raw" && chunk.codec != "huffman")) {
                error = "Manifest contains invalid payload chunk metadata.";
                return false;
            }

            if (chunk.offset < 8 || chunk.offset > bundleSize ||
                chunk.storedSize > bundleSize - chunk.offset) {
                error = "Payload chunk points outside the bundle file.";
                return false;
            }

            std::vector<uint8_t> decodedBytes;
            if (!readStoredChunk(payload, chunk, decodedBytes, error)) {
                return false;
            }

            ExtractedChunk ext;
            ext.fingerprint = chunk.fingerprint;
            ext.originalSize = chunk.originalSize;
            ext.crc32 = chunk.crc32;
            ext.codec = chunk.codec;
            ext.data = std::move(decodedBytes);
            outChunks.push_back(std::move(ext));
        }

        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

