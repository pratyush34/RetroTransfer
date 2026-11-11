#include "storage/chunk_record.h"

#include <algorithm>
#include <cctype>

namespace retro::storage {

static bool isValidFingerprint(const std::string& fp) {
    if (fp.size() != 32) return false;
    return std::all_of(fp.begin(), fp.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0;
    });
}

bool ChunkRecord::isValid() const {
    if (!isValidFingerprint(fingerprint)) return false;
    if (filename.empty()) return false;
    // Filename must not contain path traversal characters
    if (filename.find('/') != std::string::npos ||
        filename.find('\\') != std::string::npos ||
        filename.find("..") != std::string::npos) {
        return false;
    }
    return true;
}

StorageJsonValue ChunkRecord::toJson() const {
    StorageJsonValue obj = StorageJsonValue::makeObject();
    obj.object["fingerprint"] = StorageJsonValue::makeString(fingerprint);
    obj.object["size"] = StorageJsonValue::makeNumber(size);
    obj.object["crc32"] = StorageJsonValue::makeNumber(static_cast<uint64_t>(crc32));
    obj.object["filename"] = StorageJsonValue::makeString(filename);
    obj.object["status"] = StorageJsonValue::makeString(status);
    obj.object["version"] = StorageJsonValue::makeNumber(static_cast<uint64_t>(version));
    obj.object["createdAt"] = StorageJsonValue::makeNumber(createdAt);
    return obj;
}

bool ChunkRecord::fromJson(const StorageJsonValue& json, ChunkRecord& outRecord, std::string& error) {
    if (!json.isObject()) {
        error = "Expected JSON object for chunk record";
        return false;
    }

    auto itFp = json.object.find("fingerprint");
    if (itFp == json.object.end() || !itFp->second.isString()) {
        error = "Missing or invalid 'fingerprint' field in chunk record";
        return false;
    }
    outRecord.fingerprint = itFp->second.asString();

    auto itSize = json.object.find("size");
    if (itSize == json.object.end() || !itSize->second.isNumber()) {
        error = "Missing or invalid 'size' field in chunk record";
        return false;
    }
    outRecord.size = itSize->second.asUint64();

    auto itCrc = json.object.find("crc32");
    if (itCrc == json.object.end() || !itCrc->second.isNumber()) {
        error = "Missing or invalid 'crc32' field in chunk record";
        return false;
    }
    outRecord.crc32 = static_cast<uint32_t>(itCrc->second.asUint64());

    auto itFile = json.object.find("filename");
    if (itFile == json.object.end() || !itFile->second.isString()) {
        error = "Missing or invalid 'filename' field in chunk record";
        return false;
    }
    outRecord.filename = itFile->second.asString();

    auto itStatus = json.object.find("status");
    if (itStatus != json.object.end() && itStatus->second.isString()) {
        outRecord.status = itStatus->second.asString();
    } else {
        outRecord.status = "available";
    }

    auto itVer = json.object.find("version");
    if (itVer != json.object.end() && itVer->second.isNumber()) {
        outRecord.version = static_cast<uint32_t>(itVer->second.asUint64());
    } else {
        outRecord.version = 1;
    }

    auto itTime = json.object.find("createdAt");
    if (itTime != json.object.end() && itTime->second.isNumber()) {
        outRecord.createdAt = itTime->second.asUint64();
    } else {
        outRecord.createdAt = 0;
    }

    if (!outRecord.isValid()) {
        error = "Chunk record failed validation check";
        return false;
    }

    return true;
}

} // namespace retro::storage
