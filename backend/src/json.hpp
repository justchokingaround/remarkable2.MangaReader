#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace Json {

enum class Type { Null, Bool, Number, String, Array, Object };

class Value {
public:
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value>;

private:
    Type type_ = Type::Null;
    bool boolVal_ = false;
    double numVal_ = 0.0;
    std::string strVal_;
    Array arrVal_;
    Object objVal_;

public:
    Value() : type_(Type::Null) {}
    Value(std::nullptr_t) : type_(Type::Null) {}
    Value(bool b) : type_(Type::Bool), boolVal_(b) {}
    Value(int i) : type_(Type::Number), numVal_(static_cast<double>(i)) {}
    Value(int64_t i) : type_(Type::Number), numVal_(static_cast<double>(i)) {}
    Value(size_t u) : type_(Type::Number), numVal_(static_cast<double>(u)) {}
    Value(double d) : type_(Type::Number), numVal_(d) {}
    Value(const char* s) : type_(Type::String), strVal_(s ? s : "") {}
    Value(std::string s) : type_(Type::String), strVal_(std::move(s)) {}
    Value(Array a) : type_(Type::Array), arrVal_(std::move(a)) {}
    Value(Object o) : type_(Type::Object), objVal_(std::move(o)) {}

    static Value array() { return Value(Array{}); }
    static Value object() { return Value(Object{}); }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool boolean(bool def = false) const {
        if (type_ == Type::Bool) return boolVal_;
        if (type_ == Type::Number) return numVal_ != 0.0;
        return def;
    }

    double num(double def = 0.0) const {
        if (type_ == Type::Number) return numVal_;
        if (type_ == Type::String && !strVal_.empty()) {
            char* end = nullptr;
            double v = std::strtod(strVal_.c_str(), &end);
            if (end != strVal_.c_str()) return v;
        }
        return def;
    }

    int64_t integer(int64_t def = 0) const {
        if (type_ == Type::Number) return static_cast<int64_t>(numVal_);
        if (type_ == Type::String && !strVal_.empty()) {
            char* end = nullptr;
            long long v = std::strtoll(strVal_.c_str(), &end, 10);
            if (end != strVal_.c_str()) return static_cast<int64_t>(v);
        }
        return def;
    }

    std::string str(const std::string& def = "") const {
        if (type_ == Type::String) return strVal_;
        if (type_ == Type::Number) {
            if (std::floor(numVal_) == numVal_ && std::fabs(numVal_) < 1e15) {
                return std::to_string(static_cast<int64_t>(numVal_));
            }
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.6g", numVal_);
            return buf;
        }
        if (type_ == Type::Bool) return boolVal_ ? "true" : "false";
        return def;
    }

    size_t size() const {
        if (type_ == Type::Array) return arrVal_.size();
        if (type_ == Type::Object) return objVal_.size();
        return 0;
    }

    bool empty() const { return size() == 0; }

    void push(Value v) {
        if (type_ != Type::Array) {
            type_ = Type::Array;
            arrVal_.clear();
        }
        arrVal_.push_back(std::move(v));
    }

    bool has(const std::string& key) const {
        if (type_ != Type::Object) return false;
        auto it = objVal_.find(key);
        return it != objVal_.end() && !it->second.isNull();
    }

    void erase(const std::string& key) {
        if (type_ == Type::Object) {
            objVal_.erase(key);
        }
    }

    Value& operator[](const std::string& key) {
        if (type_ != Type::Object) {
            type_ = Type::Object;
            objVal_.clear();
        }
        return objVal_[key];
    }

    const Value& operator[](const std::string& key) const {
        static const Value kNull;
        if (type_ != Type::Object) return kNull;
        auto it = objVal_.find(key);
        return it == objVal_.end() ? kNull : it->second;
    }

    Value& operator[](size_t idx) {
        if (type_ != Type::Array || idx >= arrVal_.size()) {
            throw std::out_of_range("Json::Value array index out of range");
        }
        return arrVal_[idx];
    }

    const Value& operator[](size_t idx) const {
        static const Value kNull;
        if (type_ != Type::Array || idx >= arrVal_.size()) return kNull;
        return arrVal_[idx];
    }

    const Array& asArray() const {
        static const Array kEmpty;
        return type_ == Type::Array ? arrVal_ : kEmpty;
    }

    Array& asArrayMut() {
        if (type_ != Type::Array) {
            type_ = Type::Array;
            arrVal_.clear();
        }
        return arrVal_;
    }

    const Object& asObject() const {
        static const Object kEmpty;
        return type_ == Type::Object ? objVal_ : kEmpty;
    }

    Object& asObjectMut() {
        if (type_ != Type::Object) {
            type_ = Type::Object;
            objVal_.clear();
        }
        return objVal_;
    }
};

namespace detail {

inline void appendUtf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

class Parser {
    const std::string& src_;
    size_t pos_ = 0;

