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

    bool Initialize(HWND hwnd, int width, int height);
    void Resize(int width, int height);
    void Render(const RainSystem& system);
    void SetLightMode(bool enabled);

private:
    bool CreateRenderTarget(int width, int height);
    bool CreateLightModeBrushes();
    void ReleaseResources();

    HWND hwnd_ = nullptr;
    int width_ = 0;
    int height_ = 0;
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
