// SPDX-License-Identifier: MIT
#include "json.h"

#include <charconv>
#include <cstdlib>

namespace glasir {
namespace {
const std::string kEmptyString;
const std::vector<Json> kEmptyItems;
const std::vector<std::pair<std::string, Json>> kEmptyMembers;
}  // namespace

const std::string &Json::string() const { return type_ == Type::String ? string_ : kEmptyString; }
const std::vector<Json> &Json::items() const { return type_ == Type::Array ? items_ : kEmptyItems; }
const std::vector<std::pair<std::string, Json>> &Json::members() const {
    return type_ == Type::Object ? members_ : kEmptyMembers;
}

const Json *Json::get(std::string_view key) const {
    for (const auto &[k, v] : members())
        if (k == key) return &v;
    return nullptr;
}

const std::string &Json::str(std::string_view key) const {
    const Json *v = get(key);
    return v ? v->string() : kEmptyString;
}

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : s_(text) {}

    bool document(Json &out) {
        if (!value(out, 0)) return false;
        space();
        return i_ == s_.size();
    }

private:
    static constexpr int kMaxDepth = 64;

    void space() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
    }

    bool literal(std::string_view word) {
        if (s_.substr(i_, word.size()) != word) return false;
        i_ += word.size();
        return true;
    }

    bool value(Json &out, int depth) {
        if (depth > kMaxDepth) return false;
        space();
        if (i_ >= s_.size()) return false;
        switch (s_[i_]) {
        case '{': return object(out, depth);
        case '[': return array(out, depth);
        case '"': out.type_ = Json::Type::String; return string(out.string_);
        case 't': out.type_ = Json::Type::Bool; out.bool_ = true; return literal("true");
        case 'f': out.type_ = Json::Type::Bool; out.bool_ = false; return literal("false");
        case 'n': out.type_ = Json::Type::Null; return literal("null");
        default: return number(out);
        }
    }

    bool object(Json &out, int depth) {
        out.type_ = Json::Type::Object;
        ++i_;
        space();
        if (i_ < s_.size() && s_[i_] == '}') return ++i_, true;
        for (;;) {
            space();
            std::string key;
            if (i_ >= s_.size() || s_[i_] != '"' || !string(key)) return false;
            space();
            if (i_ >= s_.size() || s_[i_++] != ':') return false;
            Json v;
            if (!value(v, depth + 1)) return false;
            out.members_.emplace_back(std::move(key), std::move(v));
            space();
            if (i_ >= s_.size()) return false;
            if (s_[i_] == ',') { ++i_; continue; }
            if (s_[i_] == '}') return ++i_, true;
            return false;
        }
    }

    bool array(Json &out, int depth) {
        out.type_ = Json::Type::Array;
        ++i_;
        space();
        if (i_ < s_.size() && s_[i_] == ']') return ++i_, true;
        for (;;) {
            Json v;
            if (!value(v, depth + 1)) return false;
            out.items_.push_back(std::move(v));
            space();
            if (i_ >= s_.size()) return false;
            if (s_[i_] == ',') { ++i_; continue; }
            if (s_[i_] == ']') return ++i_, true;
            return false;
        }
    }

    bool hex4(unsigned &out) {
        if (i_ + 4 > s_.size()) return false;
        out = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s_[i_++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') out |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= unsigned(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    static void utf8(std::string &out, unsigned cp) {
        if (cp < 0x80) {
            out += char(cp);
        } else if (cp < 0x800) {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    }

    bool string(std::string &out) {
        ++i_;  // the opening quote
        while (i_ < s_.size()) {
            const char c = s_[i_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i_ >= s_.size()) return false;
            switch (s_[i_++]) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned cp = 0;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00 && s_.substr(i_, 2) == "\\u") {
                    i_ += 2;
                    unsigned low = 0;
                    if (!hex4(low) || low < 0xDC00 || low >= 0xE000) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                } else if (cp >= 0xD800 && cp < 0xE000) {
                    cp = 0xFFFD;  // a lone surrogate
                }
                utf8(out, cp);
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool number(Json &out) {
        const size_t start = i_;
        if (i_ < s_.size() && s_[i_] == '-') ++i_;
        const size_t digits = i_;
        while (i_ < s_.size() && ((s_[i_] >= '0' && s_[i_] <= '9') || s_[i_] == '.' || s_[i_] == 'e' ||
                                  s_[i_] == 'E' || s_[i_] == '+' || s_[i_] == '-'))
            ++i_;
        if (i_ == digits) return false;
        double v = 0;
        const auto [end, ec] = std::from_chars(s_.data() + start, s_.data() + i_, v);
        if (ec != std::errc() || end != s_.data() + i_) return false;
        out.type_ = Json::Type::Number;
        out.number_ = v;
        return true;
    }

    std::string_view s_;
    size_t i_ = 0;
};

std::optional<Json> Json::parse(std::string_view text) {
    Json out;
    JsonParser p(text);
    if (!p.document(out)) return std::nullopt;
    return out;
}

}  // namespace glasir
