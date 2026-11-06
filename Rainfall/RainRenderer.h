#pragma once

#include "RainSystem.h"

#include <d2d1.h>
#include <wrl/client.h>

class RainRenderer
{
public:
    static bool InitializeFactory();
    static void ShutdownFactory();

    RainRenderer() = default;
    ~RainRenderer();

    RainRenderer(const RainRenderer&) = delete;
    RainRenderer& operator=(const RainRenderer&) = delete;

    // width/height 是物理像素。dpi 必须是该显示器的 DPI，不能传 0。
    bool Initialize(HWND hwnd, int width, int height, UINT dpi);
    void Render(const RainSystem& system);
    void SetLightMode(bool enabled);

private:
    bool CreateRenderTarget(int width, int height);
    bool CreateLightModeBrushes();
    void ReleaseResources();

    HWND hwnd_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    UINT dpi_ = 96;
    bool lightMode_ = false;

    Microsoft::WRL::ComPtr<ID2D1RenderTarget> renderTarget_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    // Light mode: cool-toned body with fading tail + white highlight (glass look)
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> bodyBrush_;
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> highlightBrush_;
    HDC memDc_ = nullptr;
    HBITMAP dibBitmap_ = nullptr;
    void* dibBits_ = nullptr;

    static Microsoft::WRL::ComPtr<ID2D1Factory> factory_;
};
