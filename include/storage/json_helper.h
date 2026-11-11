#ifndef RETROTRANSFER_STORAGE_JSON_HELPER_H
#define RETROTRANSFER_STORAGE_JSON_HELPER_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace retro::storage {

struct StorageJsonValue {
    enum class Type { Null, Boolean, Number, String, Array, Object };
    Type type = Type::Null;
    std::string text;
    std::vector<StorageJsonValue> array;
    std::map<std::string, StorageJsonValue> object;

    bool isNull() const { return type == Type::Null; }
    bool isBool() const { return type == Type::Boolean; }
    bool isNumber() const { return type == Type::Number; }
    bool isString() const { return type == Type::String; }
    bool isArray() const { return type == Type::Array; }
    bool isObject() const { return type == Type::Object; }

    bool asBool(bool defaultValue = false) const;
    uint64_t asUint64(uint64_t defaultValue = 0) const;
    int64_t asInt64(int64_t defaultValue = 0) const;
    double asDouble(double defaultValue = 0.0) const;
    const std::string& asString() const { return text; }

    static StorageJsonValue makeNull();
    static StorageJsonValue makeBool(bool val);
    static StorageJsonValue makeNumber(uint64_t val);
    static StorageJsonValue makeNumber(int64_t val);
    static StorageJsonValue makeNumber(double val);
    static StorageJsonValue makeString(const std::string& val);
    static StorageJsonValue makeArray();
    static StorageJsonValue makeObject();
};

class StorageJsonParser {
public:
    explicit StorageJsonParser(const std::string& input);
    bool parse(StorageJsonValue& outValue, std::string& outError);

private:
    const std::string& input_;
    size_t position_ = 0;

    void skipWhitespace();
    bool consume(char expected);
    bool parseValue(StorageJsonValue& outValue, std::string& outError);
    bool parseString(std::string& outString, std::string& outError);
    bool parseNumber(StorageJsonValue& outValue, std::string& outError);
    bool parseObject(StorageJsonValue& outValue, std::string& outError);
    bool parseArray(StorageJsonValue& outValue, std::string& outError);
    uint32_t parseHexQuad(bool& ok);
    static void appendUtf8(std::string& output, uint32_t codePoint);
};

class StorageJsonWriter {
public:
    static std::string serialize(const StorageJsonValue& value, int indentLevel = 0);
    static void escapeString(const std::string& input, std::string& output);
};

bool readJsonFile(const std::string& filePath, StorageJsonValue& outValue, std::string& outError);
bool safeWriteJsonFile(const std::string& filePath, const StorageJsonValue& value, std::string& outError);

} // namespace retro::storage

#endif // RETROTRANSFER_STORAGE_JSON_HELPER_H
