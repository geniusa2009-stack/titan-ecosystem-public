// TITAN Engine — JSON implementation. Original TITAN code.
#include "titan/json.h"
#include "titan/detail/json_number.h"

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>

namespace titan {

namespace {
const Json& sharedNull() {
    static const Json kNull;
    return kNull;
}

const char* typeName(Json::Type t) {
    switch (t) {
        case Json::Type::Null: return "null";
        case Json::Type::Bool: return "bool";
        case Json::Type::Number: return "number";
        case Json::Type::String: return "string";
        case Json::Type::Array: return "array";
        case Json::Type::Object: return "object";
    }
    return "?";
}

[[noreturn]] void typeMismatch(Json::Type want, Json::Type got) {
    throw JsonError(std::string("json: expected ") + typeName(want) + ", got " + typeName(got));
}

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

void escapeString(std::string& out, const std::string& s) {
    out += '"';
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
                    static const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[c >> 4];
                    out += hex[c & 0xF];
                } else {
                    out += static_cast<char>(c);  // UTF-8 passes through unchanged
                }
        }
    }
    out += '"';
}

void formatNumber(std::string& out, double d) {
    if (!std::isfinite(d)) {  // JSON has no NaN/Inf; emit null rather than invalid text
        out += "null";
        return;
    }
    char buf[64];
    if (std::floor(d) == d && std::fabs(d) <= 9007199254740992.0) {
        auto r = std::to_chars(buf, buf + sizeof buf, static_cast<long long>(d));
        out.append(buf, r.ptr);
        return;
    }
    auto r = std::to_chars(buf, buf + sizeof buf, d);
    out.append(buf, r.ptr);
}

class Parser {
public:
    Parser(std::string_view t, int maxDepth) : t_(t), maxDepth_(maxDepth) {}

    Json parseDocument() {
        skipWs();
        Json v = parseValue(0);
        skipWs();
        if (p_ != t_.size()) fail("trailing characters");
        return v;
    }

private:
    [[noreturn]] void fail(const char* what) const {
        throw JsonError(std::string("json parse error at offset ") + std::to_string(p_) + ": " + what);
    }
    void skipWs() {
        while (p_ < t_.size() && (t_[p_] == ' ' || t_[p_] == '\t' || t_[p_] == '\n' || t_[p_] == '\r')) ++p_;
    }
    bool consume(std::string_view lit) {
        if (t_.substr(p_, lit.size()) == lit) {
            p_ += lit.size();
            return true;
        }
        return false;
    }

    Json parseValue(int depth) {
        if (depth > maxDepth_) fail("nesting too deep");
        if (p_ >= t_.size()) fail("unexpected end");
        char c = t_[p_];
        if (c == '{') return parseObject(depth);
        if (c == '[') return parseArray(depth);
        if (c == '"') return Json(parseString());
        if (consume("true")) return Json(true);
        if (consume("false")) return Json(false);
        if (consume("null")) return Json();
        if (c == '-' || (c >= '0' && c <= '9')) return parseNumber();
        fail("unexpected character");
    }

    Json parseNumber() {
        size_t start = p_;
        if (t_[p_] == '-') ++p_;
        auto digits = [&] {
            size_t s = p_;
            while (p_ < t_.size() && t_[p_] >= '0' && t_[p_] <= '9') ++p_;
            return p_ - s;
        };
        if (p_ < t_.size() && t_[p_] == '0') {
            ++p_;
        } else if (digits() == 0) {
            fail("bad number");
        }
        if (p_ < t_.size() && t_[p_] == '.') {
            ++p_;
            if (digits() == 0) fail("bad fraction");
        }
        if (p_ < t_.size() && (t_[p_] == 'e' || t_[p_] == 'E')) {
            ++p_;
            if (p_ < t_.size() && (t_[p_] == '+' || t_[p_] == '-')) ++p_;
            if (digits() == 0) fail("bad exponent");
        }
        double d = 0;
        if (detail::parseJsonDouble(t_.data() + start, t_.data() + p_, d) != detail::NumberParse::Ok)
            fail("number out of range");
        return Json(d);
    }

