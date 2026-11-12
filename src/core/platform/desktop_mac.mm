// Computer use on the Mac: the accessibility API (AXUIElement) for windows, the
// tree of controls and acting on them; ScreenCaptureKit for screenshots (JPEG);
// CGEvent for the keyboard and mouse. Runs on the chat worker thread. Needs the
// Accessibility permission, and Screen Recording for screenshots; macOS asks for
// both the first time. Compiled with ARC.
//
// Coordinates: the accessibility API and CGEvent both use global points with the
// origin at the top left of the main display, so they need no flipping.

#include "core/desktop.h"
#include "core/util.h"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <Carbon/Carbon.h>
#import <ImageIO/ImageIO.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <dlfcn.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

const int kMaxWidth = 1280;
const int kMaxHeight = 800;

void Pause(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// A retained Core Foundation reference.
template <typename T>
class CF {
public:
    CF() = default;
    // Takes ownership of a +1 reference (Copy/Create).
    explicit CF(T ref) : m_ref(ref) {}
    CF(const CF& o) : m_ref(o.m_ref) { if (m_ref) CFRetain(m_ref); }
    CF(CF&& o) noexcept : m_ref(o.m_ref) { o.m_ref = nullptr; }
    CF& operator=(CF o) { std::swap(m_ref, o.m_ref); return *this; }
    ~CF() { if (m_ref) CFRelease(m_ref); }
    static CF Retain(T ref) { if (ref) CFRetain(ref); return CF(ref); }
    T get() const { return m_ref; }
    explicit operator bool() const { return m_ref != nullptr; }

private:
    T m_ref = nullptr;
};

using Element = CF<AXUIElementRef>;

std::string ToStd(CFStringRef s) {
    if (!s) return std::string();
    return std::string([(__bridge NSString*)s UTF8String] ?: "");
}

CF<CFTypeRef> Attr(AXUIElementRef el, CFStringRef name) {
    CFTypeRef value = nullptr;
    if (AXUIElementCopyAttributeValue(el, name, &value) != kAXErrorSuccess) return CF<CFTypeRef>();
    return CF<CFTypeRef>(value);
}

std::string StrAttr(AXUIElementRef el, CFStringRef name) {
    CF<CFTypeRef> v = Attr(el, name);
    if (v && CFGetTypeID(v.get()) == CFStringGetTypeID()) return ToStd(static_cast<CFStringRef>(v.get()));
    return std::string();
}

bool BoolAttr(AXUIElementRef el, CFStringRef name, bool fallback = false) {
    CF<CFTypeRef> v = Attr(el, name);
    if (v && CFGetTypeID(v.get()) == CFBooleanGetTypeID()) return CFBooleanGetValue(static_cast<CFBooleanRef>(v.get()));
    return fallback;
}

Element ElementAttr(AXUIElementRef el, CFStringRef name) {
    CF<CFTypeRef> v = Attr(el, name);
    if (v && CFGetTypeID(v.get()) == AXUIElementGetTypeID()) return Element::Retain(static_cast<AXUIElementRef>(v.get()));
    return Element();
}

std::vector<Element> ElementsAttr(AXUIElementRef el, CFStringRef name) {
    std::vector<Element> list;
    CF<CFTypeRef> v = Attr(el, name);
    if (!v || CFGetTypeID(v.get()) != CFArrayGetTypeID()) return list;
    CFArrayRef array = static_cast<CFArrayRef>(v.get());
    for (CFIndex i = 0; i < CFArrayGetCount(array); i++) {
        CFTypeRef item = CFArrayGetValueAtIndex(array, i);
        if (item && CFGetTypeID(item) == AXUIElementGetTypeID()) list.push_back(Element::Retain(static_cast<AXUIElementRef>(item)));
    }
    return list;
}

bool Frame(AXUIElementRef el, CGRect& rect) {
    CF<CFTypeRef> pos = Attr(el, kAXPositionAttribute), size = Attr(el, kAXSizeAttribute);
    if (!pos || !size || CFGetTypeID(pos.get()) != AXValueGetTypeID() || CFGetTypeID(size.get()) != AXValueGetTypeID()) return false;
    CGPoint p;
    CGSize s;
    if (!AXValueGetValue(static_cast<AXValueRef>(pos.get()), static_cast<AXValueType>(kAXValueCGPointType), &p) ||
        !AXValueGetValue(static_cast<AXValueRef>(size.get()), static_cast<AXValueType>(kAXValueCGSizeType), &s)) {
        return false;
    }
    rect = CGRectMake(p.x, p.y, s.width, s.height);
    return true;
}

bool IsAlive(AXUIElementRef el) {
    CFTypeRef role = nullptr;
    AXError err = AXUIElementCopyAttributeValue(el, kAXRoleAttribute, &role);
    if (role) CFRelease(role);
    return err != kAXErrorInvalidUIElement && err != kAXErrorCannotComplete;
}

pid_t PidOf(AXUIElementRef el) {
    pid_t pid = 0;
    AXUIElementGetPid(el, &pid);
    return pid;
}

std::string AppName(pid_t pid) {
    NSRunningApplication* app = [NSRunningApplication runningApplicationWithProcessIdentifier:pid];
    return app.localizedName ? std::string(app.localizedName.UTF8String) : std::string();
}

// The application that has keyboard focus, from the accessibility system itself
// (always current, unlike NSWorkspace off the main thread).
pid_t FocusedPid() {
    Element system(AXUIElementCreateSystemWide());
    Element app = ElementAttr(system.get(), kAXFocusedApplicationAttribute);
    return app ? PidOf(app.get()) : 0;
}

// What a person calls the element: its title, description, label or placeholder.
std::string NameOf(AXUIElementRef el, const std::string& role) {
    std::string name = StrAttr(el, kAXTitleAttribute);
    if (name.empty()) name = StrAttr(el, kAXDescriptionAttribute);
    if (name.empty()) {
        Element label = ElementAttr(el, kAXTitleUIElementAttribute);
        if (label) name = StrAttr(label.get(), kAXValueAttribute);
    }
    if (name.empty() && role == "AXStaticText") name = StrAttr(el, kAXValueAttribute);
    if (name.empty()) name = StrAttr(el, kAXPlaceholderValueAttribute);
    if (name.empty()) name = StrAttr(el, kAXHelpAttribute);
    return name;
}

// "button", "text field", "check box": the system's own words, as VoiceOver says them.
std::string RoleWords(AXUIElementRef el, const std::string& role) {
    std::string words = StrAttr(el, kAXRoleDescriptionAttribute);
    if (!words.empty()) return words;
    if (role.rfind("AX", 0) == 0) return ToLower(role.substr(2));
    return role.empty() ? std::string("element") : role;
}

// Containers that say nothing on their own; their children are listed in their place.
bool IsQuietContainer(const std::string& role) {
    return role == "AXGroup" || role == "AXSplitGroup" || role == "AXScrollArea" || role == "AXLayoutArea" ||
           role == "AXLayoutItem" || role == "AXUnknown" || role == "AXGenericElement" || role == "AXSplitter";
}

// A value worth showing: text, or a number for sliders and the like.
std::string ValueText(AXUIElementRef el) {
    CF<CFTypeRef> v = Attr(el, kAXValueAttribute);
    if (!v) return std::string();
    CFTypeID type = CFGetTypeID(v.get());
    if (type == CFStringGetTypeID()) return ToStd(static_cast<CFStringRef>(v.get()));
    if (type == CFNumberGetTypeID()) {
        double d = 0;
        CFNumberGetValue(static_cast<CFNumberRef>(v.get()), kCFNumberDoubleType, &d);
        char buf[32];
        snprintf(buf, sizeof(buf), "%g", d);
        return buf;
    }
    return std::string();
}

int NumberValue(AXUIElementRef el, int fallback) {
    CF<CFTypeRef> v = Attr(el, kAXValueAttribute);
    if (!v || CFGetTypeID(v.get()) != CFNumberGetTypeID()) return fallback;
    int n = fallback;
    CFNumberGetValue(static_cast<CFNumberRef>(v.get()), kCFNumberIntType, &n);
    return n;
}

bool HasAction(AXUIElementRef el, CFStringRef action) {
    CFArrayRef names = nullptr;
    if (AXUIElementCopyActionNames(el, &names) != kAXErrorSuccess || !names) return false;
    bool found = CFArrayContainsValue(names, CFRangeMake(0, CFArrayGetCount(names)), action);
    CFRelease(names);
    return found;
}

bool Settable(AXUIElementRef el, CFStringRef attribute) {
    Boolean settable = false;
    return AXUIElementIsAttributeSettable(el, attribute, &settable) == kAXErrorSuccess && settable;
}

struct WindowInfo {
    pid_t pid = 0;
    Element window;
    std::string title;
    bool minimized = false;
};

std::string Describe(const WindowInfo& w) {
    if (!w.window) return std::string();
    std::string app = AppName(w.pid);
    std::string title = w.title.empty() ? std::string("Untitled window") : w.title;
    return app.empty() ? title : title + " (" + app + ")";
}

WindowInfo InfoFor(pid_t pid, Element window) {
    WindowInfo info;
    info.pid = pid;
    info.window = std::move(window);
    info.title = StrAttr(info.window.get(), kAXTitleAttribute);
    info.minimized = BoolAttr(info.window.get(), kAXMinimizedAttribute);
    return info;
}

// The windows a person would switch to, front to back, never FastChat's own:
// apps in the order their windows are stacked on screen, then the rest of the
// regular apps (minimized or on another Space), each app's windows in its order.
std::vector<WindowInfo> UserWindows() {
    std::vector<pid_t> pids;
    pid_t self = getpid();
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
                                                 kCGNullWindowID);
    if (list) {
        for (NSDictionary* w in (__bridge NSArray*)list) {
            if ([w[(id)kCGWindowLayer] intValue] != 0) continue;
            pid_t pid = [w[(id)kCGWindowOwnerPID] intValue];
            if (pid != self && std::find(pids.begin(), pids.end(), pid) == pids.end()) pids.push_back(pid);
        }
        CFRelease(list);
    }
    for (NSRunningApplication* app in [NSWorkspace sharedWorkspace].runningApplications) {
        pid_t pid = app.processIdentifier;
        if (app.activationPolicy == NSApplicationActivationPolicyRegular && pid != self &&
            std::find(pids.begin(), pids.end(), pid) == pids.end()) {
            pids.push_back(pid);
        }
    }
    std::vector<WindowInfo> windows;
    for (pid_t pid : pids) {
        Element app(AXUIElementCreateApplication(pid));
        for (Element& w : ElementsAttr(app.get(), kAXWindowsAttribute)) {
            std::string role = StrAttr(w.get(), kAXRoleAttribute);
            std::string subrole = StrAttr(w.get(), kAXSubroleAttribute);
            if (role != "AXWindow") continue;
            if (subrole != "AXStandardWindow" && subrole != "AXDialog" && subrole != "AXSystemDialog" &&
                subrole != "AXFloatingWindow") {
                continue;
            }
            windows.push_back(InfoFor(pid, w));
        }
    }
    return windows;
}

