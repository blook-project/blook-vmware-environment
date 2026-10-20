// blook-vmware-environment: JSON implementation.
#include "blook/bvmenv/json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace blook::bvmenv::json {
namespace {

static const Value kNull{};

void append_escaped(std::string& out, std::string_view s) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

class Parser {
public:
    Parser(std::string_view text, std::string* error) : text_(text), error_(error) {}

    Value parse() {
        skip_ws();
        Value v = parse_value();
        skip_ws();
        if (pos_ != text_.size()) {
            fail("trailing characters");
        }
        return v;
    }

    bool ok() const { return ok_; }

private:
    void fail(const std::string& message) {
        if (ok_) {
            ok_ = false;
            if (error_) {
                *error_ = message + " at offset " + std::to_string(pos_);
            }
        }
    }

    void skip_ws() {
        while (pos_ < text_.size()) {
            char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool consume(char c) {
        if (pos_ < text_.size() && text_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    Value parse_value() {
        if (!ok_) return Value{};
        if (pos_ >= text_.size()) {
            fail("unexpected end of input");
            return Value{};
        }
        char c = text_[pos_];
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (c == '"') return Value(parse_string());
        if (c == 't') return parse_literal("true", Value(true));
        if (c == 'f') return parse_literal("false", Value(false));
        if (c == 'n') return parse_literal("null", Value{});
        return parse_number();
    }

    Value parse_literal(std::string_view lit, Value v) {
        if (text_.substr(pos_, lit.size()) != lit) {
            fail("invalid literal");
            return Value{};
        }
        pos_ += lit.size();
        return v;
    }

    Value parse_number() {
        std::size_t start = pos_;
        if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_;
        while (pos_ < text_.size()) {
            char c = text_[pos_];
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '-' ||
                c == '+') {
                ++pos_;
            } else {
                break;
            }
        }
        if (start == pos_) {
            fail("invalid value");
            return Value{};
        }
        std::string token(text_.substr(start, pos_ - start));
        try {
            return Value(std::stod(token));
        } catch (...) {
            fail("invalid number");
            return Value{};
        }
    }

    std::string parse_string() {
        if (!consume('"')) {
            fail("expected string");
            return {};
        }
        std::string out;
        while (pos_ < text_.size()) {
            char c = text_[pos_++];
            if (c == '"') return out;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= text_.size()) break;
            char esc = text_[pos_++];
            switch (esc) {
                case '"':
                case '\\':
                case '/':
                    out.push_back(esc);
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'u': {
                    if (pos_ + 4 > text_.size()) {
                        fail("truncated \\u escape");
                        return out;
                    }
                    unsigned code = 0;
                    for (int i = 0; i < 4; ++i) {
                        char h = text_[pos_++];
                        code <<= 4;
                        if (h >= '0' && h <= '9') {
                            code |= static_cast<unsigned>(h - '0');
                        } else if (h >= 'a' && h <= 'f') {
                            code |= static_cast<unsigned>(h - 'a' + 10);
                        } else if (h >= 'A' && h <= 'F') {
                            code |= static_cast<unsigned>(h - 'A' + 10);
                        } else {
                            fail("invalid \\u escape");
                            return out;
                        }
                    }
                    // Only ASCII range is needed by this project; encode the
                    // rest as UTF-8 (no surrogate pair handling beyond BMP).
                    if (code < 0x80) {
                        out.push_back(static_cast<char>(code));
                    } else if (code < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    }
                    break;
                }
                default:
                    fail("invalid escape");
                    return out;
            }
        }
        fail("unterminated string");
        return out;
    }

    Value parse_array() {
        consume('[');
        Array items;
        skip_ws();
        if (consume(']')) return Value(std::move(items));
        for (;;) {
            items.push_back(parse_value());
            skip_ws();
            if (consume(',')) {
                skip_ws();
                continue;
            }
            if (consume(']')) break;
            fail("expected ',' or ']'");
            break;
        }
        return Value(std::move(items));
    }

    Value parse_object() {
        consume('{');
        Object fields;
        skip_ws();
        if (consume('}')) return Value(std::move(fields));
        for (;;) {
            skip_ws();
            if (pos_ >= text_.size() || text_[pos_] != '"') {
                fail("expected object key");
                break;
            }
            std::string key = parse_string();
            skip_ws();
            if (!consume(':')) {
                fail("expected ':'");
                break;
            }
            skip_ws();
            fields.emplace(std::move(key), parse_value());
            skip_ws();
            if (consume(',')) continue;
            if (consume('}')) break;
            fail("expected ',' or '}'");
            break;
        }
        return Value(std::move(fields));
    }

    std::string_view text_;
    std::string* error_ = nullptr;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

}  // namespace

std::string escape(std::string_view s) {
    std::string out;
    append_escaped(out, s);
    return out;
}

Value& Value::operator[](std::string_view key) {
    if (type_ != Type::Object) {
        type_ = Type::Object;
        array_.clear();
        string_.clear();
    }
    auto it = object_.find(key);
    if (it == object_.end()) {
        it = object_.emplace(std::string(key), Value{}).first;
    }
    return it->second;
}

const Value& Value::operator[](std::string_view key) const {
    static const Value empty;
    if (type_ != Type::Object) return empty;
    auto it = object_.find(key);
    if (it == object_.end()) return empty;
    return it->second;
}

Value& Value::push(Value v) {
    if (type_ != Type::Array) {
        type_ = Type::Array;
        object_.clear();
        string_.clear();
    }
    array_.push_back(std::move(v));
    return array_.back();
}

void Value::dump_to(std::string& out, int indent, int depth) const {
    const std::string pad(indent > 0 ? static_cast<std::size_t>(indent * (depth + 1)) : 0, ' ');
    const std::string pad_close(indent > 0 ? static_cast<std::size_t>(indent * depth) : 0, ' ');
    const char* nl = indent > 0 ? "\n" : "";
    switch (type_) {
        case Type::Null:
            out += "null";
            break;
        case Type::Bool:
            out += bool_ ? "true" : "false";
            break;
        case Type::Number: {
            char buf[64];
            if (std::isfinite(number_) && number_ == static_cast<double>(static_cast<long long>(number_)) &&
                std::abs(number_) < 9.0e15) {
                std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(number_));
            } else {
                std::snprintf(buf, sizeof(buf), "%.17g", number_);
            }
            out += buf;
            break;
        }
        case Type::String:
            append_escaped(out, string_);
            break;
        case Type::Array: {
            if (array_.empty()) {
                out += "[]";
                break;
            }
            out += '[';
            out += nl;
            for (std::size_t i = 0; i < array_.size(); ++i) {
                out += pad;
                array_[i].dump_to(out, indent, depth + 1);
                if (i + 1 != array_.size()) out += ',';
                out += nl;
            }
            out += pad_close;
            out += ']';
            break;
        }
        case Type::Object: {
            if (object_.empty()) {
                out += "{}";
                break;
            }
            out += '{';
            out += nl;
            std::size_t i = 0;
            for (const auto& [key, value] : object_) {
                out += pad;
                append_escaped(out, key);
                out += indent > 0 ? ": " : ":";
                value.dump_to(out, indent, depth + 1);
                if (++i != object_.size()) out += ',';
                out += nl;
            }
            out += pad_close;
            out += '}';
            break;
        }
    }
}

std::string Value::dump(int indent) const {
    std::string out;
    out.reserve(256);
    dump_to(out, indent, 0);
    return out;
}

std::optional<Value> Value::parse(std::string_view text, std::string* error) {
    std::string local_error;
    Parser parser(text, error ? error : &local_error);
    Value v = parser.parse();
    if (!parser.ok()) return std::nullopt;
    return v;
}

}  // namespace blook::bvmenv::json
