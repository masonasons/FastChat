// Computer use on Windows: UI Automation for the accessibility tree and acting
// on elements, GDI + WIC for screenshots (JPEG), SendInput for the keyboard
// and mouse. Runs on the chat worker thread, which it sets up for COM (MTA) and
// per-monitor DPI so that every coordinate is a physical pixel.

#include "core/desktop.h"
#include "core/util.h"

#include <windows.h>
#include <dwmapi.h>
#include <objbase.h>
#include <UIAutomation.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

const int kMaxWidth = 1280;
const int kMaxHeight = 800;

std::string WindowTitle(HWND h) {
    wchar_t buf[512];
    int n = GetWindowTextW(h, buf, 512);
    return WideToUtf8(std::wstring(buf, n > 0 ? n : 0));
}

std::string ClassOf(HWND h) {
    wchar_t buf[256];
    int n = GetClassNameW(h, buf, 256);
    return WideToUtf8(std::wstring(buf, n > 0 ? n : 0));
}

std::string ProcessName(HWND h) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return std::string();
    wchar_t path[MAX_PATH * 2];
    DWORD size = MAX_PATH * 2;
    std::string name;
    if (QueryFullProcessImageNameW(p, 0, path, &size)) {
        std::wstring s(path, size);
        size_t slash = s.find_last_of(L"\\/");
        name = WideToUtf8(slash == std::wstring::npos ? s : s.substr(slash + 1));
    }
    CloseHandle(p);
    return name;
}

bool IsOwnWindow(HWND h) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    return pid == GetCurrentProcessId();
}

// Windows a person would switch to: visible, titled, not a tool window, not
// hidden by the shell (cloaked), and not part of the shell itself.
bool IsUserWindow(HWND h) {
    if (!IsWindowVisible(h) || IsOwnWindow(h)) return false;
    if (GetWindowTextLengthW(h) == 0) return false;
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if ((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW)) return false;
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return false;
    std::string cls = ClassOf(h);
    if (cls == "Progman" || cls == "WorkerW" || cls == "Shell_TrayWnd" || cls == "Shell_SecondaryTrayWnd") return false;
    return true;
}

std::vector<HWND> UserWindows() {
    std::vector<HWND> list;
    EnumWindows(
        [](HWND h, LPARAM l) -> BOOL {
            if (IsUserWindow(h)) reinterpret_cast<std::vector<HWND>*>(l)->push_back(h);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&list));
    return list;
}

std::string Describe(HWND h) {
    if (!h) return std::string();
    std::string text = WindowTitle(h);
    std::string proc = ProcessName(h);
    if (!proc.empty()) text += " (" + proc + ")";
    return text;
}

const char* ControlTypeName(CONTROLTYPEID id) {
    switch (id) {
        case UIA_ButtonControlTypeId: return "button";
        case UIA_CalendarControlTypeId: return "calendar";
        case UIA_CheckBoxControlTypeId: return "check box";
        case UIA_ComboBoxControlTypeId: return "combo box";
        case UIA_EditControlTypeId: return "edit";
        case UIA_HyperlinkControlTypeId: return "link";
        case UIA_ImageControlTypeId: return "image";
        case UIA_ListItemControlTypeId: return "list item";
        case UIA_ListControlTypeId: return "list";
        case UIA_MenuControlTypeId: return "menu";
        case UIA_MenuBarControlTypeId: return "menu bar";
        case UIA_MenuItemControlTypeId: return "menu item";
        case UIA_ProgressBarControlTypeId: return "progress bar";
        case UIA_RadioButtonControlTypeId: return "radio button";
        case UIA_ScrollBarControlTypeId: return "scroll bar";
        case UIA_SliderControlTypeId: return "slider";
        case UIA_SpinnerControlTypeId: return "spinner";
        case UIA_StatusBarControlTypeId: return "status bar";
        case UIA_TabControlTypeId: return "tab list";
        case UIA_TabItemControlTypeId: return "tab";
        case UIA_TextControlTypeId: return "text";
        case UIA_ToolBarControlTypeId: return "toolbar";
        case UIA_ToolTipControlTypeId: return "tooltip";
        case UIA_TreeControlTypeId: return "tree";
        case UIA_TreeItemControlTypeId: return "tree item";
        case UIA_CustomControlTypeId: return "custom";
        case UIA_GroupControlTypeId: return "group";
        case UIA_ThumbControlTypeId: return "thumb";
        case UIA_DataGridControlTypeId: return "grid";
        case UIA_DataItemControlTypeId: return "grid item";
        case UIA_DocumentControlTypeId: return "document";
        case UIA_SplitButtonControlTypeId: return "split button";
        case UIA_WindowControlTypeId: return "window";
        case UIA_PaneControlTypeId: return "pane";
        case UIA_HeaderControlTypeId: return "header";
        case UIA_HeaderItemControlTypeId: return "header item";
        case UIA_TableControlTypeId: return "table";
        case UIA_TitleBarControlTypeId: return "title bar";
        case UIA_SeparatorControlTypeId: return "separator";
        case UIA_AppBarControlTypeId: return "app bar";
        default: return "element";
    }
}

