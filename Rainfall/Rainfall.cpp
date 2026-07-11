#include "framework.h"
#include "Rainfall.h"
#include "RainSystem.h"
#include "RainRenderer.h"

#include <shlobj.h>
#include <string>
#include <algorithm>

namespace
{
    constexpr UINT WM_TRAYICON = WM_APP + 1;
    constexpr UINT_PTR TIMER_ID = 1;
    constexpr UINT TIMER_INTERVAL_MS = 16;
    constexpr wchar_t kWindowClassName[] = L"RainfallOverlayClass";

    HINSTANCE g_instance = nullptr;
    bool g_paused = false;
    bool g_lightMode = false;
    bool g_trayAdded = false;
    int g_aliveWindows = 0;

    // 5 挡：索引 0~4，默认第 3 挡（索引 2）等于原始效果
    constexpr int kLevelCount = 5;
    constexpr int kDefaultLevel = 2;
    constexpr float kLengthScales[kLevelCount] = { 0.5f, 0.75f, 1.0f, 1.5f, 2.0f };
    constexpr float kDensityScales[kLevelCount] = { 0.4f, 0.7f, 1.0f, 1.5f, 2.0f };
    // 风力：角度倍率，越大越倾斜（第 3 挡 = 原始轻微倾斜）
    constexpr float kWindScales[kLevelCount] = { 0.3f, 0.6f, 1.0f, 2.5f, 4.5f };
    // 雨势：速度倍率，越大落得越快
    constexpr float kSpeedScales[kLevelCount] = { 0.5f, 0.75f, 1.0f, 1.5f, 2.2f };

    int g_lengthLevel = kDefaultLevel;
    int g_densityLevel = kDefaultLevel;
    int g_windLevel = kDefaultLevel;
    int g_speedLevel = kDefaultLevel;

    struct OverlayWindow
    {
        HWND hwnd = nullptr;
        RECT monitorRect{};
        std::unique_ptr<RainSystem> rainSystem;
        std::unique_ptr<RainRenderer> renderer;
    };

    std::vector<OverlayWindow> g_overlays;

    void ShowTrayMenu(HWND hwnd);
    bool AddTrayIcon(HWND hwnd);
    void RemoveTrayIcon(HWND hwnd);
    void TogglePause();
    void ToggleLightMode();
    void ApplyLightMode();
    void ApplyLength();
    void ApplyDensity();
    void ApplyWind();
    void ApplySpeed();
    void SetLengthLevel(int level);
    void SetDensityLevel(int level);
    void SetWindLevel(int level);
    void SetSpeedLevel(int level);
    std::wstring GetConfigPath();
    void LoadConfig();
    void SaveConfig();
    void DestroyAllOverlays();
    bool CreateOverlayForMonitor(HMONITOR monitor);
    BOOL CALLBACK MonitorEnumProc(HMONITOR monitor, HDC, LPRECT, LPARAM);
    LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    ATOM RegisterOverlayClass(HINSTANCE instance);
    bool CreateAllOverlays(HINSTANCE instance);
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE,
    _In_ LPWSTR,
    _In_ int)
{
    g_instance = hInstance;

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
    {
        return 1;
    }

    LoadConfig();

    if (!RainRenderer::InitializeFactory())
    {
        CoUninitialize();
        return 1;
    }

    if (!RegisterOverlayClass(hInstance))
    {
        RainRenderer::ShutdownFactory();
        CoUninitialize();
        return 1;
    }

    if (!CreateAllOverlays(hInstance))
    {
        RainRenderer::ShutdownFactory();
        CoUninitialize();
        return 1;
    }

    if (!AddTrayIcon(g_overlays.empty() ? nullptr : g_overlays.front().hwnd))
    {
        DestroyAllOverlays();
        RainRenderer::ShutdownFactory();
        CoUninitialize();
        return 1;
    }

    ApplyLength();
    ApplyDensity();
    ApplyWind();
    ApplySpeed();
    ApplyLightMode();

    MSG msg{};
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    RemoveTrayIcon(g_overlays.empty() ? nullptr : g_overlays.front().hwnd);
    DestroyAllOverlays();
    RainRenderer::ShutdownFactory();
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}

namespace
{
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

