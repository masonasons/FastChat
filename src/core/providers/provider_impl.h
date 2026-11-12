#pragma once

// The provider implementations, each in its own file.

#include "core/providers/provider.h"

std::unique_ptr<Provider> CreateOpenAIProvider(const ProviderConfig& config);
std::unique_ptr<Provider> CreateAnthropicProvider(const ProviderConfig& config);
std::unique_ptr<Provider> CreateGeminiProvider(const ProviderConfig& config);
std::unique_ptr<Provider> CreateOllamaProvider(const ProviderConfig& config);
std::unique_ptr<Provider> CreateClaudeCodeProvider(const ProviderConfig& config);
std::unique_ptr<Provider> CreateCodexProvider(const ProviderConfig& config);

// For the coding agents:
// What to send as the user's message: the new message alone when resuming a
// session, otherwise the conversation so far written out, then the new message.
std::string AgentPrompt(const AgentTurn& turn);
// Where a chat (no project folder) runs: a folder of FastChat's own.
std::string AgentChatFolder();
// Run a program to the end and collect its output.
bool CaptureProgram(const std::vector<std::string>& argv, std::string& output, std::string& error, unsigned timeoutMs);

// Claude models by alias and id, for providers that cannot list them.
std::vector<ModelInfo> KnownClaudeModels();
