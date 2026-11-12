#pragma once

// Updates: the newest build of FastChat, from the rolling "latest" release on
// GitHub that CI rebuilds with every change (there are no versioned releases).
// A build is told from another by the commit it was made from.

#include <string>

constexpr const char* kUpdateRepo = "masonasons/FastChat";

struct UpdateInfo {
    bool available = false;
    std::string commit;       // the latest build's commit (7 characters)
    std::string notes;        // the release's text
    std::string downloadUrl;  // this system's package
    std::string error;        // why it could not tell, in a sentence
};

// This build's commit (7 characters), or "" for a build made outside git.
const char* BuildCommit();

// The release asset this system updates from: "FastChat.zip" (Windows),
// "FastChat-mac.zip" (macOS).
const char* UpdatePackageName();

// What a release (GitHub's JSON for releases/tags/latest) means for a build of
// `currentCommit` that updates from `package`. A build without a commit is
// never offered one.
UpdateInfo ReadLatestRelease(const std::string& releaseJson, const std::string& currentCommit, const std::string& package);

// Asks GitHub. Blocking: call off the UI thread.
UpdateInfo CheckForUpdate();

// Downloads the package into the temp folder. Blocking.
bool DownloadUpdate(const std::string& url, std::string& path, std::string& error);

// Why FastChat in `appDir` cannot update itself there ("" when it can): a
// folder with FastChat's source in it (a build of your own), or one it cannot write to.
std::string UpdateBlocker(const std::string& appDir);
// The same for the running FastChat (its folder; on the Mac, its app bundle).
std::string UpdateBlockerHere();

// The script that waits for process `pid` to end, puts the package's files in
// `appDir` (Windows: the zip's FastChat folder over it; macOS: `appDir` is the
// FastChat.app bundle, replaced), starts FastChat again and deletes itself.
std::string WindowsUpdateScript(unsigned long pid, const std::string& zipPath, const std::string& extractDir,
                                const std::string& appDir);
std::string MacUpdateScript(long pid, const std::string& zipPath, const std::string& extractDir, const std::string& appBundle);

// Writes that script and starts it; the caller then quits FastChat.
bool StartUpdateInstall(const std::string& packagePath, std::string& error);
