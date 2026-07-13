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
    if (callback_.bufferEndEvent)
    {
        SetEvent(callback_.bufferEndEvent); // 唤醒可能在休眠的音频线程
    }
}

void RainAudio::SetPaused(bool paused)
{
    paused_ = paused;
    if (callback_.bufferEndEvent)
    {
        SetEvent(callback_.bufferEndEvent);
    }
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

    // 底噪（雨幕沙沙声）：小雨时几乎为零，避免听成溪流；大雨时才铺满
    const float hissGain = 0.002f + 0.30f * t * t * std::sqrt(t);
    const float lpCoeff = Lerp(0.045f, 0.22f, t);

    // 雨点：每声道每秒触发次数（小雨稀疏，颗颗分明）
    const float dropsPerSecond = Lerp(1.6f, 55.0f, t * t);
    const float meanInterval = static_cast<float>(kSampleRate) / std::max(dropsPerSecond, 0.5f);
    const float dropGain = Lerp(0.16f, 0.30f, t);
    const float dropDecay = 0.9974f; // 约 9ms 衰减的短促"嗒"声

    // 芭蕉叶共鸣：小雨时突出宽大叶面的低沉"啵"声，大雨时被雨幕淹没
    const float leafGain = Lerp(1.0f, 0.15f, t) ;
    const float leafDecayR = Lerp(0.9994f, 0.9985f, t); // 共鸣衰减，小雨余韵略长
    const float excDecay = 0.965f; // 敲击激励约 1ms，非常短促

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

                const float strength = 0.4f + 0.6f * (NextNoise(state.rng) * 0.5f + 0.5f);
                state.dropEnv = dropGain * strength;

                // 每颗雨点落在叶面不同位置，共鸣音高随机（约 160~420Hz 的低沉"啵"）
                const float freq = 160.0f + 260.0f * (NextNoise(state.rng) * 0.5f + 0.5f);
                const float w = 6.2831853f * freq / static_cast<float>(kSampleRate);
                state.resCoef1 = 2.0f * leafDecayR * std::cos(w);
                state.resCoef2 = -leafDecayR * leafDecayR;
                state.excEnv = strength;
            }

            if (state.dropEnv > 0.0001f)
            {
                // 雨点用偏高频的噪声（原噪声减去低通分量），听感是清脆的"嗒"
                const float dropNoise = NextNoise(state.rng);
                state.dropNoiseLp += 0.35f * (dropNoise - state.dropNoiseLp);
                sample += (dropNoise - state.dropNoiseLp) * state.dropEnv;
                state.dropEnv *= dropDecay;
            }

            // 叶面共鸣：短促激励打进谐振器，衰减成有音高的"啵"声
            float excite = 0.0f;
            if (state.excEnv > 0.0005f)
            {
                excite = NextNoise(state.rng) * state.excEnv * 0.6f;
                state.excEnv *= excDecay;
            }
            const float res = state.resCoef1 * state.res1 + state.resCoef2 * state.res2 + excite;
            state.res2 = state.res1;
            state.res1 = res;
            sample += res * leafGain * 0.22f;

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
    bool sleeping = false;

    while (!quit_)
    {
        const bool audible = enabled_.load() && !paused_.load();

        // 声音关闭且淡出已完成：停掉声音通道并深度休眠，完全不再计算
        if (!audible && gate_ < 0.001f)
        {
            if (!sleeping)
            {
                sourceVoice_->Stop(0);
                sourceVoice_->FlushSourceBuffers();
                gate_ = 0.0f;
                sleeping = true;
            }
            WaitForSingleObject(callback_.bufferEndEvent, INFINITE);
            continue;
        }

        if (sleeping)
        {
            sourceVoice_->Start(0);
            sleeping = false;
        }

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
