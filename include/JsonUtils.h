#pragma once

#include <string>
#include <sstream>
#include <vector>
#include <map>
#include <stdexcept>
#include <cstdint>
#include <iomanip>
#include <type_traits>

namespace ia64 {
namespace json {

/**
 * Simple JSON utility for serializing VM configurations
 * 
 * Provides basic JSON serialization without external dependencies.
 * Not a full JSON parser - designed specifically for VM configuration.
 */
class JsonBuilder {
private:
    std::ostringstream buffer;
    std::vector<bool> isFirstElement;
    int indentLevel;
    
    void writeIndent() {
        for (int i = 0; i < indentLevel; ++i) {
            buffer << "  ";
        }
    }
    
    void writeComma() {
        if (!isFirstElement.empty() && !isFirstElement.back()) {
            buffer << ",\n";
        } else if (!isFirstElement.empty()) {
            buffer << "\n";
            isFirstElement.back() = false;
        }
    }
    
    std::string escapeString(const std::string& str) const {
        std::ostringstream escaped;
        for (char c : str) {
            switch (c) {
                case '"': escaped << "\\\""; break;
                case '\\': escaped << "\\\\"; break;
                case '\b': escaped << "\\b"; break;
                case '\f': escaped << "\\f"; break;
                case '\n': escaped << "\\n"; break;
                case '\r': escaped << "\\r"; break;
                case '\t': escaped << "\\t"; break;
                default:
                    if (c < 32) {
                        escaped << "\\u" << std::hex << std::setw(4) 
                               << std::setfill('0') << static_cast<int>(c);
                    } else {
                        escaped << c;
                    }
            }
        }
        return escaped.str();
    }

public:
    JsonBuilder() : indentLevel(0) {}
    
    void beginObject() {
        buffer << "{";
        indentLevel++;
        isFirstElement.push_back(true);
    }
    
    void endObject() {
        indentLevel--;
        isFirstElement.pop_back();
        buffer << "\n";
        writeIndent();
        buffer << "}";
    }
    
    void beginArray() {
        buffer << "[";
        indentLevel++;
        isFirstElement.push_back(true);
    }
    
    void endArray() {
        indentLevel--;
        isFirstElement.pop_back();
        buffer << "\n";
        writeIndent();
        buffer << "]";
    }
    
    void writeKey(const std::string& key) {
        writeComma();
        writeIndent();
        buffer << "\"" << escapeString(key) << "\": ";
    }
    
    void writeString(const std::string& key, const std::string& value) {
        writeKey(key);
        buffer << "\"" << escapeString(value) << "\"";
    }
    
    // Template for all numeric types to avoid overload conflicts
    template<typename T>
    typename std::enable_if<std::is_arithmetic<T>::value && !std::is_same<T, bool>::value, void>::type
    writeNumber(const std::string& key, T value) {
        writeKey(key);
        buffer << value;
    }
    
    void writeBool(const std::string& key, bool value) {
        writeKey(key);
        buffer << (value ? "true" : "false");
    }
    
    void writeObjectValue(const std::string& key) {
        writeKey(key);
    }
    
    void writeArrayValue(const std::string& key) {
        writeKey(key);
    }
    
    void writeArrayElement() {
        writeComma();
        writeIndent();
    }
    
    std::string toString() const {
        return buffer.str();
    }
};

/**
 * Simple JSON parser for loading VM configurations
 */
class JsonParser {
private:
    std::string json;
    size_t pos;
    
    void skipWhitespace() {
        while (pos < json.length() && 
               (json[pos] == ' ' || json[pos] == '\t' || 
                json[pos] == '\n' || json[pos] == '\r')) {
            pos++;
        }
    }
    
    char peek() {
        skipWhitespace();
        if (pos >= json.length()) return '\0';
        return json[pos];
    }
    
    char consume() {
        skipWhitespace();
        if (pos >= json.length()) return '\0';
        return json[pos++];
    }
    
    bool match(char c) {
        skipWhitespace();
        if (pos < json.length() && json[pos] == c) {
            pos++;
            return true;
        }
        return false;
    }
    
    std::string parseString() {
        if (consume() != '"') {
            throw std::runtime_error("Expected '\"' at position " + std::to_string(pos));
        }
        
        std::ostringstream result;
        while (pos < json.length() && json[pos] != '"') {
            if (json[pos] == '\\') {
                pos++;
                if (pos >= json.length()) {
                    throw std::runtime_error("Unexpected end of string");
                }
                switch (json[pos]) {
                    case '"': result << '"'; break;
                    case '\\': result << '\\'; break;
                    case '/': result << '/'; break;
                    case 'b': result << '\b'; break;
                    case 'f': result << '\f'; break;
                    case 'n': result << '\n'; break;
                    case 'r': result << '\r'; break;
                    case 't': result << '\t'; break;
                    default:
                        throw std::runtime_error("Invalid escape sequence");
                }
                pos++;
            } else {
                result << json[pos++];
            }
        }
        
        if (pos >= json.length()) {
            throw std::runtime_error("Unterminated string");
        }
        pos++; // consume closing '"'
        
        return result.str();
    }
    
    uint64_t parseNumber() {
        skipWhitespace();
        size_t start = pos;
        
        if (json[pos] == '-') pos++;
        
        while (pos < json.length() && (json[pos] >= '0' && json[pos] <= '9')) {
            pos++;
        }
        
        std::string numStr = json.substr(start, pos - start);
        return std::stoull(numStr);
    }
    
    bool parseBool() {
        skipWhitespace();
        if (json.substr(pos, 4) == "true") {
            pos += 4;
            return true;
        } else if (json.substr(pos, 5) == "false") {
            pos += 5;
            return false;
        }
        throw std::runtime_error("Expected 'true' or 'false' at position " + std::to_string(pos));
    }

public:
    JsonParser(const std::string& jsonStr) : json(jsonStr), pos(0) {}
    
    bool hasNext() {
        return peek() != '\0';
    }
    
    void expectObjectStart() {
        if (consume() != '{') {
            throw std::runtime_error("Expected '{' at position " + std::to_string(pos));
        }
    }
    
    void expectObjectEnd() {
        if (consume() != '}') {
            throw std::runtime_error("Expected '}' at position " + std::to_string(pos));
        }
    }
    
    void expectArrayStart() {
        if (consume() != '[') {
            throw std::runtime_error("Expected '[' at position " + std::to_string(pos));
        }
    }
    
    void expectArrayEnd() {
        if (consume() != ']') {
            throw std::runtime_error("Expected ']' at position " + std::to_string(pos));
        }
    }
    
    bool tryConsumeComma() {
        return match(',');
    }
    
    std::string parseKey() {
        std::string key = parseString();
        if (consume() != ':') {
            throw std::runtime_error("Expected ':' after key at position " + std::to_string(pos));
        }
        return key;
    }
    
    std::string readString() {
        return parseString();
    }
    
    uint64_t readNumber() {
        return parseNumber();
    }
    
    bool readBool() {
        return parseBool();
    }
    
    bool isObjectStart() {
        return peek() == '{';
    }
    
    bool isArrayStart() {
        return peek() == '[';
    }
    
    bool isObjectEnd() {
        return peek() == '}';
    }
    
    bool isArrayEnd() {
        return peek() == ']';
    }
};

} // namespace json
} // namespace ia64
