#pragma once
// Just enough JSON to read and rewrite config.json.

#include <charconv>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

struct JsonValue
{
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::wstring string;
    std::vector<JsonValue> items;     // array elements, or object values
    std::vector<std::wstring> keys;   // object keys, parallel to items

    const JsonValue* Find(std::wstring_view key) const
    {
        if (type != Type::Object) return nullptr;
        for (size_t i = 0; i < keys.size(); i++)
            if (keys[i] == key) return &items[i];
        return nullptr;
    }

    std::wstring_view Str(std::wstring_view key) const
    {
        auto v = Find(key);
        return v && v->type == Type::String ? std::wstring_view(v->string) : std::wstring_view();
    }

    double Num(std::wstring_view key, double fallback) const
    {
        auto v = Find(key);
        return v && v->type == Type::Number ? v->number : fallback;
    }

    bool Flag(std::wstring_view key, bool fallback) const
    {
        auto v = Find(key);
        return v && v->type == Type::Bool ? v->boolean : fallback;
    }

    // Sets an object member, adding it if missing; other members keep their order.
    void Set(std::wstring_view key, JsonValue value)
    {
        type = Type::Object;
        for (size_t i = 0; i < keys.size(); i++)
            if (keys[i] == key) { items[i] = std::move(value); return; }
        keys.emplace_back(key);
        items.push_back(std::move(value));
    }

    void Remove(std::wstring_view key)
    {
        for (size_t i = 0; i < keys.size(); i++)
            if (keys[i] == key)
            {
                keys.erase(keys.begin() + i);
                items.erase(items.begin() + i);
                return;
            }
    }

    static JsonValue Bool(bool b) { JsonValue v; v.type = Type::Bool; v.boolean = b; return v; }
    static JsonValue Number(double n) { JsonValue v; v.type = Type::Number; v.number = n; return v; }
    static JsonValue EmptyArray() { JsonValue v; v.type = Type::Array; return v; }
    static JsonValue String(std::wstring s) { JsonValue v; v.type = Type::String; v.string = std::move(s); return v; }
};

// Indented like the WPF version's System.Text.Json output; non-ASCII text is written as is.
inline void WriteJson(JsonValue const& v, std::wstring& out, int depth = 0)
{
    auto indent = [&](int d) { out += L'\n'; out.append(size_t(d) * 2, L' '); };
    auto string = [&](std::wstring const& s) {
        out += L'"';
        for (wchar_t c : s)
        {
            switch (c)
            {
            case L'"': out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\n': out += L"\\n"; break;
            case L'\r': out += L"\\r"; break;
            case L'\t': out += L"\\t"; break;
            default:
                if (c < 0x20)
                {
                    wchar_t buffer[8];
                    swprintf(buffer, 8, L"\\u%04x", unsigned(c));
                    out += buffer;
                }
                else
                    out += c;
            }
        }
        out += L'"';
    };

    switch (v.type)
    {
    case JsonValue::Type::Null: out += L"null"; break;
    case JsonValue::Type::Bool: out += v.boolean ? L"true" : L"false"; break;
    case JsonValue::Type::Number:
    {
        char buffer[32];
        auto end = std::to_chars(buffer, buffer + sizeof buffer, v.number).ptr;
        out.append(buffer, end);
        break;
    }
    case JsonValue::Type::String: string(v.string); break;
    case JsonValue::Type::Array:
    case JsonValue::Type::Object:
    {
        bool object = v.type == JsonValue::Type::Object;
        out += object ? L'{' : L'[';
        for (size_t i = 0; i < v.items.size(); i++)
        {
            if (i) out += L',';
            indent(depth + 1);
            if (object)
            {
                string(v.keys[i]);
                out += L": ";
            }
            WriteJson(v.items[i], out, depth + 1);
        }
        if (!v.items.empty()) indent(depth);
        out += object ? L'}' : L']';
        break;
    }
    }
}

class JsonParser
{
public:
    explicit JsonParser(std::wstring_view text) : s(text) {}

    bool Parse(JsonValue& out)
    {
        if (!Value(out)) return false;
        Skip();
        return i == s.size();
    }

private:
    std::wstring_view s;
    size_t i = 0;

    void Skip()
    {
        while (i < s.size() && std::iswspace(s[i])) i++;
    }

    bool Literal(std::wstring_view word)
    {
        if (s.substr(i, word.size()) != word) return false;
        i += word.size();
        return true;
    }

    bool Value(JsonValue& v)
    {
        Skip();
        if (i >= s.size()) return false;
        wchar_t c = s[i];
        if (c == L'{') return Object(v);
        if (c == L'[') return Array(v);
        if (c == L'"') { v.type = JsonValue::Type::String; return String(v.string); }
        if (Literal(L"true")) { v.type = JsonValue::Type::Bool; v.boolean = true; return true; }
        if (Literal(L"false")) { v.type = JsonValue::Type::Bool; v.boolean = false; return true; }
        if (Literal(L"null")) { v.type = JsonValue::Type::Null; return true; }

        size_t start = i;
        while (i < s.size() && std::wstring_view(L"+-0123456789.eE").find(s[i]) != std::wstring_view::npos) i++;
        if (i == start) return false;
        v.type = JsonValue::Type::Number;
        v.number = std::wcstod(std::wstring(s.substr(start, i - start)).c_str(), nullptr);
        return true;
    }

    bool String(std::wstring& out)
    {
        i++;  // opening quote
        while (i < s.size())
        {
            wchar_t c = s[i++];
            if (c == L'"') return true;
            if (c != L'\\') { out += c; continue; }
            if (i >= s.size()) return false;
            wchar_t e = s[i++];
            switch (e)
            {
            case L'b': out += L'\b'; break;
            case L'f': out += L'\f'; break;
            case L'n': out += L'\n'; break;
            case L'r': out += L'\r'; break;
            case L't': out += L'\t'; break;
            case L'u':
            {
                if (i + 4 > s.size()) return false;
                out += static_cast<wchar_t>(std::wcstoul(std::wstring(s.substr(i, 4)).c_str(), nullptr, 16));
                i += 4;
                break;
            }
            default: out += e; break;  // \" \\ \/
            }
        }
        return false;
    }

    bool Array(JsonValue& v)
    {
        v.type = JsonValue::Type::Array;
        i++;
        Skip();
        if (i < s.size() && s[i] == L']') { i++; return true; }
        for (;;)
        {
            v.items.emplace_back();
            if (!Value(v.items.back())) return false;
            Skip();
            if (i >= s.size()) return false;
            if (s[i] == L',') { i++; continue; }
            if (s[i] == L']') { i++; return true; }
            return false;
        }
    }

    bool Object(JsonValue& v)
    {
        v.type = JsonValue::Type::Object;
        i++;
        Skip();
        if (i < s.size() && s[i] == L'}') { i++; return true; }
        for (;;)
        {
            Skip();
            if (i >= s.size() || s[i] != L'"') return false;
            v.keys.emplace_back();
            if (!String(v.keys.back())) return false;
            Skip();
            if (i >= s.size() || s[i] != L':') return false;
            i++;
            v.items.emplace_back();
            if (!Value(v.items.back())) return false;
            Skip();
            if (i >= s.size()) return false;
            if (s[i] == L',') { i++; continue; }
            if (s[i] == L'}') { i++; return true; }
            return false;
        }
    }
};
