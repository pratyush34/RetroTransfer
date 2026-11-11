#include "storage/json_helper.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace retro::storage {

bool StorageJsonValue::asBool(bool defaultValue) const {
    if (type == Type::Boolean) {
        return text != "false";
    }
    return defaultValue;
}

uint64_t StorageJsonValue::asUint64(uint64_t defaultValue) const {
    if (type == Type::Number && !text.empty() && text[0] != '-') {
        try {
            return std::stoull(text);
        } catch (...) {}
    }
    return defaultValue;
}

int64_t StorageJsonValue::asInt64(int64_t defaultValue) const {
    if (type == Type::Number && !text.empty()) {
        try {
            return std::stoll(text);
        } catch (...) {}
    }
    return defaultValue;
}

double StorageJsonValue::asDouble(double defaultValue) const {
    if (type == Type::Number && !text.empty()) {
        try {
            return std::stod(text);
        } catch (...) {}
    }
    return defaultValue;
}

StorageJsonValue StorageJsonValue::makeNull() {
    StorageJsonValue v;
    v.type = Type::Null;
    return v;
}

StorageJsonValue StorageJsonValue::makeBool(bool val) {
    StorageJsonValue v;
    v.type = Type::Boolean;
    v.text = val ? "true" : "false";
    return v;
}

StorageJsonValue StorageJsonValue::makeNumber(uint64_t val) {
    StorageJsonValue v;
    v.type = Type::Number;
    v.text = std::to_string(val);
    return v;
}

StorageJsonValue StorageJsonValue::makeNumber(int64_t val) {
    StorageJsonValue v;
    v.type = Type::Number;
    v.text = std::to_string(val);
    return v;
}

StorageJsonValue StorageJsonValue::makeNumber(double val) {
    StorageJsonValue v;
    v.type = Type::Number;
    std::ostringstream ss;
    ss << val;
    v.text = ss.str();
    return v;
}

StorageJsonValue StorageJsonValue::makeString(const std::string& val) {
    StorageJsonValue v;
    v.type = Type::String;
    v.text = val;
    return v;
}

StorageJsonValue StorageJsonValue::makeArray() {
    StorageJsonValue v;
    v.type = Type::Array;
    return v;
}

StorageJsonValue StorageJsonValue::makeObject() {
    StorageJsonValue v;
    v.type = Type::Object;
    return v;
}

StorageJsonParser::StorageJsonParser(const std::string& input)
    : input_(input), position_(0) {
}

void StorageJsonParser::skipWhitespace() {
    while (position_ < input_.size() &&
           (input_[position_] == ' ' || input_[position_] == '\t' ||
            input_[position_] == '\r' || input_[position_] == '\n')) {
        ++position_;
    }
}

bool StorageJsonParser::consume(char expected) {
    skipWhitespace();
    if (position_ < input_.size() && input_[position_] == expected) {
        ++position_;
        return true;
    }
    return false;
}

