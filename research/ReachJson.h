#pragma once
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace reach {
// Small bounded JSON reader for local research configuration. Duplicate members
// and non-finite numbers are rejected; there are no implicit type conversions.
struct Json {
    using Object = std::map<std::string, Json>;
    using Array = std::vector<Json>;
    std::variant<std::nullptr_t, bool, double, std::string, Object, Array> value = nullptr;
    const Object& ObjectValue() const;
    const Json& At(const std::string& key) const;
    std::string StringValue() const;
    double NumberValue() const;
};
Json ParseJson(const std::string& text);
std::string JsonString(const std::string& text);
}
