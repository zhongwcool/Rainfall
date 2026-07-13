#include "RainRenderer.h"

#include <algorithm>
#include <cmath>

Microsoft::WRL::ComPtr<ID2D1Factory> RainRenderer::factory_;


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
    bodyBrush_.Reset();
    highlightBrush_.Reset();
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

    if (!CreateLightModeBrushes())
    {
        ReleaseResources();
        return false;
    }

    return true;
}

bool RainRenderer::CreateLightModeBrushes()
{
    // Tail fully transparent, head most visible: mimics a real rain streak.
    const auto makeGradientBrush = [this](const D2D1_COLOR_F& color, float headAlpha,
        Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush>& outBrush) -> bool
    {
        D2D1_GRADIENT_STOP stops[2]{};
        stops[0].position = 0.0f;
        stops[0].color = D2D1::ColorF(color.r, color.g, color.b, 0.0f);
        stops[1].position = 1.0f;
        stops[1].color = D2D1::ColorF(color.r, color.g, color.b, headAlpha);

        Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> collection;
        if (FAILED(renderTarget_->CreateGradientStopCollection(stops, 2, collection.GetAddressOf())))
        {
            return false;
        }

        return SUCCEEDED(renderTarget_->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.0f, 0.0f), D2D1::Point2F(0.0f, 1.0f)),
            collection.Get(),
            outBrush.GetAddressOf()));
    };

    // Body: cool gray-blue, close to the tint of real rain.
    const D2D1_COLOR_F bodyColor = D2D1::ColorF(0.35f, 0.42f, 0.52f, 1.0f);
    const D2D1_COLOR_F highlightColor = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);

    return makeGradientBrush(bodyColor, 0.62f, bodyBrush_)
        && makeGradientBrush(highlightColor, 0.85f, highlightBrush_);
}

bool RainRenderer::Initialize(HWND hwnd, int width, int height)
{
    hwnd_ = hwnd;
    return CreateRenderTarget(width, height);
}

void RainRenderer::SetLightMode(bool enabled)
{
    lightMode_ = enabled;
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

    const float angle = system.GetAngleRad();
    const float sinA = std::sin(angle);
    const float cosA = std::cos(angle);

    if (lightMode_ && bodyBrush_ && highlightBrush_)
    {
        // Unit vector perpendicular to the fall direction, used to offset the highlight.
        const float perpX = cosA;
        const float perpY = -sinA;

        for (const Raindrop& drop : system.GetDrops())
        {
            const D2D1_POINT_2F head = D2D1::Point2F(drop.x, drop.y);
            const D2D1_POINT_2F tail = D2D1::Point2F(
                drop.x - sinA * drop.length,
                drop.y - cosA * drop.length);

            bodyBrush_->SetStartPoint(tail);
            bodyBrush_->SetEndPoint(head);
            bodyBrush_->SetOpacity(drop.alpha);
            renderTarget_->DrawLine(tail, head, bodyBrush_.Get(), drop.thickness * 0.85f);

            const float offset = drop.thickness * 0.25f;
            const D2D1_POINT_2F hlTail = D2D1::Point2F(tail.x + perpX * offset, tail.y + perpY * offset);
            const D2D1_POINT_2F hlHead = D2D1::Point2F(head.x + perpX * offset, head.y + perpY * offset);

            highlightBrush_->SetStartPoint(hlTail);
            highlightBrush_->SetEndPoint(hlHead);
            highlightBrush_->SetOpacity(drop.alpha * 0.9f);
            renderTarget_->DrawLine(hlTail, hlHead, highlightBrush_.Get(),
                (std::max)(drop.thickness * 0.35f, 0.6f));
        }
    }
    else
    {
        for (const Raindrop& drop : system.GetDrops())
        {
            const float tailX = drop.x - sinA * drop.length;
            const float tailY = drop.y - cosA * drop.length;

            brush_->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, drop.alpha));
            renderTarget_->DrawLine(
                D2D1::Point2F(tailX, tailY),
                D2D1::Point2F(drop.x, drop.y),
                brush_.Get(),
                drop.thickness);
        }
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