void StorageJsonParser::appendUtf8(std::string& output, uint32_t codePoint) {
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

uint32_t StorageJsonParser::parseHexQuad(bool& ok) {
    ok = false;
    if (input_.size() - position_ < 4) return 0;
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        const char ch = input_[position_++];
        value <<= 4;
        if (ch >= '0' && ch <= '9') value |= static_cast<uint32_t>(ch - '0');
        else if (ch >= 'a' && ch <= 'f') value |= static_cast<uint32_t>(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') value |= static_cast<uint32_t>(ch - 'A' + 10);
        else return 0;
    }
    ok = true;
    return value;
}

bool StorageJsonParser::parseString(std::string& outString, std::string& outError) {
    if (!consume('"')) {
        outError = "Expected '\"' at position " + std::to_string(position_);
        return false;
    }
    outString.clear();
    while (position_ < input_.size()) {
        const unsigned char ch = static_cast<unsigned char>(input_[position_++]);
        if (ch == '"') return true;
        if (ch < 0x20) {
            outError = "Unescaped control character in string at " + std::to_string(position_);
            return false;
        }
        if (ch != '\\') {
            outString.push_back(static_cast<char>(ch));
            continue;
        }
        if (position_ == input_.size()) {
            outError = "Incomplete escape sequence at end of input";
            return false;
        }
        const char esc = input_[position_++];
        switch (esc) {
            case '"': outString.push_back('"'); break;
            case '\\': outString.push_back('\\'); break;
            case '/': outString.push_back('/'); break;
            case 'b': outString.push_back('\b'); break;
            case 'f': outString.push_back('\f'); break;
            case 'n': outString.push_back('\n'); break;
            case 'r': outString.push_back('\r'); break;
            case 't': outString.push_back('\t'); break;
            case 'u': {
                bool ok = false;
                uint32_t codePoint = parseHexQuad(ok);
                if (!ok) {
                    outError = "Invalid Unicode escape sequence";
                    return false;
                }
                if (codePoint >= 0xd800 && codePoint <= 0xdbff) {
                    if (input_.size() - position_ < 6 ||
                        input_[position_] != '\\' || input_[position_ + 1] != 'u') {
                        outError = "Unpaired high surrogate in Unicode escape";
                        return false;
                    }
                    position_ += 2;
                    const uint32_t low = parseHexQuad(ok);
                    if (!ok || low < 0xdc00 || low > 0xdfff) {
                        outError = "Invalid low surrogate in Unicode escape";
                        return false;
                    }
                    codePoint = 0x10000 + ((codePoint - 0xd800) << 10) + (low - 0xdc00);
                }
                appendUtf8(outString, codePoint);
                break;
            }
            default:
                outError = std::string("Invalid escape sequence: \\") + esc;
                return false;
        }
    }
    outError = "Unterminated string at end of input";
    return false;
}

bool StorageJsonParser::parseNumber(StorageJsonValue& outValue, std::string& outError) {
    skipWhitespace();
    const size_t start = position_;
    if (position_ < input_.size() && input_[position_] == '-') ++position_;
    if (position_ == input_.size()) {
        outError = "Invalid number syntax at position " + std::to_string(start);
        return false;
    }
    if (input_[position_] == '0') {
        ++position_;
    } else if (input_[position_] >= '1' && input_[position_] <= '9') {
        while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
            ++position_;
        }
    } else {
        outError = "Invalid character in number at position " + std::to_string(position_);
        return false;
    }
    if (position_ < input_.size() && input_[position_] == '.') {
        ++position_;
        const size_t fracStart = position_;
        while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
            ++position_;
        }
        if (fracStart == position_) {
            outError = "Invalid decimal fraction in number at position " + std::to_string(position_);
            return false;
        }
    }
    if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
        ++position_;
        if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) {
            ++position_;
        }
        const size_t expStart = position_;
        while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
            ++position_;
        }
        if (expStart == position_) {
            outError = "Invalid exponent in number at position " + std::to_string(position_);
            return false;
        }
    }
    outValue.type = StorageJsonValue::Type::Number;
    outValue.text = input_.substr(start, position_ - start);
    return true;
}

bool StorageJsonParser::parseObject(StorageJsonValue& outValue, std::string& outError) {
    if (!consume('{')) {
        outError = "Expected '{' at position " + std::to_string(position_);
        return false;
    }
    outValue.type = StorageJsonValue::Type::Object;
    outValue.object.clear();
    if (consume('}')) return true;

    while (true) {
        std::string key;
        if (!parseString(key, outError)) return false;
        if (!consume(':')) {
            outError = "Expected ':' after object key at position " + std::to_string(position_);
            return false;
        }
        StorageJsonValue val;
        if (!parseValue(val, outError)) return false;
        outValue.object[key] = std::move(val);

        if (consume('}')) return true;
        if (!consume(',')) {
            outError = "Expected ',' or '}' in object at position " + std::to_string(position_);
            return false;
        }
    }
}

bool StorageJsonParser::parseArray(StorageJsonValue& outValue, std::string& outError) {
    if (!consume('[')) {
        outError = "Expected '[' at position " + std::to_string(position_);
        return false;
    }
    outValue.type = StorageJsonValue::Type::Array;
    outValue.array.clear();
    if (consume(']')) return true;

    while (true) {
        StorageJsonValue elem;
        if (!parseValue(elem, outError)) return false;
        outValue.array.push_back(std::move(elem));

        if (consume(']')) return true;
        if (!consume(',')) {
            outError = "Expected ',' or ']' in array at position " + std::to_string(position_);
            return false;
        }
    }
}

bool StorageJsonParser::parseValue(StorageJsonValue& outValue, std::string& outError) {
    skipWhitespace();
    if (position_ >= input_.size()) {
        outError = "Unexpected end of JSON input";
        return false;
    }
    char ch = input_[position_];
    if (ch == '"') {
        std::string s;
        if (!parseString(s, outError)) return false;
        outValue.type = StorageJsonValue::Type::String;
        outValue.text = std::move(s);
        return true;
    }
    if (ch == '{') {
        return parseObject(outValue, outError);
    }
    if (ch == '[') {
        return parseArray(outValue, outError);
    }
    if (input_.compare(position_, 4, "true") == 0) {
        position_ += 4;
        outValue.type = StorageJsonValue::Type::Boolean;
        outValue.text = "true";
        return true;
    }
    if (input_.compare(position_, 5, "false") == 0) {
        position_ += 5;
        outValue.type = StorageJsonValue::Type::Boolean;
        outValue.text = "false";
        return true;
    }
    if (input_.compare(position_, 4, "null") == 0) {
        position_ += 4;
        outValue.type = StorageJsonValue::Type::Null;
        outValue.text = "null";
        return true;
    }
    return parseNumber(outValue, outError);
}