// Containers that say nothing on their own; their children are listed in their place.
bool IsQuietContainer(CONTROLTYPEID id) {
    return id == UIA_PaneControlTypeId || id == UIA_GroupControlTypeId || id == UIA_CustomControlTypeId;
}

std::string BstrToUtf8(BSTR b) {
    if (!b) return std::string();
    std::string s = WideToUtf8(std::wstring(b, SysStringLen(b)));
    return s;
}

// Physical pixels everywhere, for this thread (Windows 10 1607 and later).
void UsePhysicalPixels() {
    using Fn = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
    static Fn fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext"));
    if (fn) fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

struct KeyName {
    const char* name;
    WORD vk;
};

const KeyName kKeys[] = {
    {"ctrl", VK_CONTROL}, {"control", VK_CONTROL}, {"alt", VK_MENU}, {"shift", VK_SHIFT}, {"win", VK_LWIN},
    {"windows", VK_LWIN}, {"cmd", VK_LWIN}, {"enter", VK_RETURN}, {"return", VK_RETURN}, {"tab", VK_TAB},
    {"escape", VK_ESCAPE}, {"esc", VK_ESCAPE}, {"space", VK_SPACE}, {"backspace", VK_BACK}, {"delete", VK_DELETE},
    {"del", VK_DELETE}, {"insert", VK_INSERT}, {"ins", VK_INSERT}, {"home", VK_HOME}, {"end", VK_END},
    {"pageup", VK_PRIOR}, {"pgup", VK_PRIOR}, {"pagedown", VK_NEXT}, {"pgdn", VK_NEXT}, {"up", VK_UP},
    {"down", VK_DOWN}, {"left", VK_LEFT}, {"right", VK_RIGHT}, {"apps", VK_APPS}, {"menu", VK_APPS},
    {"contextmenu", VK_APPS}, {"capslock", VK_CAPITAL}, {"numlock", VK_NUMLOCK}, {"scrolllock", VK_SCROLL},
    {"printscreen", VK_SNAPSHOT}, {"pause", VK_PAUSE}, {"plus", VK_OEM_PLUS}, {"minus", VK_OEM_MINUS},
    {"comma", VK_OEM_COMMA}, {"period", VK_OEM_PERIOD},
};

bool IsExtended(WORD vk) {
    switch (vk) {
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
        case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT: case VK_LWIN: case VK_RWIN: case VK_APPS:
        case VK_RCONTROL: case VK_RMENU: case VK_NUMLOCK: case VK_DIVIDE: case VK_SNAPSHOT:
            return true;
        default:
            return false;
    }
}

INPUT KeyInput(WORD vk, bool up) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    in.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (IsExtended(vk) ? KEYEVENTF_EXTENDEDKEY : 0);
    return in;
}

