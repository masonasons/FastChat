// The Mac's own voices directly (AVSpeechSynthesizer), with their own voice,
// rate and volume, independent of VoiceOver. The counterpart of SAPI 5 on
// Windows. Compiled with ARC.

#include "core/speech.h"

#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <vector>

namespace {

NSString* ToNS(const std::string& s) {
    return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding] ?: @"";
}

std::string FromNS(NSString* s) {
    return s ? std::string(s.UTF8String) : std::string();
}

// "Samantha (English (United States))": names repeat across languages, so the
// language is part of what is shown and saved.
std::string VoiceLabel(AVSpeechSynthesisVoice* v) {
    NSString* language = [[NSLocale currentLocale] localizedStringForLocaleIdentifier:v.language] ?: v.language;
    return FromNS([NSString stringWithFormat:@"%@ (%@)", v.name, language]);
}

class AVSpeech : public SpeechBackend {
public:
    AVSpeech() : m_synth([[AVSpeechSynthesizer alloc] init]) {}
    ~AVSpeech() override { Stop(); }

    bool Speak(const std::string& text, bool interrupt) override {
        @autoreleasepool {
            if (interrupt) Stop();
            AVSpeechUtterance* u = [AVSpeechUtterance speechUtteranceWithString:ToNS(text)];
            if (m_voice) u.voice = m_voice;
            u.rate = m_rate;
            u.volume = m_volume;
            [m_synth speakUtterance:u];
            return true;
        }
    }
    void Stop() override {
        if (m_synth.speaking) [m_synth stopSpeakingAtBoundary:AVSpeechBoundaryImmediate];
    }
    // Voices in the user's own language first, then the rest, each part by name.
    std::vector<std::string> Voices() override {
        std::vector<std::string> mine, others;
        @autoreleasepool {
            NSString* preferred = [NSLocale preferredLanguages].firstObject ?: @"en";
            NSString* language = [preferred componentsSeparatedByString:@"-"].firstObject;
            for (AVSpeechSynthesisVoice* v in [AVSpeechSynthesisVoice speechVoices]) {
                bool own = [[v.language componentsSeparatedByString:@"-"].firstObject isEqualToString:language];
                (own ? mine : others).push_back(VoiceLabel(v));
            }
        }
        for (auto* list : {&mine, &others}) {
            std::sort(list->begin(), list->end());
            list->erase(std::unique(list->begin(), list->end()), list->end());
        }
        mine.insert(mine.end(), others.begin(), others.end());
        return mine;
    }
    bool SetVoice(const std::string& name) override {
        @autoreleasepool {
            for (AVSpeechSynthesisVoice* v in [AVSpeechSynthesisVoice speechVoices]) {
                if (VoiceLabel(v) == name) {
                    m_voice = v;
                    return true;
                }
            }
        }
        return false;
    }
    // 0-100, with 50 the voice's normal speed.
    void SetRate(int rate) override {
        float t = std::clamp(rate, 0, 100) / 100.0f;
        m_rate = AVSpeechUtteranceMinimumSpeechRate + t * (AVSpeechUtteranceMaximumSpeechRate - AVSpeechUtteranceMinimumSpeechRate);
    }
    void SetVolume(int volume) override { m_volume = std::clamp(volume, 0, 100) / 100.0f; }
    std::string Name() const override { return "macOS voices"; }

private:
    AVSpeechSynthesizer* m_synth;
    AVSpeechSynthesisVoice* m_voice = nil;
    float m_rate = AVSpeechUtteranceDefaultSpeechRate;
    float m_volume = 1.0f;
};

}  // namespace

std::unique_ptr<SpeechBackend> CreateSystemVoiceBackend() {
    return std::make_unique<AVSpeech>();
}
