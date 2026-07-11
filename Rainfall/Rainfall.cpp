#include "framework.h"
#include "Rainfall.h"
#include "RainSystem.h"
#include "RainRenderer.h"

namespace
{
    constexpr UINT WM_TRAYICON = WM_APP + 1;
    constexpr UINT_PTR TIMER_ID = 1;
    constexpr UINT TIMER_INTERVAL_MS = 16;
    constexpr wchar_t kWindowClassName[] = L"RainfallOverlayClass";

    HINSTANCE g_instance = nullptr;
    bool g_paused = false;
    bool g_trayAdded = false;
    int g_aliveWindows = 0;

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
            switch (LOWORD(wParam))
            {
            case IDM_PAUSE:
                TogglePause();
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
