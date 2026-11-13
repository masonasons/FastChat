#include "core/util.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

namespace fs = std::filesystem;

std::wstring Utf8ToWide(const std::string& s) {
    std::wstring out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp;
        int extra;
        if (c < 0x80) { cp = c; extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else { cp = 0xFFFD; extra = 0; }
        i++;
        for (int k = 0; k < extra; k++) {
            if (i >= s.size() || (static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) { cp = 0xFFFD; break; }
            cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
            i++;
        }
        if constexpr (sizeof(wchar_t) == 2) {
            if (cp >= 0x10000) {
                cp -= 0x10000;
                out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
                out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
                continue;
            }
        }
        out.push_back(static_cast<wchar_t>(cp));
    }
    return out;
}

std::string WideToUtf8(const std::wstring& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        uint32_t cp = static_cast<uint32_t>(s[i]);
        if constexpr (sizeof(wchar_t) == 2) {
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size()) {
                uint32_t lo = static_cast<uint32_t>(s[i + 1]);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i++;
                }
            }
        }
        // A surrogate without its partner, or a value past Unicode, has no
        // UTF-8 form.
        if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) cp = 0xFFFD;
        if (cp < 0x80) out.push_back(static_cast<char>(cp));
        else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

int64_t NowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

int64_t NowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

static std::tm LocalTm(int64_t t) {
    std::time_t tt = static_cast<std::time_t>(t);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    return tm;
}

static std::tm UtcTm(int64_t t) {
    std::time_t tt = static_cast<std::time_t>(t);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    return tm;
}

std::string FormatLocalTime(int64_t unixSeconds, bool withSeconds) {
    std::tm tm = LocalTm(unixSeconds);
    char buf[64];
    std::strftime(buf, sizeof(buf), withSeconds ? "%Y-%m-%d %H:%M:%S" : "%Y-%m-%d %H:%M", &tm);
    return buf;
}

std::string FormatIsoUtc(int64_t unixSeconds) {
    std::tm tm = UtcTm(unixSeconds);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

int64_t StartOfMonthUtc() {
    int64_t now = NowSeconds();
    std::tm tm = UtcTm(now);
    // Seconds into the month: back up to day 1, 00:00:00.
    return now - ((tm.tm_mday - 1) * 86400LL + tm.tm_hour * 3600LL + tm.tm_min * 60LL + tm.tm_sec);
}

std::string FormatRelativeTime(int64_t unixSeconds) {
    std::tm then = LocalTm(unixSeconds);
    std::tm now = LocalTm(NowSeconds());
    char time[32];
    std::strftime(time, sizeof(time), "%H:%M", &then);
    if (then.tm_year == now.tm_year && then.tm_yday == now.tm_yday) return std::string("today ") + time;
    if (then.tm_year == now.tm_year && then.tm_yday == now.tm_yday - 1) return std::string("yesterday ") + time;
    return FormatLocalTime(unixSeconds);
}

std::string FormatThousands(int64_t n) {
    bool neg = n < 0;
    std::string digits = std::to_string(neg ? -n : n);
    std::string out;
    int count = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        if (count && count % 3 == 0) out.push_back(',');
        out.push_back(*it);
        count++;
    }
    if (neg) out.push_back('-');
    std::reverse(out.begin(), out.end());
    return out;
}

static std::string TrimZeros(std::string s) {
    if (s.find('.') == std::string::npos) return s;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

std::string FormatTokensShort(int64_t n) {
    char buf[32];
    if (n >= 1000000) {
        std::snprintf(buf, sizeof(buf), "%.2f", n / 1000000.0);
        return TrimZeros(buf) + "M";
    }
    if (n >= 1000) {
        std::snprintf(buf, sizeof(buf), "%.1f", n / 1000.0);
        return TrimZeros(buf) + "K";
    }
    return std::to_string(n);
}

std::string FormatMoney(double dollars) {
    char buf[64];
    double a = dollars < 0 ? -dollars : dollars;
    if (a == 0) return "$0.00";
    if (a < 0.01) std::snprintf(buf, sizeof(buf), "$%.5f", dollars);
    else if (a < 1) std::snprintf(buf, sizeof(buf), "$%.4f", dollars);
    else std::snprintf(buf, sizeof(buf), "$%.2f", dollars);
    return buf;
}

std::string FormatPercent(double fraction) {
    char buf[32];
    double p = fraction * 100.0;
    if (p > 0 && p < 10) std::snprintf(buf, sizeof(buf), "%.1f%%", p);
    else std::snprintf(buf, sizeof(buf), "%.0f%%", p);
    return buf;
}

std::string FormatBytes(uint64_t bytes) {
    char buf[32];
    if (bytes >= 1024ULL * 1024 * 1024) std::snprintf(buf, sizeof(buf), "%.1f GB", bytes / 1073741824.0);
    else if (bytes >= 1024 * 1024) std::snprintf(buf, sizeof(buf), "%.1f MB", bytes / 1048576.0);
    else if (bytes >= 1024) std::snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    else std::snprintf(buf, sizeof(buf), "%llu bytes", static_cast<unsigned long long>(bytes));
    return buf;
}

std::string Trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) b++;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return s.substr(b, e - b);
}

