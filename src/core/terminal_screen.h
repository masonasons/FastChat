#pragma once

// A small terminal screen: enough of a terminal emulator to read what a
// full-screen terminal interface has painted. FreeBuff is one (its interface is
// OpenTUI, drawn with cursor movement and erases), and reading its screen is how
// FastChat follows a turn while it runs - its saved chat is only written once the
// reply is finished.

#include <cstdint>
#include <string>
#include <vector>

class TerminalScreen {
public:
    TerminalScreen(int cols, int rows, size_t scrollback = 4000);

    // Feed the bytes a terminal would receive.
    void Feed(const std::string& data);
    void Resize(int cols, int rows);

    // The visible lines, right-trimmed. Wide characters (emoji, CJK) are kept
    // whole; a repaint that leaves half of one behind never breaks the read.
    std::string Display() const;
    // Everything still in the scrollback, then the visible screen.
    std::string AllText() const;

    int Cols() const { return m_cols; }
    int Rows() const { return m_rows; }

private:
    using Line = std::vector<char32_t>;

    void HandleControl(unsigned char ch);
    void Put(char32_t cp);
    void LineFeed();
    void Scroll();
    void EraseInLine(int mode);
    void EraseInDisplay(int mode);
    void MoveTo(int row, int col);
    void Csi(unsigned char final, const std::vector<int>& params, bool priv);
    Line BlankLine() const;
    static std::string LineText(const Line& line);

    int m_cols, m_rows;
    size_t m_scrollback;
    std::vector<Line> m_grid;
    std::vector<Line> m_history;
    int m_row = 0;
    int m_col = 0;
    bool m_pendingWrap = false;

    enum class State { Ground, Escape, Csi, Osc, OscEscape, Charset };
    State m_state = State::Ground;
    std::vector<int> m_params;
    int m_param = -1;
    bool m_priv = false;
    std::string m_utf8;  // the bytes of a character still arriving
};
