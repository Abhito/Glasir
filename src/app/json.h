// SPDX-License-Identifier: MIT
// A small JSON reader for GitHub's release API and mpv's IPC messages.
#pragma once
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace glasir {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type() const { return type_; }
    bool is(Type t) const { return type_ == t; }

    // The value as asked for, or the fallback if it is something else.
    bool boolean(bool fallback = false) const { return type_ == Type::Bool ? bool_ : fallback; }
    double number(double fallback = 0) const { return type_ == Type::Number ? number_ : fallback; }
    // UTF-8.
    const std::string &string() const;

    // Arrays: the items; empty for anything else.
    const std::vector<Json> &items() const;
    // Objects: the member called key, or nullptr.
    const Json *get(std::string_view key) const;
    // The member's string, or "" if there is none or it is not a string.
    const std::string &str(std::string_view key) const;
    const std::vector<std::pair<std::string, Json>> &members() const;

    // nullopt if text is not one valid JSON value.
    static std::optional<Json> parse(std::string_view text);

private:
    friend class JsonParser;
    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0;
    std::string string_;
    std::vector<Json> items_;
    std::vector<std::pair<std::string, Json>> members_;
};

}  // namespace glasir
