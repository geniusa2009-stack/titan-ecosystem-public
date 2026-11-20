// TITAN Engine — minimal, dependency-free JSON value used by the engine/editor protocol
// and the on-disk project format. Original TITAN code.
//
// Design notes:
//  * Objects preserve insertion order so files written by the engine diff cleanly.
//  * Numbers are IEEE-754 doubles. Integers are exact up to 2^53; TITAN object ids are
//    allocated sequentially and are guaranteed to stay below that bound.
//  * The parser is locale-independent (std::from_chars) and enforces a nesting limit so
//    a hostile peer cannot exhaust the stack.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace titan {

class JsonError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using Array = std::vector<Json>;
    using Member = std::pair<std::string, Json>;
    using Object = std::vector<Member>;

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : type_(Type::Bool), bool_(b) {}
    template <class T, std::enable_if_t<std::is_arithmetic_v<T> && !std::is_same_v<T, bool>, int> = 0>
    Json(T n) : type_(Type::Number), num_(static_cast<double>(n)) {}
    Json(const char* s) : type_(Type::String), str_(s ? s : "") {}
    Json(std::string s) : type_(Type::String), str_(std::move(s)) {}
    Json(std::string_view s) : type_(Type::String), str_(s) {}
    Json(Array a) : type_(Type::Array), arr_(std::move(a)) {}

    static Json object() { Json j; j.type_ = Type::Object; return j; }
    static Json array() { Json j; j.type_ = Type::Array; return j; }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool asBool() const;
    double asNumber() const;
    int64_t asInt() const;          // throws if not an integral number
    uint64_t asUInt() const;        // throws if negative / not integral
    const std::string& asString() const;
    const Array& asArray() const;
    Array& asArray();
    const Object& asObject() const;

    // Object access. const operator[] returns a shared null for missing keys.
    bool contains(std::string_view key) const;
    const Json& operator[](std::string_view key) const;
    Json& operator[](std::string_view key);   // converts null -> object, inserts if missing
    const Json* find(std::string_view key) const;
    bool erase(std::string_view key);

    // Typed getters with defaults (no throw when missing; throw when present but wrong type).
    double getNumber(std::string_view key, double def) const;
    std::string getString(std::string_view key, const std::string& def = {}) const;
    bool getBool(std::string_view key, bool def) const;

    // Array helpers.
    void push_back(Json v);
    size_t size() const;
    const Json& at(size_t i) const;

    std::string dump(int indent = -1) const;
    static Json parse(std::string_view text, int maxDepth = 128);

    friend bool operator==(const Json& a, const Json& b);
    friend bool operator!=(const Json& a, const Json& b) { return !(a == b); }

private:
    void dumpTo(std::string& out, int indent, int level) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0.0;
    std::string str_;
    Array arr_;
    Object obj_;
};

}  // namespace titan