// --- Keys ---

struct KeyName {
    const char* name;
    CGKeyCode code;
};

const KeyName kKeys[] = {
    {"enter", kVK_Return}, {"return", kVK_Return}, {"tab", kVK_Tab}, {"escape", kVK_Escape}, {"esc", kVK_Escape},
    {"space", kVK_Space}, {"backspace", kVK_Delete}, {"delete", kVK_ForwardDelete}, {"del", kVK_ForwardDelete},
    {"forwarddelete", kVK_ForwardDelete}, {"home", kVK_Home}, {"end", kVK_End}, {"pageup", kVK_PageUp},
    {"pgup", kVK_PageUp}, {"pagedown", kVK_PageDown}, {"pgdn", kVK_PageDown}, {"up", kVK_UpArrow},
    {"down", kVK_DownArrow}, {"left", kVK_LeftArrow}, {"right", kVK_RightArrow}, {"capslock", kVK_CapsLock},
    {"help", kVK_Help}, {"plus", kVK_ANSI_Equal}, {"minus", kVK_ANSI_Minus}, {"comma", kVK_ANSI_Comma},
    {"period", kVK_ANSI_Period},
};

const CGKeyCode kFunctionKeys[] = {kVK_F1,  kVK_F2,  kVK_F3,  kVK_F4,  kVK_F5,  kVK_F6,  kVK_F7,
                                   kVK_F8,  kVK_F9,  kVK_F10, kVK_F11, kVK_F12, kVK_F13, kVK_F14,
                                   kVK_F15, kVK_F16, kVK_F17, kVK_F18, kVK_F19, kVK_F20};