    void skipWs() {
        while (pos_ < src_.size()) {
            unsigned char c = static_cast<unsigned char>(src_[pos_]);
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    uint16_t parseHex4() {
        if (pos_ + 4 > src_.size()) return 0;
        uint16_t v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = src_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (c - '0');
            else if (c >= 'a' && c <= 'f') v |= (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (c - 'A' + 10);
        }
        return v;
    }

    std::string parseStringRaw(bool& ok) {
        std::string out;
        if (pos_ >= src_.size() || src_[pos_] != '"') {
            ok = false;
            return out;
        }
        ++pos_;
        while (pos_ < src_.size()) {
            char c = src_[pos_++];
            if (c == '"') {
                return out;
            }
            if (c == '\\') {
                if (pos_ >= src_.size()) {
                    ok = false;
                    return out;
                }
                char esc = src_[pos_++];
                switch (esc) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        uint32_t cp = parseHex4();
                        if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 6 <= src_.size() &&
                            src_[pos_] == '\\' && src_[pos_ + 1] == 'u') {
                            pos_ += 2;
                            uint32_t low = parseHex4();
                            if (low >= 0xDC00 && low <= 0xDFFF) {
                                cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
                            }
                        }
                        appendUtf8(out, cp);
                        break;
                    }
                    default:
                        out.push_back(esc);
                        break;
                }
            } else {
                out.push_back(c);
            }
        }
        ok = false;
        return out;
    }

public:
    explicit Parser(const std::string& s) : src_(s) {}

    Value parseValue(bool& ok, int depth = 0) {
        if (depth > 128) {
            ok = false;
            return Value();
        }
        skipWs();
        if (pos_ >= src_.size()) {
            ok = false;
            return Value();
        }
        char c = src_[pos_];
        if (c == '"') {
            return Value(parseStringRaw(ok));
        }
        if (c == '{') {
            ++pos_;
            Value::Object obj;
            skipWs();
            if (pos_ < src_.size() && src_[pos_] == '}') {
                ++pos_;
                return Value(std::move(obj));
            }
            while (pos_ < src_.size()) {
                skipWs();
                std::string key = parseStringRaw(ok);
                if (!ok) return Value();
                skipWs();
                if (pos_ >= src_.size() || src_[pos_] != ':') {
                    ok = false;
                    return Value();
                }
                ++pos_;
                Value val = parseValue(ok, depth + 1);
                if (!ok) return Value();
                obj[std::move(key)] = std::move(val);
                skipWs();
                if (pos_ < src_.size() && src_[pos_] == ',') {
                    ++pos_;
                    continue;
                }
                if (pos_ < src_.size() && src_[pos_] == '}') {
                    ++pos_;
                    return Value(std::move(obj));
                }
                ok = false;
                return Value();
            }
            ok = false;
            return Value();
        }
        if (c == '[') {
            ++pos_;
            Value::Array arr;
            skipWs();
            if (pos_ < src_.size() && src_[pos_] == ']') {
                ++pos_;
                return Value(std::move(arr));
            }
            while (pos_ < src_.size()) {
                Value val = parseValue(ok, depth + 1);
                if (!ok) return Value();
                arr.push_back(std::move(val));
                skipWs();
                if (pos_ < src_.size() && src_[pos_] == ',') {
                    ++pos_;
                    continue;
                }
                if (pos_ < src_.size() && src_[pos_] == ']') {
                    ++pos_;
                    return Value(std::move(arr));
                }
                ok = false;
                return Value();
            }
            ok = false;
            return Value();
        }
        if (src_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            return Value(true);
        }
        if (src_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            return Value(false);
        }
        if (src_.compare(pos_, 4, "null") == 0) {
            pos_ += 4;
            return Value(nullptr);
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            const char* start = src_.c_str() + pos_;
            char* end = nullptr;
            double d = std::strtod(start, &end);
            if (end == start) {
                ok = false;
                return Value();
            }
            pos_ += static_cast<size_t>(end - start);
            return Value(d);
        }
        ok = false;
        return Value();
    }
};

inline void escapeString(std::string& out, const std::string& s) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    out.push_back('"');
}

inline void serialize(std::string& out, const Value& v) {
    switch (v.type()) {
        case Type::Null:
            out += "null";
            break;
        case Type::Bool:
            out += v.boolean() ? "true" : "false";
            break;
        case Type::Number: {
            double d = v.num();
            if (!std::isfinite(d)) {
                out += "0";
            } else if (std::floor(d) == d && std::fabs(d) < 9e15) {
                out += std::to_string(static_cast<int64_t>(d));
            } else {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.12g", d);
                out += buf;
            }
            break;
        }
        case Type::String:
            escapeString(out, v.str());
            break;
        case Type::Array: {
            out.push_back('[');
            const auto& arr = v.asArray();
            for (size_t i = 0; i < arr.size(); ++i) {
                if (i > 0) out.push_back(',');
                serialize(out, arr[i]);
            }
            out.push_back(']');
            break;
        }
        case Type::Object: {
            out.push_back('{');
            const auto& obj = v.asObject();
            bool first = true;
            for (const auto& kv : obj) {
                if (!first) out.push_back(',');
                first = false;
                escapeString(out, kv.first);
                out.push_back(':');
                serialize(out, kv.second);
            }
            out.push_back('}');
            break;
        }
    }
}

}  // namespace detail

inline Value parse(const std::string& src, bool* okOut = nullptr) {
    bool ok = true;
    detail::Parser p(src);
    Value v = p.parseValue(ok);
    if (okOut) *okOut = ok;
    return ok ? v : Value();
}

inline std::string stringify(const Value& v) {
    std::string out;
    out.reserve(256);
    detail::serialize(out, v);
    return out;
}

}  // namespace Json
