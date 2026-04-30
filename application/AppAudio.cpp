#include "AppAudio.h"

bool AppAudio::Initialize() {
    HRESULT hr = XAudio2Create(&xAudio2_, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) return false;

    hr = xAudio2_->CreateMasteringVoice(&masterVoice_);
    if (FAILED(hr)) return false;

    return true;
}

void AppAudio::Finalize() {
    if (masterVoice_) {
        masterVoice_->DestroyVoice();
        masterVoice_ = nullptr;
    }
    xAudio2_.Reset();
}

audio::SoundData AppAudio::LoadWave(const char* filename) {
    return audio::SoundLoadWave(filename);
}

void AppAudio::Unload(audio::SoundData* soundData) {
    audio::SoundUnload(soundData);
}

void AppAudio::PlayWave(const audio::SoundData& soundData) {
    if (!xAudio2_) {
        return;
    }

    audio::SoundPlayWave(xAudio2_.Get(), soundData);
}
