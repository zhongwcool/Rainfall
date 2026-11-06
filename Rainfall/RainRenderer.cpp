#include "RainRenderer.h"

#include <d3d11.h>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace
{
    struct SharedDevice
    {
        Microsoft::WRL::ComPtr<ID3D11Device> d3d;
        Microsoft::WRL::ComPtr<ID2D1Factory1> d2dFactory;
        Microsoft::WRL::ComPtr<ID2D1Device> d2dDevice;
        Microsoft::WRL::ComPtr<IDCompositionDevice> composition;
        UINT64 generation = 0;
    };

    SharedDevice g_shared;

    bool IsDeviceLostError(HRESULT hr)
    {
        return hr == DXGI_ERROR_DEVICE_REMOVED
            || hr == DXGI_ERROR_DEVICE_RESET
            || hr == DXGI_ERROR_DEVICE_HUNG
            || hr == D2DERR_RECREATE_TARGET;
    }

    bool SharedDeviceRemoved()
    {
        return g_shared.d3d && FAILED(g_shared.d3d->GetDeviceRemovedReason());
    }

    void ReleaseSharedDevice()
    {
        g_shared.composition.Reset();
        g_shared.d2dDevice.Reset();
        g_shared.d3d.Reset();
    }

    bool CreateD3DDevice(Microsoft::WRL::ComPtr<ID3D11Device>& device)
    {
        const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
            D3D_FEATURE_LEVEL_9_3,
            D3D_FEATURE_LEVEL_9_1,
        };
        const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

        const auto tryCreate = [&](D3D_DRIVER_TYPE driver, const D3D_FEATURE_LEVEL* featureLevels, UINT levelCount) {
            device.Reset();
            return D3D11CreateDevice(
                nullptr,
                driver,
                nullptr,
                flags,
                featureLevels,
                levelCount,
                D3D11_SDK_VERSION,
                device.GetAddressOf(),
                nullptr,
                nullptr);
        };

        HRESULT hr = tryCreate(D3D_DRIVER_TYPE_HARDWARE, levels, static_cast<UINT>(std::size(levels)));
        if (hr == E_INVALIDARG)
        {
            hr = tryCreate(D3D_DRIVER_TYPE_HARDWARE, nullptr, 0);
        }
        if (FAILED(hr))
        {
            hr = tryCreate(D3D_DRIVER_TYPE_WARP, levels, static_cast<UINT>(std::size(levels)));
            if (hr == E_INVALIDARG)
            {
                hr = tryCreate(D3D_DRIVER_TYPE_WARP, nullptr, 0);
            }
        }
        return SUCCEEDED(hr);
    }

    bool CreateSharedDevice()
    {
        if (g_shared.d3d && g_shared.d2dDevice && g_shared.composition && !SharedDeviceRemoved())
        {
            return true;
        }

        ReleaseSharedDevice();

        if (!g_shared.d2dFactory)
        {
            const D2D1_FACTORY_OPTIONS options{};
            if (FAILED(D2D1CreateFactory(
                D2D1_FACTORY_TYPE_SINGLE_THREADED,
                options,
                g_shared.d2dFactory.GetAddressOf())))
            {
                return false;
            }
        }

        if (!CreateD3DDevice(g_shared.d3d))
        {
            return false;
        }

        Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
        if (FAILED(g_shared.d3d.As(&dxgi)))
        {
            ReleaseSharedDevice();
            return false;
        }

        Microsoft::WRL::ComPtr<IDXGIDevice1> dxgi1;
        if (SUCCEEDED(dxgi.As(&dxgi1)))
        {
            // 只排队一帧，暂停或菜单操作时画面不会多落一帧。
            dxgi1->SetMaximumFrameLatency(1);
        }

        if (FAILED(g_shared.d2dFactory->CreateDevice(dxgi.Get(), g_shared.d2dDevice.GetAddressOf())))
        {
            ReleaseSharedDevice();
            return false;
        }

        if (FAILED(DCompositionCreateDevice(
            dxgi.Get(),
            IID_PPV_ARGS(g_shared.composition.GetAddressOf()))))
        {
            ReleaseSharedDevice();
            return false;
        }

        ++g_shared.generation;
        return true;
    }
}