// Characters on a US keyboard: the key, and whether Shift makes it.
bool CharKey(char32_t c, CGKeyCode& code, bool& shift) {
    static const struct { char plain, shifted; CGKeyCode code; } kChars[] = {
        {'a', 'A', kVK_ANSI_A}, {'b', 'B', kVK_ANSI_B}, {'c', 'C', kVK_ANSI_C}, {'d', 'D', kVK_ANSI_D},
        {'e', 'E', kVK_ANSI_E}, {'f', 'F', kVK_ANSI_F}, {'g', 'G', kVK_ANSI_G}, {'h', 'H', kVK_ANSI_H},
        {'i', 'I', kVK_ANSI_I}, {'j', 'J', kVK_ANSI_J}, {'k', 'K', kVK_ANSI_K}, {'l', 'L', kVK_ANSI_L},
        {'m', 'M', kVK_ANSI_M}, {'n', 'N', kVK_ANSI_N}, {'o', 'O', kVK_ANSI_O}, {'p', 'P', kVK_ANSI_P},
        {'q', 'Q', kVK_ANSI_Q}, {'r', 'R', kVK_ANSI_R}, {'s', 'S', kVK_ANSI_S}, {'t', 'T', kVK_ANSI_T},
        {'u', 'U', kVK_ANSI_U}, {'v', 'V', kVK_ANSI_V}, {'w', 'W', kVK_ANSI_W}, {'x', 'X', kVK_ANSI_X},
        {'y', 'Y', kVK_ANSI_Y}, {'z', 'Z', kVK_ANSI_Z}, {'1', '!', kVK_ANSI_1}, {'2', '@', kVK_ANSI_2},
        {'3', '#', kVK_ANSI_3}, {'4', '$', kVK_ANSI_4}, {'5', '%', kVK_ANSI_5}, {'6', '^', kVK_ANSI_6},
        {'7', '&', kVK_ANSI_7}, {'8', '*', kVK_ANSI_8}, {'9', '(', kVK_ANSI_9}, {'0', ')', kVK_ANSI_0},
        {'-', '_', kVK_ANSI_Minus}, {'=', '+', kVK_ANSI_Equal}, {'[', '{', kVK_ANSI_LeftBracket},
        {']', '}', kVK_ANSI_RightBracket}, {'\\', '|', kVK_ANSI_Backslash}, {';', ':', kVK_ANSI_Semicolon},
        {'\'', '"', kVK_ANSI_Quote}, {',', '<', kVK_ANSI_Comma}, {'.', '>', kVK_ANSI_Period},
        {'/', '?', kVK_ANSI_Slash}, {'`', '~', kVK_ANSI_Grave},
    };
    for (const auto& k : kChars) {
        if (c == static_cast<char32_t>(k.plain)) {
            code = k.code;
            shift = false;
            return true;
        }
        if (c == static_cast<char32_t>(k.shifted)) {
            code = k.code;
            shift = true;
            return true;
        }
    }
    return false;
}

