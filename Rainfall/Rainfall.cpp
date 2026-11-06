#include "framework.h"
#include "Rainfall.h"
#include "RainSystem.h"
#include "RainRenderer.h"
#include "RainAudio.h"
#include "FrameClock.h"

#include <shlobj.h>
#include <string>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <new>

namespace
{
    constexpr UINT WM_TRAYICON = WM_APP + 1;
    constexpr UINT WM_APP_FRAME = WM_APP + 2;
    constexpr UINT WM_APP_REBUILD_OVERLAYS = WM_APP + 3;
    constexpr UINT WM_APP_ALREADY_RUNNING = WM_APP + 4;
    constexpr UINT_PTR kRebuildTimerId = 1;
    constexpr UINT_PTR kTrayRetryTimerId = 2;
    constexpr UINT_PTR kFallbackFrameTimerId = 3;
    constexpr UINT_PTR kToastTimerId = 99;
    constexpr UINT kFrameIntervalMs = 16;
    constexpr int kTrayRetryLimit = 15;
    constexpr wchar_t kWindowClassName[] = L"RainfallOverlayClass";
    constexpr wchar_t kControllerClassName[] = L"RainfallControllerClass";
    constexpr wchar_t kToastClassName[] = L"RainfallToastClass";
    constexpr wchar_t kAuthorEmail[] = L"zhongwcool@163.com";
    constexpr wchar_t kConfigSection[] = L"Rainfall";

    // 5 挡：索引 0~4，默认第 3 挡（索引 2）等于原始效果
    constexpr int kLevelCount = 5;
    constexpr int kDefaultLevel = 2;
    constexpr float kLengthScales[kLevelCount] = { 1.0f, 1.5f, 2.0f, 3.0f, 4.0f };
    constexpr float kDensityScales[kLevelCount] = { 0.1f, 0.35f, 1.0f, 1.5f, 2.0f };
    // 风力：角度倍率，越大越倾斜（第 3 挡 = 原始轻微倾斜）
    constexpr float kWindScales[kLevelCount] = { 0.3f, 0.6f, 1.0f, 2.5f, 4.5f };
    // 雨势：速度倍率，越大落得越快（默认第 3 挡 = 原第 4 挡的 1.5）
    constexpr float kSpeedScales[kLevelCount] = { 0.75f, 1.1f, 1.5f, 2.0f, 2.6f };

    enum class LevelKind
    {
        Length,
        Density,
        Wind,
        Speed,
        Count,
    };

    // 长度 / 密度 / 风力 / 雨势共用这一行。新增同类选项只加一行。
    struct LevelSetting
    {
        const wchar_t* iniKey = nullptr;
        const wchar_t* menuLabel = nullptr;
        UINT idBase = 0;
        const float* scales = nullptr;
        void (RainSystem::*apply)(float) = nullptr;
        bool affectsSound = false;
        int level = kDefaultLevel;
    };

    struct Settings
    {
        bool lightMode = false;
        bool windRight = true;
        bool soundEnabled = true;
        bool alwaysOnTop = true;
        LevelSetting levels[static_cast<int>(LevelKind::Count)]{};

        LevelSetting& Level(LevelKind kind)
        {
            return levels[static_cast<int>(kind)];
        }
        const LevelSetting& Level(LevelKind kind) const
        {
            return levels[static_cast<int>(kind)];
        }

        const std::wstring& ConfigPath();
        void Load();
        void Save();

        std::wstring configPath;
        bool configResolved = false;
    };

    struct ScopedMutex
    {
        HANDLE handle = nullptr;
        ~ScopedMutex()
        {
            if (handle)
            {
                CloseHandle(handle);
            }
        }
        ScopedMutex() = default;
        ScopedMutex(const ScopedMutex&) = delete;
        ScopedMutex& operator=(const ScopedMutex&) = delete;
    };

    struct ToastData
    {
        std::wstring text;
        HFONT font = nullptr;
        bool ownsFont = false;
    };

    struct OverlayWindow
    {
        HWND hwnd = nullptr;
        RECT monitorRect{};
        UINT dpi = 0;
        std::unique_ptr<RainSystem> rainSystem;
        std::unique_ptr<RainRenderer> renderer;
    };

    HINSTANCE g_instance = nullptr;
    HWND g_controller = nullptr;
    HWND g_aboutDlg = nullptr;
    HWND g_toastWnd = nullptr;
    bool g_paused = false;
    bool g_trayAdded = false;
    bool g_usingFallbackTimer = false;
    int g_trayRetries = 0;
    UINT g_taskbarCreatedMsg = 0;
    HICON g_trayIcon = nullptr;

