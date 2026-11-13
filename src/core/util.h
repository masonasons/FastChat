#pragma once

// Small helpers shared by everything: text conversion, time, formatting, ids.
// FastChat keeps text as UTF-8 std::string everywhere outside the windows.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& s);

// Seconds since the Unix epoch.
int64_t NowSeconds();
// Milliseconds since the Unix epoch.
int64_t NowMillis();
// "2026-11-11 16:53" in local time.
std::string FormatLocalTime(int64_t unixSeconds, bool withSeconds = false);
// "2026-11-01T00:00:00Z" for the given time, in UTC.
std::string FormatIsoUtc(int64_t unixSeconds);
// The first second of the current month, UTC.
int64_t StartOfMonthUtc();
// "today", "yesterday", "3 days ago", or a date.
std::string FormatRelativeTime(int64_t unixSeconds);

// "12,345"
std::string FormatThousands(int64_t n);
// "12.3K", "1M", "980"
std::string FormatTokensShort(int64_t n);
// "$0.0123" (more digits for tiny amounts)
std::string FormatMoney(double dollars);
// Percent with no decimals, or one below 10.
std::string FormatPercent(double fraction);
// "1.2 KB"
std::string FormatBytes(uint64_t bytes);

std::string Trim(const std::string& s);
std::string ToLower(std::string s);
bool StartsWith(const std::string& s, const std::string& prefix);
bool EndsWith(const std::string& s, const std::string& suffix);
std::vector<std::string> SplitLines(const std::string& s);
std::string ReplaceAll(std::string s, const std::string& from, const std::string& to);
// Collapse whitespace runs to single spaces and cut to `maxChars` (UTF-8 safe), adding "...".
std::string OneLine(const std::string& s, size_t maxChars = 0);
// Cut to at most maxBytes without splitting a UTF-8 sequence.
std::string TruncateUtf8(const std::string& s, size_t maxBytes);
// A random id like "c8a0290f..." (32 hex characters).
std::string NewId();

// Base64 (standard alphabet, padded).
std::string Base64Encode(const std::string& data);
std::string Base64Decode(const std::string& text);

// Percent-encode for a URL query value.
std::string UrlEncode(const std::string& s);

// Read / write a whole file. Paths are UTF-8.
bool ReadFileText(const std::string& path, std::string& out);
bool WriteFileText(const std::string& path, const std::string& data);

// A rough token estimate for text no provider has counted (about 4 characters a token).
int64_t EstimateTokens(const std::string& text);

// Remove Markdown decoration (emphasis, headings, code fences, link targets) so
// text reads well through speech.
std::string StripMarkdownForSpeech(const std::string& text);

// Code blocks (``` fenced) in Markdown text, without their fences.
std::vector<std::string> ExtractCodeBlocks(const std::string& markdown);

// Whether `s` is well-formed UTF-8 (no overlong forms, surrogates or code
// points past U+10FFFF), and a copy with every bad sequence replaced by U+FFFD.
// Text from outside (command output, files, the network) passes through
// SanitizeUtf8 before it is stored or sent anywhere.
bool IsValidUtf8(const std::string& s);
std::string SanitizeUtf8(const std::string& s);

// UTF-8 text <-> filesystem paths (C++20 made u8string() return char8_t).
std::filesystem::path PathFromUtf8(const std::string& s);
std::string PathToUtf8(const std::filesystem::path& p);

// A folder as typed: a name or relative path is inside `base` (when there is
// one); "~" is the home folder. Absolute paths stay as they are.
std::string ResolveFolder(const std::string& typed, const std::string& base, const std::string& home);
