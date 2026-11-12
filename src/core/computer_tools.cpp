#include "core/computer_tools.h"
#include "core/util.h"

#include <chrono>
#include <thread>

namespace {

json Schema(std::initializer_list<std::pair<const char*, json>> props, std::initializer_list<const char*> required) {
    json p = json::object();
    for (const auto& [name, schema] : props) p[name] = schema;
    json req = json::array();
    for (const char* r : required) req.push_back(r);
    return {{"type", "object"}, {"properties", p}, {"required", req}};
}

json Type(const char* type, const char* description) {
    return {{"type", type}, {"description", description}};
}

std::string Str(const json& args, const char* key) {
    auto it = args.find(key);
    if (it == args.end() || it->is_null()) return std::string();
    return it->is_string() ? it->get<std::string>() : JsonDump(*it);
}

bool Has(const json& args, const char* key) {
    auto it = args.find(key);
    return it != args.end() && !it->is_null();
}

int Int(const json& args, const char* key, int def) {
    auto it = args.find(key);
    if (it == args.end() || it->is_null()) return def;
    if (it->is_number()) return static_cast<int>(it->get<double>());
    if (it->is_string()) {
        try {
            return std::stoi(it->get<std::string>());
        } catch (...) {
        }
    }
    return def;
}

ToolOutcome Fail(const std::string& message) {
    return {message, true, message};
}

// Where to act: an element's centre, or coordinates.
bool Point(const json& args, ComputerContext& ctx, const char* elementKey, const char* xKey, const char* yKey, int& x, int& y,
           std::string& what, std::string& error) {
    if (Has(args, elementKey)) {
        int id = Int(args, elementKey, 0);
        what = ctx.desktop->ElementDescription(id);
        if (what.empty() || !ctx.desktop->ElementPoint(id, x, y)) {
            error = "There is no element [" + std::to_string(id) + "] on screen. Call look again.";
            return false;
        }
        return true;
    }
    if (Has(args, xKey) && Has(args, yKey)) {
        x = Int(args, xKey, 0);
        y = Int(args, yKey, 0);
        what = "the point " + std::to_string(x) + ", " + std::to_string(y);
        return true;
    }
    error = std::string("Give an element number (") + elementKey + ") or x and y.";
    return false;
}

// The question shown before an action.
bool Allowed(ComputerContext& ctx, const std::string& description) {
    if (ctx.approval == "deny") return false;
    if (ctx.approval == "auto") return true;
    return ctx.approve && ctx.approve(description);
}

ToolOutcome Acted(bool ok, const std::string& message, ComputerContext& ctx) {
    if (!ok) return Fail(message);
    // Say where focus ended up, as a screen reader would after the action.
    std::string focus = ctx.desktop->FocusedDescription();
    return {message + ".\n" + focus, false, message};
}

}  // namespace

bool IsComputerTool(const std::string& name) {
    static const char* kNames[] = {"look", "screenshot", "list_windows", "focus_window", "click", "double_click",
                                   "right_click", "drag", "scroll", "move_mouse", "type_text", "press_keys",
                                   "invoke", "set_text", "focus", "wait"};
    for (const char* n : kNames) {
        if (name == n) return true;
    }
    return false;
}

