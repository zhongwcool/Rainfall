#pragma once

#include "RainSystem.h"

#include <d2d1_1.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

// 每块屏幕一张合成交换链：D2D 画进预乘 alpha 的后缓冲，DirectComposition 交给 DWM。
// 窗口必须带 WS_EX_NOREDIRECTIONBITMAP，不能再用 UpdateLayeredWindow。
class RainRenderer
{
public:
    static bool InitializeFactory();
    static void ShutdownFactory();

    RainRenderer() = default;
    ~RainRenderer();

    RainRenderer(const RainRenderer&) = delete;
    RainRenderer& operator=(const RainRenderer&) = delete;

    // width/height 是物理像素。dpi 是该显示器的 DPI，坐标系与雨滴模拟一致（DIP）。
    bool Initialize(HWND hwnd, int width, int height, UINT dpi);
    void Render(const RainSystem& system);
    void SetLightMode(bool enabled);

private:
    enum class DrawResult
    {
        Ok,
        Lost,
        Failed,
    };

    bool CreateWindowResources();
    bool BindTarget();
    void UnbindTarget();
    HRESULT PresentFrame();
    DrawResult DrawFrame(const RainSystem& system);
    bool RecoverDevice();
    void ReleaseWindowResources();
    bool CreateLightModeBrushes();

    HWND hwnd_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    UINT dpi_ = 96;
    bool lightMode_ = false;
    UINT64 deviceGeneration_ = 0;

    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> target_;
    Microsoft::WRL::ComPtr<IDCompositionTarget> compositionTarget_;
    Microsoft::WRL::ComPtr<IDCompositionVisual> visual_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> bodyBrush_;
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> highlightBrush_;
};