class MacDesktop : public Desktop {
public:
    MacDesktop() {
        // Never wait long on an app that has stopped answering.
        Element system(AXUIElementCreateSystemWide());
        AXUIElementSetMessagingTimeout(system.get(), 2.0f);
        // The first time, macOS asks the user to allow FastChat in Privacy & Security > Accessibility.
        NSDictionary* options = @{(__bridge NSString*)kAXTrustedCheckOptionPrompt: @YES};
        m_trusted = AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)options);
        if (m_trusted) {
            auto windows = UserWindows();
            if (!windows.empty()) m_target = windows.front();  // the one in front of all but FastChat's
        }
    }

    std::string Unavailable() override {
        if (m_trusted || AXIsProcessTrusted()) {
            m_trusted = true;
            return std::string();
        }
        return "FastChat needs the Accessibility permission to see and operate other apps. Turn FastChat on in "
               "System Settings > Privacy & Security > Accessibility, then send your message again.";
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
            if (m_target.window && CFEqual(windows[i].window.get(), m_target.window.get())) out += "  <- target";
            if (windows[i].minimized) out += " (minimized)";
            out += "\n";
        }
        return out.empty() ? std::string("No windows are open.\n") : out;
    }

    bool FocusWindow(const std::string& which, std::string& message) override {
        auto windows = UserWindows();
        const WindowInfo* found = nullptr;
        try {
            size_t n = std::stoul(which);
            if (n >= 1 && n <= windows.size()) found = &windows[n - 1];
        } catch (...) {
        }
        if (!found) {
            std::string want = ToLower(which);
            for (const auto& w : windows) {
                if (ToLower(w.title).find(want) != std::string::npos || ToLower(AppName(w.pid)) == want) {
                    found = &w;
                    break;
                }
            }
        }
        if (!found) {
            message = "No window matches \"" + which + "\". Use list_windows.";
            return false;
        }
        m_target = *found;
        BringToFront();
        message = "Now working in " + Describe(m_target);
        return true;
    }

    // For tests: an AXUIElementRef of a window.
    void SetTargetHandle(void* handle) override {
        AXUIElementRef el = static_cast<AXUIElementRef>(handle);
        m_target = InfoFor(PidOf(el), Element::Retain(el));
        m_followForeground = false;
    }

    bool Capture(Screenshot& out, std::string& error) override {
        Follow();
        if (!CGPreflightScreenCaptureAccess()) {
            CGRequestScreenCaptureAccess();  // the first time, macOS asks
            error = "FastChat needs the Screen Recording permission for screenshots. Turn FastChat on in System "
                    "Settings > Privacy & Security > Screen & System Audio Recording (it may need restarting). The "
                    "accessibility tree from look still works without it.";
            return false;
        }
        Map map = CurrentMap();
        CF<CGImageRef> image = CaptureDisplay(map, error);
        if (!image) {
            if (error.empty()) error = "The screen could not be captured";
            return false;
        }
        if (!EncodeJpeg(image.get(), map.imageW, map.imageH, out.base64)) {
            error = "The screenshot could not be encoded";
            return false;
        }
        out.mediaType = "image/jpeg";
        out.width = map.imageW;
        out.height = map.imageH;
        return true;
    }

    std::string Tree(int maxElements) override {
        Follow();
        m_elements.clear();
        if (std::string why = Unavailable(); !why.empty()) return why;
        if (!m_target.window || !IsAlive(m_target.window.get())) return "There is no window to look at.";
        m_map = CurrentMap();
        Frame(m_target.window.get(), m_windowFrame);
        Element app(AXUIElementCreateApplication(m_target.pid));
        m_focused = ElementAttr(app.get(), kAXFocusedUIElementAttribute);
        std::string out = "Window: " + Describe(m_target) + "\n";
        int more = 0;
        Walk(m_target.window.get(), 0, maxElements, out, more);
        // Menus live outside the window: an open one (a context menu, or one from
        // the menu bar), then the menu bar itself.
        for (Element& child : ElementsAttr(app.get(), kAXChildrenAttribute)) {
            if (StrAttr(child.get(), kAXRoleAttribute) == "AXMenu") {
                out += "Open menu:\n";
                Walk(child.get(), 0, maxElements, out, more, true);
            }
        }
        if (Element bar = ElementAttr(app.get(), kAXMenuBarAttribute)) {
            out += "Menu bar:\n";
            for (Element& item : ElementsAttr(bar.get(), kAXChildrenAttribute)) {
                bool open = BoolAttr(item.get(), kAXSelectedAttribute);
                std::string name;
                if (open) {
                    Walk(item.get(), 1, maxElements, out, more, true);
                } else {
                    List(item.get(), 1, maxElements, out, more, true, std::string(), name);
                }
            }
        }
        if (more) out += "(" + std::to_string(more) + " more elements not shown)\n";
        return out;
    }

    std::string FocusedDescription() override {
        Follow();
        Element system(AXUIElementCreateSystemWide());
        Element app = ElementAttr(system.get(), kAXFocusedApplicationAttribute);
        if (!app) return "Nothing has focus";
        pid_t pid = PidOf(app.get());
        Element window = ElementAttr(app.get(), kAXFocusedWindowAttribute);
        std::string where = window ? Describe(InfoFor(pid, window)) : AppName(pid);
        Element focused = ElementAttr(app.get(), kAXFocusedUIElementAttribute);
        if (!focused) return "In " + where;
        return "In " + where + ", focus on " + Brief(focused.get(), true);
    }

    std::string ElementDescription(int id) override {
        AXUIElementRef el = ElementAt(id);
        return el ? Brief(el, false) : std::string();
    }

    bool ElementPoint(int id, int& x, int& y) override {
        AXUIElementRef el = ElementAt(id);
        CGRect r;
        if (!el || !Frame(el, r) || r.size.width <= 0) return false;
        Map map = CurrentMap();
        x = static_cast<int>(std::lround((CGRectGetMidX(r) - map.left) / map.scale));
        y = static_cast<int>(std::lround((CGRectGetMidY(r) - map.top) / map.scale));
        return true;
    }

    bool Invoke(int id, std::string& message) override {
        AXUIElementRef el = ElementAt(id);
        if (!el) return NoElement(id, message);
        std::string what = Brief(el, false);
        std::string role = StrAttr(el, kAXRoleAttribute);
        if (HasAction(el, kAXPressAction) && AXUIElementPerformAction(el, kAXPressAction) == kAXErrorSuccess) {
            return Done((role == "AXCheckBox" || role == "AXSwitch" ? "Toggled " : "Pressed ") + what, message);
        }
        if (Settable(el, kAXSelectedAttribute) &&
            AXUIElementSetAttributeValue(el, kAXSelectedAttribute, kCFBooleanTrue) == kAXErrorSuccess) {
            return Done("Selected " + what, message);
        }
        if (Settable(el, kAXDisclosingAttribute)) {
            bool open = BoolAttr(el, kAXDisclosingAttribute);
            if (AXUIElementSetAttributeValue(el, kAXDisclosingAttribute, open ? kCFBooleanFalse : kCFBooleanTrue) ==
                kAXErrorSuccess) {
                return Done((open ? "Collapsed " : "Expanded ") + what, message);
            }
        }
        if (Settable(el, kAXExpandedAttribute)) {
            bool open = BoolAttr(el, kAXExpandedAttribute);
            if (AXUIElementSetAttributeValue(el, kAXExpandedAttribute, open ? kCFBooleanFalse : kCFBooleanTrue) ==
                kAXErrorSuccess) {
                return Done((open ? "Collapsed " : "Expanded ") + what, message);
            }
        }
        if (HasAction(el, kAXPickAction) && AXUIElementPerformAction(el, kAXPickAction) == kAXErrorSuccess) {
            return Done("Picked " + what, message);
        }
        if (HasAction(el, kAXShowMenuAction) && AXUIElementPerformAction(el, kAXShowMenuAction) == kAXErrorSuccess) {
            return Done("Opened the menu of " + what, message);
        }
        int x, y;
        if (ElementPoint(id, x, y)) return Click(x, y, "left", 1, message);
        message = what + " cannot be pressed.";
        return false;
    }

    bool SetText(int id, const std::string& text, std::string& message) override {
        AXUIElementRef el = ElementAt(id);
        if (!el) return NoElement(id, message);
        std::string what = Brief(el, false);
        if (Settable(el, kAXValueAttribute)) {
            NSString* value = [NSString stringWithUTF8String:text.c_str()] ?: @"";
            if (AXUIElementSetAttributeValue(el, kAXValueAttribute, (__bridge CFStringRef)value) == kAXErrorSuccess &&
                ValueText(el) == text) {
                return Done("Set " + what + " to \"" + OneLine(text, 60) + "\"", message);
            }
        }
        // Otherwise: focus it, select everything, and type.
        if (!EnsureTarget(message)) return false;
        AXUIElementSetAttributeValue(el, kAXFocusedAttribute, kCFBooleanTrue);
        Pause(50);
        PressKeys("cmd+a", message);
        return TypeText(text, message);
    }

    bool FocusElement(int id, std::string& message) override {
        AXUIElementRef el = ElementAt(id);
        if (!el) return NoElement(id, message);
        if (!EnsureTarget(message)) return false;
        if (AXUIElementSetAttributeValue(el, kAXFocusedAttribute, kCFBooleanTrue) != kAXErrorSuccess) {
            message = Brief(el, false) + " cannot take focus.";
            return false;
        }
        return Done("Focused " + Brief(el, false), message);
    }

    bool Click(int x, int y, const std::string& button, int count, std::string& message) override {
        if (!EnsureTarget(message)) return false;
        CGPoint p = ToScreen(x, y);
        CGEventType down = kCGEventLeftMouseDown, up = kCGEventLeftMouseUp;
        CGMouseButton which = kCGMouseButtonLeft;
        if (button == "right") {
            down = kCGEventRightMouseDown;
            up = kCGEventRightMouseUp;
            which = kCGMouseButtonRight;
        } else if (button == "middle") {
            down = kCGEventOtherMouseDown;
            up = kCGEventOtherMouseUp;
            which = kCGMouseButtonCenter;
        }
        PostMouse(kCGEventMouseMoved, p, kCGMouseButtonLeft);
        Pause(30);
        for (int i = 1; i <= std::max(1, count); i++) {
            PostMouse(down, p, which, i);
            PostMouse(up, p, which, i);
        }
        return Done(std::string(count >= 2 ? "Double-clicked" : button == "right" ? "Right-clicked" : "Clicked") + " at " +
                        std::to_string(x) + ", " + std::to_string(y),
                    message);
    }

    bool Drag(int x1, int y1, int x2, int y2, std::string& message) override {
        if (!EnsureTarget(message)) return false;
        CGPoint a = ToScreen(x1, y1), b = ToScreen(x2, y2);
        PostMouse(kCGEventMouseMoved, a, kCGMouseButtonLeft);
        PostMouse(kCGEventLeftMouseDown, a, kCGMouseButtonLeft);
        for (int i = 1; i <= 12; i++) {
            CGPoint p = CGPointMake(a.x + (b.x - a.x) * i / 12, a.y + (b.y - a.y) * i / 12);
            PostMouse(kCGEventLeftMouseDragged, p, kCGMouseButtonLeft);
            Pause(15);
        }
        PostMouse(kCGEventLeftMouseUp, b, kCGMouseButtonLeft);
        return Done("Dragged from " + std::to_string(x1) + ", " + std::to_string(y1) + " to " + std::to_string(x2) + ", " +
                        std::to_string(y2),
                    message);
    }

    bool Scroll(int x, int y, const std::string& direction, int amount, std::string& message) override {
        if (!EnsureTarget(message)) return false;
        amount = std::clamp(amount, 1, 50);
        PostMouse(kCGEventMouseMoved, ToScreen(x, y), kCGMouseButtonLeft);
        Pause(30);
        // A notch of a wheel is three lines, as on Windows.
        int lines = amount * 3;
        int vertical = direction == "up" ? lines : direction == "down" ? -lines : 0;
        int horizontal = direction == "left" ? lines : direction == "right" ? -lines : 0;
        CF<CGEventRef> e(CGEventCreateScrollWheelEvent(Source(), kCGScrollEventUnitLine, 2, vertical, horizontal));
        if (e) CGEventPost(kCGHIDEventTap, e.get());
        return Done("Scrolled " + direction + " " + std::to_string(amount), message);
    }

    bool MoveMouse(int x, int y, std::string& message) override {
        if (!EnsureTarget(message)) return false;
        PostMouse(kCGEventMouseMoved, ToScreen(x, y), kCGMouseButtonLeft);
        return Done("Moved the mouse to " + std::to_string(x) + ", " + std::to_string(y), message);
    }

    bool TypeText(const std::string& text, std::string& message) override {
        if (!EnsureTarget(message)) return false;
        NSString* s = [NSString stringWithUTF8String:text.c_str()] ?: @"";
        std::vector<UniChar> chunk;
        auto flush = [&]() {
            if (chunk.empty()) return;
            PostKey(0, true, 0, &chunk);
            PostKey(0, false, 0, &chunk);
            chunk.clear();
            Pause(10);
        };
        for (NSUInteger i = 0; i < s.length; i++) {
            UniChar c = [s characterAtIndex:i];
            if (c == '\r') continue;
            if (c == '\n' || c == '\t') {
                flush();
                CGKeyCode code = c == '\n' ? kVK_Return : kVK_Tab;
                PostKey(code, true, 0);
                PostKey(code, false, 0);
                Pause(10);
                continue;
            }
            chunk.push_back(c);
            // Keep surrogate pairs together; events carry up to 20 characters.
            if (chunk.size() >= 16 && !CFStringIsSurrogateHighCharacter(c)) flush();
        }
        flush();
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
            std::vector<std::string> parts;
            size_t start = 0;
            while (start <= combo.size()) {
                size_t plus = combo.find('+', start);
                // "cmd++" means cmd and the plus key
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
            CGEventFlags flags = 0;
            std::vector<CGKeyCode> modifierKeys;
            bool haveKey = false;
            CGKeyCode key = 0;
            std::vector<UniChar> character;
            for (size_t i = 0; i < parts.size(); i++) {
                std::string name = ToLower(Trim(parts[i]));
                if (name.empty()) continue;
                bool last = i + 1 == parts.size();
                CGEventFlags flag = 0;
                CGKeyCode modKey = 0;
                if (name == "cmd" || name == "command" || name == "win" || name == "windows" || name == "super" ||
                    name == "meta") {
                    flag = kCGEventFlagMaskCommand;
                    modKey = kVK_Command;
                } else if (name == "ctrl" || name == "control") {
                    flag = kCGEventFlagMaskControl;
                    modKey = kVK_Control;
                } else if (name == "alt" || name == "option" || name == "opt") {
                    flag = kCGEventFlagMaskAlternate;
                    modKey = kVK_Option;
                } else if (name == "shift") {
                    flag = kCGEventFlagMaskShift;
                    modKey = kVK_Shift;
                } else if (name == "fn") {
                    flag = kCGEventFlagMaskSecondaryFn;
                    modKey = kVK_Function;
                }
                if (flag && !last) {
                    flags |= flag;
                    modifierKeys.push_back(modKey);
                    continue;
                }
                if (flag) {  // a modifier pressed on its own
                    key = modKey;
                    haveKey = true;
                    continue;
                }
                CGKeyCode code = 0;
                bool found = false;
                for (const auto& k : kKeys) {
                    if (name == k.name) {
                        code = k.code;
                        found = true;
                    }
                }
                if (!found && name.size() >= 2 && name[0] == 'f' && std::all_of(name.begin() + 1, name.end(), ::isdigit)) {
                    int n = std::stoi(name.substr(1));
                    if (n >= 1 && n <= 20) {
                        code = kFunctionKeys[n - 1];
                        found = true;
                    }
                }
                if (!found) {
                    NSString* s = [NSString stringWithUTF8String:parts[i].c_str()];
                    bool shift = false;
                    if (s.length == 1 && CharKey([s characterAtIndex:0], code, shift)) {
                        found = true;
                        if (shift) flags |= kCGEventFlagMaskShift;
                        if (!(flags & (kCGEventFlagMaskCommand | kCGEventFlagMaskControl))) {
                            character.push_back([s characterAtIndex:0]);
                        }
                    }
                }
                if (!found) {
                    message = "Unknown key \"" + parts[i] + "\".";
                    return false;
                }
                key = code;
                haveKey = true;
            }
            CGEventFlags held = 0;
            for (size_t i = 0; i < modifierKeys.size(); i++) {
                held |= ModifierFlag(modifierKeys[i]);
                PostKey(modifierKeys[i], true, held);
            }
            if (haveKey) {
                PostKey(key, true, flags, character.empty() ? nullptr : &character);
                PostKey(key, false, flags, character.empty() ? nullptr : &character);
            }
            for (auto it = modifierKeys.rbegin(); it != modifierKeys.rend(); ++it) {
                held &= ~ModifierFlag(*it);
                PostKey(*it, false, held);
            }
            Pause(60);
        }
        return Done("Pressed " + keys, message);
    }

    std::string ScreenDescription() override {
        Map map = CurrentMap();
        return std::to_string(map.imageW) + " x " + std::to_string(map.imageH);
    }