std::vector<ToolDef> ComputerToolDefinitions(bool vision) {
    json element = Type("integer", "An element's number from the last look");
    json x = Type("integer", "X in screenshot pixels");
    json y = Type("integer", "Y in screenshot pixels");
    std::vector<ToolDef> tools = {
        {"look",
         vision ? "See the window you are working in: its accessibility tree (every control, numbered, with its "
                  "position) and a screenshot of its screen. Call this first and whenever you need to see the result "
                  "of what you did."
                : "See the window you are working in: its accessibility tree (every control, numbered, with its "
                  "position). Call this first and whenever you need to see the result of what you did.",
         Schema({}, {})},
        {"list_windows", "List the open windows, numbered.", Schema({}, {})},
        {"focus_window", "Switch to another window, by its number from list_windows or part of its title.",
         Schema({{"window", Type("string", "The window's number or part of its title")}}, {"window"})},
        {"invoke",
         "Press a button, toggle a check box, select an item, or expand/collapse it, through accessibility (no "
         "mouse). The most reliable way to activate a control.",
         Schema({{"element", element}}, {"element"})},
        {"set_text", "Replace the text in an edit field.",
         Schema({{"element", element}, {"text", Type("string", "The new text")}}, {"element", "text"})},
        {"focus", "Move keyboard focus to an element.", Schema({{"element", element}}, {"element"})},
        {"type_text", "Type text into whatever has keyboard focus (\\n presses Enter).",
         Schema({{"text", Type("string", "The text to type")}}, {"text"})},
        {"press_keys",
#ifdef __APPLE__
         "Press keys: a combination like cmd+s, cmd+q, shift+tab, cmd+shift+g, or several in turn separated by "
         "spaces (\"down down enter\"). Names: cmd ctrl option (or alt) shift fn enter tab escape space backspace "
         "delete (forward delete) home end pageup pagedown up down left right f1-f20, or single characters.",
#else
         "Press keys: a combination like ctrl+s, alt+f4, shift+tab, win+r, or several in turn separated by spaces "
         "(\"down down enter\"). Names: ctrl alt shift win enter tab escape space backspace delete home end pageup "
         "pagedown up down left right f1-f24 apps, or single characters.",
#endif
         Schema({{"keys", Type("string", "The keys")}}, {"keys"})},
        {"click", "Click an element (by number) or a point.",
         Schema({{"element", element}, {"x", x}, {"y", y},
                 {"button", {{"type", "string"}, {"enum", {"left", "right", "middle"}}, {"description", "Default left"}}},
                 {"double", Type("boolean", "Double-click")}},
                {})},
        {"drag", "Drag with the left button from one element or point to another.",
         Schema({{"from_element", element}, {"from_x", x}, {"from_y", y}, {"to_element", element}, {"to_x", x}, {"to_y", y}},
                {})},
        {"scroll", "Scroll the mouse wheel over an element or point.",
         Schema({{"element", element}, {"x", x}, {"y", y},
                 {"direction", {{"type", "string"}, {"enum", {"up", "down", "left", "right"}}}},
                 {"amount", Type("integer", "Notches, default 3")}},
                {"direction"})},
        {"move_mouse", "Move the mouse over an element or point (to show a tooltip or menu).",
         Schema({{"element", element}, {"x", x}, {"y", y}}, {})},
        {"wait", "Wait for something to finish loading.", Schema({{"seconds", Type("number", "1 to 30")}}, {"seconds"})},
    };
    if (vision) {
        tools.insert(tools.begin() + 1, {"screenshot", "Take a screenshot only, without the accessibility tree.", Schema({}, {})});
    }
    return tools;
}