std::string ToLower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool StartsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool EndsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::vector<std::string> SplitLines(const std::string& s) {
    std::vector<std::string> lines;
    std::string line;
    for (char c : s) {
        if (c == '\n') {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
            line.clear();
        } else {
            line.push_back(c);
        }
    }
    if (!line.empty()) {
        if (line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

std::string ReplaceAll(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::string TruncateUtf8(const std::string& s, size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) cut--;
    return s.substr(0, cut);
}

std::string OneLine(const std::string& s, size_t maxChars) {
    std::string out;
    out.reserve(s.size());
    bool space = false;
    for (char c : s) {
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            space = true;
            continue;
        }
        if (space && !out.empty()) out.push_back(' ');
        space = false;
        out.push_back(c);
    }
    if (maxChars && out.size() > maxChars) {
        out = TruncateUtf8(out, maxChars) + "...";
    }
    return out;
}

std::string NewId() {
    static thread_local std::mt19937_64 rng(std::random_device{}() ^
        static_cast<uint64_t>(NowMillis()));
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%016llx%016llx",
        static_cast<unsigned long long>(rng()), static_cast<unsigned long long>(rng()));
    return buf;
}

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(const std::string& data) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    size_t i = 0;
    while (i + 2 < data.size()) {
        uint32_t n = (static_cast<unsigned char>(data[i]) << 16) | (static_cast<unsigned char>(data[i + 1]) << 8) |
                     static_cast<unsigned char>(data[i + 2]);
        out.push_back(kB64[(n >> 18) & 63]);
        out.push_back(kB64[(n >> 12) & 63]);
        out.push_back(kB64[(n >> 6) & 63]);
        out.push_back(kB64[n & 63]);
        i += 3;
    }
    if (i < data.size()) {
        uint32_t n = static_cast<unsigned char>(data[i]) << 16;
        if (i + 1 < data.size()) n |= static_cast<unsigned char>(data[i + 1]) << 8;
        out.push_back(kB64[(n >> 18) & 63]);
        out.push_back(kB64[(n >> 12) & 63]);
        out.push_back(i + 1 < data.size() ? kB64[(n >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

std::string Base64Decode(const std::string& text) {
    int map[256];
    std::fill(std::begin(map), std::end(map), -1);
    for (int i = 0; i < 64; i++) map[static_cast<unsigned char>(kB64[i])] = i;
    std::string out;
    uint32_t buf = 0;
    int bits = 0;
    for (char c : text) {
        int v = map[static_cast<unsigned char>(c)];
        if (v < 0) continue;
        buf = (buf << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

std::string UrlEncode(const std::string& s) {
    std::string out;
    char hex[4];
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            std::snprintf(hex, sizeof(hex), "%%%02X", c);
            out += hex;
        }
    }
    return out;
}

bool ReadFileText(const std::string& path, std::string& out) {
    std::ifstream in(PathFromUtf8(path), std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool WriteFileText(const std::string& path, const std::string& data) {
    std::ofstream o(PathFromUtf8(path), std::ios::binary | std::ios::trunc);
    if (!o) return false;
    o.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(o);
}

int64_t EstimateTokens(const std::string& text) {
    return static_cast<int64_t>(text.size() / 4 + 1);
}

std::string StripMarkdownForSpeech(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool inFence = false;
    for (const auto& rawLine : SplitLines(text)) {
        std::string line = rawLine;
        std::string trimmed = Trim(line);
        if (StartsWith(trimmed, "```")) {
            inFence = !inFence;
            continue;
        }
        if (!inFence) {
            // Headings and quote markers
            size_t p = 0;
            while (p < line.size() && (line[p] == '#' || line[p] == '>')) p++;
            if (p > 0 && p < line.size() && line[p] == ' ') line = line.substr(p + 1);
            // Bullets
            std::string t = Trim(line);
            if (StartsWith(t, "- ") || StartsWith(t, "* ") || StartsWith(t, "+ ")) line = t.substr(2);
            // Horizontal rules
            if (t == "---" || t == "***" || t == "___") continue;
            // Links: [text](url) -> text
            std::string cleaned;
            for (size_t i = 0; i < line.size(); i++) {
                char c = line[i];
                if (c == '[') {
                    size_t close = line.find(']', i);
                    if (close != std::string::npos && close + 1 < line.size() && line[close + 1] == '(') {
                        size_t paren = line.find(')', close);
                        if (paren != std::string::npos) {
                            cleaned += line.substr(i + 1, close - i - 1);
                            i = paren;
                            continue;
                        }
                    }
                }
                if (c == '*' || c == '`' || (c == '_' && (i == 0 || i + 1 == line.size() ||
                    !std::isalnum(static_cast<unsigned char>(line[i - 1])) ||
                    !std::isalnum(static_cast<unsigned char>(line[i + 1]))))) {
                    continue;
                }
                if (c == '|') {
                    cleaned += ", ";
                    continue;
                }
                cleaned.push_back(c);
            }
            line = cleaned;
        }
        out += line;
        out.push_back('\n');
    }
    return Trim(out);
}

std::vector<std::string> ExtractCodeBlocks(const std::string& markdown) {
    std::vector<std::string> blocks;
    bool in = false;
    std::string current;
    for (const auto& line : SplitLines(markdown)) {
        if (StartsWith(Trim(line), "```")) {
            if (in) {
                blocks.push_back(current);
                current.clear();
            }
            in = !in;
            continue;
        }
        if (in) {
            current += line;
            current.push_back('\n');
        }
    }
    if (in && !current.empty()) blocks.push_back(current);
    return blocks;
}

std::filesystem::path PathFromUtf8(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

std::string PathToUtf8(const std::filesystem::path& p) {
    std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::string ResolveFolder(const std::string& typed, const std::string& base, const std::string& home) {
    std::string t = Trim(typed);
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"') t = t.substr(1, t.size() - 2);  // a pasted "C:\My dev"
    if (t.empty()) return Trim(base);
    std::filesystem::path p = PathFromUtf8(t);
    if ((t == "~" || StartsWith(t, "~/") || StartsWith(t, "~\\")) && !home.empty()) {
        p = PathFromUtf8(home) / PathFromUtf8(t.size() > 2 ? t.substr(2) : std::string());
    } else if (p.is_relative() && !p.has_root_name() && !p.has_root_directory() && !Trim(base).empty()) {
        p = PathFromUtf8(Trim(base)) / p;
    }
    p = p.lexically_normal();
    // No trailing separator, except for a drive's or the system's root.
    if (!p.has_filename() && p != p.root_path()) p = p.parent_path();
    return PathToUtf8(p);
}

// Length of the well-formed UTF-8 sequence at s[i], or 0 if it is not one.
static size_t Utf8SequenceLength(const std::string& s, size_t i) {
    auto byte = [&](size_t k) { return k < s.size() ? static_cast<unsigned char>(s[k]) : 0u; };
    unsigned char c = byte(i);
    if (c < 0x80) return 1;
    auto cont = [&](size_t k, unsigned lo = 0x80, unsigned hi = 0xBF) {
        unsigned b = byte(k);
        return k < s.size() && b >= lo && b <= hi;
    };
    if (c >= 0xC2 && c <= 0xDF) return cont(i + 1) ? 2 : 0;
    if (c == 0xE0) return cont(i + 1, 0xA0) && cont(i + 2) ? 3 : 0;
    if ((c >= 0xE1 && c <= 0xEC) || c == 0xEE || c == 0xEF) return cont(i + 1) && cont(i + 2) ? 3 : 0;
    if (c == 0xED) return cont(i + 1, 0x80, 0x9F) && cont(i + 2) ? 3 : 0;  // no surrogates
    if (c == 0xF0) return cont(i + 1, 0x90) && cont(i + 2) && cont(i + 3) ? 4 : 0;
    if (c >= 0xF1 && c <= 0xF3) return cont(i + 1) && cont(i + 2) && cont(i + 3) ? 4 : 0;
    if (c == 0xF4) return cont(i + 1, 0x80, 0x8F) && cont(i + 2) && cont(i + 3) ? 4 : 0;
    return 0;
}

bool IsValidUtf8(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        size_t n = Utf8SequenceLength(s, i);
        if (!n) return false;
        i += n;
    }
    return true;
}

std::string SanitizeUtf8(const std::string& s) {
    if (IsValidUtf8(s)) return s;
    std::string out;
    out.reserve(s.size() + 16);
    for (size_t i = 0; i < s.size();) {
        size_t n = Utf8SequenceLength(s, i);
        if (n) {
            out.append(s, i, n);
            i += n;
        } else {
            out += "\xEF\xBF\xBD";  // U+FFFD, the replacement character
            i++;
        }
    }
    return out;
}
