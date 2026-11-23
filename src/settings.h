// Text editing used by WARVRConfig.exe: WARVR.ini, dxvk.conf and the private server's World.xml.
// Pure C++ (no Windows headers) so tests/settings_test.cpp runs on any platform.
//
// Every edit keeps the rest of the file as it was (comments, unknown keys, order, line endings): the tool only
// rewrites the lines it owns, so hand-edited files and the settings window can be used side by side.
#pragma once
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

namespace settings {

inline std::string Trim(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

inline bool IEq(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}

// "on", "1", "true", "yes" (any case) -> true.
inline bool IsOn(const std::string& v)
{
    return IEq(v, "on") || IEq(v, "1") || IEq(v, "true") || IEq(v, "yes");
}

// Lines without their terminators; *crlf tells whether the file uses \r\n (new files: \r\n, it is a Windows tool).
inline std::vector<std::string> SplitLines(const std::string& text, bool* crlf)
{
    *crlf = text.empty() || text.find("\r\n") != std::string::npos;
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(line);
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return out;
}

inline std::string JoinLines(const std::vector<std::string>& lines, bool crlf)
{
    std::string out;
    for (const std::string& l : lines) { out += l; out += crlf ? "\r\n" : "\n"; }
    return out;
}

// One "key<sep>value" line. Comments (; or #) and lines without the separator are not settings.
inline bool ParseLine(const std::string& line, char sep, std::string* key, std::string* val)
{
    const std::string t = Trim(line);
    if (t.empty() || t[0] == ';' || t[0] == '#') return false;
    const size_t eq = t.find(sep);
    if (eq == std::string::npos) return false;
    *key = Trim(t.substr(0, eq));
    *val = Trim(t.substr(eq + 1));
    return true;
}

// WARVR.ini: the mod reads lines top to bottom and the last one for a key wins. Its value stops at the first space,
// so "laser=on ; my note" means on. wholeValue keeps the rest of the line (for a folder name with spaces in it).
inline bool IniGet(const std::string& text, const std::string& key, std::string* out, bool wholeValue = false)
{
    bool crlf, found = false;
    for (const std::string& line : SplitLines(text, &crlf)) {
        std::string k, v;
        if (!ParseLine(line, '=', &k, &v) || !IEq(k, key)) continue;
        const size_t sp = wholeValue ? std::string::npos : v.find_first_of(" \t");
        *out = sp == std::string::npos ? v : v.substr(0, sp);
        found = true;
    }
    return found;
}

// Sets key=value: rewrites every line that sets the key (so no later line overrides it), or appends one line.
// The mod does not trim the key, so the tool always writes "key=value" with no spaces.
inline std::string IniSet(const std::string& text, const std::string& key, const std::string& value)
{
    bool crlf;
    std::vector<std::string> lines = SplitLines(text, &crlf);
    bool done = false;
    for (std::string& line : lines) {
        std::string k, v;
        if (ParseLine(line, '=', &k, &v) && IEq(k, key)) { line = key + "=" + value; done = true; }
    }
    if (!done) lines.push_back(key + "=" + value);
    return JoinLines(lines, crlf);
}

// Turns every line that sets the key into a comment ("; key=value"), so the mod ignores it but the value stays visible.
inline std::string IniDisable(const std::string& text, const std::string& key)
{
    bool crlf;
    std::vector<std::string> lines = SplitLines(text, &crlf);
    for (std::string& line : lines) {
        std::string k, v;
        if (ParseLine(line, '=', &k, &v) && IEq(k, key)) line = "; " + line;
    }
    return JoinLines(lines, crlf);
}

// dxvk.conf ("d3d9.samplerAnisotropy = 16"): an empty value removes the setting, so DXVK falls back to its default.
inline bool ConfGet(const std::string& text, const std::string& key, std::string* out)
{
    bool crlf, found = false;
    for (const std::string& line : SplitLines(text, &crlf)) {
        std::string k, v;
        if (ParseLine(line, '=', &k, &v) && k == key) { *out = v; found = true; }
    }
    return found;
}

inline std::string ConfSet(const std::string& text, const std::string& key, const std::string& value)
{
    bool crlf;
    std::vector<std::string> in = SplitLines(text, &crlf), out;
    bool done = false;
    for (const std::string& line : in) {
        std::string k, v;
        if (ParseLine(line, '=', &k, &v) && k == key) {
            if (!value.empty() && !done) out.push_back(key + " = " + value);
            done = true;
            continue;
        }
        out.push_back(line);
    }
    if (!done && !value.empty()) out.push_back(key + " = " + value);
    return JoinLines(out, crlf);
}

// World.xml: <Name>value</Name>. Returns false if the element is not there.
inline bool XmlGet(const std::string& text, const std::string& name, std::string* out)
{
    const std::string open = "<" + name + ">", close = "</" + name + ">";
    const size_t a = text.find(open);
    if (a == std::string::npos) return false;
    const size_t b = text.find(close, a + open.size());
    if (b == std::string::npos) return false;
    *out = Trim(text.substr(a + open.size(), b - a - open.size()));
    return true;
}

// Sets <Name>value</Name>. If it is missing, it goes on its own line after the line holding </After> (same indent),
// or, failing that, before the last closing tag of the file. Returns false only for text that is not XML at all.
inline bool XmlSet(std::string* text, const std::string& name, const std::string& value, const std::string& after)
{
    const std::string open = "<" + name + ">", close = "</" + name + ">";
    const size_t a = text->find(open);
    if (a != std::string::npos) {
        const size_t b = text->find(close, a + open.size());
        if (b == std::string::npos) return false;
        text->replace(a + open.size(), b - a - open.size(), value);
        return true;
    }
    const std::string nl = text->find("\r\n") != std::string::npos ? "\r\n" : "\n";
    size_t at = std::string::npos;
    std::string indent = "  ";
    const size_t prev = after.empty() ? std::string::npos : text->find("</" + after + ">");
    if (prev != std::string::npos) {
        const size_t ls = text->rfind('\n', prev);
        const size_t lineStart = ls == std::string::npos ? 0 : ls + 1;
        indent = text->substr(lineStart, text->find_first_not_of(" \t", lineStart) - lineStart);
        const size_t le = text->find('\n', prev);
        at = le == std::string::npos ? text->size() : le + 1;
        const std::string line = indent + open + value + close + nl;
        if (le == std::string::npos) text->append(nl + line);
        else text->insert(at, line);
        return true;
    }
    const size_t last = text->rfind("</");
    if (last == std::string::npos) return false;
    const size_t ls = text->rfind('\n', last);
    at = ls == std::string::npos ? last : ls + 1;
    text->insert(at, indent + open + value + close + nl);
    return true;
}

// What d3d9.dll in the game folder is, from strings inside the file.
enum class D3D9Kind { None, Dxvk, DgVoodoo, Other };

inline D3D9Kind DetectD3D9(const std::string& bytes)
{
    if (bytes.empty()) return D3D9Kind::None;
    if (bytes.find("dxvk") != std::string::npos || bytes.find("DXVK") != std::string::npos) return D3D9Kind::Dxvk;
    if (bytes.find("dgVoodoo") != std::string::npos) return D3D9Kind::DgVoodoo;
    return D3D9Kind::Other;
}

// The server reads World.xml into a class; a .NET assembly keeps its field names as plain strings, so a WorldServer.exe
// built with the time-of-day change contains this one.
inline bool ServerHasTimeOffset(const std::string& exeBytes)
{
    return exeBytes.find("TimeOfDayOffsetHours") != std::string::npos;
}

} // namespace settings
