#include "core/sse.h"

bool SseParser::Feed(const char* data, size_t size) {
    m_buffer.append(data, size);
    size_t start = 0;
    for (;;) {
        size_t nl = m_buffer.find('\n', start);
        if (nl == std::string::npos) break;
        std::string line = m_buffer.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        start = nl + 1;
        if (!HandleLine(line)) {
            m_buffer.erase(0, start);
            return false;
        }
    }
    m_buffer.erase(0, start);
    return true;
}

bool SseParser::HandleLine(const std::string& line) {
    if (line.empty()) return Dispatch();
    if (line[0] == ':') return true;  // comment (keep-alive)
    size_t colon = line.find(':');
    std::string field = colon == std::string::npos ? line : line.substr(0, colon);
    std::string value;
    if (colon != std::string::npos) {
        value = line.substr(colon + 1);
        if (!value.empty() && value[0] == ' ') value.erase(0, 1);
    }
    if (field == "event") {
        m_event = value;
    } else if (field == "data") {
        if (m_hasData) m_data.push_back('\n');
        m_data += value;
        m_hasData = true;
    }
    return true;
}

bool SseParser::Dispatch() {
    if (!m_hasData) {
        m_event.clear();
        return true;
    }
    std::string event = std::move(m_event);
    std::string data = std::move(m_data);
    m_event.clear();
    m_data.clear();
    m_hasData = false;
    return m_handler(event, data);
}

bool SseParser::Finish() {
    if (!m_buffer.empty()) {
        std::string line = m_buffer;
        m_buffer.clear();
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!HandleLine(line)) return false;
    }
    return Dispatch();
}

bool NdjsonParser::Feed(const char* data, size_t size) {
    m_buffer.append(data, size);
    size_t start = 0;
    for (;;) {
        size_t nl = m_buffer.find('\n', start);
        if (nl == std::string::npos) break;
        std::string line = m_buffer.substr(start, nl - start);
        start = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (!m_handler(line)) {
            m_buffer.erase(0, start);
            return false;
        }
    }
    m_buffer.erase(0, start);
    return true;
}

bool NdjsonParser::Finish() {
    if (m_buffer.empty()) return true;
    std::string line = std::move(m_buffer);
    m_buffer.clear();
    return m_handler(line);
}