    uint32_t parseHex4() {
        if (p_ + 4 > t_.size()) fail("short \\u escape");
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = t_[p_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
            else fail("bad hex digit");
        }
        return v;
    }

    std::string parseString() {
        ++p_;  // opening quote
        std::string out;
        while (true) {
            if (p_ >= t_.size()) fail("unterminated string");
            unsigned char c = static_cast<unsigned char>(t_[p_++]);
            if (c == '"') break;
            if (c < 0x20) fail("control character in string");
            if (c != '\\') {
                out += static_cast<char>(c);
                continue;
            }
            if (p_ >= t_.size()) fail("bad escape");
            char e = t_[p_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp = parseHex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (!consume("\\u")) fail("lone high surrogate");
                        uint32_t lo = parseHex4();
                        if (lo < 0xDC00 || lo > 0xDFFF) fail("bad low surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        fail("lone low surrogate");
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: fail("unknown escape");
            }
        }
        return out;
    }

    Json parseArray(int depth) {
        ++p_;
        Json arr = Json::array();
        skipWs();
        if (p_ < t_.size() && t_[p_] == ']') {
            ++p_;
            return arr;
        }
        while (true) {
            skipWs();
            arr.push_back(parseValue(depth + 1));
            skipWs();
            if (p_ >= t_.size()) fail("unterminated array");
            if (t_[p_] == ',') { ++p_; continue; }
            if (t_[p_] == ']') { ++p_; return arr; }
            fail("expected , or ]");
        }
    }

    Json parseObject(int depth) {
        ++p_;
        Json obj = Json::object();
        skipWs();
        if (p_ < t_.size() && t_[p_] == '}') {
            ++p_;
            return obj;
        }
        while (true) {
            skipWs();
            if (p_ >= t_.size() || t_[p_] != '"') fail("expected key");
            std::string key = parseString();
            skipWs();
            if (p_ >= t_.size() || t_[p_] != ':') fail("expected :");
            ++p_;
            skipWs();
            obj[key] = parseValue(depth + 1);  // duplicate keys: last one wins
            skipWs();
            if (p_ >= t_.size()) fail("unterminated object");
            if (t_[p_] == ',') { ++p_; continue; }
            if (t_[p_] == '}') { ++p_; return obj; }
            fail("expected , or }");
        }
    }

    std::string_view t_;
    size_t p_ = 0;
    int maxDepth_;
};
}  // namespace

bool Json::asBool() const {
    if (type_ != Type::Bool) typeMismatch(Type::Bool, type_);
    return bool_;
}
double Json::asNumber() const {
    if (type_ != Type::Number) typeMismatch(Type::Number, type_);
    return num_;
}
int64_t Json::asInt() const {
    double d = asNumber();
    if (std::floor(d) != d || std::fabs(d) > 9007199254740992.0) throw JsonError("json: expected integer");
    return static_cast<int64_t>(d);
}
uint64_t Json::asUInt() const {
    int64_t v = asInt();
    if (v < 0) throw JsonError("json: expected non-negative integer");
    return static_cast<uint64_t>(v);
}
const std::string& Json::asString() const {
    if (type_ != Type::String) typeMismatch(Type::String, type_);
    return str_;
}
const Json::Array& Json::asArray() const {
    if (type_ != Type::Array) typeMismatch(Type::Array, type_);
    return arr_;
}
Json::Array& Json::asArray() {
    if (type_ != Type::Array) typeMismatch(Type::Array, type_);
    return arr_;
}
const Json::Object& Json::asObject() const {
    if (type_ != Type::Object) typeMismatch(Type::Object, type_);
    return obj_;
}

