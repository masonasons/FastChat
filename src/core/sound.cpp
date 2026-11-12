#include "core/sound.h"
#include "core/util.h"

#include <miniaudio.h>

#include <cmath>
#include <list>
#include <memory>
#include <vector>

SoundPlayer& AppSound() {
    static SoundPlayer player;
    return player;
}

struct Playing {
    ma_sound sound{};
    ma_audio_buffer buffer{};
    std::vector<float> pcm;
    bool hasBuffer = false;
    bool soundReady = false;
    ~Playing() {
        if (soundReady) ma_sound_uninit(&sound);
        if (hasBuffer) ma_audio_buffer_uninit(&buffer);
    }
};

struct SoundPlayer::Impl {
    ma_engine engine{};
    bool ready = false;
    std::list<std::unique_ptr<Playing>> playing;
};

SoundPlayer::~SoundPlayer() {
    Shutdown();
}

bool SoundPlayer::Init() {
    if (m_impl) return m_impl->ready;
    m_impl = new Impl();
    m_impl->ready = ma_engine_init(nullptr, &m_impl->engine) == MA_SUCCESS;
    return m_impl->ready;
}

void SoundPlayer::Shutdown() {
    if (!m_impl) return;
    Cleanup(true);
    if (m_impl->ready) ma_engine_uninit(&m_impl->engine);
    delete m_impl;
    m_impl = nullptr;
}

void SoundPlayer::SetVolume(int volume) {
    if (m_impl && m_impl->ready) ma_engine_set_volume(&m_impl->engine, volume / 100.0f);
}

void SoundPlayer::Cleanup(bool all) {
    if (!m_impl) return;
    for (auto it = m_impl->playing.begin(); it != m_impl->playing.end();) {
        if (all || ma_sound_at_end(&(*it)->sound) || !ma_sound_is_playing(&(*it)->sound)) {
            it = m_impl->playing.erase(it);
        } else {
            ++it;
        }
    }
}

bool SoundPlayer::PlayFile(const std::string& path) {
    if (!m_impl || !m_impl->ready) return false;
    Cleanup(false);
    auto p = std::make_unique<Playing>();
#ifdef _WIN32
    std::wstring wpath = Utf8ToWide(path);
    ma_result r = ma_sound_init_from_file_w(&m_impl->engine, wpath.c_str(), MA_SOUND_FLAG_DECODE, nullptr, nullptr, &p->sound);
#else
    ma_result r = ma_sound_init_from_file(&m_impl->engine, path.c_str(), MA_SOUND_FLAG_DECODE, nullptr, nullptr, &p->sound);
#endif
    if (r != MA_SUCCESS) return false;
    p->soundReady = true;
    ma_sound_start(&p->sound);
    m_impl->playing.push_back(std::move(p));
    return true;
}

void SoundPlayer::PlayTone(float tone1, float tone2, int ms) {
    if (!m_impl || !m_impl->ready) return;
    Cleanup(false);
    const ma_uint32 rate = ma_engine_get_sample_rate(&m_impl->engine);
    auto p = std::make_unique<Playing>();
    const size_t noteSamples = static_cast<size_t>(rate) * static_cast<size_t>(ms) / 1000;
    const size_t fade = std::min<size_t>(noteSamples / 4, rate / 200);  // 5 ms edges, no clicks
    auto note = [&](float freq) {
        for (size_t i = 0; i < noteSamples; i++) {
            float env = 1.0f;
            if (i < fade) env = static_cast<float>(i) / fade;
            else if (i > noteSamples - fade) env = static_cast<float>(noteSamples - i) / fade;
            float t = static_cast<float>(i) / rate;
            // A sine with a touch of its octave, softer than a bare beep.
            float v = 0.8f * std::sin(2.0f * 3.14159265f * freq * t) + 0.2f * std::sin(4.0f * 3.14159265f * freq * t);
            p->pcm.push_back(0.35f * env * v);
        }
    };
    note(tone1);
    if (tone2 > 0) note(tone2);

    ma_audio_buffer_config cfg = ma_audio_buffer_config_init(ma_format_f32, 1, p->pcm.size(), p->pcm.data(), nullptr);
    if (ma_audio_buffer_init(&cfg, &p->buffer) != MA_SUCCESS) return;
    p->hasBuffer = true;
    if (ma_sound_init_from_data_source(&m_impl->engine, &p->buffer, 0, nullptr, &p->sound) != MA_SUCCESS) return;
    p->soundReady = true;
    ma_sound_start(&p->sound);
    m_impl->playing.push_back(std::move(p));
}
