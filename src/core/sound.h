#pragma once

// Sound events through miniaudio: sound files (WAV, MP3, FLAC, OGG...) or
// FastChat's built-in tones. UI thread only.

#include <string>

class SoundPlayer {
public:
    ~SoundPlayer();
    bool Init();
    void Shutdown();
    void SetVolume(int volume0to100);
    // Play a file; false if it could not be opened.
    bool PlayFile(const std::string& path);
    // Two short notes (Hz; tone2 may be 0) of `ms` milliseconds each.
    void PlayTone(float tone1, float tone2, int ms);

private:
    void Cleanup(bool all);
    struct Impl;
    Impl* m_impl = nullptr;
};

SoundPlayer& AppSound();
