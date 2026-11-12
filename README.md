# FastChat

An accessible desktop app for chatting and coding with AI models from many
providers, written in C++ with wxWidgets.

## What it does

- **Conversations list** (the main window). New conversation (Ctrl+N) asks for a
  name (leave it empty to have one generated after the first reply), the type -
  chat, code or computer use - and for code, a folder.
- **Conversation windows**: the messages, a multiline box to write in (Enter
  sends, Shift+Enter starts a new line; can be switched to Ctrl+Enter), an
  Actions button, and a status bar with the model, context used of the model's
  window, the last response's tokens and cost, the conversation's total, and the
  provider's credits where it reports them.
- **Send while it works**: a message sent while the model is busy is queued
  ("Queued: ..." in the list) and goes to it at its next step, between tool
  calls or right after it answers. Delete on a queued row takes it back; if you
  stop the response, queued messages return to the message box. Claude Code
  takes them while it works; Codex gets them as a follow-up turn.
- **Activity** (Ctrl+T, or Actions): what is running alongside the
  conversation and what has finished - Claude Code's subagents (their prompt,
  every step they take, tokens and tool uses, and their summary), background
  shells (their command and live output), and FastChat's own tools. It updates
  every second without moving you while you read the details. A subagent's
  current step also shows in the waiting row, its approval requests name it
  ("Agent 'count files' wants to: Run: dir /b"), and agents and background
  shells starting and finishing are events with their own sounds and speech.
- **Compaction**: Ctrl+K (or Actions) summarizes the conversation so far; the
  summary replaces the older messages in what is sent, and the list then starts
  at the summary, as the model's context does (the older messages stay in the
  Full history buffer). It also happens on its own when the context passes a
  share of the model's window (80% by default, Settings > General). Claude Code
  uses its own /compact; its compactions, automatic ones too, show its summary
  in the list (Enter reads all of it).
- **Errors explain themselves**: each one becomes a row in the conversation
  ("Error: ..."); Enter on it shows the provider, address, HTTP status, request
  id and what the server sent. Rate limits, server errors and dropped
  connections are tried again automatically (twice) before giving up. Failures
  are also written to fastchat.log.
- **Code conversations** give the model tools inside the folder: list, read,
  find, search, write and edit files, and run commands (asks first by default;
  can be set to run freely or never).
- **Computer use conversations** (Windows and the Mac) let a model operate this computer.
  It sees the window in front through its accessibility tree (every control,
  numbered, the way a screen reader reads it) and, for models that take
  images, a screenshot. It acts on controls directly (press, set text, focus)
  or with the keyboard and mouse (keys, typing, clicks, drags, scrolling).
  FastChat asks before every action (Allow / Don't allow, with an option to
  allow the rest of the conversation), never acts on its own windows, and sends
  nothing unless the target app is really in front. Ctrl+Alt+Escape
  (Control+Option+Escape on the Mac) stops it from any window. Coding agents (Claude Code, Codex) can't drive it; use a
  model from an API provider.
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
  complete, error, tool call, tool result, tool failed, command approval, and
  more) can play a sound (a file, a sound pack's, or a built-in tone) and speak
  a text template such as
  `{model} is responding`. Responses can be spoken when complete or sentence by
  sentence as they arrive. Tool calls and commands are only spoken with "Speak
  tool calls and commands" on (Settings > Speech, or per conversation). Speech goes through **Prism** (statically linked:
  NVDA, JAWS, ZDSR, OneCore, SAPI, UIA... or automatic) or straight to **SAPI 5**
  with its own voice, rate and volume.
