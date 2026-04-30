#pragma once
#include <cstdint>
#include <string>
#include <xaudio2.h>
#include <Windows.h>
#include <wrl.h>

namespace audio {

// チャンクヘッダ
struct ChunkHeader {
	char id[4];   // チャンク毎のID
	int32_t size; // チャンクサイズ
};

// RIFFヘッダチャンク
struct RiffHeader {
	ChunkHeader chunk; // "RIFF"
	char type[4];      // "WAVE"
};

// FMTチャンク
struct FormatChunk {
	ChunkHeader chunk; // "fmt "
	WAVEFORMATEX fmt;  // 波形フォーマット
};

// 音声データ
struct SoundData {

	// 波形フォーマット
	WAVEFORMATEX wfex;

	// バッファの先頭アドレス
	BYTE* pBuffer;

	// バッファのサイズ
	unsigned int bufferSize;
};



SoundData SoundLoadWave(const char* filename);
void SoundUnload(SoundData* soundData);
void SoundPlayWave(IXAudio2* xAudio2, const SoundData& soundData);

} // namespace audio
