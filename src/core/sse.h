#pragma once

// Server-sent events and newline-delimited JSON, fed piece by piece as a
// streamed response arrives.

#include <functional>
#include <string>

class SseParser {
public:
    // event: the "event:" field (empty if none); data: the joined "data:" lines.
    using Handler = std::function<bool(const std::string& event, const std::string& data)>;
    explicit SseParser(Handler handler) : m_handler(std::move(handler)) {}

    // Returns false once the handler asks to stop.
    bool Feed(const char* data, size_t size);
    // Deliver an event left without its closing blank line.
    bool Finish();

private:
    bool Dispatch();
    bool HandleLine(const std::string& line);

    Handler m_handler;
    std::string m_buffer;
    std::string m_event;
    std::string m_data;
    bool m_hasData = false;
};

class NdjsonParser {
public:
    using Handler = std::function<bool(const std::string& line)>;
    explicit NdjsonParser(Handler handler) : m_handler(std::move(handler)) {}
    bool Feed(const char* data, size_t size);
    bool Finish();

private:
    Handler m_handler;
    std::string m_buffer;
};
