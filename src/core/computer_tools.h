#pragma once

// The tools a computer use conversation gives the model: looking at the screen
// (accessibility tree, screenshots) and acting on it (keyboard, mouse,
// elements). Every action asks first unless the conversation allows them.

#include "core/code_tools.h"
#include "core/desktop.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

struct ComputerContext {
    Desktop* desktop = nullptr;
    bool vision = true;                 // the model can see screenshots
    std::string approval = "ask";       // "ask", "auto", "deny"
    // Asked before each action when approval is "ask". Blocks; true to go ahead.
    std::function<bool(const std::string& description)> approve;
    const std::atomic<bool>* cancel = nullptr;
};

bool IsComputerTool(const std::string& name);
std::vector<ToolDef> ComputerToolDefinitions(bool vision);
// Screenshots come back in ToolOutcome::images.
ToolOutcome RunComputerTool(const ToolCall& call, ComputerContext& context);
std::string ComputerSystemPrompt(bool vision, const std::string& screen);