    Settings g_settings{
        .levels = {
            { L"LengthLevel", L"雨滴长度(&G)", IDM_LENGTH_BASE, kLengthScales, &RainSystem::SetLengthScale, false, kDefaultLevel },
            { L"DensityLevel", L"雨滴密度(&D)", IDM_DENSITY_BASE, kDensityScales, &RainSystem::SetDensityScale, true, kDefaultLevel },
            { L"WindLevel", L"风力(&W)", IDM_WIND_BASE, kWindScales, &RainSystem::SetWindScale, false, kDefaultLevel },
            { L"SpeedLevel", L"雨势(&S)", IDM_SPEED_BASE, kSpeedScales, &RainSystem::SetSpeedScale, true, kDefaultLevel },
        },
    };
    RainAudio g_rainAudio;
    FrameClock g_frameClock;
    LARGE_INTEGER g_qpcFrequency{};
    LONGLONG g_lastFrameQpc = 0;

    std::vector<std::unique_ptr<OverlayWindow>> g_overlays;

    // 进程退出时按声明的反序析构：先停声音，再拆窗口，最后释放工厂和 COM。
    struct ScopedCom
    {
        HRESULT hr = E_FAIL;
        ScopedCom()
        {
            hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        }
        ~ScopedCom()
        {
            if (SUCCEEDED(hr))
            {
                CoUninitialize();
            }
        }
        explicit operator bool() const
        {
            return SUCCEEDED(hr);
        }
        ScopedCom(const ScopedCom&) = delete;
        ScopedCom& operator=(const ScopedCom&) = delete;
    };

    struct ScopedRendererFactory
    {
        bool active = false;
        ScopedRendererFactory()
        {
            active = RainRenderer::InitializeFactory();
        }
        ~ScopedRendererFactory()
        {
            if (active)
            {
                RainRenderer::ShutdownFactory();
            }
        }
        explicit operator bool() const
        {
            return active;
        }
        ScopedRendererFactory(const ScopedRendererFactory&) = delete;
        ScopedRendererFactory& operator=(const ScopedRendererFactory&) = delete;
    };

    struct ScopedControllerWindow
    {
        ~ScopedControllerWindow()
        {
            g_frameClock.Stop();
            if (!g_controller)
            {
                return;
            }
            DestroyWindow(g_controller);
            g_controller = nullptr;
        }
    };

    struct ScopedAudio
    {
        ~ScopedAudio()
        {
            g_rainAudio.Shutdown();
        }
    };

