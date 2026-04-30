#pragma once
#include "audio/AudioWave.h"

#include <xaudio2.h>
#include <wrl.h>

class AppAudio {
public:
    bool Initialize();
    void Finalize();

    audio::SoundData LoadWave(const char* filename);
    void Unload(audio::SoundData* soundData);
    void PlayWave(const audio::SoundData& soundData);

private:
    Microsoft::WRL::ComPtr<IXAudio2> xAudio2_;
    IXAudio2MasteringVoice* masterVoice_ = nullptr;
};
