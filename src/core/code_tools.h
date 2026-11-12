#pragma once

// The tools a code conversation gives the model: reading, searching and editing
// files inside the conversation's folder, and running commands there (with the
// user's approval unless they allow it outright).

#include "core/providers/provider.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

struct ToolContext {
    std::string root;                    // the project folder; tools cannot leave it
    const std::atomic<bool>* cancel = nullptr;
    std::string commandApproval = "ask"; // "ask", "auto", "deny"
    unsigned commandTimeoutMs = 120000;
    // Asked (on the worker thread, blocking) before running a command when
    // approval is "ask". Returns true to run it.
    std::function<bool(const std::string& command)> approveCommand;
};

struct ToolOutcome {
    std::string output;   // what goes back to the model
    bool error = false;
    std::string summary;  // one line for the conversation list and speech
    std::vector<ImageData> images;  // screenshots (computer use)
};

std::vector<ToolDef> CodeToolDefinitions();
ToolOutcome RunTool(const ToolCall& call, const ToolContext& context);

// "Read src/main.cpp", "Run: cmake --build build" - for the list and speech.
std::string DescribeToolCall(const ToolCall& call);

// The built-in instructions for code conversations.
std::string CodeSystemPrompt(const std::string& folder);
