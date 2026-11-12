# FastChat

An accessible desktop app for chatting and coding with AI models from many
providers, written in C++ with wxWidgets.

## What it does

- **Conversations list** (the main window). New conversation (Ctrl+N) asks for a
  name (leave it empty to have one generated after the first reply), the type -
  chat or code - and for code, a folder.
- **Conversation windows**: the messages, a multiline box to write in (Enter
  sends, Shift+Enter starts a new line; can be switched to Ctrl+Enter), an
  Actions button, and a status bar with the model, context used of the model's
  window, the last response's tokens and cost, the conversation's total, and the
  provider's credits where it reports them.
- **Code conversations** give the model tools inside the folder: list, read,
  find, search, write and edit files, and run commands (asks first by default;
  can be set to run freely or never).
- **Providers** (Settings > Providers): OpenAI, Anthropic, Google Gemini,
  OpenRouter, Ollama Cloud, Ollama on your own machine, DeepSeek, Groq, Mistral,
  xAI, any OpenAI-compatible server, and the coding agents **Claude Code** and
  **Codex CLI**, run on this computer with your own login. Each lists its models with context
  length and prices where the service gives them, and reports what it can about
  the account:
  - OpenRouter: credits remaining, purchased and used; the key's limit and its
    usage today, this week, this month; free-tier status.
  - OpenAI and Anthropic: this month's spend and token counts, with an optional
    admin key.
  - DeepSeek: balance.
  - Ollama: version and the models loaded in memory.
  - Claude Code: your login and plan, and plan usage (for example "50% used,
    seven-day window") as it reports it, shown in the status bar.
  - Codex: your login status.
  - Every provider: the rate-limit headers of the last response.
- **Coding agents** (Claude Code, Codex) run their own tools in code
  conversations. Claude Code's approval requests become FastChat's Allow / Don't
  allow dialog. FastChat starts it in its default permission mode while asking
  is on, even if your own settings use auto mode; your CLAUDE.md, MCP servers
  and allow list still apply. Codex can't ask in headless mode, so it gets file
  edits inside the folder only, with no network access. Each reply keeps the
  agent's session id, so the next message resumes that session.
- **Speech and sounds**: every event (message sent, response started, thinking,
  complete, error, tool call, tool result, command approval, and more) can play
  a sound (a file, or a built-in tone) and speak a text template such as
  `{model} is responding`. Responses can be spoken when complete or sentence by
  sentence as they arrive. Tool calls and commands are only spoken with "Speak
  tool calls and commands" on (Settings > Speech, or per conversation). Speech goes through **Prism** (statically linked:
  NVDA, JAWS, ZDSR, OneCore, SAPI, UIA... or automatic) or straight to **SAPI 5**
  with its own voice, rate and volume.
- **Per-conversation settings** (Ctrl+Shift+Comma) override any of it: model,
  system prompt, reasoning effort, temperature, response length, command
  approval, speech, voice, rate, and each event's sound and speech.

## Keys

Main window:

| Key | Does |
|---|---|
| Ctrl+N | New conversation |
| Enter | Open the conversation |
| F2 | Rename |
| Delete | Delete |
| Ctrl+P | Pin or unpin |
| Ctrl+E | Export |
| Ctrl+F | Search |
| Ctrl+Comma | Settings |
| Ctrl+Shift+A | Account and credits |
| Shift+F10 / Applications | Context menu |

Conversation window:

| Key | Does |
|---|---|
| Enter | Send (Shift+Enter: new line; Ctrl+Enter always sends) |
| Escape | Stop the response, or close the window when nothing is running |
| F6 | Move between the messages and the message box |
| Alt+M / Alt+W | The messages / the message box |
| Alt+A | Actions |
| Ctrl+M | Change model |
| Ctrl+Shift+Comma | Conversation settings |
| Ctrl+R | Regenerate the last response |
| Ctrl+Shift+R | Speak the last response |
| Ctrl+Shift+S | Stop speaking |
| Ctrl+Shift+C | Copy the last response |
| Ctrl+I | Conversation information (tokens, cost, model, limits) |
| Ctrl+Shift+A | Account and credits |
| F2 | Rename |
| Ctrl+E | Export |
| Ctrl+W | Close |

In the messages list, the model's text and each command it runs are separate
rows: "Command: cmake --build build, exit code 0".

On a message: Enter views it, Ctrl+C copies it, Delete deletes it (a tool call
goes with its results), Shift+Delete deletes it and everything after, and
Shift+F10 opens a menu with view, copy, copy code blocks, speak, show thinking,
details, edit and resend, regenerate from here, and delete.

## Building

Windows, with Visual Studio 2022 (C++ workload), CMake 3.24+ and Git:

```bat
build.bat
```

CMake fetches wxWidgets, Prism, nlohmann/json, SQLite and miniaudio into
`build\` on the first run, which takes a while. To reuse a wxWidgets checkout you
already have (FastPlay's, for instance), configure once with
`-DFETCHCONTENT_SOURCE_DIR_WXWIDGETS=<its source folder>`.

macOS and Linux build with the same CMakeLists.txt (libcurl for HTTP); they have
not been tried yet.

## Tests

```bat
build.bat tests
python tests\mock_server.py 8765
set FASTCHAT_MOCK=http://127.0.0.1:8765
build\Release\fastchat_tests.exe
```

`mock_server.py` imitates the OpenAI, Anthropic, Gemini and Ollama streaming
APIs, and `fake_claude.py` / `fake_codex.py` imitate the two coding agents, so
the providers, tool loops, approvals and account reports are tested without
keys. Set `FASTCHAT_REAL_CLAUDE=1` to also run a short real Claude Code session
(a chat, an approved file write and a resume, using Haiku; it costs a fraction
of a cent).

## Data

Settings (`settings.json`, API keys encrypted for your Windows account with
DPAPI) and conversations (`fastchat.db`) live in `%APPDATA%\FastChat`, or beside
FastChat.exe when a file named `portable.txt` is there. `FASTCHAT_DATA_DIR`
overrides both. Unexpected errors are written to `fastchat.log` there.

## Layout

```
src/core/            everything but the windows
  providers/         one file per API style: OpenAI-compatible, Anthropic, Gemini, Ollama
  platform/          HTTP, paths, secrets, processes, SAPI - per system
  chat_engine.*      runs a turn on a worker thread, with the tool loop
  code_tools.*       the tools code conversations give the model
  notify.*, speech.*, sound.*, events.*   speech and sound events
src/ui/              the wxWidgets windows and dialogs
tests/               core tests and the mock server
```
