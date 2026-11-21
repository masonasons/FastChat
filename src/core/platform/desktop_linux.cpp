// Computer use on Linux, in an X11 session: the window manager's EWMH
// properties for the windows, AT-SPI for the accessibility tree and acting on
// elements, XTest for the keyboard and mouse, XGetImage (with the pointer from
// XFixes) and gdk-pixbuf for screenshots (JPEG). The Desktop has an X
// connection of its own, used on the chat worker thread only; the stop key
// (Ctrl+Alt+Escape) is watched through XCB on a thread of its own.

#include "core/desktop.h"
#include "core/util.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include <poll.h>
#include <unistd.h>

#include <atspi/atspi.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>
#include <xcb/xcb.h>
#include <xcb/xinput.h>

// Last: Xlib defines None, Bool, Status and the like as macros.
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrandr.h>
#include <X11/keysym.h>

namespace {

const int kMaxWidth = 1280;
const int kMaxHeight = 800;
const int kMaxVisits = 4000;     // accessibility nodes read for one look
const int kMaxChildren = 400;    // children of one node (long lists and tables)

void SleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// ---------------------------------------------------------------------------
// X errors
// ---------------------------------------------------------------------------

// Xlib reports errors through one handler for the whole process, and GTK has
// its own. A window can close between listing it and reading it (BadWindow):
// errors on FastChat's desktop connections are noted and ignored, and the rest
// go on to the handler that was there before.
std::mutex g_errorLock;
std::set<Display*> g_ourDisplays;
XErrorHandler g_previousHandler = nullptr;

int OnXError(Display* d, XErrorEvent* e) {
    {
        std::lock_guard<std::mutex> lock(g_errorLock);
        if (g_ourDisplays.count(d)) return 0;
    }
    return g_previousHandler ? g_previousHandler(d, e) : 0;
}

void AdoptDisplay(Display* d) {
    static std::once_flag once;
    std::call_once(once, []() { g_previousHandler = XSetErrorHandler(OnXError); });
    std::lock_guard<std::mutex> lock(g_errorLock);
    g_ourDisplays.insert(d);
}

void ReleaseDisplay(Display* d) {
    std::lock_guard<std::mutex> lock(g_errorLock);
    g_ourDisplays.erase(d);
}

// ---------------------------------------------------------------------------
// AT-SPI
// ---------------------------------------------------------------------------

// An accessible object, released when done with.
struct Acc {
    AtspiAccessible* p = nullptr;
    Acc() = default;
    explicit Acc(AtspiAccessible* a) : p(a) {}
    Acc(const Acc& o) : p(o.p) { if (p) g_object_ref(p); }
    Acc(Acc&& o) noexcept : p(o.p) { o.p = nullptr; }
    Acc& operator=(Acc o) { std::swap(p, o.p); return *this; }
    ~Acc() { if (p) g_object_unref(p); }
    explicit operator bool() const { return p != nullptr; }
};

std::string Take(gchar* s) {
    if (!s) return std::string();
    std::string out(s);
    g_free(s);
    return out;
}

void Clear(GError*& e) {
    if (e) g_error_free(e);
    e = nullptr;
}

// Turn on accessibility for the session, as Orca does: Qt, Chromium and
// Electron apps only expose their controls when it is on.
void EnableSessionAccessibility() {
    GError* err = nullptr;
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
    if (!bus) {
        Clear(err);
        return;
    }
    GVariant* got = g_dbus_connection_call_sync(bus, "org.a11y.Bus", "/org/a11y/bus", "org.freedesktop.DBus.Properties",
                                                "Get", g_variant_new("(ss)", "org.a11y.Status", "IsEnabled"),
                                                G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, &err);
    bool enabled = false;
    if (got) {
        GVariant* v = nullptr;
        g_variant_get(got, "(v)", &v);
        if (v && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN)) enabled = g_variant_get_boolean(v);
        if (v) g_variant_unref(v);
        g_variant_unref(got);
    }
    Clear(err);
    if (!enabled) {
        GVariant* set = g_dbus_connection_call_sync(
            bus, "org.a11y.Bus", "/org/a11y/bus", "org.freedesktop.DBus.Properties", "Set",
            g_variant_new("(ssv)", "org.a11y.Status", "IsEnabled", g_variant_new_boolean(TRUE)), nullptr,
            G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, &err);
        if (set) g_variant_unref(set);
        Clear(err);
    }
    g_object_unref(bus);
}

// Once for the process. AT-SPI's own events go to a main context of its own,
// never run, so nothing of it happens on GTK's thread.
bool InitAtspi() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, []() {
        EnableSessionAccessibility();
        int r = atspi_init();
        ok = r == 0 || r == 1;  // 1: already set up
        if (ok) {
            atspi_set_main_context(g_main_context_new());
            // An app that does not answer holds up a look for this long, not the default 25 seconds.
            atspi_set_timeout(3000, 15000);
        }
    });
    return ok;
}

bool HasState(AtspiStateSet* set, AtspiStateType s) {
    return set && atspi_state_set_contains(set, s);
}

std::string RoleName(AtspiRole role, AtspiAccessible* el) {
    switch (role) {
        case ATSPI_ROLE_PUSH_BUTTON: return "button";
        case ATSPI_ROLE_TOGGLE_BUTTON: return "toggle button";
        case ATSPI_ROLE_CHECK_BOX: return "check box";
        case ATSPI_ROLE_RADIO_BUTTON: return "radio button";
        case ATSPI_ROLE_COMBO_BOX: return "combo box";
        case ATSPI_ROLE_ENTRY: case ATSPI_ROLE_EDITBAR: return "edit";
        case ATSPI_ROLE_PASSWORD_TEXT: return "password edit";
        case ATSPI_ROLE_SPIN_BUTTON: return "spinner";
        case ATSPI_ROLE_LABEL: case ATSPI_ROLE_STATIC: return "text";
        case ATSPI_ROLE_PAGE_TAB: return "tab";
        case ATSPI_ROLE_PAGE_TAB_LIST: return "tab list";
        case ATSPI_ROLE_FRAME: case ATSPI_ROLE_WINDOW: return "window";
        case ATSPI_ROLE_DIALOG: return "dialog";
        case ATSPI_ROLE_LINK: return "link";
        case ATSPI_ROLE_TEXT: {
            AtspiStateSet* st = atspi_accessible_get_state_set(el);
            bool multi = HasState(st, ATSPI_STATE_MULTI_LINE), editable = HasState(st, ATSPI_STATE_EDITABLE);
            if (st) g_object_unref(st);
            if (editable) return multi ? "text entry area" : "edit";
            return "text";
        }
        default: break;
    }
    std::string name = Take(atspi_accessible_get_role_name(el, nullptr));
    return name.empty() ? std::string("element") : name;
}

// Containers that say nothing on their own; their children are listed in their place.
bool IsQuietContainer(AtspiRole role) {
    switch (role) {
        case ATSPI_ROLE_PANEL: case ATSPI_ROLE_FILLER: case ATSPI_ROLE_SECTION: case ATSPI_ROLE_UNKNOWN:
        case ATSPI_ROLE_REDUNDANT_OBJECT: case ATSPI_ROLE_SCROLL_PANE: case ATSPI_ROLE_VIEWPORT:
        case ATSPI_ROLE_SPLIT_PANE: case ATSPI_ROLE_LAYERED_PANE: case ATSPI_ROLE_GROUPING: case ATSPI_ROLE_INVALID:
        case ATSPI_ROLE_BLOCK_QUOTE: case ATSPI_ROLE_PARAGRAPH: case ATSPI_ROLE_ROOT_PANE: case ATSPI_ROLE_GLASS_PANE:
        case ATSPI_ROLE_OPTION_PANE:
            return true;
        default:
            return false;
    }
}