bool RainRenderer::InitializeFactory()
{
    return CreateSharedDevice();
}

void RainRenderer::ShutdownFactory()
{
    ReleaseSharedDevice();
    g_shared.d2dFactory.Reset();
    g_shared.generation = 0;
}

RainRenderer::~RainRenderer()
{
    ReleaseWindowResources();
}

void RainRenderer::ReleaseWindowResources()
{
    UnbindTarget();
    brush_.Reset();
    bodyBrush_.Reset();
    highlightBrush_.Reset();
    context_.Reset();

    if (visual_)
    {
        visual_->SetContent(nullptr);
    }
    visual_.Reset();
    compositionTarget_.Reset();
    swapChain_.Reset();
    deviceGeneration_ = 0;
}

void RainRenderer::UnbindTarget()
{
    if (context_)
    {
        context_->SetTarget(nullptr);
    }
    target_.Reset();
}

bool RainRenderer::CreateWindowResources()
{
    ReleaseWindowResources();

    if (!hwnd_ || width_ <= 0 || height_ <= 0 || !CreateSharedDevice())
    {
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    if (FAILED(g_shared.d3d.As(&dxgi))
        || FAILED(dxgi->GetAdapter(adapter.GetAddressOf()))
        || FAILED(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf()))))
    {
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = static_cast<UINT>(width_);
    desc.Height = static_cast<UINT>(height_);
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    desc.Scaling = DXGI_SCALING_STRETCH;

    HRESULT hr = factory->CreateSwapChainForComposition(
        g_shared.d3d.Get(), &desc, nullptr, swapChain_.GetAddressOf());
    if (FAILED(hr))
    {
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        hr = factory->CreateSwapChainForComposition(
            g_shared.d3d.Get(), &desc, nullptr, swapChain_.GetAddressOf());
    }
    if (FAILED(hr))
    {
        ReleaseWindowResources();
        return false;
    }

    if (FAILED(g_shared.d2dDevice->CreateDeviceContext(
        D2D1_DEVICE_CONTEXT_OPTIONS_NONE, context_.GetAddressOf())))
    {
        ReleaseWindowResources();
        return false;
    }

    const float dpi = static_cast<float>(dpi_);
    context_->SetDpi(dpi, dpi);

    if (FAILED(context_->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), brush_.GetAddressOf()))
        || !CreateLightModeBrushes())
    {
        ReleaseWindowResources();
        return false;
    }

    if (FAILED(g_shared.composition->CreateTargetForHwnd(
            hwnd_, TRUE, compositionTarget_.GetAddressOf()))
        || FAILED(g_shared.composition->CreateVisual(visual_.GetAddressOf()))
        || FAILED(visual_->SetContent(swapChain_.Get()))
        || FAILED(compositionTarget_->SetRoot(visual_.Get())))
    {
        ReleaseWindowResources();
        return false;
    }

    if (!BindTarget())
    {
        ReleaseWindowResources();
        return false;
    }

    context_->BeginDraw();
    context_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    hr = context_->EndDraw();
    if (SUCCEEDED(hr))
    {
        hr = PresentFrame();
    }
    if (FAILED(hr))
    {
        ReleaseWindowResources();
        return false;
    }

    deviceGeneration_ = g_shared.generation;
    return true;
}

bool RainRenderer::BindTarget()
{
    UnbindTarget();
    if (!context_ || !swapChain_)
    {
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGISurface> surface;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(surface.GetAddressOf()))))
    {
        return false;
    }

    const float dpi = static_cast<float>(dpi_);
    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        dpi,
        dpi);

    if (FAILED(context_->CreateBitmapFromDxgiSurface(surface.Get(), &props, target_.GetAddressOf())))
    {
        return false;
    }

    context_->SetTarget(target_.Get());
    context_->SetDpi(dpi, dpi);
    return true;
}