- **Sound packs**: a folder of sound files named after the events, chosen in
  Settings > Sounds and events. "Import sound pack" takes a zip of the sound
  files (named after the zip, or after the one folder they are in) and chooses
  it; "Open sound packs folder" opens where packs are kept: `soundpacks` in the
  data folder (packs in `soundpacks` beside FastChat.exe come with it). A file is named after the event's key, such as
  `response_complete.wav` (WAV, OGG, MP3 or FLAC). Tool events can have
  variants, the most specific found winning: `tool_call-command-git-commit`,
  `tool_call-command-git`, `tool_call-bash` (the tool's own name),
  `tool_call-command` (its kind: command, read, write, edit, search, list, web,
  agent, todo, plan, mcp), then `tool_call`. The same goes for `tool_result`
  and `tool_failed`; `task_started` and `task_finished` have `-agent` and
  `-shell`, and `task_finished` also `-agent-failed`. An event the pack has no
  sound for plays its built-in tone, and a sound file chosen for an event
  plays instead of the pack's. Event keys: message_sent, response_started,
  reasoning_started, response_complete, response_error, generation_stopped,
  tool_call, tool_result, tool_failed, approval_needed, conversation_opened,
  conversation_created, title_generated, message_deleted, compacted,
  task_started, task_finished.
- **Per-conversation settings** (Ctrl+Shift+Comma) override any of it: model,
  system prompt, reasoning effort, temperature, response length, command
  approval, speech, voice, rate, sound pack, and each event's sound and speech.

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
| Ctrl+Alt+Escape | Stop computer use, from any window |
| F6 | Move between the messages and the message box |
| Alt+M / Alt+W | The messages / the message box |
| Alt+A | Actions |
| Ctrl+M | Change model |
| Ctrl+Shift+Comma | Conversation settings |
| Ctrl+R | Regenerate the last response |
| Left / Right | Previous / next buffer of the messages list |
| Ctrl+K | Compact the conversation |
| Ctrl+T | Running tools and agents (Activity) |
| Ctrl | Stop speaking (anywhere in FastChat; can be turned off in Settings > Speech) |
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

The messages list has buffers, switched with Left and Right (or Actions >
Show): All, Conversation (your messages and the replies), Responses, Yours,
Thinking (the model's reasoning), Tools, Commands, Errors and notes, and Full
history. After a compaction all but Full history start at its summary.
Switching says the buffer's name, how many items it holds and the one you are
on, and keeps you on the same message when it is in the new buffer.
These announcements go to the screen reader through Prism, whichever engine reads
the replies (Settings > Speech can send them to the speech engine instead).

On a message: Enter views it, Ctrl+C copies it, Delete deletes it (a tool call
goes with its results), Shift+Delete deletes it and everything after, and
Shift+F10 opens a menu with view, copy, copy code blocks, speak, show thinking,
details, edit and resend, regenerate from here, and delete.

## Building

Windows, with Visual Studio 2022 (C++ workload), CMake 3.24+ and Git:

```bat
build.bat
```

To package it, run `dist.bat` (or `build.bat dist` to build first): it makes
`FastChat.zip`, a FastChat folder with FastChat.exe and this readme. FastChat.exe
needs nothing else; everything it uses comes with Windows. `dist.bat portable`
makes `FastChat-portable.zip`, which keeps settings and conversations beside the
exe instead of in %APPDATA%.

It works while FastChat is running: the running FastChat.exe is renamed to
FastChat.old.exe so the new one can be written; restart FastChat to use it.

CMake fetches wxWidgets, Prism, nlohmann/json, SQLite and miniaudio into
`build\` on the first run, which takes a while. To reuse a wxWidgets checkout you
already have (FastPlay's, for instance), configure once with
`-DFETCHCONTENT_SOURCE_DIR_WXWIDGETS=<its source folder>`.

### macOS

With the Xcode command line tools (`xcode-select --install`), CMake 3.24+ and
Ninja (`brew install cmake ninja`):

```sh
./build.sh
```

It makes `FastChat.app` in this folder, one app for Apple silicon and Intel Macs
(macOS 11 or later); `./build.sh native` builds for this Mac only, which is
faster. `./dist.sh` (or `./build.sh dist`) makes `FastChat-mac.zip` with the app
and this readme. The app is signed ad hoc rather than by an Apple developer, so
on another Mac it opens the first time with Control-click > Open.

`./dist.sh notarize` (or `./build.sh notarize`) instead signs the app with a
Developer ID and has Apple notarize it, making `FastChat-mac.dmg` (the app beside
a link to Applications) and `FastChat-mac.zip`, which open on any Mac without
warnings. It needs a Developer ID Application certificate in the keychain and an
App Store Connect API key for the notary service, `AuthKey_<key ID>.p8` in
`~/.appstoreconnect/private_keys` (set `ASC_KEY_ID`, `ASC_ISSUER_ID` and
`ASC_KEY_PATH` for another key; the comments in `dist.sh` have the rest).

On the Mac:

- The keys above that use Ctrl use Command (Command+N, Command+Comma...), except
  stopping speech, which is Control as in VoiceOver, and stopping computer use,
  which is Control+Option+Escape (Command+Option+Escape is Force Quit). Command+Delete deletes in
  the lists. Settings is in the FastChat menu.
- Speech goes through Prism (VoiceOver, or the system voices), or straight to
  the macOS voices (Settings > Speech > Engine) with their own voice, rate and
  volume.
- FastChat takes the PATH your shell sets up (as Terminal has it), so Claude
  Code, Codex, Homebrew's tools and the commands of code conversations are found
  even when it is opened from the Finder.
- Data lives in `~/Library/Application Support/FastChat`. API keys are only
  obfuscated there, not encrypted; the settings file is readable by you alone.
- Computer use needs two permissions, which macOS asks for the first time:
  Accessibility (to read and operate other apps) and Screen & System Audio
  Recording (for screenshots; the accessibility tree works without it). Turn
  FastChat on in System Settings > Privacy & Security. Because the app is
  signed ad hoc, macOS treats each new build as a different app: after
  rebuilding, remove FastChat from those lists and add it again. The look tool
  lists the window's controls, then the app's open menu and its menu bar.

Linux builds with the same CMakeLists.txt (libcurl for HTTP); it has not been
tried yet.

## Tests

```bat
build.bat tests
python tests\mock_server.py 8765
set FASTCHAT_MOCK=http://127.0.0.1:8765
build\Release\fastchat_tests.exe
```

On the Mac: `./build.sh tests`, `python3 tests/mock_server.py 8765 &`, then
`FASTCHAT_MOCK=http://127.0.0.1:8765 build/fastchat_tests`.

`mock_server.py` imitates the OpenAI, Anthropic, Gemini and Ollama streaming
APIs, and `fake_claude.py` / `fake_codex.py` imitate the two coding agents, so
the providers, tool loops, approvals and account reports are tested without
keys. The desktop tests drive a small window of their own through accessibility
only; set `FASTCHAT_TEST_INPUT=1` to also send it real keystrokes and a click
(it takes the foreground for a moment). Set `FASTCHAT_REAL_CLAUDE=1` to also run
a short real Claude Code session
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
  soundpack.*        sound packs: finding them, the sound for an event, and
                     importing a pack's zip (with miniz)
src/ui/              the wxWidgets windows and dialogs
tests/               core tests and the mock server
```
