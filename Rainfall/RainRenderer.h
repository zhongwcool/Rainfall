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

private:
    bool CreateRenderTarget(int width, int height);
    void ReleaseResources();

    HWND hwnd_ = nullptr;
    int width_ = 0;
    int height_ = 0;

    Microsoft::WRL::ComPtr<ID2D1RenderTarget> renderTarget_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    HDC memDc_ = nullptr;
    HBITMAP dibBitmap_ = nullptr;
    void* dibBits_ = nullptr;

    static Microsoft::WRL::ComPtr<ID2D1Factory> factory_;
};