        HWND hwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
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
            nullptr);

        if (!hwnd)
        {
            return false;
        }

        OverlayWindow overlay{};
        overlay.hwnd = hwnd;
        overlay.monitorRect = rect;
        overlay.rainSystem = std::make_unique<RainSystem>();
        overlay.renderer = std::make_unique<RainRenderer>();

        overlay.rainSystem->Initialize(width, height);
        if (!overlay.renderer->Initialize(hwnd, width, height))
        {
            DestroyWindow(hwnd);
            return false;
        }

        SetWindowLongPtr(hwnd, GWLP_USERDATA, static_cast<LONG_PTR>(g_overlays.size()));
        g_overlays.push_back(std::move(overlay));
        ++g_aliveWindows;

        OverlayWindow& created = g_overlays.back();
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        SetTimer(hwnd, TIMER_ID, TIMER_INTERVAL_MS, nullptr);

        created.rainSystem->Update();
        created.renderer->Render(*created.rainSystem);
        return true;
    }

    bool CreateAllOverlays(HINSTANCE)
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
            if (overlay.hwnd)
            {
                KillTimer(overlay.hwnd, TIMER_ID);
                DestroyWindow(overlay.hwnd);
                overlay.hwnd = nullptr;
            }
        }

        g_overlays.clear();
        g_aliveWindows = 0;
    }

    bool AddTrayIcon(HWND hwnd)
    {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(NOTIFYICONDATAW);
        nid.hWnd = hwnd;
        nid.uID = 1;
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        nid.uCallbackMessage = WM_TRAYICON;
        nid.hIcon = LoadIcon(g_instance, MAKEINTRESOURCE(IDI_RAINFALL));
        wcscpy_s(nid.szTip, L"Rainfall - 屏幕雨幕");

        g_trayAdded = Shell_NotifyIconW(NIM_ADD, &nid) == TRUE;
        return g_trayAdded;
    }

    void RemoveTrayIcon(HWND hwnd)
    {
        if (!g_trayAdded)
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

    std::wstring GetConfigPath()
    {
        PWSTR appData = nullptr;
        std::wstring path;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)))
        {
            path = appData;
            CoTaskMemFree(appData);
            path += L"\\Rainfall";
            CreateDirectoryW(path.c_str(), nullptr);
            path += L"\\config.ini";
        }
        return path;
    }

    void LoadConfig()
    {
        const std::wstring path = GetConfigPath();
        if (path.empty())
        {
            return;
        }

        g_lightMode = GetPrivateProfileIntW(L"Rainfall", L"LightMode", 0, path.c_str()) != 0;
        g_lengthLevel = std::clamp(
            static_cast<int>(GetPrivateProfileIntW(L"Rainfall", L"LengthLevel", kDefaultLevel, path.c_str())),
            0, kLevelCount - 1);
        g_densityLevel = std::clamp(
            static_cast<int>(GetPrivateProfileIntW(L"Rainfall", L"DensityLevel", kDefaultLevel, path.c_str())),
            0, kLevelCount - 1);
        g_windLevel = std::clamp(
            static_cast<int>(GetPrivateProfileIntW(L"Rainfall", L"WindLevel", kDefaultLevel, path.c_str())),
            0, kLevelCount - 1);
        g_speedLevel = std::clamp(
            static_cast<int>(GetPrivateProfileIntW(L"Rainfall", L"SpeedLevel", kDefaultLevel, path.c_str())),
            0, kLevelCount - 1);
    }

    void SaveConfig()
    {
        const std::wstring path = GetConfigPath();
        if (path.empty())
        {
            return;
        }

        WritePrivateProfileStringW(L"Rainfall", L"LightMode", g_lightMode ? L"1" : L"0", path.c_str());

        wchar_t buf[16];
        _itow_s(g_lengthLevel, buf, 10);
        WritePrivateProfileStringW(L"Rainfall", L"LengthLevel", buf, path.c_str());
        _itow_s(g_densityLevel, buf, 10);
        WritePrivateProfileStringW(L"Rainfall", L"DensityLevel", buf, path.c_str());
        _itow_s(g_windLevel, buf, 10);
        WritePrivateProfileStringW(L"Rainfall", L"WindLevel", buf, path.c_str());
        _itow_s(g_speedLevel, buf, 10);
        WritePrivateProfileStringW(L"Rainfall", L"SpeedLevel", buf, path.c_str());
    }

    void ApplyLightMode()
    {
        // 浅色模式下用中间灰，深浅背景都能看见；关闭后恢复纯白
        const float gray = g_lightMode ? 0.85f : 1.0f;

        for (auto& overlay : g_overlays)
        {
            if (!overlay.hwnd || !overlay.renderer)
            {
                continue;
            }

            overlay.renderer->SetDropColor(gray, gray, gray);
            overlay.renderer->Render(*overlay.rainSystem);
        }
    }

    void ToggleLightMode()
    {
        g_lightMode = !g_lightMode;
        ApplyLightMode();
        SaveConfig();
    }

    void ApplyLength()
    {
        const float scale = kLengthScales[g_lengthLevel];
        for (auto& overlay : g_overlays)
        {
            if (overlay.rainSystem)
            {
                overlay.rainSystem->SetLengthScale(scale);
            }
        }
    }

    void ApplyDensity()
    {
        const float scale = kDensityScales[g_densityLevel];
        for (auto& overlay : g_overlays)
        {
            if (overlay.rainSystem)
            {
                overlay.rainSystem->SetDensityScale(scale);
            }
        }
    }

    void SetLengthLevel(int level)
    {
        g_lengthLevel = std::clamp(level, 0, kLevelCount - 1);
        ApplyLength();
        SaveConfig();
    }

    void SetDensityLevel(int level)
    {
        g_densityLevel = std::clamp(level, 0, kLevelCount - 1);
        ApplyDensity();
        SaveConfig();
    }

    void ApplyWind()
    {
        const float scale = kWindScales[g_windLevel];
        for (auto& overlay : g_overlays)
        {
            if (overlay.rainSystem)
            {
                overlay.rainSystem->SetWindScale(scale);
            }
        }
    }

    void ApplySpeed()
    {
        const float scale = kSpeedScales[g_speedLevel];
        for (auto& overlay : g_overlays)
        {
            if (overlay.rainSystem)
            {
                overlay.rainSystem->SetSpeedScale(scale);
            }
        }
    }

    void SetWindLevel(int level)
    {
        g_windLevel = std::clamp(level, 0, kLevelCount - 1);
        ApplyWind();
        SaveConfig();
    }

    void SetSpeedLevel(int level)
    {
        g_speedLevel = std::clamp(level, 0, kLevelCount - 1);
        ApplySpeed();
        SaveConfig();
    }

    void TogglePause()
    {
        g_paused = !g_paused;

        for (auto& overlay : g_overlays)
        {
            if (!overlay.hwnd)
            {
                continue;
            }

            if (g_paused)
            {
                KillTimer(overlay.hwnd, TIMER_ID);
            }
            else
            {
                SetTimer(overlay.hwnd, TIMER_ID, TIMER_INTERVAL_MS, nullptr);
            }
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
        AppendMenuW(menu, MF_STRING | (g_lightMode ? MF_CHECKED : MF_UNCHECKED),
            IDM_LIGHT_MODE, L"浅色背景增强(&L)");

        HMENU lengthMenu = CreatePopupMenu();
        HMENU densityMenu = CreatePopupMenu();
        HMENU windMenu = CreatePopupMenu();
        HMENU speedMenu = CreatePopupMenu();
        const wchar_t* levelLabels[kLevelCount] = { L"1 档", L"2 档", L"默认", L"4 档", L"5 档" };
        for (int i = 0; i < kLevelCount; ++i)
        {
            AppendMenuW(lengthMenu, MF_STRING | (i == g_lengthLevel ? MF_CHECKED : MF_UNCHECKED),
                IDM_LENGTH_BASE + i, levelLabels[i]);
            AppendMenuW(densityMenu, MF_STRING | (i == g_densityLevel ? MF_CHECKED : MF_UNCHECKED),
                IDM_DENSITY_BASE + i, levelLabels[i]);
            AppendMenuW(windMenu, MF_STRING | (i == g_windLevel ? MF_CHECKED : MF_UNCHECKED),
                IDM_WIND_BASE + i, levelLabels[i]);
            AppendMenuW(speedMenu, MF_STRING | (i == g_speedLevel ? MF_CHECKED : MF_UNCHECKED),
                IDM_SPEED_BASE + i, levelLabels[i]);
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(lengthMenu), L"雨滴长度(&G)");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(densityMenu), L"雨滴密度(&D)");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(windMenu), L"风力(&W)");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(speedMenu), L"雨势(&S)");

        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, L"退出(&X)");

        SetForegroundWindow(hwnd);

        POINT cursor{};
        GetCursorPos(&cursor);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
            cursor.x, cursor.y, 0, hwnd, nullptr);
        DestroyMenu(menu);
    }

    LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        const size_t index = static_cast<size_t>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        if (index >= g_overlays.size())
        {
            return DefWindowProc(hwnd, message, wParam, lParam);
        }

        OverlayWindow& overlay = g_overlays[index];

        switch (message)
        {
        case WM_TIMER:
            if (wParam == TIMER_ID && !g_paused)
            {
                overlay.rainSystem->Update();
                overlay.renderer->Render(*overlay.rainSystem);
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
            const int cmd = LOWORD(wParam);
            if (cmd >= IDM_LENGTH_BASE && cmd < IDM_LENGTH_BASE + kLevelCount)
            {
                SetLengthLevel(cmd - IDM_LENGTH_BASE);
                return 0;
            }
            if (cmd >= IDM_DENSITY_BASE && cmd < IDM_DENSITY_BASE + kLevelCount)
            {
                SetDensityLevel(cmd - IDM_DENSITY_BASE);
                return 0;
            }
            if (cmd >= IDM_WIND_BASE && cmd < IDM_WIND_BASE + kLevelCount)
            {
                SetWindLevel(cmd - IDM_WIND_BASE);
                return 0;
            }
            if (cmd >= IDM_SPEED_BASE && cmd < IDM_SPEED_BASE + kLevelCount)
            {
                SetSpeedLevel(cmd - IDM_SPEED_BASE);
                return 0;
            }
            switch (cmd)
            {
            case IDM_PAUSE:
                TogglePause();
                return 0;
            case IDM_LIGHT_MODE:
                ToggleLightMode();
                return 0;
            case IDM_TRAY_EXIT:
                RemoveTrayIcon(hwnd);
                DestroyAllOverlays();
                PostQuitMessage(0);
                return 0;
            default:
                break;
            }
            break;
        }

        case WM_DESTROY:
            KillTimer(hwnd, TIMER_ID);
            overlay.hwnd = nullptr;
            --g_aliveWindows;
            if (g_aliveWindows <= 0)
            {
                PostQuitMessage(0);
            }
            return 0;

        default:
            break;
        }

        return DefWindowProc(hwnd, message, wParam, lParam);
    }
}
