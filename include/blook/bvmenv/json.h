// blook-vmware-environment: JSON value, writer and parser.
//
// Deliberately small: the CLI only has to emit machine readable reports and
// read back its own result files (and unit tests need to parse them).
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace blook::bvmenv::json {

class Value;
using Array = std::vector<Value>;
using Object = std::map<std::string, Value, std::less<>>;

class Value {
   public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : type_(Type::Bool), bool_(b) {}
    Value(double n) : type_(Type::Number), number_(n) {}
    Value(int n) : type_(Type::Number), number_(static_cast<double>(n)) {}
    Value(long long n) : type_(Type::Number), number_(static_cast<double>(n)) {}
    Value(unsigned long long n) : type_(Type::Number), number_(static_cast<double>(n)) {}
    Value(std::string s) : type_(Type::String), string_(std::move(s)) {}
    Value(const char* s) : type_(Type::String), string_(s) {}
    Value(Array a) : type_(Type::Array), array_(std::move(a)) {}
    Value(Object o) : type_(Type::Object), object_(std::move(o)) {}

    [[nodiscard]] Type type() const { return type_; }
    [[nodiscard]] bool is_null() const { return type_ == Type::Null; }
    [[nodiscard]] bool is_object() const { return type_ == Type::Object; }
    [[nodiscard]] bool is_array() const { return type_ == Type::Array; }

    [[nodiscard]] bool as_bool(bool fallback = false) const {
        return type_ == Type::Bool ? bool_ : fallback;
    }
    [[nodiscard]] double as_number(double fallback = 0.0) const {
        return type_ == Type::Number ? number_ : fallback;
    }
    [[nodiscard]] long long as_int(long long fallback = 0) const {
        return type_ == Type::Number ? static_cast<long long>(number_) : fallback;
    }
    [[nodiscard]] const std::string& as_string() const { return string_; }
    [[nodiscard]] const Array& items() const { return array_; }
    [[nodiscard]] const Object& fields() const { return object_; }

    // Object access. set() on a non-object promotes it to an object.
    Value& operator[](std::string_view key);
    const Value& operator[](std::string_view key) const;
    Value& push(Value v);

    // Serialise. indent=0 produces a single line.
    [[nodiscard]] std::string dump(int indent = 2) const;

    // Parse. Returns nullopt and fills `error` on malformed input.
    static std::optional<Value> parse(std::string_view text, std::string* error = nullptr);

   private:
    void dump_to(std::string& out, int indent, int depth) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    Array array_;
    Object object_;
};

std::string escape(std::string_view s);

}  // namespace blook::bvmenv::json
