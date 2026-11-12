#pragma once

// Sound packs: a folder of sound files named after the events they are for.
//
//   response_complete.wav        the sound of an event (any format miniaudio
//                                reads: .wav, .ogg, .mp3, .flac)
//   tool_call-edit.wav           a variant: more specific names win, and an
//   tool_call-command-git.wav    event without a matching variant plays its
//   tool_call-command-git-commit.wav   plain sound, else the built-in tone
//
// Packs live in "soundpacks" in the data folder, and beside FastChat itself
// (packs that come with it). UI thread only.

#include <string>
#include <vector>

struct SoundPackInfo {
    std::string name;    // the folder's name
    std::string folder;
};

// Every pack found, sorted by name; one in the data folder hides a bundled
// pack of the same name.
std::vector<SoundPackInfo> ListSoundPacks();

// The data folder's "soundpacks", created if missing.
std::string UserSoundPacksFolder();

// The folder of the pack with this name, or "" when there is none.
std::string FindSoundPackFolder(const std::string& name);

// The file for the first of `names` (without extension) that the pack has, or "".
std::string FindPackSound(const std::string& folder, const std::vector<std::string>& names);

// Whether the pack has any sound for this event key (plain or a variant).
bool PackHasEventSound(const std::string& folder, const std::string& eventKey);

// Forget the packs and files seen, so added or changed ones are found.
void ForgetSoundPackFiles();

// The name a zip's pack gets: the one folder its sounds are in, if they are
// all in one, else the zip's own name. "" if it cannot be read.
std::string SoundPackNameForZip(const std::string& zipPath);

struct SoundPackImport {
    bool ok = false;
    std::string name;
    std::string folder;
    int sounds = 0;
    std::string error;
};

// Unpacks the zip's sound files (.wav .ogg .mp3 .flac, anywhere in it) into a
// pack called `name` in the user's packs folder, replacing a pack of that name.
// Packs are flat: only file names are kept, never paths from the zip. A zip
// that fails leaves the pack that was there.
SoundPackImport ImportSoundPackZip(const std::string& zipPath, const std::string& name);

// What kind of tool a tool name is, the same across providers: "command",
// "read", "write", "edit", "search", "list", "web", "agent", "todo", "plan",
// "mcp", or "" for others.
std::string ToolKind(const std::string& toolName);

// The program and subcommand a command line runs, lower case: "git commit -m x"
// and "git -C repo commit" give {"git", "commit"}; "cd src && C:\Python\python.exe
// -m pytest" gives {"python", "m"} (an option without its dashes). Leading "cd"
// and variable settings are skipped.
std::vector<std::string> CommandWords(const std::string& command);

// The variants a tool's sounds can have, most specific first (the event key
// goes in front of each): "command-git-commit", "command-git", "bash",
// "command" for a Bash call running "git commit"; "read_file", "read" for
// read_file.
std::vector<std::string> ToolSoundVariants(const std::string& toolName, const std::string& command);
