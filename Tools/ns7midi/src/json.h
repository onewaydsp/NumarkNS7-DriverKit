// json.h
// A small JSON value with a parser and a pretty printer. It covers what the
// ns7midi files need: objects keep their key order, numbers are doubles, and
// strings are UTF-8. No third-party code.

#pragma once

#include <string>
#include <utility>
#include <vector>

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool value) : type_(Type::Bool), bool_(value) {}
    Json(int value) : type_(Type::Number), number_(value) {}
    Json(unsigned value) : type_(Type::Number), number_(value) {}
    Json(long value) : type_(Type::Number), number_(double(value)) {}
    Json(unsigned long value) : type_(Type::Number), number_(double(value)) {}
    Json(double value) : type_(Type::Number), number_(value) {}
    Json(const char * value) : type_(Type::String), string_(value) {}
    Json(std::string value) : type_(Type::String), string_(std::move(value)) {}

    static Json Array() { Json j; j.type_ = Type::Array; return j; }
    static Json Object() { Json j; j.type_ = Type::Object; return j; }

    Type type() const { return type_; }
    bool IsNull() const { return type_ == Type::Null; }
    bool IsBool() const { return type_ == Type::Bool; }
    bool IsNumber() const { return type_ == Type::Number; }
    bool IsString() const { return type_ == Type::String; }
    bool IsArray() const { return type_ == Type::Array; }
    bool IsObject() const { return type_ == Type::Object; }

    bool AsBool(bool fallback = false) const { return IsBool() ? bool_ : fallback; }
    double AsNumber(double fallback = 0) const { return IsNumber() ? number_ : fallback; }
    std::string AsString(const std::string & fallback = "") const { return IsString() ? string_ : fallback; }

    // Arrays.
    size_t size() const { return IsArray() ? array_.size() : IsObject() ? object_.size() : 0; }
    const std::vector<Json> & items() const { return array_; }
    std::vector<Json> & items() { return array_; }
    void Push(Json value);   // turns a null into an array

    // Objects. Get returns nullptr when the key is missing or this is not an
    // object; Set replaces in place and keeps the key's position.
    const Json * Get(const std::string & key) const;
    Json * Get(const std::string & key);
    void Set(const std::string & key, Json value);   // turns a null into an object
    bool Remove(const std::string & key);
    const std::vector<std::pair<std::string, Json>> & members() const { return object_; }

    // Convenience lookups that never fail.
    std::string StringAt(const std::string & key, const std::string & fallback = "") const;
    bool BoolAt(const std::string & key, bool fallback = false) const;
    double NumberAt(const std::string & key, double fallback = 0) const;
    const Json & At(const std::string & key) const;   // a shared null when missing

    std::string Dump(int indent = 2) const;
    bool operator==(const Json & other) const;
    bool operator!=(const Json & other) const { return !(*this == other); }

    // Parses text; on failure returns false and sets *error to a message with
    // the line and column.
    static bool Parse(const std::string & text, Json * out, std::string * error);

private:
    void DumpTo(std::string * out, int indent, int depth) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0;
    std::string string_;
    std::vector<Json> array_;
    std::vector<std::pair<std::string, Json>> object_;
};

// File helpers. ReadJsonFile reports why it failed in *error.
bool ReadJsonFile(const std::string & path, Json * out, std::string * error);
bool WriteJsonFile(const std::string & path, const Json & value, std::string * error);