class WindowsDesktop : public Desktop {
public:
    WindowsDesktop() {
        UsePhysicalPixels();
        m_com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_uia));
        if (m_uia) m_uia->get_ControlViewWalker(&m_walker);
        auto windows = UserWindows();
        if (!windows.empty()) m_target = windows.front();  // the one in front of all but FastChat's
    }
    ~WindowsDesktop() override {
        m_elements.clear();
        m_walker.Reset();
        m_uia.Reset();
        if (m_com) CoUninitialize();
    }

    std::string TargetDescription() override {
        Follow();
        return Describe(m_target);
    }

    std::string ListWindows() override {
        Follow();
        std::string out;
        auto windows = UserWindows();
        for (size_t i = 0; i < windows.size(); i++) {
            out += std::to_string(i + 1) + ". " + Describe(windows[i]);
            if (windows[i] == m_target) out += "  <- target";
            if (IsIconic(windows[i])) out += " (minimized)";
            out += "\n";
        }
        return out.empty() ? std::string("No windows are open.\n") : out;
    }

    bool FocusWindow(const std::string& which, std::string& message) override {
        auto windows = UserWindows();
        HWND found = nullptr;
        try {
            size_t n = std::stoul(which);
            if (n >= 1 && n <= windows.size()) found = windows[n - 1];
        } catch (...) {
        }
        if (!found) {
            std::string want = ToLower(which);
            for (HWND h : windows) {
                if (ToLower(WindowTitle(h)).find(want) != std::string::npos || ToLower(ProcessName(h)) == want) {
                    found = h;
                    break;
                }
            }
        }
        if (!found) {
            message = "No window matches \"" + which + "\". Use list_windows.";
            return false;
        }
        m_target = found;
        BringToFront(m_target);
        message = "Now working in " + Describe(m_target);
        return true;
    }

    void SetTargetHandle(void* handle) override { m_target = static_cast<HWND>(handle); m_followForeground = false; }

    bool Capture(Screenshot& out, std::string& error) override {
        Follow();
        Map map = CurrentMap();
        int w = map.screenW, h = map.screenH;
        HDC screen = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screen);
        HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
        HGDIOBJ old = SelectObject(mem, bmp);
        BOOL ok = BitBlt(mem, 0, 0, w, h, screen, map.left, map.top, SRCCOPY | CAPTUREBLT);
        // The mouse pointer, so the model sees where it is.
        CURSORINFO ci{sizeof(ci)};
        if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING)) {
            ICONINFO ii{};
            if (GetIconInfo(ci.hCursor, &ii)) {
                DrawIconEx(mem, ci.ptScreenPos.x - map.left - static_cast<int>(ii.xHotspot),
                           ci.ptScreenPos.y - map.top - static_cast<int>(ii.yHotspot), ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
                if (ii.hbmMask) DeleteObject(ii.hbmMask);
                if (ii.hbmColor) DeleteObject(ii.hbmColor);
            }
        }
        SelectObject(mem, old);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        if (!ok) {
            DeleteObject(bmp);
            error = "The screen could not be captured";
            return false;
        }
        bool encoded = EncodeJpeg(bmp, map.imageW, map.imageH, out.base64, error);
        DeleteObject(bmp);
        if (!encoded) return false;
        out.mediaType = "image/jpeg";
        out.width = map.imageW;
        out.height = map.imageH;
        return true;
    }

    std::string Tree(int maxElements) override {
        Follow();
        m_elements.clear();
        if (!m_uia || !m_target || !IsWindow(m_target)) return "There is no window to look at.";
        ComPtr<IUIAutomationElement> root;
        if (FAILED(m_uia->ElementFromHandle(m_target, &root)) || !root) return "The window's controls could not be read.";
        ComPtr<IUIAutomationCacheRequest> cache;
        m_uia->CreateCacheRequest(&cache);
        for (PROPERTYID p : {UIA_NamePropertyId, UIA_ControlTypePropertyId, UIA_BoundingRectanglePropertyId,
                             UIA_IsOffscreenPropertyId, UIA_IsEnabledPropertyId, UIA_HasKeyboardFocusPropertyId,
                             UIA_ValueValuePropertyId, UIA_ToggleToggleStatePropertyId,
                             UIA_ExpandCollapseExpandCollapseStatePropertyId, UIA_SelectionItemIsSelectedPropertyId}) {
            cache->AddProperty(p);
        }
        ComPtr<IUIAutomationElement> cachedRoot;
        root->BuildUpdatedCache(cache.Get(), &cachedRoot);
        m_map = CurrentMap();
        std::string out = "Window: " + Describe(m_target) + "\n";
        int more = 0;
        Walk(cachedRoot ? cachedRoot.Get() : root.Get(), cache.Get(), 0, maxElements, out, more);
        if (more) out += "(" + std::to_string(more) + " more elements not shown)\n";
        return out;
    }

    std::string FocusedDescription() override {
        Follow();
        std::string where = Describe(GetForegroundWindow());
        if (!m_uia) return "In " + where;
        ComPtr<IUIAutomationElement> el;
        if (FAILED(m_uia->GetFocusedElement(&el)) || !el) return "In " + where;
        return "In " + where + ", focus on " + Brief(el.Get(), false);
    }

    std::string ElementDescription(int id) override {
        auto* el = Element(id);
        return el ? Brief(el, true) : std::string();
    }

    bool ElementPoint(int id, int& x, int& y) override {
        auto* el = Element(id);
        if (!el) return false;
        POINT p;
        BOOL got = FALSE;
        if (FAILED(el->GetClickablePoint(&p, &got)) || !got) {
            RECT r;
            if (FAILED(el->get_CurrentBoundingRectangle(&r)) || r.right <= r.left) return false;
            p = {(r.left + r.right) / 2, (r.top + r.bottom) / 2};
        }
        Map map = CurrentMap();
        x = static_cast<int>(std::lround((p.x - map.left) / map.scale));
        y = static_cast<int>(std::lround((p.y - map.top) / map.scale));
        return true;
    }

    bool Invoke(int id, std::string& message) override {
        auto* el = Element(id);
        if (!el) return NoElement(id, message);
        std::string what = Brief(el, false);
        ComPtr<IUIAutomationInvokePattern> invoke;
        if (SUCCEEDED(el->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&invoke))) && invoke &&
            SUCCEEDED(invoke->Invoke())) {
            return Done("Pressed " + what, message);
        }
        ComPtr<IUIAutomationTogglePattern> toggle;
        if (SUCCEEDED(el->GetCurrentPatternAs(UIA_TogglePatternId, IID_PPV_ARGS(&toggle))) && toggle &&
            SUCCEEDED(toggle->Toggle())) {
            return Done("Toggled " + what, message);
        }
        ComPtr<IUIAutomationSelectionItemPattern> select;
        if (SUCCEEDED(el->GetCurrentPatternAs(UIA_SelectionItemPatternId, IID_PPV_ARGS(&select))) && select &&
            SUCCEEDED(select->Select())) {
            return Done("Selected " + what, message);
        }
        ComPtr<IUIAutomationExpandCollapsePattern> expand;
        if (SUCCEEDED(el->GetCurrentPatternAs(UIA_ExpandCollapsePatternId, IID_PPV_ARGS(&expand))) && expand) {
            ExpandCollapseState state;
            expand->get_CurrentExpandCollapseState(&state);
            if (SUCCEEDED(state == ExpandCollapseState_Expanded ? expand->Collapse() : expand->Expand())) {
                return Done((state == ExpandCollapseState_Expanded ? "Collapsed " : "Expanded ") + what, message);
            }
        }
        int x, y;
        if (ElementPoint(id, x, y)) return Click(x, y, "left", 1, message);
        message = what + " cannot be pressed.";
        return false;
    }

    bool SetText(int id, const std::string& text, std::string& message) override {
        auto* el = Element(id);
        if (!el) return NoElement(id, message);
        std::string what = Brief(el, false);
        ComPtr<IUIAutomationValuePattern> value;
        if (SUCCEEDED(el->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&value))) && value) {
            BSTR b = SysAllocString(Utf8ToWide(text).c_str());
            HRESULT hr = value->SetValue(b);
            SysFreeString(b);
            if (SUCCEEDED(hr)) return Done("Set " + what + " to \"" + OneLine(text, 60) + "\"", message);
        }
        // Otherwise: focus it, select everything, and type.
        if (!EnsureTarget(message)) return false;
        el->SetFocus();
        Sleep(50);
        PressKeys("ctrl+a", message);
        return TypeText(text, message);
    }

    bool FocusElement(int id, std::string& message) override {
        auto* el = Element(id);
        if (!el) return NoElement(id, message);
        if (!EnsureTarget(message)) return false;
        if (FAILED(el->SetFocus())) {
            message = Brief(el, false) + " cannot take focus.";
            return false;
        }
        return Done("Focused " + Brief(el, false), message);
    }

    bool Click(int x, int y, const std::string& button, int count, std::string& message) override {
        if (!EnsureTarget(message)) return false;
        POINT p = ToScreen(x, y);
        DWORD down = MOUSEEVENTF_LEFTDOWN, up = MOUSEEVENTF_LEFTUP;
        if (button == "right") {
            down = MOUSEEVENTF_RIGHTDOWN;
            up = MOUSEEVENTF_RIGHTUP;
        } else if (button == "middle") {
            down = MOUSEEVENTF_MIDDLEDOWN;
            up = MOUSEEVENTF_MIDDLEUP;
        }
        std::vector<INPUT> inputs;
        inputs.push_back(MouseMoveInput(p));
        for (int i = 0; i < std::max(1, count); i++) {
            inputs.push_back(MouseButtonInput(down));
            inputs.push_back(MouseButtonInput(up));
        }
        if (!Send(inputs, message)) return false;
        return Done(std::string(count >= 2 ? "Double-clicked" : button == "right" ? "Right-clicked" : "Clicked") + " at " +
                        std::to_string(x) + ", " + std::to_string(y),
                    message);
    }

    bool Drag(int x1, int y1, int x2, int y2, std::string& message) override {
        if (!EnsureTarget(message)) return false;
        POINT a = ToScreen(x1, y1), b = ToScreen(x2, y2);
        std::vector<INPUT> start = {MouseMoveInput(a), MouseButtonInput(MOUSEEVENTF_LEFTDOWN)};
        if (!Send(start, message)) return false;
        for (int i = 1; i <= 12; i++) {
            POINT p{a.x + (b.x - a.x) * i / 12, a.y + (b.y - a.y) * i / 12};
            std::vector<INPUT> step = {MouseMoveInput(p)};
            Send(step, message);
            Sleep(15);
        }
        std::vector<INPUT> end = {MouseButtonInput(MOUSEEVENTF_LEFTUP)};
        if (!Send(end, message)) return false;
        return Done("Dragged from " + std::to_string(x1) + ", " + std::to_string(y1) + " to " + std::to_string(x2) + ", " +
                        std::to_string(y2),
                    message);
    }

    bool Scroll(int x, int y, const std::string& direction, int amount, std::string& message) override {
        if (!EnsureTarget(message)) return false;
        amount = std::clamp(amount, 1, 50);
        INPUT wheel{};
        wheel.type = INPUT_MOUSE;
        bool horizontal = direction == "left" || direction == "right";
        wheel.mi.dwFlags = horizontal ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
        int sign = (direction == "down" || direction == "left") ? -1 : 1;
        wheel.mi.mouseData = static_cast<DWORD>(sign * amount * WHEEL_DELTA);
        std::vector<INPUT> inputs = {MouseMoveInput(ToScreen(x, y)), wheel};
        if (!Send(inputs, message)) return false;
        return Done("Scrolled " + direction + " " + std::to_string(amount), message);
    }

    bool MoveMouse(int x, int y, std::string& message) override {
        if (!EnsureTarget(message)) return false;
        std::vector<INPUT> inputs = {MouseMoveInput(ToScreen(x, y))};
        if (!Send(inputs, message)) return false;
        return Done("Moved the mouse to " + std::to_string(x) + ", " + std::to_string(y), message);
    }

    bool TypeText(const std::string& text, std::string& message) override {
        if (!EnsureTarget(message)) return false;
        std::wstring w = Utf8ToWide(text);
        std::vector<INPUT> batch;
        auto flush = [&]() {
            if (batch.empty()) return true;
            bool ok = Send(batch, message);
            batch.clear();
            Sleep(10);
            return ok;
        };
        for (wchar_t c : w) {
            if (c == L'\r') continue;
            if (c == L'\n' || c == L'\t') {
                WORD vk = c == L'\n' ? VK_RETURN : VK_TAB;
                batch.push_back(KeyInput(vk, false));
                batch.push_back(KeyInput(vk, true));
            } else {
                INPUT down{};
                down.type = INPUT_KEYBOARD;
                down.ki.wScan = c;
                down.ki.dwFlags = KEYEVENTF_UNICODE;
                INPUT up = down;
                up.ki.dwFlags |= KEYEVENTF_KEYUP;
                batch.push_back(down);
                batch.push_back(up);
            }
            if (batch.size() >= 64 && !flush()) return false;
        }
        if (!flush()) return false;
        return Done("Typed \"" + OneLine(text, 60) + "\"", message);
    }

    bool PressKeys(const std::string& keys, std::string& message) override {
        if (!EnsureTarget(message)) return false;
        std::vector<std::string> combos;
        std::string current;
        for (char c : keys) {
            if (c == ' ' || c == ',') {
                if (!current.empty()) combos.push_back(current);
                current.clear();
            } else {
                current.push_back(c);
            }
        }
        if (!current.empty()) combos.push_back(current);
        if (combos.empty()) {
            message = "No keys given.";
            return false;
        }
        for (const auto& combo : combos) {
            std::vector<WORD> mods;
            WORD key = 0;
            bool shiftForChar = false;
            size_t start = 0;
            std::vector<std::string> parts;
            while (start <= combo.size()) {
                size_t plus = combo.find('+', start);
                // "ctrl++" means ctrl and the plus key
                if (plus == start && plus != std::string::npos) {
                    parts.push_back("+");
                    start = plus + 1;
                    if (start < combo.size() && combo[start] == '+') start++;
                    continue;
                }
                parts.push_back(combo.substr(start, plus == std::string::npos ? std::string::npos : plus - start));
                if (plus == std::string::npos) break;
                start = plus + 1;
            }
            for (size_t i = 0; i < parts.size(); i++) {
                std::string name = ToLower(Trim(parts[i]));
                if (name.empty()) continue;
                WORD vk = 0;
                for (const auto& k : kKeys) {
                    if (name == k.name) vk = k.vk;
                }
                if (!vk && name.size() >= 2 && name[0] == 'f' && std::all_of(name.begin() + 1, name.end(), ::isdigit)) {
                    int n = std::stoi(name.substr(1));
                    if (n >= 1 && n <= 24) vk = static_cast<WORD>(VK_F1 + n - 1);
                }
                if (!vk) {
                    std::wstring w = Utf8ToWide(parts[i]);
                    if (w.size() == 1) {
                        SHORT scan = VkKeyScanW(w[0]);
                        if (scan != -1) {
                            vk = LOBYTE(scan);
                            if (HIBYTE(scan) & 1) shiftForChar = true;
                        }
                    }
                }
                if (!vk) {
                    message = "Unknown key \"" + parts[i] + "\".";
                    return false;
                }
                bool modifier = vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT || vk == VK_LWIN;
                if (modifier && i + 1 < parts.size()) mods.push_back(vk);
                else key = vk;
            }
            if (shiftForChar && std::find(mods.begin(), mods.end(), VK_SHIFT) == mods.end()) mods.push_back(VK_SHIFT);
            std::vector<INPUT> inputs;
            for (WORD m : mods) inputs.push_back(KeyInput(m, false));
            if (key) {
                inputs.push_back(KeyInput(key, false));
                inputs.push_back(KeyInput(key, true));
            }
            for (auto it = mods.rbegin(); it != mods.rend(); ++it) inputs.push_back(KeyInput(*it, true));
            if (!Send(inputs, message)) return false;
            Sleep(60);
        }
        return Done("Pressed " + keys, message);
    }

    std::string ScreenDescription() override {
        Map map = CurrentMap();
        return std::to_string(map.imageW) + " x " + std::to_string(map.imageH);
    }

