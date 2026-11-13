// The terminal screen FreeBuff's interface is read off.

#include "core/terminal_screen.h"

#include <algorithm>

namespace {

// A character drawn two cells wide: CJK text, and the emoji FreeBuff's
// interface is full of. The cell after it is left as a stub, which reads back as
// nothing - a repaint that strands one must never break a read.
bool IsWide(char32_t cp) {
    return (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0x303E) ||
           (cp >= 0x3041 && cp <= 0x33FF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
           (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xA000 && cp <= 0xA4CF) ||
           (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
           (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F000 && cp <= 0x1FAFF) ||
           (cp >= 0x20000 && cp <= 0x3FFFD);
}

void AppendUtf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
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

int FirstByteLength(unsigned char ch) {
    if (ch < 0x80) return 1;
    if ((ch & 0xE0) == 0xC0) return 2;
    if ((ch & 0xF0) == 0xE0) return 3;
    if ((ch & 0xF8) == 0xF0) return 4;
    return 0;  // a stray continuation byte
}

}  // namespace

TerminalScreen::TerminalScreen(int cols, int rows, size_t scrollback)
    : m_cols(std::max(1, cols)), m_rows(std::max(1, rows)), m_scrollback(scrollback) {
    m_grid.assign(static_cast<size_t>(m_rows), BlankLine());
}

TerminalScreen::Line TerminalScreen::BlankLine() const {
    return Line(static_cast<size_t>(m_cols), U' ');
}

void TerminalScreen::Resize(int cols, int rows) {
    if (cols == m_cols && rows == m_rows) return;
    m_cols = std::max(1, cols);
    m_rows = std::max(1, rows);
    m_history.clear();
    m_grid.assign(static_cast<size_t>(m_rows), BlankLine());
    m_row = m_col = 0;
    m_pendingWrap = false;
}

void TerminalScreen::Feed(const std::string& data) {
    for (unsigned char ch : data) {
        // Inside an escape sequence the bytes are never text.
        if (m_state == State::Escape) {
            if (ch == '[') {
                m_state = State::Csi;
                m_params.clear();
                m_param = -1;
                m_priv = false;
            } else if (ch == ']') {
                m_state = State::Osc;
            } else if (ch == '(' || ch == ')' || ch == '*' || ch == '+' || ch == '#') {
                m_state = State::Charset;
            } else {
                m_state = State::Ground;
            }
            continue;
        }
        if (m_state == State::Charset) {
            m_state = State::Ground;
            continue;
        }
        if (m_state == State::Osc) {
            if (ch == 0x07) m_state = State::Ground;
            else if (ch == 0x1b) m_state = State::OscEscape;
            continue;
        }
        if (m_state == State::OscEscape) {
            m_state = ch == '\\' ? State::Ground : State::Osc;
            continue;
        }
        if (m_state == State::Csi) {
            if (ch >= '0' && ch <= '9') {
                m_param = (m_param < 0 ? 0 : m_param) * 10 + (ch - '0');
                if (m_param > 100000) m_param = 100000;
                continue;
            }
            if (ch == ';' || ch == ':') {
                m_params.push_back(m_param < 0 ? 0 : m_param);
                m_param = -1;
                continue;
            }
            if (ch == '?') {
                m_priv = true;
                continue;
            }
            if (ch >= 0x20 && ch <= 0x2F) continue;  // an intermediate byte
            if (ch >= 0x40 && ch <= 0x7E) {
                if (m_param >= 0) m_params.push_back(m_param);
                m_param = -1;
                m_state = State::Ground;
                Csi(ch, m_params, m_priv);
                m_params.clear();
                m_priv = false;
                continue;
            }
            m_state = State::Ground;
            continue;
        }

        // Ground.
        if (ch == 0x1b) {
            m_state = State::Escape;
            continue;
        }
        if (!m_utf8.empty() || ch >= 0x80) {
            if (m_utf8.empty() && FirstByteLength(ch) == 0) continue;  // a stray byte
            m_utf8.push_back(static_cast<char>(ch));
            size_t want = static_cast<size_t>(FirstByteLength(static_cast<unsigned char>(m_utf8[0])));
            if (m_utf8.size() < want) continue;
            unsigned char lead = static_cast<unsigned char>(m_utf8[0]);
            char32_t cp = 0;
            if (want == 2) cp = static_cast<char32_t>(lead & 0x1F);
            else if (want == 3) cp = static_cast<char32_t>(lead & 0x0F);
            else if (want == 4) cp = static_cast<char32_t>(lead & 0x07);
            for (size_t i = 1; i < m_utf8.size(); i++) {
                cp = (cp << 6) | (static_cast<unsigned char>(m_utf8[i]) & 0x3F);
            }
            m_utf8.clear();
            Put(cp);
            continue;
        }
        HandleControl(ch);
    }
}

void TerminalScreen::HandleControl(unsigned char ch) {
    switch (ch) {
        case '\r': m_col = 0; m_pendingWrap = false; break;
        case '\n': LineFeed(); break;
        case 0x08:  // backspace
            if (m_col > 0) m_col--;
            m_pendingWrap = false;
            break;
        case '\t': {
            int next = (m_col / 8 + 1) * 8;
            m_col = std::min(m_cols - 1, next);
            m_pendingWrap = false;
            break;
        }
        case 0x07:  // bell
        case 0x00:
            break;
        default:
            if (ch >= 0x20) Put(ch);
            break;
    }
}

void TerminalScreen::Put(char32_t cp) {
    if (m_pendingWrap) {
        m_col = 0;
        LineFeed();
        m_pendingWrap = false;
    }
    int width = IsWide(cp) ? 2 : 1;
    if (m_col >= m_cols) {
        m_col = 0;
        LineFeed();
    }
    if (m_col + width > m_cols) {
        // No room for a wide character at the right edge; the next line gets it.
        m_col = 0;
        LineFeed();
    }
    Line& line = m_grid[static_cast<size_t>(m_row)];
    line[static_cast<size_t>(m_col)] = cp;
    if (width == 2 && m_col + 1 < m_cols) line[static_cast<size_t>(m_col + 1)] = 0;
    m_col += width;
    m_pendingWrap = m_col >= m_cols;
}

void TerminalScreen::LineFeed() {
    m_pendingWrap = false;
    if (m_row + 1 < m_rows) {
        m_row++;
        return;
    }
    Scroll();
}

void TerminalScreen::Scroll() {
    if (m_scrollback > 0) {
        m_history.push_back(m_grid.front());
        if (m_history.size() > m_scrollback) m_history.erase(m_history.begin());
    }
    m_grid.erase(m_grid.begin());
    m_grid.push_back(BlankLine());
    m_row = m_rows - 1;
}

void TerminalScreen::EraseInLine(int mode) {
    Line& line = m_grid[static_cast<size_t>(m_row)];
    if (mode == 0) {
        for (int x = m_col; x < m_cols; x++) line[static_cast<size_t>(x)] = U' ';
    } else if (mode == 1) {
        for (int x = 0; x <= m_col && x < m_cols; x++) line[static_cast<size_t>(x)] = U' ';
    } else if (mode == 2) {
        line.assign(static_cast<size_t>(m_cols), U' ');
    }
    m_pendingWrap = false;
}

void TerminalScreen::EraseInDisplay(int mode) {
    if (mode == 0) {
        EraseInLine(0);
        for (int y = m_row + 1; y < m_rows; y++) m_grid[static_cast<size_t>(y)] = BlankLine();
    } else if (mode == 1) {
        for (int y = 0; y < m_row; y++) m_grid[static_cast<size_t>(y)] = BlankLine();
        EraseInLine(1);
    } else {
        for (auto& line : m_grid) line = BlankLine();
        m_row = m_col = 0;
    }
    m_pendingWrap = false;
}

void TerminalScreen::MoveTo(int row, int col) {
    m_row = std::clamp(row, 0, m_rows - 1);
    m_col = std::clamp(col, 0, m_cols - 1);
    m_pendingWrap = false;
}

void TerminalScreen::Csi(unsigned char final, const std::vector<int>& params, bool priv) {
    auto arg = [&](size_t index, int fallback) {
        if (index >= params.size() || params[index] == 0) return fallback;
        return params[index];
    };
    switch (final) {
        case 'A': MoveTo(m_row - arg(0, 1), m_col); break;
        case 'B': MoveTo(m_row + arg(0, 1), m_col); break;
        case 'C': MoveTo(m_row, m_col + arg(0, 1)); break;
        case 'D': MoveTo(m_row, m_col - arg(0, 1)); break;
        case 'E': MoveTo(m_row + arg(0, 1), 0); break;
        case 'F': MoveTo(m_row - arg(0, 1), 0); break;
        case 'G': MoveTo(m_row, arg(0, 1) - 1); break;
        case 'd': MoveTo(arg(0, 1) - 1, m_col); break;
        case 'H':
        case 'f': MoveTo(arg(0, 1) - 1, arg(1, 1) - 1); break;
        case 'J': EraseInDisplay(params.empty() ? 0 : params[0]); break;
        case 'K': EraseInLine(params.empty() ? 0 : params[0]); break;
        case 'P': {  // delete characters
            int count = arg(0, 1);
            Line& line = m_grid[static_cast<size_t>(m_row)];
            for (int x = m_col; x < m_cols; x++) {
                line[static_cast<size_t>(x)] = x + count < m_cols ? line[static_cast<size_t>(x + count)] : U' ';
            }
            break;
        }
        case '@': {  // insert characters
            int count = arg(0, 1);
            Line& line = m_grid[static_cast<size_t>(m_row)];
            for (int x = m_cols - 1; x >= m_col; x--) {
                line[static_cast<size_t>(x)] = x - count >= m_col ? line[static_cast<size_t>(x - count)] : U' ';
            }
            break;
        }
        case 'X': {  // erase characters
            int count = arg(0, 1);
            Line& line = m_grid[static_cast<size_t>(m_row)];
            for (int x = m_col; x < m_cols && x < m_col + count; x++) line[static_cast<size_t>(x)] = U' ';
            break;
        }
        case 'L': {  // insert lines
            int count = arg(0, 1);
            for (int i = 0; i < count; i++) {
                m_grid.insert(m_grid.begin() + m_row, BlankLine());
                m_grid.pop_back();
            }
            break;
        }
        case 'M': {  // delete lines
            int count = arg(0, 1);
            for (int i = 0; i < count; i++) {
                m_grid.erase(m_grid.begin() + m_row);
                m_grid.push_back(BlankLine());
            }
            break;
        }
        case 'S':  // scroll up
            for (int i = 0; i < arg(0, 1); i++) Scroll();
            break;
        case 'T': {  // scroll down
            int count = arg(0, 1);
            for (int i = 0; i < count; i++) {
                m_grid.insert(m_grid.begin(), BlankLine());
                m_grid.pop_back();
            }
            break;
        }
        case 'h':
        case 'l':
            // The alternate screen: clearing on the way in and out keeps the
            // screen from mixing two interfaces' rows.
            if (priv) {
                for (int p : params) {
                    if (p == 47 || p == 1047 || p == 1049) {
                        for (auto& line : m_grid) line = BlankLine();
                        m_row = m_col = 0;
                    }
                }
            }
            break;
        default:
            break;  // colors, modes, cursor visibility: nothing to read
    }
}

std::string TerminalScreen::LineText(const Line& line) {
    std::string text;
    for (char32_t cp : line) {
        if (cp == ' ') {
            text.push_back(' ');
        } else if (cp == 0) {
            // A wide character's stub cell: the character itself is beside it.
        } else {
            AppendUtf8(text, cp);
        }
    }
    while (!text.empty() && text.back() == ' ') text.pop_back();
    return text;
}

std::string TerminalScreen::Display() const {
    std::string out;
    for (int y = 0; y < m_rows; y++) {
        if (y) out.push_back('\n');
        out += LineText(m_grid[static_cast<size_t>(y)]);
    }
    return out;
}

std::string TerminalScreen::AllText() const {
    std::string out;
    for (const auto& line : m_history) {
        out += LineText(line);
        out.push_back('\n');
    }
    out += Display();
    return out;
}
