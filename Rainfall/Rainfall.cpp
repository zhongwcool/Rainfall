#include "framework.h"
#include "Rainfall.h"
#include "RainSystem.h"
#include "RainRenderer.h"
#include "RainAudio.h"

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
    constexpr float kLengthScales[kLevelCount] = { 1.0f, 1.5f, 2.0f, 3.0f, 4.0f };
    constexpr float kDensityScales[kLevelCount] = { 0.1f, 0.35f, 1.0f, 1.5f, 2.0f };
    // 风力：角度倍率，越大越倾斜（第 3 挡 = 原始轻微倾斜）
    constexpr float kWindScales[kLevelCount] = { 0.3f, 0.6f, 1.0f, 2.5f, 4.5f };
    // 雨势：速度倍率，越大落得越快（默认第 3 挡 = 原第 4 挡的 1.5）
    constexpr float kSpeedScales[kLevelCount] = { 0.75f, 1.1f, 1.5f, 2.0f, 2.6f };

    int g_lengthLevel = kDefaultLevel;
    int g_densityLevel = kDefaultLevel;
    int g_windLevel = kDefaultLevel;
    int g_speedLevel = kDefaultLevel;
    bool g_windRight = true; // true: 向右下；false: 向左下

    bool g_soundEnabled = true;
    RainAudio g_rainAudio;

    struct OverlayWindow
    {
        HWND hwnd = nullptr;
        RECT monitorRect{};
        std::unique_ptr<RainSystem> rainSystem;
        std::unique_ptr<RainRenderer> renderer;
    };

    std::vector<OverlayWindow> g_overlays;

    void ShowTrayMenu(HWND hwnd);
    void ShowAboutDialog(HWND hwnd);
    INT_PTR CALLBACK AboutDlgProc(HWND dlg, UINT message, WPARAM wParam, LPARAM lParam);
    void ShowCopyToast(const wchar_t* message);
    LRESULT CALLBACK ToastWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    void RegisterToastClass();
    bool AddTrayIcon(HWND hwnd);
    void RemoveTrayIcon(HWND hwnd);
    void TogglePause();
    void ToggleLightMode();
    void ApplyLightMode();
    void ApplySound();
    void ToggleSound();
    void ApplyLength();
    void ApplyDensity();
    void ApplyWind();
    void ApplySpeed();
    void ApplyWindDirection();
    void SetWindDirection(bool right);
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
    ApplyWindDirection();
    ApplyLightMode();

    // 声音初始化失败也不影响视觉效果，静默降级
    if (g_rainAudio.Initialize())
    {
        ApplySound();
    }

    MSG msg{};
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    g_rainAudio.Shutdown();
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
        g_windRight = GetPrivateProfileIntW(L"Rainfall", L"WindRight", 1, path.c_str()) != 0;
        g_soundEnabled = GetPrivateProfileIntW(L"Rainfall", L"SoundEnabled", 1, path.c_str()) != 0;
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
        WritePrivateProfileStringW(L"Rainfall", L"WindRight", g_windRight ? L"1" : L"0", path.c_str());
        WritePrivateProfileStringW(L"Rainfall", L"SoundEnabled", g_soundEnabled ? L"1" : L"0", path.c_str());
    }

    void ApplyLightMode()
    {
        // 浅色模式：冷灰蓝雨丝 + 白色高光 + 尾部渐隐；关闭后恢复纯白实线
        for (auto& overlay : g_overlays)
        {
            if (!overlay.hwnd || !overlay.renderer)
            {
                continue;
            }

            overlay.renderer->SetLightMode(g_lightMode);
            overlay.renderer->Render(*overlay.rainSystem);
        }
    }

    void ToggleLightMode()
    {
        g_lightMode = !g_lightMode;
        ApplyLightMode();
        SaveConfig();
    }

    void ApplySound()
    {
        // 雨声强度由"雨势"和"雨滴密度"共同决定：雨越大越密，声音越大越急
        const float speedT = static_cast<float>(g_speedLevel) / (kLevelCount - 1);
        const float densityT = static_cast<float>(g_densityLevel) / (kLevelCount - 1);
        g_rainAudio.SetIntensity(0.65f * speedT + 0.35f * densityT);
        g_rainAudio.SetEnabled(g_soundEnabled);
        g_rainAudio.SetPaused(g_paused);
    }

    void ToggleSound()
    {
        g_soundEnabled = !g_soundEnabled;
        ApplySound();
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
        ApplySound();
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

    void ApplyWindDirection()
    {
        for (auto& overlay : g_overlays)
        {
            if (overlay.rainSystem)
            {
                overlay.rainSystem->SetWindDirection(g_windRight ? 1 : -1);
            }
        }
    }

    void SetWindDirection(bool right)
    {
        g_windRight = right;
        ApplyWindDirection();
        SaveConfig();
    }

    void SetSpeedLevel(int level)
    {
        g_speedLevel = std::clamp(level, 0, kLevelCount - 1);
        ApplySpeed();
        ApplySound();
        SaveConfig();
    }

    void TogglePause()
    {
        g_paused = !g_paused;
        g_rainAudio.SetPaused(g_paused);

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
        AppendMenuW(menu, MF_STRING | (g_soundEnabled ? MF_CHECKED : MF_UNCHECKED),
            IDM_SOUND, L"雨声(&V)");

        HMENU lengthMenu = CreatePopupMenu();
        HMENU densityMenu = CreatePopupMenu();
        HMENU windMenu = CreatePopupMenu();
        HMENU speedMenu = CreatePopupMenu();
        HMENU dirMenu = CreatePopupMenu();
        AppendMenuW(dirMenu, MF_STRING | (g_windRight ? MF_CHECKED : MF_UNCHECKED),
            IDM_DIR_RIGHT, L"向右下");
        AppendMenuW(dirMenu, MF_STRING | (!g_windRight ? MF_CHECKED : MF_UNCHECKED),
            IDM_DIR_LEFT, L"向左下");
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
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(dirMenu), L"风向(&F)");

        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_ABOUT, L"关于(&A)");
        AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, L"退出(&X)");

        SetForegroundWindow(hwnd);

        POINT cursor{};
        GetCursorPos(&cursor);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
            cursor.x, cursor.y, 0, hwnd, nullptr);
        DestroyMenu(menu);
    }

    constexpr wchar_t kAuthorEmail[] = L"zhongwcool@163.com";

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
        if (!OpenClipboard(nullptr))
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
                SetClipboardData(CF_UNICODETEXT, mem);
            }
        }

        CloseClipboard();
    }

    constexpr UINT_PTR kToastTimerId = 99;
    constexpr wchar_t kToastClassName[] = L"RainfallToastClass";
    HWND g_toastWnd = nullptr;

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
            const auto* text = reinterpret_cast<const wchar_t*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);

            RECT rc{};
            GetClientRect(hwnd, &rc);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255, 255, 255));
            HFONT oldFont = static_cast<HFONT>(SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT)));
            if (text)
            {
                DrawTextW(dc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            SelectObject(dc, oldFont);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_TIMER:
            if (wParam == kToastTimerId)
            {
                DestroyWindow(hwnd);
            }
            return 0;
        case WM_DESTROY:
            if (g_toastWnd == hwnd)
            {
                g_toastWnd = nullptr;
            }
            free(reinterpret_cast<void*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)));
            return 0;
        default:
            break;
        }

        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    void ShowCopyToast(const wchar_t* message)
    {
        RegisterToastClass();

        if (g_toastWnd)
        {
            KillTimer(g_toastWnd, kToastTimerId);
            DestroyWindow(g_toastWnd);
            g_toastWnd = nullptr;
        }

        wchar_t* text = _wcsdup(message);
        if (!text)
        {
            return;
        }

        constexpr int toastW = 300;
        constexpr int toastH = 44;
        const int screenW = GetSystemMetrics(SM_CXSCREEN);
        const int screenH = GetSystemMetrics(SM_CYSCREEN);
        const int x = (screenW - toastW) / 2;
        const int y = screenH - toastH - 80;

        g_toastWnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kToastClassName,
            L"",
            WS_POPUP | WS_BORDER,
            x,
            y,
            toastW,
            toastH,
            nullptr,
            nullptr,
            g_instance,
            nullptr);

        if (!g_toastWnd)
        {
            free(text);
            return;
        }

        SetWindowLongPtrW(g_toastWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(text));
        ShowWindow(g_toastWnd, SW_SHOWNOACTIVATE);
        UpdateWindow(g_toastWnd);
        SetTimer(g_toastWnd, kToastTimerId, 2000, nullptr);
    }

    INT_PTR CALLBACK AboutDlgProc(HWND dlg, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_INITDIALOG:
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
        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_ABOUT_EMAIL && HIWORD(wParam) == STN_CLICKED)
            {
                CopyTextToClipboard(kAuthorEmail);
                ShowCopyToast(L"邮箱已复制到剪贴板");
                return TRUE;
            }
            if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
            {
                EndDialog(dlg, LOWORD(wParam));
                return TRUE;
            }
            break;
        default:
            break;
        }
        return FALSE;
    }

    void ShowAboutDialog(HWND hwnd)
    {
        DialogBoxW(g_instance, MAKEINTRESOURCE(IDD_ABOUTBOX), hwnd, AboutDlgProc);
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
            case IDM_SOUND:
                ToggleSound();
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