ToolOutcome RunComputerTool(const ToolCall& call, ComputerContext& ctx) {
    if (!ctx.desktop) return Fail("Computer use is not available on this system yet (Windows and the Mac only).");
    json args = ArgumentsToObject(call.arguments);
    const std::string& n = call.name;
    Desktop& d = *ctx.desktop;
    std::string message;

    try {
        if (n == "look" || n == "screenshot") {
            ToolOutcome out;
            std::string window = d.TargetDescription();
            if (n == "look") {
                out.output = d.Tree(300) + "\n" + d.FocusedDescription() + "\n";
                out.summary = "Looked at " + (window.empty() ? std::string("the screen") : window);
            } else {
                out.summary = "Took a screenshot";
            }
            if (ctx.vision) {
                Screenshot shot;
                std::string error;
                if (d.Capture(shot, error)) {
                    out.images.push_back({shot.mediaType, shot.base64});
                    out.output += "Screenshot attached (" + std::to_string(shot.width) + " x " + std::to_string(shot.height) +
                                  " pixels; coordinates are in these pixels).";
                } else {
                    out.output += "No screenshot: " + error;
                }
            }
            return out;
        }
        if (n == "list_windows") return {d.ListWindows(), false, "Listed the windows"};
        if (n == "wait") {
            int seconds = std::clamp(Int(args, "seconds", 1), 1, 30);
            for (int i = 0; i < seconds * 10; i++) {
                if (ctx.cancel && ctx.cancel->load()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            return {"Waited " + std::to_string(seconds) + " seconds.\n" + d.FocusedDescription(), false,
                    "Waited " + std::to_string(seconds) + " seconds"};
        }

        // Actions: each asks first.
        std::string description;
        std::function<bool()> act;
        if (n == "focus_window") {
            std::string which = Str(args, "window");
            description = "Switch to window " + which;
            act = [&]() { return d.FocusWindow(which, message); };
        } else if (n == "invoke" || n == "focus" || n == "set_text") {
            int id = Int(args, "element", 0);
            std::string what = d.ElementDescription(id);
            if (what.empty()) return Fail("There is no element [" + std::to_string(id) + "]. Call look again.");
            if (n == "invoke") {
                description = "Press " + what;
                act = [&, id]() { return d.Invoke(id, message); };
            } else if (n == "focus") {
                description = "Focus " + what;
                act = [&, id]() { return d.FocusElement(id, message); };
            } else {
                std::string text = Str(args, "text");
                description = "Set " + what + " to \"" + OneLine(text, 100) + "\"";
                act = [&, id, text]() { return d.SetText(id, text, message); };
            }
        } else if (n == "type_text") {
            std::string text = Str(args, "text");
            description = "Type \"" + OneLine(text, 200) + "\" in " + d.TargetDescription();
            act = [&, text]() { return d.TypeText(text, message); };
        } else if (n == "press_keys") {
            std::string keys = Str(args, "keys");
            description = "Press " + keys + " in " + d.TargetDescription();
            act = [&, keys]() { return d.PressKeys(keys, message); };
        } else if (n == "click" || n == "double_click" || n == "right_click") {
            int x = 0, y = 0;
            std::string what, error;
            if (!Point(args, ctx, "element", "x", "y", x, y, what, error)) return Fail(error);
            std::string button = n == "right_click" ? "right" : Str(args, "button");
            if (button.empty()) button = "left";
            int count = (n == "double_click" || args.value("double", false)) ? 2 : 1;
            description = std::string(count == 2 ? "Double-click " : button == "right" ? "Right-click " : "Click ") + what;
            act = [&, x, y, button, count]() { return d.Click(x, y, button, count, message); };
        } else if (n == "drag") {
            int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
            std::string from, to, error;
            if (!Point(args, ctx, "from_element", "from_x", "from_y", x1, y1, from, error)) return Fail(error);
            if (!Point(args, ctx, "to_element", "to_x", "to_y", x2, y2, to, error)) return Fail(error);
            description = "Drag from " + from + " to " + to;
            act = [&, x1, y1, x2, y2]() { return d.Drag(x1, y1, x2, y2, message); };
        } else if (n == "scroll") {
            int x = 0, y = 0;
            std::string what, error;
            if (!Point(args, ctx, "element", "x", "y", x, y, what, error)) return Fail(error);
            std::string direction = Str(args, "direction");
            int amount = Int(args, "amount", 3);
            description = "Scroll " + direction + " over " + what;
            act = [&, x, y, direction, amount]() { return d.Scroll(x, y, direction, amount, message); };
        } else if (n == "move_mouse") {
            int x = 0, y = 0;
            std::string what, error;
            if (!Point(args, ctx, "element", "x", "y", x, y, what, error)) return Fail(error);
            description = "Move the mouse over " + what;
            act = [&, x, y]() { return d.MoveMouse(x, y, message); };
        } else {
            return Fail("There is no tool named " + n + ".");
        }
        if (!Allowed(ctx, description)) {
            return Fail(ctx.approval == "deny" ? "Actions are turned off for this conversation."
                                               : "The user declined: " + description + ". Ask them what to do instead.");
        }
        return Acted(act(), message, ctx);
    } catch (const std::exception& e) {
        return Fail(std::string("The tool failed: ") + e.what());
    }
}

std::string ComputerSystemPrompt(bool vision, const std::string& screen) {
#ifdef __APPLE__
    const char* system = "Mac";
#else
    const char* system = "Windows computer";
#endif
    std::string prompt =
        std::string("You operate the user's ") + system +
        " for them through tools. The user may be using a screen reader, so "
        "work the way one does: rely on the accessibility tree from look, act on elements by number (invoke, "
        "set_text, focus) and with keyboard shortcuts before falling back on mouse coordinates.\n\n"
        "How to work:\n"
        "- Start with look to see the window you are in; use list_windows and focus_window to change windows.\n"
        "- Every action reports where keyboard focus ended up. Call look again when you need to see more, and check "
        "that each step did what you meant before the next.\n"
        "- Element numbers change after each look; use the latest ones.\n"
        "- The user approves each action, so describe briefly what you are about to do and why.\n"
        "- Ask the user before anything that cannot be undone: deleting, sending, buying, changing settings.\n"
        "- When the task is done, say so in a sentence or two.\n";
#ifdef __APPLE__
    prompt +=
        "- This is macOS: shortcuts use cmd (cmd+s saves, cmd+w closes a window, cmd+q quits, cmd+tab switches apps), "
        "and an app's menus are in the menu bar at the top of the screen, listed after the window in look; invoke a "
        "menu bar item to open its menu, then look again to see its items.\n";
#endif
    if (vision) {
        prompt += "- Screenshots are " + screen +
                  " pixels; coordinates you give (x, y) are in those pixels. Prefer element numbers when an element is "
                  "in the tree.\n";
    } else {
        prompt += "- You cannot see screenshots; use the accessibility tree and element numbers.\n";
    }
    return prompt;
}
