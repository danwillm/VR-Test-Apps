#pragma once

#include <openvr_driver.h>

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace sample
{
class PreviewWindow
{
public:
    PreviewWindow() = default;
    ~PreviewWindow();

    PreviewWindow( const PreviewWindow & ) = delete;
    PreviewWindow &operator=( const PreviewWindow & ) = delete;

    bool Initialize( int32_t nAdapterIndex, uint32_t nWindowWidth, uint32_t nWindowHeight );
    void Start();
    void Stop();

    void SubmitFrame( const vr::PresentInfo_t &presentInfo );

    [[nodiscard]] uint64_t GetAdapterLuid() const { return m_nAdapterLuid; }
    [[nodiscard]] bool IsValid() const { return m_bInitialized; }

private:
    struct PendingFrame
    {
        vr::SharedTextureHandle_t hTexture = INVALID_SHARED_TEXTURE_HANDLE;
        uint64_t nFrameId = 0;
        double flVsyncTimeInSeconds = 0.0;
        vr::EVSync eVsync = vr::VSync_None;
    };

    static LRESULT CALLBACK WindowProc( HWND hWnd, UINT uMessage, WPARAM wParam, LPARAM lParam );

    void ThreadMain();
    bool CreatePreviewWindow();
    bool OpenSharedTexture( vr::SharedTextureHandle_t hTexture );
    bool EnsureSwapChain( const D3D11_TEXTURE2D_DESC &srcDesc );
    void RenderFrame( const PendingFrame &frame );
    void ReleaseFrameResources();

    static DXGI_FORMAT GetSwapChainFormat( DXGI_FORMAT srcFormat );

    int32_t m_nAdapterIndex = 0;
    uint32_t m_nWindowWidth = 1280;
    uint32_t m_nWindowHeight = 720;
    uint64_t m_nAdapterLuid = 0;

    Microsoft::WRL::ComPtr< IDXGIFactory2 > m_factory;
    Microsoft::WRL::ComPtr< IDXGIAdapter1 > m_adapter;
    Microsoft::WRL::ComPtr< ID3D11Device > m_device;
    Microsoft::WRL::ComPtr< ID3D11DeviceContext > m_context;

    HWND m_hWnd = nullptr;
    Microsoft::WRL::ComPtr< IDXGISwapChain1 > m_swapChain;
    Microsoft::WRL::ComPtr< ID3D11Texture2D > m_sharedTexture;
    vr::SharedTextureHandle_t m_hOpenedTexture = INVALID_SHARED_TEXTURE_HANDLE;
    uint32_t m_nSwapChainWidth = 0;
    uint32_t m_nSwapChainHeight = 0;
    DXGI_FORMAT m_swapChainFormat = DXGI_FORMAT_UNKNOWN;

    std::thread m_thread;
    std::atomic< bool > m_bStop{ false };
    bool m_bInitialized = false;

    std::mutex m_frameMutex;
    std::condition_variable m_frameCv;
    PendingFrame m_pendingFrame{};
    bool m_bFramePending = false;
};
} // namespace sample