bool IsTextRole(AtspiRole role) {
    return role == ATSPI_ROLE_ENTRY || role == ATSPI_ROLE_TEXT || role == ATSPI_ROLE_PASSWORD_TEXT ||
           role == ATSPI_ROLE_SPIN_BUTTON || role == ATSPI_ROLE_COMBO_BOX || role == ATSPI_ROLE_DOCUMENT_TEXT ||
           role == ATSPI_ROLE_TERMINAL || role == ATSPI_ROLE_EDITBAR;
}

// The text an edit field holds (not a password's).
std::string FieldValue(AtspiAccessible* el, AtspiRole role, size_t maxChars) {
    if (role == ATSPI_ROLE_PASSWORD_TEXT) return std::string();
    if (IsTextRole(role)) {
        AtspiText* text = atspi_accessible_get_text_iface(el);
        if (text) {
            GError* err = nullptr;
            int count = atspi_text_get_character_count(text, &err);
            Clear(err);
            std::string s;
            if (count > 0) s = Take(atspi_text_get_text(text, 0, std::min<int>(count, static_cast<int>(maxChars)), &err));
            Clear(err);
            g_object_unref(text);
            if (!s.empty()) return s;
        }
    }
    AtspiValue* value = atspi_accessible_get_value_iface(el);
    if (value) {
        GError* err = nullptr;
        std::string s = Take(atspi_value_get_text(value, &err));
        Clear(err);
        if (s.empty()) {
            double v = atspi_value_get_current_value(value, &err);
            if (!err) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%g", v);
                s = buf;
            }
            Clear(err);
        }
        g_object_unref(value);
        return s;
    }
    return std::string();
}

bool Extents(AtspiAccessible* el, int& x, int& y, int& w, int& h) {
    AtspiComponent* comp = atspi_accessible_get_component_iface(el);
    if (!comp) return false;
    GError* err = nullptr;
    AtspiRect* r = atspi_component_get_extents(comp, ATSPI_COORD_TYPE_SCREEN, &err);
    g_object_unref(comp);
    bool ok = r && !err && r->width > 0 && r->height > 0;
    if (r) {
        x = r->x;
        y = r->y;
        w = r->width;
        h = r->height;
        g_free(r);
    }
    Clear(err);
    return ok;
}

// An element's name, or for a control without one, the label's that names it
// (GTK links "Name:" to the field after it this way).
std::string NameOf(AtspiAccessible* el, AtspiRole role) {
    GError* err = nullptr;
    std::string name = Take(atspi_accessible_get_name(el, &err));
    Clear(err);
    if (!name.empty() || IsQuietContainer(role) || role == ATSPI_ROLE_LABEL || role == ATSPI_ROLE_SEPARATOR) return name;
    GArray* relations = atspi_accessible_get_relation_set(el, &err);
    Clear(err);
    if (!relations) return name;
    for (guint i = 0; i < relations->len && name.empty(); i++) {
        AtspiRelation* r = g_array_index(relations, AtspiRelation*, i);
        if (atspi_relation_get_relation_type(r) == ATSPI_RELATION_LABELLED_BY && atspi_relation_get_n_targets(r) > 0) {
            AtspiAccessible* label = atspi_relation_get_target(r, 0);
            if (label) {
                name = Take(atspi_accessible_get_name(label, &err));
                Clear(err);
                g_object_unref(label);
            }
        }
    }
    for (guint i = 0; i < relations->len; i++) g_object_unref(g_array_index(relations, AtspiRelation*, i));
    g_array_free(relations, TRUE);
    return name;
}

// "Save button", "Name edit: Bob".
std::string Brief(AtspiAccessible* el) {
    GError* err = nullptr;
    AtspiRole role = atspi_accessible_get_role(el, &err);
    Clear(err);
    std::string name = OneLine(NameOf(el, role), 80);
    std::string text = name.empty() ? RoleName(role, el) : name + " " + RoleName(role, el);
    if (role == ATSPI_ROLE_ENTRY || role == ATSPI_ROLE_COMBO_BOX || role == ATSPI_ROLE_SPIN_BUTTON ||
        role == ATSPI_ROLE_TEXT || role == ATSPI_ROLE_EDITBAR) {
        std::string v = OneLine(FieldValue(el, role, 200), 80);
        if (!v.empty() && v != name) text += ": " + v;
    }
    return text;
}

// ---------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------

struct KeyName {
    const char* name;
    KeySym sym;
};

const KeyName kKeys[] = {
    {"ctrl", XK_Control_L}, {"control", XK_Control_L}, {"alt", XK_Alt_L}, {"option", XK_Alt_L},
    {"shift", XK_Shift_L}, {"win", XK_Super_L}, {"windows", XK_Super_L}, {"super", XK_Super_L},
    {"meta", XK_Super_L}, {"cmd", XK_Super_L}, {"altgr", XK_ISO_Level3_Shift}, {"enter", XK_Return},
    {"return", XK_Return}, {"tab", XK_Tab}, {"escape", XK_Escape}, {"esc", XK_Escape}, {"space", XK_space},
    {"backspace", XK_BackSpace}, {"delete", XK_Delete}, {"del", XK_Delete}, {"insert", XK_Insert},
    {"ins", XK_Insert}, {"home", XK_Home}, {"end", XK_End}, {"pageup", XK_Prior}, {"pgup", XK_Prior},
    {"pagedown", XK_Next}, {"pgdn", XK_Next}, {"up", XK_Up}, {"down", XK_Down}, {"left", XK_Left},
    {"right", XK_Right}, {"apps", XK_Menu}, {"menu", XK_Menu}, {"contextmenu", XK_Menu},
    {"capslock", XK_Caps_Lock}, {"numlock", XK_Num_Lock}, {"scrolllock", XK_Scroll_Lock},
    {"printscreen", XK_Print}, {"print", XK_Print}, {"pause", XK_Pause}, {"plus", XK_plus},
    {"minus", XK_minus}, {"comma", XK_comma}, {"period", XK_period},
};

bool IsModifier(KeySym s) {
    return s == XK_Control_L || s == XK_Alt_L || s == XK_Shift_L || s == XK_Super_L || s == XK_ISO_Level3_Shift;
}

KeySym CharKeySym(char32_t c) {
    if (c == '\n') return XK_Return;
    if (c == '\t') return XK_Tab;
    if ((c >= 0x20 && c <= 0x7e) || (c >= 0xa0 && c <= 0xff)) return static_cast<KeySym>(c);  // Latin-1: the same numbers
    return static_cast<KeySym>(0x01000000 | c);
}

std::u32string Utf8ToCodepoints(const std::string& s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        char32_t cp;
        int len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1f; len = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0f; len = 3; }
        else if ((c >> 3) == 30) { cp = c & 0x07; len = 4; }
        else { i++; continue; }
        if (i + len > s.size()) break;
        for (int k = 1; k < len; k++) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3f);
        out.push_back(cp);
        i += len;
    }
    return out;
}

// ---------------------------------------------------------------------------
// The desktop
// ---------------------------------------------------------------------------

class LinuxDesktop : public Desktop {
public:
    LinuxDesktop() {
        m_dpy = XOpenDisplay(nullptr);
        if (m_dpy) {
            AdoptDisplay(m_dpy);
            m_root = DefaultRootWindow(m_dpy);
            int ev, err, major, minor;
            m_xtest = XTestQueryExtension(m_dpy, &ev, &err, &major, &minor);
            m_xfixes = XFixesQueryExtension(m_dpy, &ev, &err);
            m_xrandr = XRRQueryExtension(m_dpy, &ev, &err);
            auto windows = UserWindows();
            if (!windows.empty()) m_target = windows.front();  // the one in front of all but FastChat's
            m_atspi = InitAtspi();  // without a display there is no session to read
        }
    }
    ~LinuxDesktop() override {
        m_elements.clear();
        if (m_dpy) {
            RestoreScratchKey();
            ReleaseDisplay(m_dpy);
            XCloseDisplay(m_dpy);
        }
    }