const Json* Json::find(std::string_view key) const {
    if (type_ != Type::Object) return nullptr;
    for (const auto& m : obj_)
        if (m.first == key) return &m.second;
    return nullptr;
}
bool Json::contains(std::string_view key) const { return find(key) != nullptr; }
const Json& Json::operator[](std::string_view key) const {
    const Json* f = find(key);
    return f ? *f : sharedNull();
}
Json& Json::operator[](std::string_view key) {
    if (type_ == Type::Null) type_ = Type::Object;
    if (type_ != Type::Object) typeMismatch(Type::Object, type_);
    for (auto& m : obj_)
        if (m.first == key) return m.second;
    obj_.emplace_back(std::string(key), Json());
    return obj_.back().second;
}
bool Json::erase(std::string_view key) {
    if (type_ != Type::Object) return false;
    for (auto it = obj_.begin(); it != obj_.end(); ++it) {
        if (it->first == key) {
            obj_.erase(it);
            return true;
        }
    }
    return false;
}

double Json::getNumber(std::string_view key, double def) const {
    const Json* f = find(key);
    return f && !f->isNull() ? f->asNumber() : def;
}
std::string Json::getString(std::string_view key, const std::string& def) const {
    const Json* f = find(key);
    return f && !f->isNull() ? f->asString() : def;
}
bool Json::getBool(std::string_view key, bool def) const {
    const Json* f = find(key);
    return f && !f->isNull() ? f->asBool() : def;
}

void Json::push_back(Json v) {
    if (type_ == Type::Null) type_ = Type::Array;
    if (type_ != Type::Array) typeMismatch(Type::Array, type_);
    arr_.push_back(std::move(v));
}
size_t Json::size() const {
    if (type_ == Type::Array) return arr_.size();
    if (type_ == Type::Object) return obj_.size();
    return 0;
}
const Json& Json::at(size_t i) const {
    const auto& a = asArray();
    if (i >= a.size()) throw JsonError("json: array index out of range");
    return a[i];
}

bool operator==(const Json& a, const Json& b) {
    if (a.type_ != b.type_) return false;
    switch (a.type_) {
        case Json::Type::Null: return true;
        case Json::Type::Bool: return a.bool_ == b.bool_;
        case Json::Type::Number: return a.num_ == b.num_;
        case Json::Type::String: return a.str_ == b.str_;
        case Json::Type::Array: return a.arr_ == b.arr_;
        case Json::Type::Object: {
            if (a.obj_.size() != b.obj_.size()) return false;
            for (const auto& m : a.obj_) {  // order-insensitive comparison
                const Json* o = b.find(m.first);
                if (!o || *o != m.second) return false;
            }
            return true;
        }
    }
    return false;
}

void Json::dumpTo(std::string& out, int indent, int level) const {
    auto newline = [&](int lvl) {
        if (indent < 0) return;
        out += '\n';
        out.append(static_cast<size_t>(indent * lvl), ' ');
    };
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Number: formatNumber(out, num_); break;
        case Type::String: escapeString(out, str_); break;
        case Type::Array:
            if (arr_.empty()) { out += "[]"; break; }
            out += '[';
            for (size_t i = 0; i < arr_.size(); ++i) {
                if (i) out += ',';
                newline(level + 1);
                arr_[i].dumpTo(out, indent, level + 1);
            }
            newline(level);
            out += ']';
            break;
        case Type::Object:
            if (obj_.empty()) { out += "{}"; break; }
            out += '{';
            for (size_t i = 0; i < obj_.size(); ++i) {
                if (i) out += ',';
                newline(level + 1);
                escapeString(out, obj_[i].first);
                out += indent < 0 ? ":" : ": ";
                obj_[i].second.dumpTo(out, indent, level + 1);
            }
            newline(level);
            out += '}';
            break;
    }
}

std::string Json::dump(int indent) const {
    std::string out;
    dumpTo(out, indent, 0);
    return out;
}

Json Json::parse(std::string_view text, int maxDepth) { return Parser(text, maxDepth).parseDocument(); }

}  // namespace titan