    void ApplySound();
    void EnsureTrayIcon(HWND hwnd);
    ATOM RegisterOverlayClass(HINSTANCE instance);
    ATOM RegisterControllerClass(HINSTANCE instance);
    bool CreateOverlayForMonitor(HMONITOR monitor);
    bool CreateAllOverlays();
    LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT CALLBACK ControllerWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE,
    _In_ LPWSTR,
    _In_ int)
{
    ScopedMutex instanceMutex;
    instanceMutex.handle = CreateMutexW(nullptr, FALSE,
        L"Local\\Rainfall.SingleInstance.{4A0518FE-ECD4-4FBE-B9E8-317957DAE93C}");
    if (instanceMutex.handle && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (HWND existing = FindWindowW(kControllerClassName, nullptr))
        {
            PostMessageW(existing, WM_APP_ALREADY_RUNNING, 0, 0);
        }
        return 0;
    }

    g_instance = hInstance;

    ScopedCom com;
    if (!com)
    {
        return 1;
    }

    g_settings.Load();

    ScopedRendererFactory factory;
    if (!factory || !RegisterOverlayClass(hInstance) || !RegisterControllerClass(hInstance))
    {
        return 1;
    }

    g_controller = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        kControllerClassName,
        L"Rainfall",
        WS_POPUP,
        0,
        0,
        0,
        0,
        nullptr,
        nullptr,
        hInstance,
        nullptr);
    if (!g_controller)
    {
        return 1;
    }
    ScopedControllerWindow controllerWindow;

    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    if (g_taskbarCreatedMsg != 0)
    {
        ChangeWindowMessageFilterEx(g_controller, g_taskbarCreatedMsg, MSGFLT_ALLOW, nullptr);
    }

    if (!CreateAllOverlays())
    {
        return 1;
    }

    EnsureTrayIcon(g_controller);
    QueryPerformanceFrequency(&g_qpcFrequency);

    ScopedAudio audio;
    if (g_rainAudio.Initialize())
    {
        ApplySound();
    }

    if (!g_frameClock.Start(g_controller, WM_APP_FRAME, kFrameIntervalMs))
    {
        g_usingFallbackTimer = true;
        SetTimer(g_controller, kFallbackFrameTimerId, kFrameIntervalMs, nullptr);
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        if (g_aboutDlg && IsDialogMessageW(g_aboutDlg, &msg))
        {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return static_cast<int>(msg.wParam);
}

namespace
{
    const std::wstring& Settings::ConfigPath()
    {
        if (configResolved)
        {
            return configPath;
        }

        configResolved = true;
        PWSTR appData = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)))
        {
            configPath = appData;
            CoTaskMemFree(appData);
            configPath += L"\\Rainfall";
            CreateDirectoryW(configPath.c_str(), nullptr);
            configPath += L"\\config.ini";
        }
        return configPath;
    }

    void Settings::Load()
    {
        const std::wstring& path = ConfigPath();
        if (path.empty())
        {
            return;
        }

        const wchar_t* file = path.c_str();
        lightMode = GetPrivateProfileIntW(kConfigSection, L"LightMode", 0, file) != 0;
        windRight = GetPrivateProfileIntW(kConfigSection, L"WindRight", 1, file) != 0;
        soundEnabled = GetPrivateProfileIntW(kConfigSection, L"SoundEnabled", 1, file) != 0;
        alwaysOnTop = GetPrivateProfileIntW(kConfigSection, L"AlwaysOnTop", 1, file) != 0;
        for (LevelSetting& setting : levels)
        {
            setting.level = std::clamp(
                static_cast<int>(GetPrivateProfileIntW(kConfigSection, setting.iniKey, kDefaultLevel, file)),
                0, kLevelCount - 1);
        }
    }

    void WriteProfileBool(const wchar_t* key, bool value, const wchar_t* path)
    {
        WritePrivateProfileStringW(kConfigSection, key, value ? L"1" : L"0", path);
    }

    void WriteProfileInt(const wchar_t* key, int value, const wchar_t* path)
    {
        wchar_t buf[16];
        _itow_s(value, buf, 10);
        WritePrivateProfileStringW(kConfigSection, key, buf, path);
    }

    void Settings::Save()
    {
        const std::wstring& path = ConfigPath();
        if (path.empty())
        {
            return;
        }

        const wchar_t* file = path.c_str();
        WriteProfileBool(L"LightMode", lightMode, file);
        WriteProfileBool(L"WindRight", windRight, file);
        WriteProfileBool(L"SoundEnabled", soundEnabled, file);
        WriteProfileBool(L"AlwaysOnTop", alwaysOnTop, file);
        for (const LevelSetting& setting : levels)
        {
            WriteProfileInt(setting.iniKey, setting.level, file);
        }
    }

    ATOM RegisterOverlayClass(HINSTANCE instance)
    {
        WNDCLASSEXW wcex{};
        wcex.cbSize = sizeof(WNDCLASSEXW);
        wcex.style = CS_HREDRAW | CS_VREDRAW;
        wcex.lpfnWndProc = OverlayWndProc;
        wcex.hInstance = instance;
        wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wcex.hbrBackground = nullptr;
        wcex.lpszClassName = kWindowClassName;
        return RegisterClassExW(&wcex);
    }

    ATOM RegisterControllerClass(HINSTANCE instance)
    {
        WNDCLASSEXW wcex{};
        wcex.cbSize = sizeof(WNDCLASSEXW);
        wcex.lpfnWndProc = ControllerWndProc;
        wcex.hInstance = instance;
        wcex.lpszClassName = kControllerClassName;
        return RegisterClassExW(&wcex);
    }

    int PixelsToDips(int pixels, UINT dpi)
    {
        if (dpi == 0)
        {
            dpi = 96;
        }
        return MulDiv(pixels, 96, static_cast<int>(dpi));
    }

    void ApplyLevel(const LevelSetting& setting)
    {
        const float scale = setting.scales[setting.level];
        for (const auto& overlay : g_overlays)
        {
            if (overlay && overlay->rainSystem)
            {
                std::invoke(setting.apply, *overlay->rainSystem, scale);
            }
        }
    }

    void ApplyWindDirection()
    {
        const int direction = g_settings.windRight ? 1 : -1;
        for (const auto& overlay : g_overlays)
        {
            if (overlay && overlay->rainSystem)
            {
                overlay->rainSystem->SetWindDirection(direction);
            }
        }
    }

    void ApplySettingsTo(OverlayWindow& overlay)
    {
        if (overlay.rainSystem)
        {
            for (const LevelSetting& setting : g_settings.levels)
            {
                std::invoke(setting.apply, *overlay.rainSystem, setting.scales[setting.level]);
            }
            overlay.rainSystem->SetWindDirection(g_settings.windRight ? 1 : -1);
        }
        if (overlay.renderer)
        {
            overlay.renderer->SetLightMode(g_settings.lightMode);
        }
    }

    void ApplyLightMode()
    {
        for (const auto& overlay : g_overlays)
        {
            if (!overlay || !overlay->hwnd || !overlay->renderer || !overlay->rainSystem)
            {
                continue;
            }

            overlay->renderer->SetLightMode(g_settings.lightMode);
            overlay->renderer->Render(*overlay->rainSystem);
        }
    }

    void ApplySound()
    {
        // 雨声强度由"雨势"和"雨滴密度"共同决定：雨越大越密，声音越大越急
        const float speedT = static_cast<float>(g_settings.Level(LevelKind::Speed).level) / (kLevelCount - 1);
        const float densityT = static_cast<float>(g_settings.Level(LevelKind::Density).level) / (kLevelCount - 1);
        g_rainAudio.SetIntensity(0.65f * speedT + 0.35f * densityT);
        g_rainAudio.SetEnabled(g_settings.soundEnabled);
        g_rainAudio.SetPaused(g_paused);
    }

    void ApplyAlwaysOnTop()
    {
        const HWND zOrder = g_settings.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST;
        for (const auto& overlay : g_overlays)
        {
            if (!overlay || !overlay->hwnd)
            {
                continue;
            }

            SetWindowPos(overlay->hwnd, zOrder, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }

    BOOL CALLBACK MonitorEnumProc(HMONITOR monitor, HDC, LPRECT, LPARAM)
    {
        CreateOverlayForMonitor(monitor);
        return TRUE;
    }

    bool CreateOverlayForMonitor(HMONITOR monitor)
    {
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(MONITORINFO);
        if (!GetMonitorInfo(monitor, &monitorInfo))
        {
            return false;
        }

        const RECT& rect = monitorInfo.rcMonitor;
        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;
        if (width <= 0 || height <= 0)
        {
            return false;
        }

        auto overlay = std::make_unique<OverlayWindow>();
        overlay->monitorRect = rect;
        overlay->rainSystem = std::make_unique<RainSystem>();
        overlay->renderer = std::make_unique<RainRenderer>();

        DWORD exStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
        if (g_settings.alwaysOnTop)
        {
            exStyle |= WS_EX_TOPMOST;
        }

        HWND hwnd = CreateWindowExW(
            exStyle,
            kWindowClassName,
            L"Rainfall",
            WS_POPUP,
            rect.left,
            rect.top,
            width,
            height,
            nullptr,
            nullptr,
            g_instance,
            overlay.get());
        if (!hwnd)
        {
            return false;
        }

        UINT dpi = GetDpiForWindow(hwnd);
        if (dpi == 0)
        {
            dpi = 96;
        }
        overlay->dpi = dpi;
        // 尺寸还是 0，SetDensityScale 不会重建；紧接着的 Initialize 只生成一次雨滴。
        ApplySettingsTo(*overlay);

        if (!overlay->renderer->Initialize(hwnd, width, height, dpi))
        {
            DestroyWindow(hwnd);
            return false;
        }

        overlay->rainSystem->Initialize(PixelsToDips(width, dpi), PixelsToDips(height, dpi));

        g_overlays.push_back(std::move(overlay));
        OverlayWindow& created = *g_overlays.back();
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        created.renderer->Render(*created.rainSystem);
        return true;
    }

    bool CreateAllOverlays()
    {
        if (!EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, 0))
        {
            return false;
        }

        return !g_overlays.empty();
    }

    void DestroyAllOverlays()
    {
        for (auto& overlay : g_overlays)
        {
            if (overlay && overlay->hwnd)
            {
                DestroyWindow(overlay->hwnd);
            }
        }

        g_overlays.clear();
    }

    void RebuildOverlays()
    {
        DestroyAllOverlays();
        EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, 0);
        g_lastFrameQpc = 0;
    }

    void TickFrame()
    {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);

        float dt = 1.0f / 60.0f;
        if (g_lastFrameQpc != 0 && g_qpcFrequency.QuadPart > 0)
        {
            const double seconds = static_cast<double>(now.QuadPart - g_lastFrameQpc)
                / static_cast<double>(g_qpcFrequency.QuadPart);
            dt = static_cast<float>(seconds);
        }
        g_lastFrameQpc = now.QuadPart;
        dt = std::clamp(dt, 0.0f, 0.1f);

        for (const auto& overlay : g_overlays)
        {
            if (!overlay || !overlay->hwnd || !overlay->rainSystem || !overlay->renderer)
            {
                continue;
            }

            overlay->rainSystem->Update(dt);
            overlay->renderer->Render(*overlay->rainSystem);
        }
    }

    bool AddTrayIcon(HWND hwnd)
    {
        if (!hwnd)
        {
            return false;
        }

        UINT dpi = GetDpiForWindow(hwnd);
        if (dpi == 0)
        {
            dpi = 96;
        }

        int cx = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
        int cy = GetSystemMetricsForDpi(SM_CYSMICON, dpi);
        if (cx <= 0)
        {
            cx = 16;
        }
        if (cy <= 0)
        {
            cy = 16;
        }

        HICON icon = static_cast<HICON>(LoadImageW(
            g_instance, MAKEINTRESOURCEW(IDI_RAINFALL), IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR));
        const bool ownsIcon = icon != nullptr;
        if (!icon)
        {
            icon = LoadIconW(g_instance, MAKEINTRESOURCEW(IDI_RAINFALL));
        }
        if (!icon)
        {
            return false;
        }

        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(NOTIFYICONDATAW);
        nid.hWnd = hwnd;
        nid.uID = 1;
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        nid.uCallbackMessage = WM_TRAYICON;
        nid.hIcon = icon;
        wcscpy_s(nid.szTip, L"Rainfall - 屏幕雨幕");

        if (Shell_NotifyIconW(NIM_ADD, &nid) != TRUE)
        {
            if (ownsIcon)
            {
                DestroyIcon(icon);
            }
            g_trayAdded = false;
            return false;
        }

        if (g_trayIcon && g_trayIcon != icon)
        {
            DestroyIcon(g_trayIcon);
        }
        g_trayIcon = ownsIcon ? icon : nullptr;
        g_trayAdded = true;
        return true;
    }

    void RemoveTrayIcon(HWND hwnd)
    {
        if (!g_trayAdded || !hwnd)
        {
            return;
        }

        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(NOTIFYICONDATAW);
        nid.hWnd = hwnd;
        nid.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        g_trayAdded = false;
    }

    void DestroyTrayIconHandle()
    {
        if (g_trayIcon)
        {
            DestroyIcon(g_trayIcon);
            g_trayIcon = nullptr;
        }
    }

    void EnsureTrayIcon(HWND hwnd)
    {
        if (!hwnd || g_trayAdded)
        {
            if (hwnd)
            {
                KillTimer(hwnd, kTrayRetryTimerId);
            }
            return;
        }

        if (AddTrayIcon(hwnd))
        {
            g_trayRetries = 0;
            KillTimer(hwnd, kTrayRetryTimerId);
            return;
        }

        if (g_trayRetries >= kTrayRetryLimit)
        {
            KillTimer(hwnd, kTrayRetryTimerId);
            return;
        }

        ++g_trayRetries;
        SetTimer(hwnd, kTrayRetryTimerId, 2000, nullptr);
    }

    void ToggleLightMode()
    {
        g_settings.lightMode = !g_settings.lightMode;
        ApplyLightMode();
        g_settings.Save();
    }

    void ToggleSound()
    {
        g_settings.soundEnabled = !g_settings.soundEnabled;
        ApplySound();
        g_settings.Save();
    }

    void ToggleAlwaysOnTop()
    {
        g_settings.alwaysOnTop = !g_settings.alwaysOnTop;
        ApplyAlwaysOnTop();
        g_settings.Save();
    }

    void SetLevel(LevelSetting& setting, int level)
    {
        setting.level = std::clamp(level, 0, kLevelCount - 1);
        ApplyLevel(setting);
        if (setting.affectsSound)
        {
            ApplySound();
        }
        g_settings.Save();
    }

    void SetWindDirection(bool right)
    {
        g_settings.windRight = right;
        ApplyWindDirection();
        g_settings.Save();
    }

    void TogglePause()
    {
        g_paused = !g_paused;
        g_rainAudio.SetPaused(g_paused);

        if (g_paused)
        {
            g_frameClock.SetRunning(false);
            if (g_usingFallbackTimer && g_controller)
            {
                KillTimer(g_controller, kFallbackFrameTimerId);
            }
            return;
        }

        g_lastFrameQpc = 0;
        g_frameClock.SetRunning(true);
        if (g_usingFallbackTimer && g_controller)
        {
            SetTimer(g_controller, kFallbackFrameTimerId, kFrameIntervalMs, nullptr);
        }
    }

    void ShowTrayMenu(HWND hwnd)
    {
        HMENU menu = CreatePopupMenu();
        if (!menu)
        {
            return;
        }

        const wchar_t* pauseLabel = g_paused ? L"继续(&R)" : L"暂停(&P)";
        AppendMenuW(menu, MF_STRING, IDM_PAUSE, pauseLabel);
        AppendMenuW(menu, MF_STRING | (g_settings.lightMode ? MF_CHECKED : MF_UNCHECKED),
            IDM_LIGHT_MODE, L"浅色背景增强(&L)");
        AppendMenuW(menu, MF_STRING | (g_settings.soundEnabled ? MF_CHECKED : MF_UNCHECKED),
            IDM_SOUND, L"雨声(&V)");
        AppendMenuW(menu, MF_STRING | (g_settings.alwaysOnTop ? MF_CHECKED : MF_UNCHECKED),
            IDM_ALWAYS_ON_TOP, L"总在最前(&T)");

        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

        const wchar_t* levelLabels[kLevelCount] = { L"1 档", L"2 档", L"默认", L"4 档", L"5 档" };
        for (const LevelSetting& setting : g_settings.levels)
        {
            HMENU sub = CreatePopupMenu();
            if (!sub)
            {
                continue;
            }
            for (int i = 0; i < kLevelCount; ++i)
            {
                AppendMenuW(sub, MF_STRING | (i == setting.level ? MF_CHECKED : MF_UNCHECKED),
                    setting.idBase + i, levelLabels[i]);
            }
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), setting.menuLabel);
        }

        HMENU dirMenu = CreatePopupMenu();
        if (dirMenu)
        {
            AppendMenuW(dirMenu, MF_STRING | (g_settings.windRight ? MF_CHECKED : MF_UNCHECKED),
                IDM_DIR_RIGHT, L"向右下");
            AppendMenuW(dirMenu, MF_STRING | (!g_settings.windRight ? MF_CHECKED : MF_UNCHECKED),
                IDM_DIR_LEFT, L"向左下");
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(dirMenu), L"风向(&F)");
        }

        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_ABOUT, L"关于(&A)");
        AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, L"退出(&X)");

        SetForegroundWindow(hwnd);
        POINT cursor{};
        GetCursorPos(&cursor);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
            cursor.x, cursor.y, 0, hwnd, nullptr);
        PostMessageW(hwnd, WM_NULL, 0, 0);
        DestroyMenu(menu);
    }

    void CenterWindowOnScreen(HWND wnd)
    {
        RECT rc{};
        if (!GetWindowRect(wnd, &rc))
        {
            return;
        }

        const int width = rc.right - rc.left;
        const int height = rc.bottom - rc.top;
        const int screenW = GetSystemMetrics(SM_CXSCREEN);
        const int screenH = GetSystemMetrics(SM_CYSCREEN);
        const int x = (screenW - width) / 2;
        const int y = (screenH - height) / 2;
        SetWindowPos(wnd, HWND_TOP, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }

    void CopyTextToClipboard(const wchar_t* text)
    {
        if (!text || !OpenClipboard(nullptr))
        {
            return;
        }

        EmptyClipboard();

        const size_t bytes = (wcslen(text) + 1) * sizeof(wchar_t);
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (mem)
        {
            void* dst = GlobalLock(mem);
            if (dst)
            {
                memcpy(dst, text, bytes);
                GlobalUnlock(mem);
                if (!SetClipboardData(CF_UNICODETEXT, mem))
                {
                    GlobalFree(mem);
                }
            }
            else
            {
                GlobalFree(mem);
            }
        }

        CloseClipboard();
    }

    LRESULT CALLBACK ToastWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    void RegisterToastClass()
    {
        static bool registered = false;
        if (registered)
        {
            return;
        }

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.lpfnWndProc = ToastWndProc;
        wc.hInstance = g_instance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = CreateSolidBrush(RGB(50, 50, 50));
        wc.lpszClassName = kToastClassName;
        RegisterClassExW(&wc);
        registered = true;
    }

    LRESULT CALLBACK ToastWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_PAINT:
        {
            const auto* data = reinterpret_cast<const ToastData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);

            RECT rc{};
            GetClientRect(hwnd, &rc);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255, 255, 255));
            HFONT oldFont = nullptr;
            if (data && data->font)
            {
                oldFont = static_cast<HFONT>(SelectObject(dc, data->font));
            }
            if (data)
            {
                DrawTextW(dc, data->text.c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            if (oldFont)
            {
                SelectObject(dc, oldFont);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_TIMER:
            if (wParam == kToastTimerId)
            {
                DestroyWindow(hwnd);
            }
            return 0;
        case WM_NCDESTROY:
        {
            auto* data = reinterpret_cast<ToastData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (data)
            {
                if (data->ownsFont && data->font)
                {
                    DeleteObject(data->font);
                }
                delete data;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            }
            if (g_toastWnd == hwnd)
            {
                g_toastWnd = nullptr;
            }
            break;
        }
        default:
            break;
        }

        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    void ShowToast(const wchar_t* message)
    {
        RegisterToastClass();

        if (g_toastWnd)
        {
            const HWND old = g_toastWnd;
            KillTimer(old, kToastTimerId);
            DestroyWindow(old);
            g_toastWnd = nullptr;
        }

        constexpr int baseW = 300;
        constexpr int baseH = 44;
        constexpr int baseMargin = 80;

        g_toastWnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kToastClassName,
            L"",
            WS_POPUP | WS_BORDER,
            0,
            0,
            baseW,
            baseH,
            nullptr,
            nullptr,
            g_instance,
            nullptr);
        if (!g_toastWnd)
        {
            return;
        }

        UINT dpi = GetDpiForWindow(g_toastWnd);
        if (dpi == 0)
        {
            dpi = 96;
        }

        const int toastW = MulDiv(baseW, static_cast<int>(dpi), 96);
        const int toastH = MulDiv(baseH, static_cast<int>(dpi), 96);
        const int margin = MulDiv(baseMargin, static_cast<int>(dpi), 96);
        const int screenW = GetSystemMetrics(SM_CXSCREEN);
        const int screenH = GetSystemMetrics(SM_CYSCREEN);
        const int x = (screenW - toastW) / 2;
        const int y = screenH - toastH - margin;
        SetWindowPos(g_toastWnd, HWND_TOPMOST, x, y, toastW, toastH, SWP_NOACTIVATE);

        auto* data = new (std::nothrow) ToastData{};
        if (!data)
        {
            DestroyWindow(g_toastWnd);
            g_toastWnd = nullptr;
            return;
        }
        data->text = message ? message : L"";

        NONCLIENTMETRICS ncm{};
        ncm.cbSize = sizeof(NONCLIENTMETRICS);
        if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi))
        {
            data->font = CreateFontIndirectW(&ncm.lfMessageFont);
            data->ownsFont = data->font != nullptr;
        }
        if (!data->font)
        {
            data->font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        }

        SetWindowLongPtrW(g_toastWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(data));
        ShowWindow(g_toastWnd, SW_SHOWNOACTIVATE);
        UpdateWindow(g_toastWnd);
        SetTimer(g_toastWnd, kToastTimerId, 2000, nullptr);
    }

    INT_PTR CALLBACK AboutDlgProc(HWND dlg, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_INITDIALOG:
            g_aboutDlg = dlg;
            CenterWindowOnScreen(dlg);
            return TRUE;
        case WM_SETCURSOR:
            if (reinterpret_cast<HWND>(lParam) == GetDlgItem(dlg, IDC_ABOUT_EMAIL))
            {
                SetCursor(LoadCursor(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_CTLCOLORSTATIC:
            if (reinterpret_cast<HWND>(lParam) == GetDlgItem(dlg, IDC_ABOUT_EMAIL))
            {
                HDC dc = reinterpret_cast<HDC>(wParam);
                SetTextColor(dc, RGB(0, 102, 204));
                SetBkMode(dc, TRANSPARENT);
                return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_3DFACE));
            }
            break;
        case WM_CLOSE:
            DestroyWindow(dlg);
            return TRUE;
        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_ABOUT_EMAIL && HIWORD(wParam) == STN_CLICKED)
            {
                CopyTextToClipboard(kAuthorEmail);
                ShowToast(L"邮箱已复制到剪贴板");
                return TRUE;
            }
            if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
            {
                DestroyWindow(dlg);
                return TRUE;
            }
            break;
        case WM_DESTROY:
            if (g_aboutDlg == dlg)
            {
                g_aboutDlg = nullptr;
            }
            break;
        default:
            break;
        }
        return FALSE;
    }

    void ShowAboutDialog(HWND owner)
    {
        if (g_aboutDlg)
        {
            SetForegroundWindow(g_aboutDlg);
            return;
        }

        HWND dlg = CreateDialogW(g_instance, MAKEINTRESOURCEW(IDD_ABOUTBOX), owner, AboutDlgProc);
        if (!dlg)
        {
            g_aboutDlg = nullptr;
            return;
        }

        g_aboutDlg = dlg;
        ShowWindow(dlg, SW_SHOW);
        SetForegroundWindow(dlg);
    }

    LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_NCCREATE)
        {
            auto* created = static_cast<OverlayWindow*>(
                reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            if (created)
            {
                created->hwnd = hwnd;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created));
            }
        }

        auto* overlay = reinterpret_cast<OverlayWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        if (message == WM_DPICHANGED)
        {
            if (overlay && overlay->dpi != 0 && LOWORD(wParam) != overlay->dpi && g_controller)
            {
                PostMessageW(g_controller, WM_APP_REBUILD_OVERLAYS, 0, 0);
            }
            return 0;
        }

        if (message == WM_NCDESTROY && overlay)
        {
            overlay->hwnd = nullptr;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }

        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    LRESULT CALLBACK ControllerWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (g_taskbarCreatedMsg != 0 && message == g_taskbarCreatedMsg)
        {
            g_trayAdded = false;
            g_trayRetries = 0;
            EnsureTrayIcon(hwnd);
            return 0;
        }

        switch (message)
        {
        case WM_APP_FRAME:
            if (!g_paused)
            {
                TickFrame();
            }
            g_frameClock.Acknowledge();
            return 0;

        case WM_APP_REBUILD_OVERLAYS:
        case WM_DISPLAYCHANGE:
            SetTimer(hwnd, kRebuildTimerId, 500, nullptr);
            return 0;

        case WM_APP_ALREADY_RUNNING:
            ShowToast(L"Rainfall 已在运行，可在系统托盘中设置");
            return 0;

        case WM_TIMER:
            if (wParam == kRebuildTimerId)
            {
                KillTimer(hwnd, kRebuildTimerId);
                RebuildOverlays();
                return 0;
            }
            if (wParam == kTrayRetryTimerId)
            {
                EnsureTrayIcon(hwnd);
                return 0;
            }
            if (wParam == kFallbackFrameTimerId && !g_paused)
            {
                TickFrame();
            }
            return 0;

        case WM_TRAYICON:
            if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU)
            {
                ShowTrayMenu(hwnd);
            }
            return 0;

        case WM_COMMAND:
        {
            const UINT command = LOWORD(wParam);
            for (LevelSetting& setting : g_settings.levels)
            {
                if (command >= setting.idBase && command < setting.idBase + static_cast<UINT>(kLevelCount))
                {
                    SetLevel(setting, static_cast<int>(command - setting.idBase));
                    return 0;
                }
            }
            switch (command)
            {
            case IDM_PAUSE:
                TogglePause();
                return 0;
            case IDM_LIGHT_MODE:
                ToggleLightMode();
                return 0;
            case IDM_SOUND:
                ToggleSound();
                return 0;
            case IDM_ALWAYS_ON_TOP:
                ToggleAlwaysOnTop();
                return 0;
            case IDM_DIR_RIGHT:
                SetWindDirection(true);
                return 0;
            case IDM_DIR_LEFT:
                SetWindDirection(false);
                return 0;
            case IDM_ABOUT:
                ShowAboutDialog(hwnd);
                return 0;
            case IDM_TRAY_EXIT:
                DestroyWindow(hwnd);
                return 0;
            default:
                break;
            }
            break;
        }

        case WM_DESTROY:
            g_frameClock.Stop();
            KillTimer(hwnd, kRebuildTimerId);
            KillTimer(hwnd, kTrayRetryTimerId);
            KillTimer(hwnd, kFallbackFrameTimerId);
            RemoveTrayIcon(hwnd);
            DestroyTrayIconHandle();
            if (g_toastWnd)
            {
                DestroyWindow(g_toastWnd);
                g_toastWnd = nullptr;
            }
            if (g_aboutDlg)
            {
                DestroyWindow(g_aboutDlg);
                g_aboutDlg = nullptr;
            }
            DestroyAllOverlays();
            g_controller = nullptr;
            PostQuitMessage(0);
            return 0;

        default:
            break;
        }

        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}
