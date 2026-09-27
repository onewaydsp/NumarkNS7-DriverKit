// json.cpp

#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

void Json::Push(Json value)
{
    if (type_ == Type::Null) type_ = Type::Array;
    if (type_ == Type::Array) array_.push_back(std::move(value));
}

const Json * Json::Get(const std::string & key) const
{
    if (type_ != Type::Object) return nullptr;
    for (const auto & member : object_)
        if (member.first == key) return &member.second;
    return nullptr;
}

Json * Json::Get(const std::string & key)
{
    return const_cast<Json *>(static_cast<const Json *>(this)->Get(key));
}

void Json::Set(const std::string & key, Json value)
{
    if (type_ == Type::Null) type_ = Type::Object;
    if (type_ != Type::Object) return;
    if (Json * existing = Get(key)) { *existing = std::move(value); return; }
    object_.emplace_back(key, std::move(value));
}

bool Json::Remove(const std::string & key)
{
    for (auto it = object_.begin(); it != object_.end(); ++it)
        if (it->first == key) { object_.erase(it); return true; }
    return false;
}

const Json & Json::At(const std::string & key) const
{
    static const Json null;
    const Json * value = Get(key);
    return value ? *value : null;
}

std::string Json::StringAt(const std::string & key, const std::string & fallback) const
{
    return At(key).AsString(fallback);
}

bool Json::BoolAt(const std::string & key, bool fallback) const
{
    return At(key).AsBool(fallback);
}

double Json::NumberAt(const std::string & key, double fallback) const
{
    return At(key).AsNumber(fallback);
}

bool Json::operator==(const Json & other) const
{
    if (type_ != other.type_) return false;
    switch (type_) {
    case Type::Null: return true;
    case Type::Bool: return bool_ == other.bool_;
    case Type::Number: return number_ == other.number_;
    case Type::String: return string_ == other.string_;
    case Type::Array: return array_ == other.array_;
    case Type::Object: return object_ == other.object_;
    }
    return false;
}

// ---------------------------------------------------------------- printing

static void EscapeString(const std::string & s, std::string * out)
{
    out->push_back('"');
    for (unsigned char c : s) {
        switch (c) {
        case '"': *out += "\\\""; break;
        case '\\': *out += "\\\\"; break;
        case '\n': *out += "\\n"; break;
        case '\r': *out += "\\r"; break;
        case '\t': *out += "\\t"; break;
        case '\b': *out += "\\b"; break;
        case '\f': *out += "\\f"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                *out += buf;
            } else {
                out->push_back(char(c));
            }
        }
    }
    out->push_back('"');
}

static void FormatNumber(double n, std::string * out)
{
    if (!std::isfinite(n)) { *out += "null"; return; }
    char buf[40];
    if (n == std::floor(n) && std::fabs(n) < 1e15)
        std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(n));
    else
        std::snprintf(buf, sizeof buf, "%.17g", n);
    *out += buf;
}

// Arrays of scalars stay on one line, which keeps sample lists readable.
static bool AllScalars(const std::vector<Json> & items)
{
    for (const Json & item : items)
        if (item.IsArray() || item.IsObject()) return false;
    return true;
}

void Json::DumpTo(std::string * out, int indent, int depth) const
{
    const std::string pad(size_t(indent * (depth + 1)), ' ');
    const std::string closePad(size_t(indent * depth), ' ');
    switch (type_) {
    case Type::Null: *out += "null"; break;
    case Type::Bool: *out += bool_ ? "true" : "false"; break;
    case Type::Number: FormatNumber(number_, out); break;
    case Type::String: EscapeString(string_, out); break;
    case Type::Array:
        if (array_.empty()) { *out += "[]"; break; }
        if (indent == 0 || AllScalars(array_)) {
            *out += "[";
            for (size_t i = 0; i < array_.size(); i++) {
                if (i) *out += indent ? ", " : ",";
                array_[i].DumpTo(out, indent, depth + 1);
            }
            *out += "]";
            break;
        }
        *out += "[\n";
        for (size_t i = 0; i < array_.size(); i++) {
            *out += pad;
            array_[i].DumpTo(out, indent, depth + 1);
            *out += i + 1 < array_.size() ? ",\n" : "\n";
        }
        *out += closePad + "]";
        break;
    case Type::Object:
        if (object_.empty()) { *out += "{}"; break; }
        *out += indent ? "{\n" : "{";
        for (size_t i = 0; i < object_.size(); i++) {
            if (indent) *out += pad;
            EscapeString(object_[i].first, out);
            *out += indent ? ": " : ":";
            object_[i].second.DumpTo(out, indent, depth + 1);
            if (i + 1 < object_.size()) *out += ",";
            if (indent) *out += "\n";
        }
        if (indent) *out += closePad;
        *out += "}";
        break;
    }
}

