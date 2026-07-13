#include "RainAudio.h"

#include <algorithm>
#include <cmath>

namespace
{
    float Lerp(float a, float b, float t)
    {
        return a + (b - a) * t;
    }
}

RainAudio::~RainAudio()
{
    Shutdown();
}

bool RainAudio::Initialize()
{
    if (initialized_)
    {
        return true;
    }

    if (FAILED(XAudio2Create(&xaudio_, 0, XAUDIO2_DEFAULT_PROCESSOR)))
    {
        return false;
    }

    if (FAILED(xaudio_->CreateMasteringVoice(&masterVoice_, kChannels, kSampleRate)))
    {
        xaudio_->Release();
        xaudio_ = nullptr;
        return false;
    }

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = kChannels;
    format.nSamplesPerSec = kSampleRate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    callback_.bufferEndEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!callback_.bufferEndEvent)
    {
        Shutdown();
        return false;
    }

    if (FAILED(xaudio_->CreateSourceVoice(&sourceVoice_, &format, 0,
        XAUDIO2_DEFAULT_FREQ_RATIO, &callback_)))
    {
        Shutdown();
        return false;
    }

    // 左右声道用不同随机种子，声音才不会"贴在正中间"
    channels_[0].rng = 0x12345678u;
    channels_[1].rng = 0x87654321u;

    bufferData_.resize(static_cast<size_t>(kFramesPerBuffer) * kChannels * kBufferCount);

    sourceVoice_->Start(0);

    quit_ = false;
    streamThread_ = std::thread(&RainAudio::StreamThread, this);

    initialized_ = true;
    return true;
}

void RainAudio::Shutdown()
{
    quit_ = true;

    if (callback_.bufferEndEvent)
    {
        SetEvent(callback_.bufferEndEvent);
    }

    if (streamThread_.joinable())
    {
        streamThread_.join();
    }

    if (sourceVoice_)
    {
        sourceVoice_->Stop(0);
        sourceVoice_->FlushSourceBuffers();
        sourceVoice_->DestroyVoice();
        sourceVoice_ = nullptr;
    }

    if (masterVoice_)
    {
        masterVoice_->DestroyVoice();
        masterVoice_ = nullptr;
    }

    if (xaudio_)
    {
        xaudio_->Release();
        xaudio_ = nullptr;
    }

    if (callback_.bufferEndEvent)
    {
        CloseHandle(callback_.bufferEndEvent);
        callback_.bufferEndEvent = nullptr;
    }

    initialized_ = false;
}

void RainAudio::SetEnabled(bool enabled)
{
    enabled_ = enabled;
}

void RainAudio::SetPaused(bool paused)
{
    paused_ = paused;
}

void RainAudio::SetIntensity(float intensity)
{
    targetIntensity_ = std::clamp(intensity, 0.0f, 1.0f);
}

float RainAudio::NextNoise(uint32_t& rng)
{
    // xorshift32：足够快的白噪声源
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return static_cast<float>(static_cast<int32_t>(rng)) * (1.0f / 2147483648.0f);
}

void RainAudio::FillBuffer(short* samples)
{
    // 每个缓冲区（100ms）向目标强度靠拢一步，雨势切换时音色平滑过渡
    intensity_ += (targetIntensity_.load() - intensity_) * 0.12f;

    const float t = intensity_;

    // 底噪（雨幕沙沙声）：强度越大越响、越"亮"
    const float hissGain = 0.015f + 0.24f * t * std::sqrt(t);
    const float lpCoeff = Lerp(0.045f, 0.22f, t);

    // 雨点脆响：每声道每秒触发次数
    const float dropsPerSecond = Lerp(3.0f, 55.0f, t * t);
    const float meanInterval = static_cast<float>(kSampleRate) / std::max(dropsPerSecond, 0.5f);
    const float dropGain = Lerp(0.10f, 0.30f, t);
    const float dropDecay = 0.9974f; // 约 9ms 衰减的短促"嗒"声

    const bool audible = enabled_.load() && !paused_.load();
    const float gateTarget = audible ? 1.0f : 0.0f;

    for (UINT32 frame = 0; frame < kFramesPerBuffer; ++frame)
    {
        gate_ += (gateTarget - gate_) * 0.0008f;

        for (UINT32 ch = 0; ch < kChannels; ++ch)
        {
            ChannelState& state = channels_[ch];

            const float noise = NextNoise(state.rng);

            // 两级一阶低通串联，让白噪声变成柔和的雨声底噪
            state.lp1 += lpCoeff * (noise - state.lp1);
            state.lp2 += lpCoeff * (state.lp1 - state.lp2);
            float sample = state.lp2 * hissGain * 4.0f;

            // 随机触发单个雨点
            if (--state.dropCountdown <= 0)
            {
                const float u = NextNoise(state.rng) * 0.5f + 0.5f; // 0~1
                state.dropCountdown = static_cast<int>(meanInterval * (0.3f + 1.4f * u)) + 1;
                state.dropEnv = dropGain * (0.4f + 0.6f * (NextNoise(state.rng) * 0.5f + 0.5f));
            }

            if (state.dropEnv > 0.0001f)
            {
                // 雨点用偏高频的噪声（原噪声减去低通分量），听感是清脆的"嗒"
                const float dropNoise = NextNoise(state.rng);
                state.dropNoiseLp += 0.35f * (dropNoise - state.dropNoiseLp);
                sample += (dropNoise - state.dropNoiseLp) * state.dropEnv;
                state.dropEnv *= dropDecay;
            }

            sample *= gate_;

            // 软限幅防爆音
            sample = std::tanh(sample * 1.5f);

            const int value = static_cast<int>(sample * 30000.0f);
            samples[frame * kChannels + ch] = static_cast<short>(std::clamp(value, -32768, 32767));
        }
    }
}

void RainAudio::StreamThread()
{
    const UINT32 samplesPerBuffer = kFramesPerBuffer * kChannels;
    UINT32 next = 0;

    while (!quit_)
    {
        XAUDIO2_VOICE_STATE state{};
        sourceVoice_->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);

        while (state.BuffersQueued < kBufferCount && !quit_)
        {
            short* samples = bufferData_.data() + static_cast<size_t>(next) * samplesPerBuffer;
            FillBuffer(samples);

            XAUDIO2_BUFFER buffer{};
            buffer.AudioBytes = samplesPerBuffer * sizeof(short);
            buffer.pAudioData = reinterpret_cast<const BYTE*>(samples);
            if (FAILED(sourceVoice_->SubmitSourceBuffer(&buffer)))
            {
                break;
            }

            next = (next + 1) % kBufferCount;
            sourceVoice_->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        }

        WaitForSingleObject(callback_.bufferEndEvent, 200);
    }
}