bool StorageJsonParser::parse(StorageJsonValue& outValue, std::string& outError) {
    if (!parseValue(outValue, outError)) return false;
    skipWhitespace();
    if (position_ != input_.size()) {
        outError = "Unexpected trailing characters after JSON data at position " + std::to_string(position_);
        return false;
    }
    return true;
}

void StorageJsonWriter::escapeString(const std::string& input, std::string& output) {
    output.push_back('"');
    for (char c : input) {
        switch (c) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char hex[8];
                    std::snprintf(hex, sizeof(hex), "\\u%04x", static_cast<unsigned char>(c));
                    output += hex;
                } else {
                    output.push_back(c);
                }
                break;
        }
    }
    output.push_back('"');
}

std::string StorageJsonWriter::serialize(const StorageJsonValue& value, int indentLevel) {
    std::string indent(indentLevel * 2, ' ');
    std::string subIndent((indentLevel + 1) * 2, ' ');

    switch (value.type) {
        case StorageJsonValue::Type::Null:
            return "null";
        case StorageJsonValue::Type::Boolean:
            return value.text.empty() ? "false" : value.text;
        case StorageJsonValue::Type::Number:
            return value.text.empty() ? "0" : value.text;
        case StorageJsonValue::Type::String: {
            std::string out;
            escapeString(value.text, out);
            return out;
        }
        case StorageJsonValue::Type::Array: {
            if (value.array.empty()) return "[]";
            std::string out = "[\n";
            for (size_t i = 0; i < value.array.size(); ++i) {
                out += subIndent + serialize(value.array[i], indentLevel + 1);
                if (i + 1 < value.array.size()) out += ",";
                out += "\n";
            }
            out += indent + "]";
            return out;
        }
        case StorageJsonValue::Type::Object: {
            if (value.object.empty()) return "{}";
            std::string out = "{\n";
            size_t idx = 0;
            for (const auto& kv : value.object) {
                std::string keyEsc;
                escapeString(kv.first, keyEsc);
                out += subIndent + keyEsc + ": " + serialize(kv.second, indentLevel + 1);
                if (++idx < value.object.size()) out += ",";
                out += "\n";
            }
            out += indent + "}";
            return out;
        }
    }
    return "null";
}

bool readJsonFile(const std::string& filePath, StorageJsonValue& outValue, std::string& outError) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        outError = "Could not open JSON file: " + filePath;
        return false;
    }
    std::stringstream ss;
    ss << file.rdbuf();
    std::string content = ss.str();
    if (content.empty()) {
        outError = "JSON file is empty: " + filePath;
        return false;
    }
    StorageJsonParser parser(content);
    return parser.parse(outValue, outError);
}

bool safeWriteJsonFile(const std::string& filePath, const StorageJsonValue& value, std::string& outError) {
    std::string content = StorageJsonWriter::serialize(value, 0) + "\n";
    std::string tempPath = filePath + ".tmp";

    try {
        fs::path p(filePath);
        if (p.has_parent_path()) {
            std::error_code ec;
            fs::create_directories(p.parent_path(), ec);
        }

        std::ofstream tempFile(tempPath, std::ios::binary | std::ios::trunc);
        if (!tempFile.is_open()) {
            outError = "Could not open temporary JSON file for writing: " + tempPath;
            return false;
        }

        tempFile.write(content.data(), static_cast<std::streamsize>(content.size()));
        tempFile.flush();
        if (!tempFile.good()) {
            outError = "Failed to write complete JSON data to: " + tempPath;
            tempFile.close();
            std::error_code ec;
            fs::remove(tempPath, ec);
            return false;
        }
        tempFile.close();

        std::error_code ec;
        fs::remove(filePath, ec); // Remove existing file before renaming (Windows safe)
        ec.clear();
        fs::rename(tempPath, filePath, ec);
        if (ec) {
            outError = "Failed to atomically rename " + tempPath + " to " + filePath + ": " + ec.message();
            fs::remove(tempPath, ec);
            return false;
        }
    } catch (const std::exception& ex) {
        outError = std::string("Exception during safe write: ") + ex.what();
        return false;
    }

    return true;
}

} // namespace retro::storage
