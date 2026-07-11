#include "RainRenderer.h"

#include <cmath>

Microsoft::WRL::ComPtr<ID2D1Factory> RainRenderer::factory_;

namespace
{
    constexpr float kAngleRad = 0.18f;
}

bool RainRenderer::InitializeFactory()
{
    if (factory_)
    {
        return true;
    }

    D2D1_FACTORY_OPTIONS options{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, options, factory_.GetAddressOf())))
    {
        return false;
    }

    return true;
}

void RainRenderer::ShutdownFactory()
{
    factory_.Reset();
}

RainRenderer::~RainRenderer()
{
    ReleaseResources();
}

void RainRenderer::ReleaseResources()
{
    brush_.Reset();
    renderTarget_.Reset();

    if (memDc_)
    {
        if (dibBitmap_)
        {
            SelectObject(memDc_, GetStockObject(BLACK_BRUSH));
        }
        DeleteDC(memDc_);
        memDc_ = nullptr;
    }

    if (dibBitmap_)
    {
        DeleteObject(dibBitmap_);
        dibBitmap_ = nullptr;
    }

    dibBits_ = nullptr;
}

bool RainRenderer::CreateRenderTarget(int width, int height)
{
    ReleaseResources();

    width_ = width;
    height_ = height;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width_;
    bmi.bmiHeader.biHeight = -height_;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC screenDc = GetDC(nullptr);
    if (!screenDc)
    {
        return false;
    }

    dibBitmap_ = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &dibBits_, nullptr, 0);
    ReleaseDC(nullptr, screenDc);

    if (!dibBitmap_ || !dibBits_)
    {
        ReleaseResources();
        return false;
    }

    memDc_ = CreateCompatibleDC(nullptr);
    if (!memDc_)
    {
        ReleaseResources();
        return false;
    }

    SelectObject(memDc_, dibBitmap_);

    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        0,
        0,
        D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE);

    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> dcRenderTarget;
    if (FAILED(factory_->CreateDCRenderTarget(&rtProps, dcRenderTarget.GetAddressOf())))
    {
        ReleaseResources();
        return false;
    }

    RECT bindRect{ 0, 0, width_, height_ };
    if (FAILED(dcRenderTarget->BindDC(memDc_, &bindRect)))
    {
        ReleaseResources();
        return false;
    }

    renderTarget_ = dcRenderTarget;
    if (FAILED(renderTarget_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), brush_.GetAddressOf())))
    {
        ReleaseResources();
        return false;
    }

    return true;
}

bool RainRenderer::Initialize(HWND hwnd, int width, int height)
{
    hwnd_ = hwnd;
    return CreateRenderTarget(width, height);
}

void RainRenderer::SetDropColor(float r, float g, float b)
{
    colorR_ = r;
    colorG_ = g;
    colorB_ = b;
}

void RainRenderer::Resize(int width, int height)
{
    if (width != width_ || height != height_)
    {
        CreateRenderTarget(width, height);
    }
}

void RainRenderer::Render(const RainSystem& system)
{
    if (!renderTarget_ || !brush_ || !hwnd_ || !memDc_)
    {
        return;
    }

    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> dcRenderTarget;
    if (FAILED(renderTarget_.As(&dcRenderTarget)))
    {
        return;
    }

    RECT bindRect{ 0, 0, width_, height_ };
    if (FAILED(dcRenderTarget->BindDC(memDc_, &bindRect)))
    {
        return;
    }

    renderTarget_->BeginDraw();
    renderTarget_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    const float sinA = std::sin(kAngleRad);
    const float cosA = std::cos(kAngleRad);

    for (const Raindrop& drop : system.GetDrops())
    {
        const float tailX = drop.x - sinA * drop.length;
        const float tailY = drop.y - cosA * drop.length;

        brush_->SetColor(D2D1::ColorF(colorR_, colorG_, colorB_, drop.alpha));
        renderTarget_->DrawLine(
            D2D1::Point2F(tailX, tailY),
            D2D1::Point2F(drop.x, drop.y),
            brush_.Get(),
            drop.thickness);
    }

    if (FAILED(renderTarget_->EndDraw()))
    {
        return;
    }

    POINT ptSrc{ 0, 0 };
    SIZE size{ width_, height_ };
    POINT ptDst{};
    RECT windowRect{};
    GetWindowRect(hwnd_, &windowRect);
    ptDst.x = windowRect.left;
    ptDst.y = windowRect.top;

    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(hwnd_, nullptr, &ptDst, &size, memDc_, &ptSrc, 0, &blend, ULW_ALPHA);
}
