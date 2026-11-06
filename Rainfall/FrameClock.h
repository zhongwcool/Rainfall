#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <thread>

// 独立线程等待高精度计时器，到点后向窗口投递一条帧消息。
// 队列里最多保留一条，渲染跟不上时自动丢帧而不是堆积。
class FrameClock
{
public:
    FrameClock() = default;
    ~FrameClock();

    FrameClock(const FrameClock&) = delete;
    FrameClock& operator=(const FrameClock&) = delete;

    bool Start(HWND target, UINT message, UINT intervalMs);
    void Stop();
    void SetRunning(bool running);
    void SetInterval(UINT intervalMs);
    // 返回 true 表示已经等到一次屏幕刷新。失败时改用定时器。
    void SetRefreshWait(bool (*wait)());
    void Acknowledge();

private:
    bool ArmTimer();
    void ThreadProc();

    HANDLE timer_ = nullptr;
    HANDLE stopEvent_ = nullptr;
    std::thread thread_;
    std::atomic<bool> pending_{ false };
    std::atomic<bool> running_{ false };
    std::atomic<bool> oneShot_{ false };
    std::atomic<bool (*)()> refreshWait_{ nullptr };
    HWND target_ = nullptr;
    UINT message_ = 0;
    UINT intervalMs_ = 16;
};
