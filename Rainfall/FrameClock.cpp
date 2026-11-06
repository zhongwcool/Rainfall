#include "FrameClock.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

FrameClock::~FrameClock()
{
    Stop();
}

bool FrameClock::Start(HWND target, UINT message, UINT intervalMs)
{
    Stop();

    target_ = target;
    message_ = message;
    intervalMs_ = intervalMs == 0 ? 1 : intervalMs;
    pending_.store(false);
    oneShot_.store(false);

    timer_ = CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer_)
    {
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }
    if (!timer_)
    {
        return false;
    }

    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_)
    {
        CloseHandle(timer_);
        timer_ = nullptr;
        return false;
    }

    if (!ArmTimer())
    {
        Stop();
        return false;
    }

    running_.store(true);
    try
    {
        thread_ = std::thread(&FrameClock::ThreadProc, this);
    }
    catch (...)
    {
        Stop();
        return false;
    }

    return true;
}

void FrameClock::Stop()
{
    running_.store(false);

    if (stopEvent_)
    {
        SetEvent(stopEvent_);
    }

    if (thread_.joinable())
    {
        thread_.join();
    }

    if (timer_)
    {
        CancelWaitableTimer(timer_);
        CloseHandle(timer_);
        timer_ = nullptr;
    }

    if (stopEvent_)
    {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }

    pending_.store(false);
    oneShot_.store(false);
    target_ = nullptr;
}

void FrameClock::SetInterval(UINT intervalMs)
{
    intervalMs_ = intervalMs == 0 ? 1 : intervalMs;
    if (timer_ && running_.load())
    {
        ArmTimer();
    }
}

void FrameClock::SetRefreshWait(bool (*wait)())
{
    refreshWait_.store(wait);
}

void FrameClock::SetRunning(bool running)
{
    running_.store(running);
    if (!running)
    {
        pending_.store(false);
    }

    if (!timer_)
    {
        return;
    }

    if (running)
    {
        ArmTimer();
    }
    else
    {
        CancelWaitableTimer(timer_);
    }
}

void FrameClock::Acknowledge()
{
    pending_.store(false);
}

bool FrameClock::ArmTimer()
{
    if (!timer_ || intervalMs_ == 0)
    {
        return false;
    }

    LARGE_INTEGER due{};
    due.QuadPart = -10000LL * static_cast<LONGLONG>(intervalMs_);
    if (SetWaitableTimer(timer_, &due, static_cast<LONG>(intervalMs_), nullptr, nullptr, FALSE))
    {
        oneShot_.store(false);
        return true;
    }

    due.QuadPart = -10000LL * static_cast<LONGLONG>(intervalMs_);
    if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE))
    {
        oneShot_.store(true);
        return true;
    }

    return false;
}

void FrameClock::ThreadProc()
{
    const HANDLE handles[2] = { stopEvent_, timer_ };
    while (true)
    {
        if (!running_.load())
        {
            const DWORD result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            if (result == WAIT_OBJECT_0 || result == WAIT_FAILED)
            {
                break;
            }
            continue;
        }

        // 跟垂直空白走，雨丝才会按屏幕刷新一格一格往下走。等不到就退回定时器。
        bool paced = false;
        if (const auto wait = refreshWait_.load())
        {
            LARGE_INTEGER before{};
            QueryPerformanceCounter(&before);
            paced = wait();
            LARGE_INTEGER after{};
            QueryPerformanceCounter(&after);
            LARGE_INTEGER frequency{};
            QueryPerformanceFrequency(&frequency);
            const double elapsedMs = frequency.QuadPart > 0
                ? static_cast<double>(after.QuadPart - before.QuadPart) * 1000.0
                    / static_cast<double>(frequency.QuadPart)
                : 0.0;
            // 立刻返回说明没有真正卡在刷新上，继续等会把这一帧线程转满。
            if (elapsedMs < 1.0)
            {
                paced = false;
            }
        }

        if (!paced)
        {
            const DWORD result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            if (result == WAIT_OBJECT_0 || result == WAIT_FAILED)
            {
                break;
            }
            if (result != WAIT_OBJECT_0 + 1 || !running_.load())
            {
                continue;
            }

            if (oneShot_.load())
            {
                LARGE_INTEGER due{};
                due.QuadPart = -10000LL * static_cast<LONGLONG>(intervalMs_);
                SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE);
            }
        }

        if (WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0)
        {
            break;
        }
        if (!running_.load())
        {
            continue;
        }

        if (!pending_.exchange(true))
        {
            if (!target_ || !PostMessageW(target_, message_, 0, 0))
            {
                pending_.store(false);
            }
        }
    }
}