HRESULT RainRenderer::PresentFrame()
{
    // Flip 交换链要求 Present 前放开后缓冲。同步间隔 0 且不等待：
    // 帧率由 FrameClock 控制，多显示器时不能在 UI 线程里等垂直同步。
    UnbindTarget();
    if (!swapChain_ || !g_shared.composition)
    {
        return E_FAIL;
    }

    const HRESULT presentHr = swapChain_->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
    if (presentHr == DXGI_ERROR_WAS_STILL_DRAWING)
    {
        return S_OK;
    }
    if (FAILED(presentHr))
    {
        return presentHr;
    }

    return g_shared.composition->Commit();
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
        if (FAILED(context_->CreateGradientStopCollection(stops, 2, collection.GetAddressOf())))
        {
            return false;
        }

        return SUCCEEDED(context_->CreateLinearGradientBrush(
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

bool RainRenderer::Initialize(HWND hwnd, int width, int height, UINT dpi)
{
    hwnd_ = hwnd;
    dpi_ = dpi == 0 ? 96 : dpi;
    width_ = width;
    height_ = height;
    return CreateWindowResources();
}

void RainRenderer::SetLightMode(bool enabled)
{
    lightMode_ = enabled;
}

bool RainRenderer::RecoverDevice()
{
    ReleaseWindowResources();
    ReleaseSharedDevice();
    if (!CreateSharedDevice())
    {
        return false;
    }
    return CreateWindowResources();
}

RainRenderer::DrawResult RainRenderer::DrawFrame(const RainSystem& system)
{
    if (!hwnd_)
    {
        return DrawResult::Failed;
    }
    if (SharedDeviceRemoved())
    {
        return DrawResult::Lost;
    }
    if (!g_shared.d3d && !CreateSharedDevice())
    {
        return DrawResult::Failed;
    }
    if (!context_ || !swapChain_ || deviceGeneration_ != g_shared.generation)
    {
        if (!CreateWindowResources())
        {
            return SharedDeviceRemoved() ? DrawResult::Lost : DrawResult::Failed;
        }
    }
    if (!brush_ || !BindTarget())
    {
        return SharedDeviceRemoved() ? DrawResult::Lost : DrawResult::Failed;
    }

    context_->BeginDraw();
    context_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

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
            context_->DrawLine(tail, head, bodyBrush_.Get(), drop.thickness * 0.85f);

            const float offset = drop.thickness * 0.25f;
            const D2D1_POINT_2F hlTail = D2D1::Point2F(tail.x + perpX * offset, tail.y + perpY * offset);
            const D2D1_POINT_2F hlHead = D2D1::Point2F(head.x + perpX * offset, head.y + perpY * offset);

            highlightBrush_->SetStartPoint(hlTail);
            highlightBrush_->SetEndPoint(hlHead);
            highlightBrush_->SetOpacity(drop.alpha * 0.9f);
            context_->DrawLine(hlTail, hlHead, highlightBrush_.Get(),
                std::max(drop.thickness * 0.35f, 0.6f));
        }
    }
    else
    {
        for (const Raindrop& drop : system.GetDrops())
        {
            const float tailX = drop.x - sinA * drop.length;
            const float tailY = drop.y - cosA * drop.length;

            brush_->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, drop.alpha));
            context_->DrawLine(
                D2D1::Point2F(tailX, tailY),
                D2D1::Point2F(drop.x, drop.y),
                brush_.Get(),
                drop.thickness);
        }
    }

    const HRESULT endHr = context_->EndDraw();
    if (FAILED(endHr))
    {
        UnbindTarget();
        return IsDeviceLostError(endHr) || SharedDeviceRemoved() ? DrawResult::Lost : DrawResult::Failed;
    }

    const HRESULT presentHr = PresentFrame();
    if (FAILED(presentHr))
    {
        return IsDeviceLostError(presentHr) || SharedDeviceRemoved() ? DrawResult::Lost : DrawResult::Failed;
    }

    return DrawResult::Ok;
}

void RainRenderer::Render(const RainSystem& system)
{
    if (DrawFrame(system) == DrawResult::Lost && RecoverDevice())
    {
        DrawFrame(system);
    }
}
