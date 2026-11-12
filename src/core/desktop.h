#pragma once

// Seeing and acting on the desktop, for computer use conversations: a target
// window (never one of FastChat's own), its accessibility tree with numbered
// elements, screenshots, and keyboard and mouse input. Windows
// (platform/desktop_windows.cpp) and the Mac (platform/desktop_mac.mm);
// elsewhere Create() returns null.
//
// Create and use a Desktop on one thread (on Windows it sets that thread up for
// COM and per-monitor DPI); the chat worker thread does.

#include <memory>
#include <string>

struct Screenshot {
    std::string base64;
    std::string mediaType;
    int width = 0;   // image pixels
    int height = 0;
};

class Desktop {
public:
    static std::unique_ptr<Desktop> Create();
    virtual ~Desktop() = default;

    // Why it cannot work now (a permission the system has not given), or empty.
    virtual std::string Unavailable() { return std::string(); }

    // The window actions go to, described ("Untitled - Notepad (notepad.exe)"),
    // or empty when there is none.
    virtual std::string TargetDescription() = 0;
    // The open windows, numbered, the target marked.
    virtual std::string ListWindows() = 0;
    // Make a window (its number from ListWindows, or part of its title) the
    // target and bring it to the front.
    virtual bool FocusWindow(const std::string& which, std::string& message) = 0;
    // For tests: make this native window the target.
    virtual void SetTargetHandle(void* handle) = 0;

    // The target's monitor, scaled to fit 1280 x 800. Coordinates the model
    // gives are in this image's pixels.
    virtual bool Capture(Screenshot& out, std::string& error) = 0;
    // The target window's accessibility tree, elements numbered [1], [2]...
    // (numbers stay valid until the next call), with positions in screenshot
    // pixels. `maxElements` caps it.
    virtual std::string Tree(int maxElements) = 0;
    // The element with keyboard focus, described, after an action.
    virtual std::string FocusedDescription() = 0;
    // An element's description ("Save button"), or empty if there is no such number.
    virtual std::string ElementDescription(int id) = 0;
    // An element's centre in screenshot pixels.
    virtual bool ElementPoint(int id, int& x, int& y) = 0;

    // Acting on elements without the mouse (the accessibility way): press a
    // button / toggle a check box / select an item; put text in a field; focus.
    virtual bool Invoke(int id, std::string& message) = 0;
    virtual bool SetText(int id, const std::string& text, std::string& message) = 0;
    virtual bool FocusElement(int id, std::string& message) = 0;

    // Input, at screenshot-pixel coordinates. The target comes to the front first.
    virtual bool Click(int x, int y, const std::string& button, int count, std::string& message) = 0;
    virtual bool Drag(int x1, int y1, int x2, int y2, std::string& message) = 0;
    virtual bool Scroll(int x, int y, const std::string& direction, int amount, std::string& message) = 0;
    virtual bool MoveMouse(int x, int y, std::string& message) = 0;
    virtual bool TypeText(const std::string& text, std::string& message) = 0;
    // "ctrl+s", "alt+f4", "enter"; several separated by spaces are pressed in turn.
    virtual bool PressKeys(const std::string& keys, std::string& message) = 0;

    // The screen size in screenshot pixels ("1280 x 720").
    virtual std::string ScreenDescription() = 0;
};