std::string Json::Dump(int indent) const
{
    std::string out;
    DumpTo(&out, indent, 0);
    return out;
}

// ----------------------------------------------------------------- parsing

namespace {

class Parser {
public:
    explicit Parser(const std::string & text) : text_(text) {}

    bool ParseDocument(Json * out, std::string * error)
    {
        SkipSpace();
        if (!ParseValue(out, 0)) { *error = error_; return false; }
        SkipSpace();
        if (pos_ != text_.size()) { Fail("trailing characters"); *error = error_; return false; }
        return true;
    }

private:
    bool Fail(const std::string & what)
    {
        if (!error_.empty()) return false;
        size_t line = 1, column = 1;
        for (size_t i = 0; i < pos_ && i < text_.size(); i++) {
            if (text_[i] == '\n') { line++; column = 1; } else { column++; }
        }
        error_ = what + " at line " + std::to_string(line) + ", column " + std::to_string(column);
        return false;
    }

    void SkipSpace()
    {
        while (pos_ < text_.size() &&
               (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r'))
            pos_++;
    }

    bool Literal(const char * word)
    {
        const std::string w(word);
        if (text_.compare(pos_, w.size(), w) != 0) return Fail("unexpected token");
        pos_ += w.size();
        return true;
    }

    bool ParseValue(Json * out, int depth)
    {
        if (depth > 64) return Fail("nesting too deep");
        if (pos_ >= text_.size()) return Fail("unexpected end of input");
        const char c = text_[pos_];
        if (c == '{') return ParseObject(out, depth);
        if (c == '[') return ParseArray(out, depth);
        if (c == '"') {
            std::string s;
            if (!ParseString(&s)) return false;
            *out = Json(std::move(s));
            return true;
        }
        if (c == 't') { if (!Literal("true")) return false; *out = Json(true); return true; }
        if (c == 'f') { if (!Literal("false")) return false; *out = Json(false); return true; }
        if (c == 'n') { if (!Literal("null")) return false; *out = Json(); return true; }
        if (c == '-' || (c >= '0' && c <= '9')) return ParseNumber(out);
        return Fail("unexpected character");
    }

    bool ParseNumber(Json * out)
    {
        const size_t start = pos_;
        if (text_[pos_] == '-') pos_++;
        auto digits = [&] {
            const size_t before = pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') pos_++;
            return pos_ > before;
        };
        if (!digits()) return Fail("bad number");
        if (pos_ < text_.size() && text_[pos_] == '.') { pos_++; if (!digits()) return Fail("bad number"); }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            pos_++;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) pos_++;
            if (!digits()) return Fail("bad number");
        }
        *out = Json(std::strtod(text_.substr(start, pos_ - start).c_str(), nullptr));
        return true;
    }