private:
    struct Map {
        CGDirectDisplayID display = 0;
        double left = 0, top = 0, screenW = 0, screenH = 0;
        int imageW = 0, imageH = 0;
        double scale = 1;  // points per screenshot pixel
    };

    // The display the target is on, and how it scales to the screenshot.
    Map CurrentMap() {
        Map map;
        map.display = CGMainDisplayID();
        CGRect r;
        if (m_target.window && Frame(m_target.window.get(), r)) {
            CGDirectDisplayID ids[4];
            uint32_t count = 0;
            if (CGGetDisplaysWithPoint(CGPointMake(CGRectGetMidX(r), CGRectGetMidY(r)), 4, ids, &count) == kCGErrorSuccess &&
                count > 0) {
                map.display = ids[0];
            }
        }
        CGRect b = CGDisplayBounds(map.display);
        map.left = b.origin.x;
        map.top = b.origin.y;
        map.screenW = b.size.width;
        map.screenH = b.size.height;
        map.scale = std::max({1.0, map.screenW / kMaxWidth, map.screenH / kMaxHeight});
        map.imageW = static_cast<int>(map.screenW / map.scale);
        map.imageH = static_cast<int>(map.screenH / map.scale);
        return map;
    }

    CGPoint ToScreen(int x, int y) {
        Map map = CurrentMap();
        return CGPointMake(map.left + x * map.scale, map.top + y * map.scale);
    }

    // Whatever window (not FastChat's) has focus becomes the target: a dialog the
    // app opened, or a window the user switched to.
    void Follow() {
        if (!m_trusted) return;
        if (m_followForeground) {
            pid_t pid = FocusedPid();
            if (pid && pid != getpid()) {
                Element app(AXUIElementCreateApplication(pid));
                Element window = ElementAttr(app.get(), kAXFocusedWindowAttribute);
                if (!window) window = ElementAttr(app.get(), kAXMainWindowAttribute);
                if (window) m_target = InfoFor(pid, window);
            }
        }
        if (m_target.window && !IsAlive(m_target.window.get())) {
            auto windows = UserWindows();
            m_target = windows.empty() ? WindowInfo() : windows.front();
        }
        if (m_target.window) m_target.title = StrAttr(m_target.window.get(), kAXTitleAttribute);
    }

    void BringToFront() {
        if (!m_target.window) return;
        AXUIElementRef w = m_target.window.get();
        if (BoolAttr(w, kAXMinimizedAttribute)) AXUIElementSetAttributeValue(w, kAXMinimizedAttribute, kCFBooleanFalse);
        if (FocusedPid() == m_target.pid && BoolAttr(w, kAXMainAttribute)) return;
        Element app(AXUIElementCreateApplication(m_target.pid));
        AXUIElementSetAttributeValue(app.get(), kAXFrontmostAttribute, kCFBooleanTrue);
        AXUIElementSetAttributeValue(w, kAXMainAttribute, kCFBooleanTrue);
        AXUIElementPerformAction(w, kAXRaiseAction);
        if (FocusedPid() != m_target.pid) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            [[NSRunningApplication runningApplicationWithProcessIdentifier:m_target.pid]
                activateWithOptions:NSApplicationActivateIgnoringOtherApps];
#pragma clang diagnostic pop
        }
        for (int i = 0; i < 20 && FocusedPid() != m_target.pid; i++) Pause(25);
        Pause(100);
    }

    // Before input: the target is open and its app is in front.
    bool EnsureTarget(std::string& message) {
        if (std::string why = Unavailable(); !why.empty()) {
            message = why;
            return false;
        }
        if (!m_target.window || !IsAlive(m_target.window.get())) {
            Follow();
            if (!m_target.window) {
                message = "There is no window to act on. Use list_windows and focus_window.";
                return false;
            }
        }
        BringToFront();
        // Never send input to whatever else is in front: the target's own app
        // (it or one of its menus or dialogs) must have focus.
        if (FocusedPid() != m_target.pid) {
            message = "macOS did not bring " + Describe(m_target) +
                      " to the front, so nothing was sent. Ask the user to switch to it, then try again.";
            return false;
        }
        return true;
    }

    CGEventSourceRef Source() {
        if (!m_source) m_source = CF<CGEventSourceRef>(CGEventSourceCreate(kCGEventSourceStateHIDSystemState));
        return m_source.get();
    }

    void PostMouse(CGEventType type, CGPoint p, CGMouseButton button, int clicks = 0) {
        CF<CGEventRef> e(CGEventCreateMouseEvent(Source(), type, p, button));
        if (!e) return;
        if (clicks > 0) CGEventSetIntegerValueField(e.get(), kCGMouseEventClickState, clicks);
        CGEventPost(kCGHIDEventTap, e.get());
        Pause(10);
    }

    static CGEventFlags ModifierFlag(CGKeyCode code) {
        switch (code) {
            case kVK_Command: return kCGEventFlagMaskCommand;
            case kVK_Control: return kCGEventFlagMaskControl;
            case kVK_Option: return kCGEventFlagMaskAlternate;
            case kVK_Shift: return kCGEventFlagMaskShift;
            case kVK_Function: return kCGEventFlagMaskSecondaryFn;
            default: return 0;
        }
    }

    void PostKey(CGKeyCode code, bool down, CGEventFlags flags, const std::vector<UniChar>* text = nullptr) {
        CF<CGEventRef> e(CGEventCreateKeyboardEvent(Source(), code, down));
        if (!e) return;
        CGEventSetFlags(e.get(), flags);
        if (text && !text->empty()) CGEventKeyboardSetUnicodeString(e.get(), text->size(), text->data());
        CGEventPost(kCGHIDEventTap, e.get());
        Pause(5);
    }

    bool Done(const std::string& what, std::string& message) {
        Pause(250);  // let the app react before describing where focus is
        message = what;
        return true;
    }

    bool NoElement(int id, std::string& message) {
        message = "There is no element [" + std::to_string(id) + "]. Call look to see the current elements.";
        return false;
    }

    AXUIElementRef ElementAt(int id) {
        if (id < 1 || id > static_cast<int>(m_elements.size())) return nullptr;
        return m_elements[id - 1].get();
    }

    // "Save button", "Name text field: Bob".
    std::string Brief(AXUIElementRef el, bool withValue) {
        std::string role = StrAttr(el, kAXRoleAttribute);
        std::string name = OneLine(NameOf(el, role), 80);
        std::string words = RoleWords(el, role);
        std::string text = name.empty() ? words : name + " " + words;
        if (withValue && (role == "AXTextField" || role == "AXTextArea" || role == "AXComboBox" || role == "AXSearchField")) {
            std::string v = OneLine(ValueText(el), 80);
            if (!v.empty()) text += ": " + v;
        }
        return text;
    }

    // What a table or outline row says: the text of its cells, as VoiceOver reads it.
    std::string RowText(AXUIElementRef el, int depth = 0) {
        std::string text;
        for (Element& child : ElementsAttr(el, kAXChildrenAttribute)) {
            std::string role = StrAttr(child.get(), kAXRoleAttribute);
            std::string part;
            if (role == "AXStaticText" || role == "AXTextField") part = ValueText(child.get());
            else if (role == "AXCell" || role == "AXGroup") {
                part = NameOf(child.get(), role);
                if (part.empty() && depth < 3) part = RowText(child.get(), depth + 1);
            }
            part = Trim(part);
            if (part.empty() || text.find(part) != std::string::npos) continue;
            text += (text.empty() ? "" : ", ") + part;
            if (text.size() > 160) break;
        }
        return text;
    }

    // One element's line, numbered; false when it is not listed (unnamed container,
    // off screen, or text its row already said). `said` is what the nearest listed
    // element above it says; `name` gets what this one says.
    bool List(AXUIElementRef el, int depth, int maxElements, std::string& out, int& more, bool menu,
              const std::string& said, std::string& name) {
        std::string role = StrAttr(el, kAXRoleAttribute);
        name = OneLine(NameOf(el, role), 120);
        if ((role == "AXRow" || role == "AXOutlineRow") && name.empty()) name = OneLine(RowText(el), 160);
        if (IsQuietContainer(role) && name.empty()) return false;
        if (role == "AXMenu" && name.empty()) return false;  // its items say it all
        // A row's cells and their text repeat the row; unnamed images say nothing.
        if (role == "AXCell") return false;
        if (role == "AXImage" && name.empty()) return false;
        if (role == "AXDisclosureTriangle" && !said.empty()) return false;  // the row expands itself
        if ((role == "AXStaticText" || (role == "AXTextField" && !BoolAttr(el, kAXFocusedAttribute))) && !said.empty()) {
            std::string text = Trim(role == "AXStaticText" ? name : ValueText(el));
            if (text.empty() || said.find(text) != std::string::npos) return false;
        }
        CGRect r = CGRectZero;
        bool hasFrame = Frame(el, r) && r.size.width > 0 && r.size.height > 0;
        if (!menu && hasFrame && !CGRectIsEmpty(m_windowFrame) && !CGRectIntersectsRect(r, m_windowFrame)) return false;
        if (static_cast<int>(m_elements.size()) >= maxElements) {
            more++;
            return false;
        }
        m_elements.push_back(Element::Retain(el));
        int id = static_cast<int>(m_elements.size());
        std::string line(static_cast<size_t>(std::min(depth, 12)) * 2, ' ');
        line += "[" + std::to_string(id) + "] " + RoleWords(el, role);
        if (!name.empty()) line += " \"" + name + "\"";
        if (role == "AXCheckBox" || role == "AXSwitch") {
            int v = NumberValue(el, -1);
            if (v == 1) line += ", checked";
            else if (v == 0) line += ", not checked";
            else if (v == 2) line += ", partly checked";
        } else if (role == "AXRadioButton") {
            if (NumberValue(el, 0) == 1) line += ", selected";
        } else if (role == "AXDisclosureTriangle") {
            line += NumberValue(el, 0) ? ", expanded" : ", collapsed";
        } else if (role != "AXStaticText") {
            std::string value = ValueText(el);
            if (!value.empty() && value != name) line += " value \"" + OneLine(value, 120) + "\"";
        }
        CF<CFTypeRef> expanded = Attr(el, kAXExpandedAttribute);
        if (!expanded) expanded = Attr(el, kAXDisclosingAttribute);
        if (expanded && CFGetTypeID(expanded.get()) == CFBooleanGetTypeID()) {
            line += CFBooleanGetValue(static_cast<CFBooleanRef>(expanded.get())) ? ", expanded" : ", collapsed";
        }
        if (role != "AXRadioButton" && BoolAttr(el, kAXSelectedAttribute)) line += ", selected";
        if (!BoolAttr(el, kAXEnabledAttribute, true)) line += ", unavailable";
        // Only the one element the app says has focus (list cells claim it along with their list).
        if (m_focused && CFEqual(el, m_focused.get())) line += ", FOCUSED";
        if (hasFrame) {
            int cx = static_cast<int>(std::lround((CGRectGetMidX(r) - m_map.left) / m_map.scale));
            int cy = static_cast<int>(std::lround((CGRectGetMidY(r) - m_map.top) / m_map.scale));
            line += " at " + std::to_string(cx) + "," + std::to_string(cy);
        }
        out += line + "\n";
        return true;
    }

    void Walk(AXUIElementRef el, int depth, int maxElements, std::string& out, int& more, bool menu = false,
              const std::string& said = std::string()) {
        // A table's columns hold its cells over again.
        if (depth > 0 && StrAttr(el, kAXRoleAttribute) == "AXColumn") return;
        std::string name;
        bool listed = depth > 0 && List(el, depth, maxElements, out, more, menu, said, name);
        if (depth > 40) return;
        // Off-screen parts of the window (scrolled away) are left out, children too.
        if (depth > 0 && !menu && !listed) {
            CGRect r;
            if (Frame(el, r) && r.size.width > 0 && !CGRectIsEmpty(m_windowFrame) && !CGRectIntersectsRect(r, m_windowFrame)) {
                return;
            }
        }
        std::vector<Element> children = ElementsAttr(el, kAXVisibleChildrenAttribute);
        if (children.empty()) children = ElementsAttr(el, kAXChildrenAttribute);
        for (Element& child : children) {
            Walk(child.get(), listed || depth == 0 ? depth + 1 : depth, maxElements, out, more, menu, listed ? name : said);
        }
    }

    CF<CGImageRef> CaptureDisplay(const Map& map, std::string& error) {
        if (@available(macOS 14.0, *)) {
            __block SCDisplay* display = nil;
            __block NSError* failure = nil;
            dispatch_semaphore_t got = dispatch_semaphore_create(0);
            [SCShareableContent getShareableContentExcludingDesktopWindows:NO
                                                       onScreenWindowsOnly:YES
                                                         completionHandler:^(SCShareableContent* content, NSError* err) {
                for (SCDisplay* d in content.displays) {
                    if (d.displayID == map.display) display = d;
                }
                failure = err;
                dispatch_semaphore_signal(got);
            }];
            if (dispatch_semaphore_wait(got, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) != 0 || !display) {
                error = failure ? std::string("The screen could not be captured: ") + failure.localizedDescription.UTF8String
                                : std::string("The screen could not be captured");
                return CF<CGImageRef>();
            }
            SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
            SCStreamConfiguration* config = [[SCStreamConfiguration alloc] init];
            config.width = static_cast<size_t>(map.imageW);
            config.height = static_cast<size_t>(map.imageH);
            config.showsCursor = YES;  // so the model sees where the pointer is
            __block CGImageRef image = nullptr;
            dispatch_semaphore_t done = dispatch_semaphore_create(0);
            [SCScreenshotManager captureImageWithFilter:filter
                                          configuration:config
                                      completionHandler:^(CGImageRef img, NSError* err) {
                if (img) image = CGImageRetain(img);
                failure = err;
                dispatch_semaphore_signal(done);
            }];
            if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) != 0) {
                error = "The screen capture took too long";
                return CF<CGImageRef>();
            }
            if (!image && failure) error = std::string("The screen could not be captured: ") + failure.localizedDescription.UTF8String;
            return CF<CGImageRef>(image);
        }
        // macOS 11 to 13: the older call, gone from newer SDKs but still in the system there.
        using Fn = CGImageRef (*)(CGDirectDisplayID);
        static Fn displayImage = reinterpret_cast<Fn>(dlsym(RTLD_DEFAULT, "CGDisplayCreateImage"));
        if (!displayImage) return CF<CGImageRef>();
        return CF<CGImageRef>(displayImage(map.display));
    }

    bool EncodeJpeg(CGImageRef image, int w, int h, std::string& base64) {
        CF<CGColorSpaceRef> space(CGColorSpaceCreateDeviceRGB());
        CF<CGContextRef> ctx(CGBitmapContextCreate(nullptr, static_cast<size_t>(w), static_cast<size_t>(h), 8, 0, space.get(),
                                                   kCGImageAlphaNoneSkipLast));
        if (!ctx) return false;
        CGContextSetInterpolationQuality(ctx.get(), kCGInterpolationHigh);
        CGContextDrawImage(ctx.get(), CGRectMake(0, 0, w, h), image);
        CF<CGImageRef> scaled(CGBitmapContextCreateImage(ctx.get()));
        if (!scaled) return false;
        NSMutableData* data = [NSMutableData data];
        CF<CGImageDestinationRef> dest(
            CGImageDestinationCreateWithData((__bridge CFMutableDataRef)data, CFSTR("public.jpeg"), 1, nullptr));
        if (!dest) return false;
        NSDictionary* props = @{(__bridge NSString*)kCGImageDestinationLossyCompressionQuality: @0.75};
        CGImageDestinationAddImage(dest.get(), scaled.get(), (__bridge CFDictionaryRef)props);
        if (!CGImageDestinationFinalize(dest.get())) return false;
        base64 = Base64Encode(std::string(static_cast<const char*>(data.bytes), data.length));
        return true;
    }

    bool m_trusted = false;
    bool m_followForeground = true;
    WindowInfo m_target;
    std::vector<Element> m_elements;
    Element m_focused;
    CF<CGEventSourceRef> m_source;
    Map m_map;
    CGRect m_windowFrame = CGRectZero;
};

}  // namespace

std::unique_ptr<Desktop> Desktop::Create() {
    return std::make_unique<MacDesktop>();
}
