#pragma once
#include <d3d11.h>
#include <dxgi1_3.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// Retain the rasterized scene on the GPU. Upload only changed tiles, then
// present a complete immutable frame instead of mutating an HWND's GDI surface
// while DWM is reading it. No GDI calls target the HWND while this is active.
struct FlipPresenter {
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    winrt::com_ptr<IDXGISwapChain2> chain;
    winrt::com_ptr<ID3D11Texture2D> scene;
    HANDLE ready{};
    HRESULT failure{S_OK};
    UINT presentId{};
    DXGI_FRAME_STATISTICS displayed{};

    ~FlipPresenter() { stop(); }
    bool active() const { return chain != nullptr; }
    void stop() {
        if (ready) CloseHandle(ready);
        ready = nullptr;
        scene = nullptr; chain = nullptr;
        if (context) { context->ClearState(); context->Flush(); }
        context = nullptr; device = nullptr;
        presentId = 0; displayed = {};
    }
    bool start(HWND window, int width, int height) noexcept {
        stop(); failure = S_OK;
        try {
            // Software rasterization can be slower than the existing GDI path.
            // Use that fallback if hardware presentation is unavailable.
            winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put()));
            auto dxgi = device.as<IDXGIDevice>();
            winrt::com_ptr<IDXGIAdapter> adapter;
            winrt::check_hresult(dxgi->GetAdapter(adapter.put()));
            winrt::com_ptr<IDXGIFactory2> factory;
            winrt::check_hresult(adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void()));
            DXGI_SWAP_CHAIN_DESC1 desc{};
            desc.Width = width; desc.Height = height; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 2; desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            desc.Scaling = DXGI_SCALING_STRETCH; desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
            winrt::com_ptr<IDXGISwapChain1> created;
            winrt::check_hresult(factory->CreateSwapChainForHwnd(device.get(), window, &desc, nullptr, nullptr, created.put()));
            chain = created.as<IDXGISwapChain2>();
            winrt::check_hresult(factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER));
            winrt::check_hresult(chain->SetMaximumFrameLatency(1));
            ready = chain->GetFrameLatencyWaitableObject();
            if (!ready) winrt::throw_hresult(E_FAIL);
            D3D11_TEXTURE2D_DESC texture{};
            texture.Width = width; texture.Height = height; texture.MipLevels = texture.ArraySize = 1;
            texture.Format = desc.Format; texture.SampleDesc.Count = 1; texture.Usage = D3D11_USAGE_DEFAULT;
            winrt::check_hresult(device->CreateTexture2D(&texture, nullptr, scene.put()));
            return true;
        } catch (winrt::hresult_error const& e) { failure = e.code(); }
        catch (...) { failure = E_FAIL; }
        stop(); return false;
    }
    void upload(RECT rect, const Bitmap& tile) {
        D3D11_BOX box{static_cast<UINT>(rect.left), static_cast<UINT>(rect.top), 0,
            static_cast<UINT>(rect.right), static_cast<UINT>(rect.bottom), 1};
        context->UpdateSubresource(scene.get(), 0, &box, tile.pixels, tile.width * 4, 0);
    }
    bool present() {
        winrt::com_ptr<ID3D11Texture2D> back;
        failure = chain->GetBuffer(0, __uuidof(ID3D11Texture2D), back.put_void());
        if (FAILED(failure)) return false;
        // FLIP_DISCARD buffers have undefined contents after Present. Copy the
        // complete retained scene, including unchanged tiles, on every frame.
        context->CopyResource(back.get(), scene.get());
        failure = chain->Present(1, 0);
        if (FAILED(failure)) return false;
        chain->GetLastPresentCount(&presentId);
        DXGI_FRAME_STATISTICS stats{};
        if (SUCCEEDED(chain->GetFrameStatistics(&stats))) displayed = stats;
        return true;
    }
};