private:
    struct Map {
        int left = 0, top = 0, screenW = 0, screenH = 0, imageW = 0, imageH = 0;
        double scale = 1;
    };

    // The monitor the target is on, and how it scales to the screenshot.
    Map CurrentMap() {
        Map map;
        HMONITOR mon = MonitorFromWindow(m_target ? m_target : GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(mon, &mi);
        map.left = mi.rcMonitor.left;
        map.top = mi.rcMonitor.top;
        map.screenW = mi.rcMonitor.right - mi.rcMonitor.left;
        map.screenH = mi.rcMonitor.bottom - mi.rcMonitor.top;
        map.scale = std::max({1.0, static_cast<double>(map.screenW) / kMaxWidth, static_cast<double>(map.screenH) / kMaxHeight});
        map.imageW = static_cast<int>(map.screenW / map.scale);
        map.imageH = static_cast<int>(map.screenH / map.scale);
        return map;
    }

    POINT ToScreen(int x, int y) {
        Map map = CurrentMap();
        return {map.left + static_cast<LONG>(std::lround(x * map.scale)), map.top + static_cast<LONG>(std::lround(y * map.scale))};
    }

    // Whatever window (not FastChat's) is in front becomes the target: a dialog
    // the app opened, or a window the user switched to.
    void Follow() {
        if (!m_followForeground) return;
        HWND fg = GetForegroundWindow();
        if (fg && !IsOwnWindow(fg) && IsWindowVisible(fg) && ClassOf(fg) != "Shell_TrayWnd") m_target = fg;
        if (m_target && !IsWindow(m_target)) {
            auto windows = UserWindows();
            m_target = windows.empty() ? nullptr : windows.front();
        }
    }

    void BringToFront(HWND h) {
        if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
        HWND fg = GetForegroundWindow();
        if (fg == h) return;
        DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
        DWORD me = GetCurrentThreadId();
        bool attached = fgThread && fgThread != me && AttachThreadInput(me, fgThread, TRUE);
        SetForegroundWindow(h);
        BringWindowToTop(h);
        if (attached) AttachThreadInput(me, fgThread, FALSE);
        Sleep(150);
    }

    // Before input: the target is open and in front.
    bool EnsureTarget(std::string& message) {
        if (!m_target || !IsWindow(m_target)) {
            Follow();
            if (!m_target || !IsWindow(m_target)) {
                message = "There is no window to act on. Use list_windows and focus_window.";
                return false;
            }
        }
        BringToFront(m_target);
        // Never send input to whatever else is in front: the target's own app
        // (it or one of its menus or dialogs) must have the foreground.
        HWND fg = GetForegroundWindow();
        DWORD fgPid = 0, targetPid = 0;
        if (fg) GetWindowThreadProcessId(fg, &fgPid);
        GetWindowThreadProcessId(m_target, &targetPid);
        if (!fg || fgPid != targetPid) {
            message = "Windows did not bring " + Describe(m_target) +
                      " to the front, so nothing was sent. Ask the user to switch to it, then try again.";
            return false;
        }
        return true;
    }

    INPUT MouseMoveInput(POINT p) {
        int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dx = static_cast<LONG>(std::lround((p.x - vx) * 65535.0 / std::max(1, vw - 1)));
        in.mi.dy = static_cast<LONG>(std::lround((p.y - vy) * 65535.0 / std::max(1, vh - 1)));
        in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        return in;
    }

    INPUT MouseButtonInput(DWORD flag) {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = flag;
        return in;
    }

    bool Send(std::vector<INPUT>& inputs, std::string& message) {
        UINT sent = SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
        if (sent != inputs.size()) {
            message = "Windows blocked the input (the window may be running as administrator).";
            return false;
        }
        return true;
    }

    bool Done(const std::string& what, std::string& message) {
        Sleep(250);  // let the app react before describing where focus is
        message = what;
        return true;
    }

    bool NoElement(int id, std::string& message) {
        message = "There is no element [" + std::to_string(id) + "]. Call look to see the current elements.";
        return false;
    }

    IUIAutomationElement* Element(int id) {
        if (id < 1 || id > static_cast<int>(m_elements.size())) return nullptr;
        return m_elements[id - 1].Get();
    }

    // "Save button", "Name edit: Bob", with focus/state words.
    std::string Brief(IUIAutomationElement* el, bool cached) {
        BSTR name = nullptr;
        CONTROLTYPEID type = 0;
        if (cached) {
            el->get_CachedName(&name);
            el->get_CachedControlType(&type);
        } else {
            el->get_CurrentName(&name);
            el->get_CurrentControlType(&type);
        }
        std::string n = OneLine(BstrToUtf8(name), 80);
        SysFreeString(name);
        std::string text = n.empty() ? std::string(ControlTypeName(type)) : n + " " + ControlTypeName(type);
        if (!cached) {
            ComPtr<IUIAutomationValuePattern> value;
            if ((type == UIA_EditControlTypeId || type == UIA_ComboBoxControlTypeId) &&
                SUCCEEDED(el->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&value))) && value) {
                BSTR v = nullptr;
                value->get_CurrentValue(&v);
                std::string vs = OneLine(BstrToUtf8(v), 80);
                SysFreeString(v);
                if (!vs.empty()) text += ": " + vs;
            }
        }
        return text;
    }

    std::string CachedString(IUIAutomationElement* el, PROPERTYID id) {
        VARIANT v;
        VariantInit(&v);
        std::string s;
        if (SUCCEEDED(el->GetCachedPropertyValue(id, &v)) && v.vt == VT_BSTR) s = BstrToUtf8(v.bstrVal);
        VariantClear(&v);
        return s;
    }

    int CachedInt(IUIAutomationElement* el, PROPERTYID id, int fallback) {
        VARIANT v;
        VariantInit(&v);
        int n = fallback;
        if (SUCCEEDED(el->GetCachedPropertyValue(id, &v))) {
            if (v.vt == VT_I4) n = v.lVal;
            else if (v.vt == VT_BOOL) n = v.boolVal ? 1 : 0;
        }
        VariantClear(&v);
        return n;
    }

    void Walk(IUIAutomationElement* el, IUIAutomationCacheRequest* cache, int depth, int maxElements, std::string& out,
              int& more) {
        BOOL offscreen = FALSE;
        el->get_CachedIsOffscreen(&offscreen);
        CONTROLTYPEID type = 0;
        el->get_CachedControlType(&type);
        BSTR nameB = nullptr;
        el->get_CachedName(&nameB);
        std::string name = OneLine(BstrToUtf8(nameB), 120);
        SysFreeString(nameB);
        bool listed = false;
        if (depth > 0 && !offscreen && !(IsQuietContainer(type) && name.empty())) {
            if (static_cast<int>(m_elements.size()) >= maxElements) {
                more++;
            } else {
                m_elements.emplace_back(el);
                int id = static_cast<int>(m_elements.size());
                std::string line(static_cast<size_t>(std::min(depth, 12)) * 2, ' ');
                line += "[" + std::to_string(id) + "] " + ControlTypeName(type);
                if (!name.empty()) line += " \"" + name + "\"";
                std::string value = CachedString(el, UIA_ValueValuePropertyId);
                if (!value.empty() && value != name) line += " value \"" + OneLine(value, 120) + "\"";
                int toggle = CachedInt(el, UIA_ToggleToggleStatePropertyId, -1);
                if (toggle == ToggleState_On) line += ", checked";
                else if (toggle == ToggleState_Off && type == UIA_CheckBoxControlTypeId) line += ", not checked";
                int expand = CachedInt(el, UIA_ExpandCollapseExpandCollapseStatePropertyId, -1);
                if (expand == ExpandCollapseState_Expanded) line += ", expanded";
                else if (expand == ExpandCollapseState_Collapsed) line += ", collapsed";
                if (CachedInt(el, UIA_SelectionItemIsSelectedPropertyId, 0)) line += ", selected";
                BOOL enabled = TRUE, focused = FALSE;
                el->get_CachedIsEnabled(&enabled);
                el->get_CachedHasKeyboardFocus(&focused);
                if (!enabled) line += ", unavailable";
                if (focused) line += ", FOCUSED";
                RECT r{};
                if (SUCCEEDED(el->get_CachedBoundingRectangle(&r)) && r.right > r.left) {
                    int cx = static_cast<int>(std::lround(((r.left + r.right) / 2.0 - m_map.left) / m_map.scale));
                    int cy = static_cast<int>(std::lround(((r.top + r.bottom) / 2.0 - m_map.top) / m_map.scale));
                    line += " at " + std::to_string(cx) + "," + std::to_string(cy);
                }
                out += line + "\n";
                listed = true;
            }
        }
        if (offscreen && depth > 0) return;
        if (depth > 40) return;
        ComPtr<IUIAutomationElement> child;
        m_walker->GetFirstChildElementBuildCache(el, cache, &child);
        while (child) {
            Walk(child.Get(), cache, listed || depth == 0 ? depth + 1 : depth, maxElements, out, more);
            ComPtr<IUIAutomationElement> next;
            m_walker->GetNextSiblingElementBuildCache(child.Get(), cache, &next);
            child = next;
        }
    }

    bool EncodeJpeg(HBITMAP bmp, int w, int h, std::string& base64, std::string& error) {
        ComPtr<IWICImagingFactory> factory;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
            error = "Windows Imaging Component is not available";
            return false;
        }
        ComPtr<IWICBitmap> source;
        ComPtr<IWICBitmapScaler> scaler;
        ComPtr<IWICFormatConverter> converter;
        ComPtr<IStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> props;
        if (FAILED(factory->CreateBitmapFromHBITMAP(bmp, nullptr, WICBitmapIgnoreAlpha, &source)) ||
            FAILED(factory->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(source.Get(), w, h, WICBitmapInterpolationModeFant)) ||
            FAILED(factory->CreateFormatConverter(&converter)) ||
            FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeCustom)) ||
            FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) ||
            FAILED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder)) ||
            FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
            FAILED(encoder->CreateNewFrame(&frame, &props))) {
            error = "The screenshot could not be encoded";
            return false;
        }
        PROPBAG2 option{};
        option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT quality;
        VariantInit(&quality);
        quality.vt = VT_R4;
        quality.fltVal = 0.75f;
        props->Write(1, &option, &quality);
        WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
        if (FAILED(frame->Initialize(props.Get())) || FAILED(frame->SetSize(w, h)) || FAILED(frame->SetPixelFormat(&format)) ||
            FAILED(frame->WriteSource(converter.Get(), nullptr)) || FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
            error = "The screenshot could not be encoded";
            return false;
        }
        STATSTG stat{};
        stream->Stat(&stat, STATFLAG_NONAME);
        HGLOBAL global = nullptr;
        GetHGlobalFromStream(stream.Get(), &global);
        size_t size = static_cast<size_t>(stat.cbSize.QuadPart);
        const char* bytes = static_cast<const char*>(GlobalLock(global));
        if (!bytes) {
            error = "The screenshot could not be read back";
            return false;
        }
        base64 = Base64Encode(std::string(bytes, size));
        GlobalUnlock(global);
        return true;
    }

    bool m_com = false;
    bool m_followForeground = true;
    HWND m_target = nullptr;
    ComPtr<IUIAutomation> m_uia;
    ComPtr<IUIAutomationTreeWalker> m_walker;
    std::vector<ComPtr<IUIAutomationElement>> m_elements;
    Map m_map;
};

}  // namespace

std::unique_ptr<Desktop> Desktop::Create() {
    return std::make_unique<WindowsDesktop>();
}