    bool Hex4(unsigned * value)
    {
        if (pos_ + 4 > text_.size()) return Fail("short \\u escape");
        *value = 0;
        for (int i = 0; i < 4; i++) {
            const char h = text_[pos_++];
            *value <<= 4;
            if (h >= '0' && h <= '9') *value |= unsigned(h - '0');
            else if (h >= 'a' && h <= 'f') *value |= unsigned(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') *value |= unsigned(h - 'A' + 10);
            else return Fail("bad \\u escape");
        }
        return true;
    }

    static void AppendUtf8(unsigned cp, std::string * out)
    {
        if (cp < 0x80) {
            out->push_back(char(cp));
        } else if (cp < 0x800) {
            out->push_back(char(0xC0 | (cp >> 6)));
            out->push_back(char(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out->push_back(char(0xE0 | (cp >> 12)));
            out->push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(char(0x80 | (cp & 0x3F)));
        } else {
            out->push_back(char(0xF0 | (cp >> 18)));
            out->push_back(char(0x80 | ((cp >> 12) & 0x3F)));
            out->push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(char(0x80 | (cp & 0x3F)));
        }
    }

    bool ParseString(std::string * out)
    {
        pos_++;   // opening quote
        while (true) {
            if (pos_ >= text_.size()) return Fail("unterminated string");
            const char c = text_[pos_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return Fail("control character in string");
            if (c != '\\') { out->push_back(c); continue; }
            if (pos_ >= text_.size()) return Fail("unterminated escape");
            const char e = text_[pos_++];
            switch (e) {
            case '"': out->push_back('"'); break;
            case '\\': out->push_back('\\'); break;
            case '/': out->push_back('/'); break;
            case 'b': out->push_back('\b'); break;
            case 'f': out->push_back('\f'); break;
            case 'n': out->push_back('\n'); break;
            case 'r': out->push_back('\r'); break;
            case 't': out->push_back('\t'); break;
            case 'u': {
                unsigned cp = 0;
                if (!Hex4(&cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00) {
                    unsigned low = 0;
                    if (text_.compare(pos_, 2, "\\u") != 0) return Fail("lone surrogate");
                    pos_ += 2;
                    if (!Hex4(&low)) return false;
                    if (low < 0xDC00 || low >= 0xE000) return Fail("bad surrogate pair");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                } else if (cp >= 0xDC00 && cp < 0xE000) {
                    return Fail("lone surrogate");
                }
                AppendUtf8(cp, out);
                break;
            }
            default: return Fail("bad escape");
            }
        }
    }

    bool ParseArray(Json * out, int depth)
    {
        pos_++;
        *out = Json::Array();
        SkipSpace();
        if (pos_ < text_.size() && text_[pos_] == ']') { pos_++; return true; }
        while (true) {
            SkipSpace();
            Json item;
            if (!ParseValue(&item, depth + 1)) return false;
            out->Push(std::move(item));
            SkipSpace();
            if (pos_ >= text_.size()) return Fail("unterminated array");
            if (text_[pos_] == ',') { pos_++; continue; }
            if (text_[pos_] == ']') { pos_++; return true; }
            return Fail("expected , or ]");
        }
    }

    bool ParseObject(Json * out, int depth)
    {
        pos_++;
        *out = Json::Object();
        SkipSpace();
        if (pos_ < text_.size() && text_[pos_] == '}') { pos_++; return true; }
        while (true) {
            SkipSpace();
            if (pos_ >= text_.size() || text_[pos_] != '"') return Fail("expected key string");
            std::string key;
            if (!ParseString(&key)) return false;
            SkipSpace();
            if (pos_ >= text_.size() || text_[pos_] != ':') return Fail("expected :");
            pos_++;
            SkipSpace();
            Json value;
            if (!ParseValue(&value, depth + 1)) return false;
            out->Set(key, std::move(value));
            SkipSpace();
            if (pos_ >= text_.size()) return Fail("unterminated object");
            if (text_[pos_] == ',') { pos_++; continue; }
            if (text_[pos_] == '}') { pos_++; return true; }
            return Fail("expected , or }");
        }
    }

    const std::string & text_;
    size_t pos_ = 0;
    std::string error_;
};

}   // namespace

bool Json::Parse(const std::string & text, Json * out, std::string * error)
{
    std::string ignored;
    return Parser(text).ParseDocument(out, error ? error : &ignored);
}

bool ReadJsonFile(const std::string & path, Json * out, std::string * error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { *error = path + ": cannot open"; return false; }
    std::stringstream buffer;
    buffer << in.rdbuf();
    std::string parseError;
    if (!Json::Parse(buffer.str(), out, &parseError)) { *error = path + ": " + parseError; return false; }
    return true;
}

bool WriteJsonFile(const std::string & path, const Json & value, std::string * error)
{
    // Write to a temporary file and rename, so an interrupted session never
    // leaves a half-written result behind.
    const std::string temp = path + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) { *error = temp + ": cannot create"; return false; }
        out << value.Dump(2) << "\n";
        if (!out) { *error = temp + ": write failed"; return false; }
    }
    if (std::rename(temp.c_str(), path.c_str()) != 0) { *error = path + ": rename failed"; return false; }
    return true;
}
