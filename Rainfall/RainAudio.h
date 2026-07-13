#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <xaudio2.h>
#include <atomic>
#include <thread>
#include <vector>

// 用 XAudio2 实时合成雨声背景音（无需任何音频文件）。
// 声音 = 低通滤波的噪声底噪（雨幕沙沙声） + 随机触发的短促雨点脆响。
// 强度 0.0（毛毛雨）~ 1.0（暴雨）连续可调，音量与雨点频率平滑过渡。
class RainAudio
{
public:
    RainAudio() = default;
    ~RainAudio();

    RainAudio(const RainAudio&) = delete;
    RainAudio& operator=(const RainAudio&) = delete;

    bool Initialize();
    void Shutdown();

    void SetEnabled(bool enabled);
    void SetPaused(bool paused);
    void SetIntensity(float intensity); // 0.0 ~ 1.0

private:
    static constexpr UINT32 kSampleRate = 44100;
    static constexpr UINT32 kChannels = 2;
    static constexpr UINT32 kFramesPerBuffer = 4410; // 100ms
    static constexpr UINT32 kBufferCount = 3;

    struct VoiceCallback final : IXAudio2VoiceCallback
    {
        HANDLE bufferEndEvent = nullptr;

        void __stdcall OnBufferEnd(void*) override
        {
            SetEvent(bufferEndEvent);
        }

        void __stdcall OnStreamEnd() override {}
        void __stdcall OnVoiceProcessingPassEnd() override {}
        void __stdcall OnVoiceProcessingPassStart(UINT32) override {}
        void __stdcall OnBufferStart(void*) override {}
        void __stdcall OnLoopEnd(void*) override {}
        void __stdcall OnVoiceError(void*, HRESULT) override {}
    };

    // 单声道的合成状态（左右声道各一份，互不相关才有空间感）
    struct ChannelState
    {
        uint32_t rng = 0;
        float lp1 = 0.0f;
        float lp2 = 0.0f;
        float dropEnv = 0.0f;
        float dropNoiseLp = 0.0f;
        int dropCountdown = 0;
        // 叶面共鸣：二阶谐振器，模拟雨点砸在芭蕉叶上的"啵"声
        float res1 = 0.0f;
        float res2 = 0.0f;
        float resCoef1 = 0.0f;
        float resCoef2 = 0.0f;
        float excEnv = 0.0f;
    };

    void StreamThread();
    void FillBuffer(short* samples);
    float NextNoise(uint32_t& rng);

    IXAudio2* xaudio_ = nullptr;
    IXAudio2MasteringVoice* masterVoice_ = nullptr;
    IXAudio2SourceVoice* sourceVoice_ = nullptr;
    VoiceCallback callback_;

    std::vector<short> bufferData_;
    std::thread streamThread_;
    std::atomic<bool> quit_{ false };

    std::atomic<float> targetIntensity_{ 0.5f };
    std::atomic<bool> enabled_{ false };
    std::atomic<bool> paused_{ false };

    // 以下状态仅在音频线程使用
    float intensity_ = 0.5f;
    float gate_ = 0.0f; // 开关的平滑增益，避免爆音
    ChannelState channels_[kChannels];

    bool initialized_ = false;
};