    std::string Unavailable() override {
        const char* type = std::getenv("XDG_SESSION_TYPE");
        if ((type && std::string(type) == "wayland") || (std::getenv("WAYLAND_DISPLAY") && *std::getenv("WAYLAND_DISPLAY"))) {
            return "Computer use needs an X11 session; under Wayland other apps' windows can't be seen or operated. "
                   "Log out and choose an X11 (Xorg) session at the login screen to use it.";
        }
        if (!m_dpy) return "Computer use needs an X11 display, and none could be opened (DISPLAY is not set).";
        if (!m_xtest) return "The X server lacks the XTEST extension, which computer use needs to press keys and click.";
        if (!m_atspi) {
            return "The accessibility bus (AT-SPI) is not running, so other apps' controls can't be read. Install "
                   "at-spi2-core and log in again.";
        }
        return std::string();
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
            if (IsMinimized(windows[i])) out += " (minimized)";
            out += "\n";
        }
        return out.empty() ? std::string("No windows are open.\n") : out;
    }

    bool FocusWindow(const std::string& which, std::string& message) override {
        auto windows = UserWindows();
        Window found = 0;
        try {
            size_t n = std::stoul(which);
            if (n >= 1 && n <= windows.size()) found = windows[n - 1];
        } catch (...) {
        }
        if (!found) {
            std::string want = ToLower(which);
            for (Window w : windows) {
                if (ToLower(Title(w)).find(want) != std::string::npos || ToLower(ProcessName(Pid(w))) == want) {
                    found = w;
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

    void SetTargetHandle(void* handle) override {
        m_target = static_cast<Window>(reinterpret_cast<uintptr_t>(handle));
        m_followForeground = false;
    }

    bool Capture(Screenshot& out, std::string& error) override {
        if (!m_dpy) {
            error = "There is no display";
            return false;
        }
        Follow();
        Map map = CurrentMap();
        XImage* img = XGetImage(m_dpy, m_root, map.left, map.top, static_cast<unsigned>(map.screenW),
                                static_cast<unsigned>(map.screenH), AllPlanes, ZPixmap);
        if (!img) {
            error = "The screen could not be captured";
            return false;
        }
        int w = map.screenW, h = map.screenH;
        std::vector<guchar> rgb(static_cast<size_t>(w) * h * 3);
        bool fast = img->bits_per_pixel == 32 && img->red_mask == 0xff0000 && img->green_mask == 0xff00 &&
                    img->blue_mask == 0xff && img->byte_order == LSBFirst;
        for (int y = 0; y < h; y++) {
            guchar* row = &rgb[static_cast<size_t>(y) * w * 3];
            if (fast) {
                const unsigned char* src = reinterpret_cast<unsigned char*>(img->data) + static_cast<size_t>(y) * img->bytes_per_line;
                for (int x = 0; x < w; x++) {
                    row[x * 3] = src[x * 4 + 2];
                    row[x * 3 + 1] = src[x * 4 + 1];
                    row[x * 3 + 2] = src[x * 4];
                }
            } else {
                for (int x = 0; x < w; x++) {
                    unsigned long p = XGetPixel(img, x, y);
                    row[x * 3] = Channel(p, img->red_mask);
                    row[x * 3 + 1] = Channel(p, img->green_mask);
                    row[x * 3 + 2] = Channel(p, img->blue_mask);
                }
            }
        }
        XDestroyImage(img);
        DrawPointer(rgb, map);

        GdkPixbuf* full = gdk_pixbuf_new_from_data(rgb.data(), GDK_COLORSPACE_RGB, FALSE, 8, w, h, w * 3, nullptr, nullptr);
        GdkPixbuf* scaled = (map.imageW == w && map.imageH == h)
                                ? GDK_PIXBUF(g_object_ref(full))
                                : gdk_pixbuf_scale_simple(full, map.imageW, map.imageH, GDK_INTERP_BILINEAR);
        g_object_unref(full);
        if (!scaled) {
            error = "The screenshot could not be scaled";
            return false;
        }
        gchar* buf = nullptr;
        gsize size = 0;
        GError* err = nullptr;
        bool jpeg = gdk_pixbuf_save_to_buffer(scaled, &buf, &size, "jpeg", &err, "quality", "75", nullptr);
        Clear(err);
        if (!jpeg) gdk_pixbuf_save_to_buffer(scaled, &buf, &size, "png", &err, nullptr);  // without the JPEG loader
        g_object_unref(scaled);
        if (!buf) {
            error = std::string("The screenshot could not be encoded") + (err ? std::string(": ") + err->message : "");
            Clear(err);
            return false;
        }
        Clear(err);
        out.base64 = Base64Encode(std::string(buf, size));
        g_free(buf);
        out.mediaType = jpeg ? "image/jpeg" : "image/png";
        out.width = map.imageW;
        out.height = map.imageH;
        return true;
    }

    std::string Tree(int maxElements) override {
        Follow();
        m_elements.clear();
        if (!m_dpy || !m_target || !WindowExists(m_target)) return "There is no window to look at.";
        if (!m_atspi) return "Window: " + Describe(m_target) + "\nIts controls can't be read: the accessibility bus is not running.";
        Acc root = FrameFor(m_target);
        if (!root) {
            return "Window: " + Describe(m_target) +
                   "\nThis app shows nothing to accessibility, so its controls can't be listed. Use the keyboard"
                   " (press_keys, type_text), or the mouse with a screenshot.";
        }
        m_map = CurrentMap();
        std::string out = "Window: " + Describe(m_target) + "\n";
        int more = 0, visits = 0;
        Walk(root.p, 0, maxElements, out, more, visits);
        if (more) out += "(" + std::to_string(more) + " more elements not shown)\n";
        return out;
    }

    std::string FocusedDescription() override {
        Follow();
        Window active = ActiveWindow();
        std::string where = Describe(active ? active : m_target);
        if (!m_atspi || !active || IsOwnWindow(active)) return "In " + where;
        Acc frame = FrameFor(active);
        if (!frame) return "In " + where;
        int visits = 0;
        Acc focused = FindFocused(frame.p, 0, visits);
        if (!focused) return "In " + where;
        return "In " + where + ", focus on " + Brief(focused.p);
    }

    std::string ElementDescription(int id) override {
        AtspiAccessible* el = Element(id);
        return el ? Brief(el) : std::string();
    }

    bool ElementPoint(int id, int& x, int& y) override {
        AtspiAccessible* el = Element(id);
        if (!el) return false;
        int ex, ey, ew, eh;
        if (!Extents(el, ex, ey, ew, eh)) return false;
        Map map = CurrentMap();
        x = static_cast<int>(std::lround((ex + ew / 2.0 - map.left) / map.scale));
        y = static_cast<int>(std::lround((ey + eh / 2.0 - map.top) / map.scale));
        return true;
    }

    bool Invoke(int id, std::string& message) override {
        AtspiAccessible* el = Element(id);
        if (!el) return NoElement(id, message);
        std::string what = Brief(el);
        GError* err = nullptr;
        AtspiRole role = atspi_accessible_get_role(el, &err);
        Clear(err);

        // The element's own action: GTK's "click", Qt's "Press" and "Toggle"...
        AtspiAction* action = atspi_accessible_get_action_iface(el);
        if (action) {
            int n = atspi_action_get_n_actions(action, &err);
            Clear(err);
            static const char* kPreferred[] = {"click", "press", "activate", "toggle", "jump", "open", "select",
                                               "expand or contract", "expand or collapse"};
            int chosen = -1;
            std::string chosenName;
            for (const char* want : kPreferred) {
                for (int i = 0; i < n && chosen < 0; i++) {
                    std::string name = ToLower(Take(atspi_action_get_action_name(action, i, &err)));
                    Clear(err);
                    if (name == want) {
                        chosen = i;
                        chosenName = name;
                    }
                }
                if (chosen >= 0) break;
            }
            if (chosen < 0 && n > 0) {
                std::string name = ToLower(Take(atspi_action_get_action_name(action, 0, &err)));
                Clear(err);
                if (name != "setfocus" && name != "showmenu" && name != "show menu") {
                    chosen = 0;
                    chosenName = name;
                }
            }
            bool done = chosen >= 0 && atspi_action_do_action(action, chosen, &err) && !err;
            Clear(err);
            g_object_unref(action);
            if (done) {
                bool toggle = chosenName == "toggle" || role == ATSPI_ROLE_CHECK_BOX || role == ATSPI_ROLE_TOGGLE_BUTTON ||
                              role == ATSPI_ROLE_CHECK_MENU_ITEM;
                bool expand = chosenName.find("expand") != std::string::npos;
                return Done((toggle ? "Toggled " : expand ? "Expanded or collapsed " : "Pressed ") + what, message);
            }
        }
        // An item in a list or tab list: select it through its parent.
        Acc parent(atspi_accessible_get_parent(el, &err));
        Clear(err);
        if (parent) {
            AtspiSelection* sel = atspi_accessible_get_selection_iface(parent.p);
            if (sel) {
                int index = atspi_accessible_get_index_in_parent(el, &err);
                Clear(err);
                bool done = index >= 0 && atspi_selection_select_child(sel, index, &err) && !err;
                Clear(err);
                g_object_unref(sel);
                if (done) return Done("Selected " + what, message);
            }
        }
        int x, y;
        if (ElementPoint(id, x, y)) return Click(x, y, "left", 1, message);
        message = what + " cannot be pressed.";
        return false;
    }

    bool SetText(int id, const std::string& text, std::string& message) override {
        AtspiAccessible* el = Element(id);
        if (!el) return NoElement(id, message);
        std::string what = Brief(el);
        AtspiEditableText* edit = atspi_accessible_get_editable_text_iface(el);
        if (edit) {
            GError* err = nullptr;
            bool ok = atspi_editable_text_set_text_contents(edit, text.c_str(), &err) && !err;
            Clear(err);
            g_object_unref(edit);
            if (ok) return Done("Set " + what + " to \"" + OneLine(text, 60) + "\"", message);
        }
        // Otherwise: focus it, select everything, and type.
        if (!EnsureTarget(message)) return false;
        GrabFocus(el);
        SleepMs(50);
        PressKeys("ctrl+a", message);
        return TypeText(text, message);
    }

    bool FocusElement(int id, std::string& message) override {
        AtspiAccessible* el = Element(id);
        if (!el) return NoElement(id, message);
        if (!EnsureTarget(message)) return false;
        if (!GrabFocus(el)) {
            message = Brief(el) + " cannot take focus.";
            return false;
        }
        return Done("Focused " + Brief(el), message);
    }

    bool Click(int x, int y, const std::string& button, int count, std::string& message) override {
        if (!EnsureTarget(message)) return false;
        int sx, sy;
        ToScreen(x, y, sx, sy);
        unsigned b = button == "right" ? 3 : button == "middle" ? 2 : 1;
        XTestFakeMotionEvent(m_dpy, -1, sx, sy, CurrentTime);
        XSync(m_dpy, False);
        SleepMs(30);
        for (int i = 0; i < std::max(1, count); i++) {
            XTestFakeButtonEvent(m_dpy, b, True, CurrentTime);
            XTestFakeButtonEvent(m_dpy, b, False, CurrentTime);
        }
        XSync(m_dpy, False);
        return Done(std::string(count >= 2 ? "Double-clicked" : button == "right" ? "Right-clicked" : "Clicked") + " at " +
                        std::to_string(x) + ", " + std::to_string(y),
                    message);
    }

    bool Drag(int x1, int y1, int x2, int y2, std::string& message) override {
        if (!EnsureTarget(message)) return false;
        int ax, ay, bx, by;
        ToScreen(x1, y1, ax, ay);
        ToScreen(x2, y2, bx, by);
        XTestFakeMotionEvent(m_dpy, -1, ax, ay, CurrentTime);
        XTestFakeButtonEvent(m_dpy, 1, True, CurrentTime);
        XSync(m_dpy, False);
        for (int i = 1; i <= 12; i++) {
            XTestFakeMotionEvent(m_dpy, -1, ax + (bx - ax) * i / 12, ay + (by - ay) * i / 12, CurrentTime);
            XSync(m_dpy, False);
            SleepMs(15);
        }
        XTestFakeButtonEvent(m_dpy, 1, False, CurrentTime);
        XSync(m_dpy, False);
        return Done("Dragged from " + std::to_string(x1) + ", " + std::to_string(y1) + " to " + std::to_string(x2) + ", " +
                        std::to_string(y2),
                    message);
    }

    bool Scroll(int x, int y, const std::string& direction, int amount, std::string& message) override {
        if (!EnsureTarget(message)) return false;
        amount = std::clamp(amount, 1, 50);
        int sx, sy;
        ToScreen(x, y, sx, sy);
        unsigned b = direction == "up" ? 4 : direction == "down" ? 5 : direction == "left" ? 6 : 7;
        XTestFakeMotionEvent(m_dpy, -1, sx, sy, CurrentTime);
        for (int i = 0; i < amount; i++) {
            XTestFakeButtonEvent(m_dpy, b, True, CurrentTime);
            XTestFakeButtonEvent(m_dpy, b, False, CurrentTime);
        }
        XSync(m_dpy, False);
        return Done("Scrolled " + direction + " " + std::to_string(amount), message);
    }

    bool MoveMouse(int x, int y, std::string& message) override {
        if (!EnsureTarget(message)) return false;
        int sx, sy;
        ToScreen(x, y, sx, sy);
        XTestFakeMotionEvent(m_dpy, -1, sx, sy, CurrentTime);
        XSync(m_dpy, False);
        return Done("Moved the mouse to " + std::to_string(x) + ", " + std::to_string(y), message);
    }

    bool TypeText(const std::string& text, std::string& message) override {
        if (!EnsureTarget(message)) return false;
        for (char32_t c : Utf8ToCodepoints(text)) {
            if (c == '\r') continue;
            if (!TapKeySym(CharKeySym(c))) {
                message = "A character could not be typed.";
                RestoreScratchKey();
                return false;
            }
            SleepMs(8);
        }
        RestoreScratchKey();
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
            std::vector<KeySym> mods;
            KeySym key = 0;
            for (size_t i = 0; i < parts.size(); i++) {
                std::string name = ToLower(Trim(parts[i]));
                if (name.empty()) continue;
                KeySym sym = 0;
                for (const auto& k : kKeys) {
                    if (name == k.name) sym = k.sym;
                }
                if (!sym && name.size() >= 2 && name[0] == 'f' && std::all_of(name.begin() + 1, name.end(), ::isdigit)) {
                    int n = std::stoi(name.substr(1));
                    if (n >= 1 && n <= 24) sym = XK_F1 + n - 1;
                }
                if (!sym) {
                    std::u32string cps = Utf8ToCodepoints(parts[i]);
                    if (cps.size() == 1) sym = CharKeySym(cps[0]);
                }
                if (!sym) {
                    message = "Unknown key \"" + parts[i] + "\".";
                    return false;
                }
                if (IsModifier(sym) && i + 1 < parts.size()) mods.push_back(sym);
                else key = sym;
            }
            std::vector<KeyCode> held;
            for (KeySym m : mods) {
                KeyCode kc = XKeysymToKeycode(m_dpy, m);
                if (!kc) continue;
                XTestFakeKeyEvent(m_dpy, kc, True, CurrentTime);
                held.push_back(kc);
            }
            bool ok = true;
            if (key) {
                // A letter with ctrl is the letter's key, whatever its case.
                bool withMods = !mods.empty();
                ok = TapKeySym(key, withMods && key >= 'A' && key <= 'Z' ? key - 'A' + 'a' : key);
            }
            for (auto it = held.rbegin(); it != held.rend(); ++it) XTestFakeKeyEvent(m_dpy, *it, False, CurrentTime);
            XSync(m_dpy, False);
            RestoreScratchKey();
            if (!ok) {
                message = "The key " + combo + " could not be pressed.";
                return false;
            }
            SleepMs(60);
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

    // --- windows (EWMH) ---

    Atom A(const char* name) { return XInternAtom(m_dpy, name, False); }

    std::vector<unsigned long> Property(Window w, Atom prop, Atom type) {
        std::vector<unsigned long> out;
        Atom actual;
        int format;
        unsigned long n, after;
        unsigned char* data = nullptr;
        if (XGetWindowProperty(m_dpy, w, prop, 0, 4096, False, type, &actual, &format, &n, &after, &data) == Success &&
            data && format == 32) {
            auto* longs = reinterpret_cast<unsigned long*>(data);
            out.assign(longs, longs + n);
        }
        if (data) XFree(data);
        return out;
    }

    std::string Title(Window w) {
        Atom actual;
        int format;
        unsigned long n, after;
        unsigned char* data = nullptr;
        std::string title;
        if (XGetWindowProperty(m_dpy, w, A("_NET_WM_NAME"), 0, 1024, False, A("UTF8_STRING"), &actual, &format, &n,
                               &after, &data) == Success &&
            data && format == 8) {
            title.assign(reinterpret_cast<char*>(data), n);
        }
        if (data) XFree(data);
        if (title.empty()) {
            char* name = nullptr;
            if (XFetchName(m_dpy, w, &name) && name) {
                title = name;
                XFree(name);
            }
        }
        return title;
    }

    int Pid(Window w) {
        auto v = Property(w, A("_NET_WM_PID"), XA_CARDINAL);
        return v.empty() ? 0 : static_cast<int>(v[0]);
    }

    static std::string ProcessName(int pid) {
        if (pid <= 0) return std::string();
        std::ifstream in("/proc/" + std::to_string(pid) + "/comm");
        std::string name;
        std::getline(in, name);
        return name;
    }

    bool IsOwnWindow(Window w) { return Pid(w) == static_cast<int>(getpid()); }

    bool WindowExists(Window w) {
        XWindowAttributes a;
        return XGetWindowAttributes(m_dpy, w, &a) != 0;
    }

    bool IsMinimized(Window w) {
        Atom hidden = A("_NET_WM_STATE_HIDDEN");
        for (unsigned long s : Property(w, A("_NET_WM_STATE"), XA_ATOM)) {
            if (s == hidden) return true;
        }
        return false;
    }

    // Normal windows and dialogs, not docks, panels or the desktop.
    bool IsUserWindow(Window w) {
        if (IsOwnWindow(w) || Title(w).empty()) return false;
        auto types = Property(w, A("_NET_WM_WINDOW_TYPE"), XA_ATOM);
        if (types.empty()) return true;
        Atom normal = A("_NET_WM_WINDOW_TYPE_NORMAL"), dialog = A("_NET_WM_WINDOW_TYPE_DIALOG");
        for (unsigned long t : types) {
            if (t == normal || t == dialog) return true;
        }
        return false;
    }

    // Front to back.
    std::vector<Window> UserWindows() {
        std::vector<Window> list;
        if (!m_dpy) return list;
        auto stack = Property(m_root, A("_NET_CLIENT_LIST_STACKING"), XA_WINDOW);
        if (stack.empty()) stack = Property(m_root, A("_NET_CLIENT_LIST"), XA_WINDOW);
        for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
            if (IsUserWindow(*it)) list.push_back(*it);
        }
        return list;
    }

    Window ActiveWindow() {
        if (!m_dpy) return 0;
        auto v = Property(m_root, A("_NET_ACTIVE_WINDOW"), XA_WINDOW);
        return v.empty() ? 0 : v[0];
    }

    std::string Describe(Window w) {
        if (!w || !m_dpy) return std::string();
        std::string text = Title(w);
        std::string proc = ProcessName(Pid(w));
        if (!proc.empty()) text += " (" + proc + ")";
        return text;
    }

    bool Geometry(Window w, int& x, int& y, int& width, int& height) {
        XWindowAttributes a;
        if (!XGetWindowAttributes(m_dpy, w, &a)) return false;
        Window child;
        if (!XTranslateCoordinates(m_dpy, w, m_root, 0, 0, &x, &y, &child)) return false;
        width = a.width;
        height = a.height;
        return true;
    }

    // Whatever window (not FastChat's) is in front becomes the target: a dialog
    // the app opened, or a window the user switched to.
    void Follow() {
        if (!m_dpy) return;
        if (m_followForeground) {
            Window active = ActiveWindow();
            if (active && !IsOwnWindow(active) && WindowExists(active)) {
                auto types = Property(active, A("_NET_WM_WINDOW_TYPE"), XA_ATOM);
                Atom dock = A("_NET_WM_WINDOW_TYPE_DOCK"), desk = A("_NET_WM_WINDOW_TYPE_DESKTOP");
                bool shell = std::find(types.begin(), types.end(), dock) != types.end() ||
                             std::find(types.begin(), types.end(), desk) != types.end();
                if (!shell) m_target = active;
            }
        }
        if (m_target && !WindowExists(m_target)) {
            auto windows = UserWindows();
            m_target = windows.empty() ? 0 : windows.front();
        }
    }

    void BringToFront(Window w) {
        if (ActiveWindow() == w) return;
        // A request from a pager (source 2), which window managers honour, raising
        // and un-minimizing it and switching to its workspace.
        auto desktop = Property(w, A("_NET_WM_DESKTOP"), XA_CARDINAL);
        if (!desktop.empty() && desktop[0] != 0xFFFFFFFF) {
            SendRootMessage(m_root, A("_NET_CURRENT_DESKTOP"), static_cast<long>(desktop[0]), CurrentTime);
        }
        SendRootMessage(w, A("_NET_ACTIVE_WINDOW"), 2, CurrentTime);
        XMapRaised(m_dpy, w);
        XSync(m_dpy, False);
        for (int i = 0; i < 20 && ActiveWindow() != w; i++) SleepMs(25);
        SleepMs(100);
    }

    void SendRootMessage(Window w, Atom type, long l0, long l1) {
        XEvent e{};
        e.xclient.type = ClientMessage;
        e.xclient.window = w;
        e.xclient.message_type = type;
        e.xclient.format = 32;
        e.xclient.data.l[0] = l0;
        e.xclient.data.l[1] = l1;
        XSendEvent(m_dpy, m_root, False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
    }

    // Before input: the target is open and in front.
    bool EnsureTarget(std::string& message) {
        if (!m_dpy) {
            message = "There is no display.";
            return false;
        }
        if (!m_target || !WindowExists(m_target)) {
            Follow();
            if (!m_target || !WindowExists(m_target)) {
                message = "There is no window to act on. Use list_windows and focus_window.";
                return false;
            }
        }
        BringToFront(m_target);
        // Never send input to whatever else is in front: the target's own app
        // (it or one of its menus or dialogs) must have the focus.
        Window active = ActiveWindow();
        int activePid = active ? Pid(active) : 0, targetPid = Pid(m_target);
        bool same = active == m_target || (activePid && activePid == targetPid);
        if (!active || !same || IsOwnWindow(active)) {
            message = "The window manager did not bring " + Describe(m_target) +
                      " to the front, so nothing was sent. Ask the user to switch to it, then try again.";
            return false;
        }
        return true;
    }

    // The monitor the target is on, and how it scales to the screenshot.
    Map CurrentMap() {
        Map map;
        if (!m_dpy) {
            map.screenW = map.imageW = kMaxWidth;
            map.screenH = map.imageH = kMaxHeight;
            return map;
        }
        map.screenW = DisplayWidth(m_dpy, DefaultScreen(m_dpy));
        map.screenH = DisplayHeight(m_dpy, DefaultScreen(m_dpy));
        if (m_xrandr) {
            int cx = map.screenW / 2, cy = map.screenH / 2;
            int wx, wy, ww, wh;
            if (m_target && Geometry(m_target, wx, wy, ww, wh)) {
                cx = wx + ww / 2;
                cy = wy + wh / 2;
            }
            int n = 0;
            XRRMonitorInfo* mons = XRRGetMonitors(m_dpy, m_root, True, &n);
            int pick = -1;
            for (int i = 0; i < n; i++) {
                if (cx >= mons[i].x && cx < mons[i].x + mons[i].width && cy >= mons[i].y && cy < mons[i].y + mons[i].height) pick = i;
            }
            for (int i = 0; i < n && pick < 0; i++) {
                if (mons[i].primary) pick = i;
            }
            if (pick < 0 && n > 0) pick = 0;
            if (pick >= 0) {
                map.left = mons[pick].x;
                map.top = mons[pick].y;
                map.screenW = mons[pick].width;
                map.screenH = mons[pick].height;
            }
            if (mons) XRRFreeMonitors(mons);
        }
        map.scale = std::max({1.0, static_cast<double>(map.screenW) / kMaxWidth, static_cast<double>(map.screenH) / kMaxHeight});
        map.imageW = static_cast<int>(map.screenW / map.scale);
        map.imageH = static_cast<int>(map.screenH / map.scale);
        return map;
    }

    void ToScreen(int x, int y, int& sx, int& sy) {
        Map map = CurrentMap();
        sx = map.left + static_cast<int>(std::lround(x * map.scale));
        sy = map.top + static_cast<int>(std::lround(y * map.scale));
    }

    static guchar Channel(unsigned long pixel, unsigned long mask) {
        if (!mask) return 0;
        int shift = 0;
        while (!((mask >> shift) & 1)) shift++;
        unsigned long max = mask >> shift;
        return static_cast<guchar>(((pixel & mask) >> shift) * 255 / max);
    }

    // The mouse pointer, so the model sees where it is.
    void DrawPointer(std::vector<guchar>& rgb, const Map& map) {
        if (!m_xfixes) return;
        XFixesCursorImage* cur = XFixesGetCursorImage(m_dpy);
        if (!cur) return;
        int ox = cur->x - cur->xhot - map.left, oy = cur->y - cur->yhot - map.top;
        for (int y = 0; y < cur->height; y++) {
            for (int x = 0; x < cur->width; x++) {
                int px = ox + x, py = oy + y;
                if (px < 0 || py < 0 || px >= map.screenW || py >= map.screenH) continue;
                unsigned long argb = cur->pixels[y * cur->width + x];  // premultiplied
                unsigned a = (argb >> 24) & 0xff;
                if (!a) continue;
                guchar* d = &rgb[(static_cast<size_t>(py) * map.screenW + px) * 3];
                unsigned src[3] = {static_cast<unsigned>((argb >> 16) & 0xff), static_cast<unsigned>((argb >> 8) & 0xff),
                                   static_cast<unsigned>(argb & 0xff)};
                for (int c = 0; c < 3; c++) d[c] = static_cast<guchar>(std::min(255u, src[c] + d[c] * (255 - a) / 255));
            }
        }
        XFree(cur);
    }

    // --- keyboard ---

    // Presses and releases the key that makes `sym`, with Shift if it is on that
    // key's second level. A symbol no key makes (an accented or other letter
    // the layout lacks) is put on a spare key for the moment.
    bool TapKeySym(KeySym sym, KeySym lookup = 0) {
        if (!lookup) lookup = sym;
        KeyCode kc = XKeysymToKeycode(m_dpy, lookup);
        bool shift = false;
        if (kc) {
            if (XkbKeycodeToKeysym(m_dpy, kc, 0, 0) == lookup) shift = false;
            else if (XkbKeycodeToKeysym(m_dpy, kc, 0, 1) == lookup) shift = true;
            else kc = 0;  // on another level (AltGr) or group
        }
        if (!kc) {
            kc = ScratchKey(lookup);
            if (!kc) return false;
            shift = false;
        }
        KeyCode shiftKc = shift ? XKeysymToKeycode(m_dpy, XK_Shift_L) : 0;
        if (shiftKc) XTestFakeKeyEvent(m_dpy, shiftKc, True, CurrentTime);
        XTestFakeKeyEvent(m_dpy, kc, True, CurrentTime);
        XTestFakeKeyEvent(m_dpy, kc, False, CurrentTime);
        if (shiftKc) XTestFakeKeyEvent(m_dpy, shiftKc, False, CurrentTime);
        XSync(m_dpy, False);
        return true;
    }

    // A key with nothing on it, given `sym` until RestoreScratchKey. Apps take
    // a moment to read the new mapping before the key goes to them.
    KeyCode ScratchKey(KeySym sym) {
        if (!m_scratch) {
            int minKc, maxKc, per;
            XDisplayKeycodes(m_dpy, &minKc, &maxKc);
            KeySym* map = XGetKeyboardMapping(m_dpy, static_cast<KeyCode>(minKc), maxKc - minKc + 1, &per);
            if (!map) return 0;
            for (int kc = maxKc; kc >= minKc && !m_scratch; kc--) {
                bool empty = true;
                for (int i = 0; i < per; i++) empty = empty && map[(kc - minKc) * per + i] == NoSymbol;
                if (empty) m_scratch = static_cast<KeyCode>(kc);
            }
            XFree(map);
            if (!m_scratch) return 0;
        }
        if (m_scratchSym != sym) {
            KeySym syms[2] = {sym, sym};
            XChangeKeyboardMapping(m_dpy, m_scratch, 2, syms, 1);
            XSync(m_dpy, False);
            m_scratchSym = sym;
            SleepMs(40);
        }
        return m_scratch;
    }

    void RestoreScratchKey() {
        if (!m_scratch || !m_scratchSym || !m_dpy) return;
        SleepMs(40);  // the last key typed on it is read with its symbol
        KeySym none[2] = {NoSymbol, NoSymbol};
        XChangeKeyboardMapping(m_dpy, m_scratch, 2, none, 1);
        XSync(m_dpy, False);
        m_scratchSym = 0;
    }

    // --- accessibility ---

    // The accessible frame or dialog for a window: in the app with the window's
    // process id, the one with its title, else the active one, else the one
    // where the window is.
    Acc FrameFor(Window w) {
        int pid = Pid(w);
        if (!pid) return Acc();
        std::string title = Title(w);
        int wx = 0, wy = 0, ww = 0, wh = 0;
        bool geo = Geometry(w, wx, wy, ww, wh);
        bool isActive = ActiveWindow() == w;
        GError* err = nullptr;
        Acc desktop(atspi_get_desktop(0));
        if (!desktop) return Acc();
        int apps = atspi_accessible_get_child_count(desktop.p, &err);
        Clear(err);
        for (int i = 0; i < apps; i++) {
            Acc app(atspi_accessible_get_child_at_index(desktop.p, i, &err));
            Clear(err);
            if (!app) continue;
            int appPid = static_cast<int>(atspi_accessible_get_process_id(app.p, &err));
            Clear(err);
            if (appPid != pid) continue;
            int frames = atspi_accessible_get_child_count(app.p, &err);
            Clear(err);
            Acc byTitle, byActive, byPlace, first;
            for (int f = 0; f < frames && f < 200; f++) {
                Acc frame(atspi_accessible_get_child_at_index(app.p, f, &err));
                Clear(err);
                if (!frame) continue;
                std::string name = Take(atspi_accessible_get_name(frame.p, &err));
                Clear(err);
                AtspiStateSet* st = atspi_accessible_get_state_set(frame.p);
                bool active = HasState(st, ATSPI_STATE_ACTIVE), showing = HasState(st, ATSPI_STATE_SHOWING);
                if (st) g_object_unref(st);
                if (!byTitle && !title.empty() && name == title && showing) byTitle = frame;
                if (!byActive && active) byActive = frame;
                int fx, fy, fw, fh;
                if (!byPlace && geo && showing && Extents(frame.p, fx, fy, fw, fh) && std::abs(fx - wx) < 64 &&
                    std::abs(fy - wy) < 64 && std::abs(fw - ww) < 64 && std::abs(fh - wh) < 64) {
                    byPlace = frame;
                }
                if (!first && showing) first = frame;
            }
            if (byTitle) return byTitle;
            if (isActive && byActive) return byActive;
            if (byPlace) return byPlace;
            if (byActive) return byActive;
            if (first) return first;
        }
        return Acc();
    }

    void Walk(AtspiAccessible* el, int depth, int maxElements, std::string& out, int& more, int& visits) {
        if (++visits > kMaxVisits || depth > 40) return;
        GError* err = nullptr;
        AtspiStateSet* st = atspi_accessible_get_state_set(el);
        bool showing = HasState(st, ATSPI_STATE_SHOWING) || depth == 0;
        if (!showing) {
            if (st) g_object_unref(st);
            return;
        }
        AtspiRole role = atspi_accessible_get_role(el, &err);
        Clear(err);
        std::string name = OneLine(NameOf(el, role), 120);
        bool listed = false;
        if (depth > 0 && !(IsQuietContainer(role) && name.empty())) {
            if (static_cast<int>(m_elements.size()) >= maxElements) {
                more++;
            } else {
                m_elements.emplace_back(Acc(static_cast<AtspiAccessible*>(g_object_ref(el))));
                int id = static_cast<int>(m_elements.size());
                std::string line(static_cast<size_t>(std::min(depth, 12)) * 2, ' ');
                line += "[" + std::to_string(id) + "] " + RoleName(role, el);
                if (!name.empty()) line += " \"" + name + "\"";
                std::string value = OneLine(FieldValue(el, role, 240), 120);
                if (!value.empty() && value != name) line += " value \"" + value + "\"";
                if (HasState(st, ATSPI_STATE_CHECKED) || (role == ATSPI_ROLE_TOGGLE_BUTTON && HasState(st, ATSPI_STATE_PRESSED))) {
                    line += role == ATSPI_ROLE_TOGGLE_BUTTON ? ", pressed" : ", checked";
                }
                else if (HasState(st, ATSPI_STATE_INDETERMINATE)) line += ", partly checked";
                else if (HasState(st, ATSPI_STATE_CHECKABLE) || role == ATSPI_ROLE_CHECK_BOX ||
                         role == ATSPI_ROLE_TOGGLE_BUTTON || role == ATSPI_ROLE_CHECK_MENU_ITEM) {
                    line += role == ATSPI_ROLE_TOGGLE_BUTTON ? ", not pressed" : ", not checked";
                }
                if (HasState(st, ATSPI_STATE_EXPANDABLE)) line += HasState(st, ATSPI_STATE_EXPANDED) ? ", expanded" : ", collapsed";
                if (HasState(st, ATSPI_STATE_SELECTED)) line += ", selected";
                if (!HasState(st, ATSPI_STATE_SENSITIVE) && !HasState(st, ATSPI_STATE_ENABLED)) line += ", unavailable";
                if (HasState(st, ATSPI_STATE_FOCUSED)) line += ", FOCUSED";
                int x, y, w, h;
                if (Extents(el, x, y, w, h)) {
                    int cx = static_cast<int>(std::lround((x + w / 2.0 - m_map.left) / m_map.scale));
                    int cy = static_cast<int>(std::lround((y + h / 2.0 - m_map.top) / m_map.scale));
                    line += " at " + std::to_string(cx) + "," + std::to_string(cy);
                }
                out += line + "\n";
                listed = true;
            }
        }
        if (st) g_object_unref(st);
        // Past the limit, only counting is left, which is not worth a visit to every row.
        if (static_cast<int>(m_elements.size()) >= maxElements && more > 0) return;
        int n = atspi_accessible_get_child_count(el, &err);
        Clear(err);
        for (int i = 0; i < n && i < kMaxChildren; i++) {
            Acc child(atspi_accessible_get_child_at_index(el, i, &err));
            Clear(err);
            if (child) Walk(child.p, listed || depth == 0 ? depth + 1 : depth, maxElements, out, more, visits);
            if (visits > kMaxVisits) return;
        }
        if (n > kMaxChildren) more += n - kMaxChildren;
    }

    Acc FindFocused(AtspiAccessible* el, int depth, int& visits) {
        if (++visits > kMaxVisits || depth > 40) return Acc();
        AtspiStateSet* st = atspi_accessible_get_state_set(el);
        bool showing = HasState(st, ATSPI_STATE_SHOWING) || depth == 0;
        bool focused = HasState(st, ATSPI_STATE_FOCUSED);
        if (st) g_object_unref(st);
        if (!showing) return Acc();
        if (focused && depth > 0) return Acc(static_cast<AtspiAccessible*>(g_object_ref(el)));
        GError* err = nullptr;
        int n = atspi_accessible_get_child_count(el, &err);
        Clear(err);
        for (int i = 0; i < n && i < kMaxChildren; i++) {
            Acc child(atspi_accessible_get_child_at_index(el, i, &err));
            Clear(err);
            if (!child) continue;
            Acc found = FindFocused(child.p, depth + 1, visits);
            if (found) return found;
        }
        return Acc();
    }

    bool GrabFocus(AtspiAccessible* el) {
        AtspiComponent* comp = atspi_accessible_get_component_iface(el);
        if (!comp) return false;
        GError* err = nullptr;
        bool ok = atspi_component_grab_focus(comp, &err) && !err;
        Clear(err);
        g_object_unref(comp);
        return ok;
    }

    AtspiAccessible* Element(int id) {
        if (id < 1 || id > static_cast<int>(m_elements.size())) return nullptr;
        return m_elements[id - 1].p;
    }

    bool Done(const std::string& what, std::string& message) {
        SleepMs(250);  // let the app react before describing where focus is
        message = what;
        return true;
    }

    bool NoElement(int id, std::string& message) {
        message = "There is no element [" + std::to_string(id) + "]. Call look to see the current elements.";
        return false;
    }

    Display* m_dpy = nullptr;
    Window m_root = 0;
    Window m_target = 0;
    bool m_followForeground = true;
    bool m_xtest = false, m_xfixes = false, m_xrandr = false, m_atspi = false;
    KeyCode m_scratch = 0;
    KeySym m_scratchSym = 0;
    std::vector<Acc> m_elements;
    Map m_map;
};

// ---------------------------------------------------------------------------
// The stop key
// ---------------------------------------------------------------------------

// Ctrl+Alt+Escape for the whole desktop, on an XCB connection and thread of
// its own. XCB reports a refused grab as a reply, not through Xlib's
// process-wide error handler. The key is grabbed when it can be; when another
// program holds it (MATE's window manager does, for "cycle panels"), FastChat
// watches XInput2's raw key events instead, which come whatever holds a grab.
struct StopKey {
    std::mutex lock;
    std::thread thread;
    int wake[2] = {-1, -1};
};

StopKey& TheStopKey() {
    static StopKey key;
    return key;
}

// Every keycode that makes one of these symbols.
std::vector<xcb_keycode_t> KeycodesFor(xcb_connection_t* c, std::initializer_list<uint32_t> wanted) {
    std::vector<xcb_keycode_t> found;
    const xcb_setup_t* setup = xcb_get_setup(c);
    xcb_keycode_t first = setup->min_keycode;
    uint8_t count = static_cast<uint8_t>(setup->max_keycode - setup->min_keycode + 1);
    xcb_get_keyboard_mapping_reply_t* reply =
        xcb_get_keyboard_mapping_reply(c, xcb_get_keyboard_mapping(c, first, count), nullptr);
    if (!reply) return found;
    xcb_keysym_t* syms = xcb_get_keyboard_mapping_keysyms(reply);
    int per = reply->keysyms_per_keycode;
    for (int i = 0; i < count; i++) {
        for (int j = 0; j < per; j++) {
            if (std::find(wanted.begin(), wanted.end(), syms[i * per + j]) != wanted.end()) {
                found.push_back(static_cast<xcb_keycode_t>(first + i));
                break;
            }
        }
    }
    free(reply);
    return found;
}

bool GrabKey(xcb_connection_t* c, xcb_window_t root, xcb_keycode_t key) {
    // With and without Caps Lock and Num Lock (Mod2) on.
    const uint16_t base = XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1;
    const uint16_t extras[] = {0, XCB_MOD_MASK_LOCK, XCB_MOD_MASK_2, XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2};
    bool grabbed = true;
    for (uint16_t extra : extras) {
        xcb_generic_error_t* e = xcb_request_check(
            c, xcb_grab_key_checked(c, 1, root, base | extra, key, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC));
        if (e) {
            grabbed = false;
            free(e);
        }
    }
    return grabbed;
}

// Raw key presses and releases from every keyboard, sent to this connection.
bool WatchRawKeys(xcb_connection_t* c, xcb_window_t root, uint8_t& opcode) {
    const xcb_query_extension_reply_t* ext = xcb_get_extension_data(c, &xcb_input_id);
    if (!ext || !ext->present) return false;
    opcode = ext->major_opcode;
    xcb_input_xi_query_version_reply_t* v =
        xcb_input_xi_query_version_reply(c, xcb_input_xi_query_version(c, 2, 2), nullptr);
    bool recent = v && (v->major_version > 2 || (v->major_version == 2 && v->minor_version >= 1));
    free(v);
    if (!recent) return false;  // before XInput 2.1, raw events stopped at grabs
    struct {
        xcb_input_event_mask_t head;
        uint32_t mask;
    } m{};
    m.head.deviceid = XCB_INPUT_DEVICE_ALL_MASTER;
    m.head.mask_len = 1;
    m.mask = XCB_INPUT_XI_EVENT_MASK_RAW_KEY_PRESS | XCB_INPUT_XI_EVENT_MASK_RAW_KEY_RELEASE;
    xcb_generic_error_t* e = xcb_request_check(c, xcb_input_xi_select_events_checked(c, root, 1, &m.head));
    if (e) {
        free(e);
        return false;
    }
    return true;
}

}  // namespace

std::unique_ptr<Desktop> Desktop::Create() {
    return std::make_unique<LinuxDesktop>();
}

bool GrabStopKey(std::function<void()> pressed) {
    {
        // One conversation at a time holds the key, as with Windows' hotkeys;
        // another one stops with Escape in its own window.
        StopKey& key = TheStopKey();
        std::lock_guard<std::mutex> held(key.lock);
        if (key.thread.joinable()) return false;
    }
    xcb_connection_t* c = xcb_connect(nullptr, nullptr);
    if (!c || xcb_connection_has_error(c)) {
        if (c) xcb_disconnect(c);
        return false;
    }
    xcb_window_t root = xcb_setup_roots_iterator(xcb_get_setup(c)).data->root;
    std::vector<xcb_keycode_t> escape = KeycodesFor(c, {XK_Escape});
    std::vector<xcb_keycode_t> ctrl = KeycodesFor(c, {XK_Control_L, XK_Control_R});
    std::vector<xcb_keycode_t> alt = KeycodesFor(c, {XK_Alt_L, XK_Alt_R, XK_Meta_L, XK_Meta_R});
    if (escape.empty()) {
        xcb_disconnect(c);
        return false;
    }
    bool grabbed = GrabKey(c, root, escape[0]);
    uint8_t xinput = 0;
    if (!grabbed) {
        // Let go of any of the four combinations that did grab, then listen instead.
        xcb_ungrab_key(c, escape[0], root, XCB_MOD_MASK_ANY);
        if (!WatchRawKeys(c, root, xinput)) {
            xcb_disconnect(c);
            return false;
        }
    }
    StopKey& key = TheStopKey();
    std::lock_guard<std::mutex> held(key.lock);
    if (pipe(key.wake) != 0) {
        xcb_disconnect(c);
        return false;
    }
    int wakeFd = key.wake[0];
    key.thread = std::thread([c, wakeFd, grabbed, xinput, escape, ctrl, alt, pressed = std::move(pressed)]() {
        int xfd = xcb_get_file_descriptor(c);
        std::set<xcb_keycode_t> down;  // raw events carry no modifier state
        auto held = [&](const std::vector<xcb_keycode_t>& keys) {
            return std::any_of(keys.begin(), keys.end(), [&](xcb_keycode_t k) { return down.count(k) > 0; });
        };
        auto isEscape = [&](xcb_keycode_t k) { return std::find(escape.begin(), escape.end(), k) != escape.end(); };
        for (;;) {
            while (xcb_generic_event_t* ev = xcb_poll_for_event(c)) {
                uint8_t type = ev->response_type & 0x7f;
                bool fire = false;
                if (grabbed) {
                    fire = type == XCB_KEY_PRESS;
                } else if (type == XCB_GE_GENERIC) {
                    auto* ge = reinterpret_cast<xcb_ge_generic_event_t*>(ev);
                    if (ge->extension == xinput && ge->event_type == XCB_INPUT_RAW_KEY_PRESS) {
                        auto k = static_cast<xcb_keycode_t>(reinterpret_cast<xcb_input_raw_key_press_event_t*>(ev)->detail);
                        down.insert(k);
                        fire = isEscape(k) && held(ctrl) && held(alt);
                    } else if (ge->extension == xinput && ge->event_type == XCB_INPUT_RAW_KEY_RELEASE) {
                        down.erase(static_cast<xcb_keycode_t>(reinterpret_cast<xcb_input_raw_key_release_event_t*>(ev)->detail));
                    }
                }
                free(ev);
                if (fire && pressed) pressed();
            }
            if (xcb_connection_has_error(c)) break;
            pollfd fds[2] = {{xfd, POLLIN, 0}, {wakeFd, POLLIN, 0}};
            if (poll(fds, 2, -1) < 0 && errno != EINTR) break;
            if (fds[1].revents) break;
        }
        xcb_disconnect(c);  // which lets any grab go
    });
    return true;
}

void ReleaseStopKey() {
    StopKey& key = TheStopKey();
    std::lock_guard<std::mutex> held(key.lock);
    if (!key.thread.joinable()) return;
    char b = 1;
    ssize_t w = write(key.wake[1], &b, 1);
    (void)w;
    key.thread.join();
    close(key.wake[0]);
    close(key.wake[1]);
    key.wake[0] = key.wake[1] = -1;
}
