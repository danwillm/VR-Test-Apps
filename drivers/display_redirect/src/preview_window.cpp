#include "preview_window.h"

#include "log.h"

#include <d3d11_1.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

using Microsoft::WRL::ComPtr;

namespace sample
{
	namespace
	{
		constexpr wchar_t k_pchWindowClassName[] = L"OpenVRDisplayRedirectSamplePreview";

		uint64_t LuidToUint64(const LUID& luid)
		{
			return static_cast<uint64_t>(static_cast<uint32_t>(luid.LowPart)) |
				(static_cast<uint64_t>(static_cast<uint32_t>(luid.HighPart)) << 32);
		}
	} // namespace

	PreviewWindow::~PreviewWindow()
	{
		Stop();
	}

	bool PreviewWindow::Initialize(int32_t nAdapterIndex, uint32_t nWindowWidth, uint32_t nWindowHeight)
	{
		m_nAdapterIndex = std::max(nAdapterIndex, 0);
		m_nWindowWidth = std::max(nWindowWidth, 320u);
		m_nWindowHeight = std::max(nWindowHeight, 240u);

		HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&m_factory));
		if (FAILED(hr))
		{
			Log("display_redirect_sample: CreateDXGIFactory1 failed: 0x%08x", static_cast<unsigned>(hr));
			return false;
		}

		for (UINT i = 0;; ++i)
		{
			ComPtr< IDXGIAdapter1 > adapter;
			if (m_factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
			{
				break;
			}

			DXGI_ADAPTER_DESC1 desc{};
			adapter->GetDesc1(&desc);

			char description[256]{};
			WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, description,
				static_cast<int>(sizeof(description)), nullptr, nullptr);

			Log("display_redirect_sample: DXGI adapter %u: %s (LUID 0x%016llx)%s",
				i,
				description,
				static_cast<unsigned long long>(LuidToUint64(desc.AdapterLuid)),
				i == static_cast<UINT>(m_nAdapterIndex) ? " [selected]" : "");

			if (i == static_cast<UINT>(m_nAdapterIndex))
			{
				m_adapter = adapter;
				m_nAdapterLuid = LuidToUint64(desc.AdapterLuid);
			}
		}

		if (!m_adapter)
		{
			Log("display_redirect_sample: adapterIndex=%d does not exist", m_nAdapterIndex);
			return false;
		}

		constexpr D3D_FEATURE_LEVEL featureLevels[] = {
			D3D_FEATURE_LEVEL_11_1,
			D3D_FEATURE_LEVEL_11_0,
			D3D_FEATURE_LEVEL_10_1,
			D3D_FEATURE_LEVEL_10_0,
		};

		D3D_FEATURE_LEVEL featureLevel{};
		UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

		hr = D3D11CreateDevice(
			m_adapter.Get(),
			D3D_DRIVER_TYPE_UNKNOWN,
			nullptr,
			flags,
			featureLevels,
			static_cast<UINT>(_countof(featureLevels)),
			D3D11_SDK_VERSION,
			&m_device,
			&featureLevel,
			&m_context);

		if (FAILED(hr))
		{
			Log("display_redirect_sample: D3D11CreateDevice failed: 0x%08x", static_cast<unsigned>(hr));
			return false;
		}

		Log("display_redirect_sample: D3D11 preview device created on adapter %d, feature level 0x%x",
			m_nAdapterIndex, static_cast<unsigned>(featureLevel));

		m_bInitialized = true;
		return true;
	}

	void PreviewWindow::Start()
	{
		if (!m_bInitialized)
		{
			Log("display_redirect_sample: PreviewWindow::Start ignored because Initialize failed/not run");
			return;
		}

		if (m_thread.joinable())
		{
			Log("display_redirect_sample: PreviewWindow::Start ignored because preview thread is already running");
			return;
		}

		Log("display_redirect_sample: PreviewWindow::Start creating preview thread");
		m_bStop = false;
		m_thread = std::thread(&PreviewWindow::ThreadMain, this);
	}

	void PreviewWindow::Stop()
	{
		m_bStop = true;
		m_frameCv.notify_all();

		if (m_thread.joinable())
		{
			Log("display_redirect_sample: stopping preview thread");
			m_thread.join();
		}

		m_swapChain.Reset();
		m_sharedTexture.Reset();
		m_hOpenedTexture = INVALID_SHARED_TEXTURE_HANDLE;
		m_nSwapChainWidth = 0;
		m_nSwapChainHeight = 0;
		m_swapChainFormat = DXGI_FORMAT_UNKNOWN;
	}

	void PreviewWindow::SubmitFrame(const vr::PresentInfo_t& presentInfo)
	{
		if (!m_bInitialized)
		{
			return;
		}

		{
			std::lock_guard lock(m_frameMutex);
			m_pendingFrame.hTexture = presentInfo.backbufferTextureHandle;
			m_pendingFrame.nFrameId = presentInfo.nFrameId;
			m_pendingFrame.flVsyncTimeInSeconds = presentInfo.flVSyncTimeInSeconds;
			m_pendingFrame.eVsync = presentInfo.vsync;
			m_bFramePending = true;
		}

		m_frameCv.notify_one();
	}

	LRESULT CALLBACK PreviewWindow::WindowProc(HWND hWnd, UINT uMessage, WPARAM wParam, LPARAM lParam)
	{
		switch (uMessage)
		{
		case WM_CLOSE:
			ShowWindow(hWnd, SW_HIDE);
			return 0;

		case WM_ERASEBKGND:
			return 1;

		case WM_PAINT:
		{
			PAINTSTRUCT ps{};
			HDC hdc = BeginPaint(hWnd, &ps);
			RECT rect{};
			GetClientRect(hWnd, &rect);

			HBRUSH brush = CreateSolidBrush(RGB(24, 24, 24));
			FillRect(hdc, &rect, brush);
			DeleteObject(brush);

			SetBkMode(hdc, TRANSPARENT);
			SetTextColor(hdc, RGB(235, 235, 235));
			DrawTextW(hdc,
				L"OpenVR Display Redirect Sample\nWaiting for IVRVirtualDisplay::Present...",
				-1,
				&rect,
				DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);
			EndPaint(hWnd, &ps);
			return 0;
		}

		default:
			break;
		}

		return DefWindowProcW(hWnd, uMessage, wParam, lParam);
	}

	bool PreviewWindow::CreatePreviewWindow()
	{
		WNDCLASSEXW wc{};
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = &PreviewWindow::WindowProc;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		wc.lpszClassName = k_pchWindowClassName;

		if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
		{
			Log("display_redirect_sample: RegisterClassExW failed: %lu", GetLastError());
			return false;
		}

		RECT rect{ 0, 0, static_cast<LONG>(m_nWindowWidth), static_cast<LONG>(m_nWindowHeight) };
		AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

		m_hWnd = CreateWindowExW(
			0,
			k_pchWindowClassName,
			L"OpenVR Display Redirect Preview",
			WS_OVERLAPPEDWINDOW | WS_VISIBLE,
			CW_USEDEFAULT,
			CW_USEDEFAULT,
			rect.right - rect.left,
			rect.bottom - rect.top,
			nullptr,
			nullptr,
			GetModuleHandleW(nullptr),
			nullptr);

		if (!m_hWnd)
		{
			Log("display_redirect_sample: CreateWindowExW failed: %lu", GetLastError());
			return false;
		}

		ShowWindow(m_hWnd, SW_SHOWNORMAL);
		UpdateWindow(m_hWnd);

		Log("display_redirect_sample: preview window created hwnd=%p", m_hWnd);
		return true;
	}

	void PreviewWindow::ThreadMain()
	{
		Log("display_redirect_sample: preview thread entered");

		if (!CreatePreviewWindow())
		{
			Log("display_redirect_sample: preview thread exiting because window creation failed");
			return;
		}

		while (!m_bStop.load())
		{
			MSG msg{};
			while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
			{
				TranslateMessage(&msg);
				DispatchMessageW(&msg);
			}

			PendingFrame frame{};
			bool bHaveFrame = false;
			{
				std::unique_lock lock(m_frameMutex);
				m_frameCv.wait_for(lock, std::chrono::milliseconds(4), [this] {
					return m_bFramePending || m_bStop.load();
					});

				if (m_bFramePending)
				{
					frame = m_pendingFrame;
					m_bFramePending = false;
					bHaveFrame = true;
				}
			}

			if (bHaveFrame)
			{
				RenderFrame(frame);
			}
		}

		if (m_hWnd)
		{
			DestroyWindow(m_hWnd);
			m_hWnd = nullptr;
		}

		Log("display_redirect_sample: preview thread exited");
	}

	bool PreviewWindow::OpenSharedTexture(vr::SharedTextureHandle_t hTexture)
	{
		if (hTexture == INVALID_SHARED_TEXTURE_HANDLE)
		{
			return false;
		}

		if (m_sharedTexture && hTexture == m_hOpenedTexture)
		{
			return true;
		}

		m_sharedTexture.Reset();
		m_hOpenedTexture = INVALID_SHARED_TEXTURE_HANDLE;

		HANDLE hNative = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(hTexture));
		HRESULT hr = m_device->OpenSharedResource(hNative, IID_PPV_ARGS(&m_sharedTexture));

		if (FAILED(hr))
		{
			ComPtr< ID3D11Device1 > device1;
			if (SUCCEEDED(m_device.As(&device1)))
			{
				hr = device1->OpenSharedResource1(hNative, IID_PPV_ARGS(&m_sharedTexture));
			}
		}

		if (FAILED(hr) || !m_sharedTexture)
		{
			Log("display_redirect_sample: failed to open shared backbuffer handle 0x%llx: 0x%08x",
				static_cast<unsigned long long>(hTexture), static_cast<unsigned>(hr));
			return false;
		}

		m_hOpenedTexture = hTexture;

		D3D11_TEXTURE2D_DESC desc{};
		m_sharedTexture->GetDesc(&desc);
		Log("display_redirect_sample: opened compositor backbuffer: handle=0x%llx size=%ux%u format=%u array=%u samples=%u",
			static_cast<unsigned long long>(hTexture), desc.Width, desc.Height,
			static_cast<unsigned>(desc.Format), desc.ArraySize, desc.SampleDesc.Count);
		return true;
	}

	DXGI_FORMAT PreviewWindow::GetSwapChainFormat(DXGI_FORMAT srcFormat)
	{
		switch (srcFormat)
		{
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		case DXGI_FORMAT_R8G8B8A8_UNORM:
			return DXGI_FORMAT_R8G8B8A8_UNORM;

		case DXGI_FORMAT_B8G8R8A8_TYPELESS:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		case DXGI_FORMAT_B8G8R8A8_UNORM:
			return DXGI_FORMAT_B8G8R8A8_UNORM;

		case DXGI_FORMAT_R10G10B10A2_TYPELESS:
		case DXGI_FORMAT_R10G10B10A2_UNORM:
			return DXGI_FORMAT_R10G10B10A2_UNORM;

		case DXGI_FORMAT_R16G16B16A16_TYPELESS:
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
			return DXGI_FORMAT_R16G16B16A16_FLOAT;

		default:
			return DXGI_FORMAT_UNKNOWN;
		}
	}

	bool PreviewWindow::EnsureSwapChain(const D3D11_TEXTURE2D_DESC& srcDesc)
	{
		const DXGI_FORMAT swapFormat = GetSwapChainFormat(srcDesc.Format);
		if (swapFormat == DXGI_FORMAT_UNKNOWN)
		{
			Log("display_redirect_sample: source format %u is not supported by the simple preview swapchain",
				static_cast<unsigned>(srcDesc.Format));
			return false;
		}

		if (m_swapChain &&
			m_nSwapChainWidth == srcDesc.Width &&
			m_nSwapChainHeight == srcDesc.Height &&
			m_swapChainFormat == swapFormat)
		{
			return true;
		}

		m_swapChain.Reset();

		DXGI_SWAP_CHAIN_DESC1 desc{};
		desc.Width = srcDesc.Width;
		desc.Height = srcDesc.Height;
		desc.Format = swapFormat;
		desc.Stereo = FALSE;
		desc.SampleDesc.Count = 1;
		desc.SampleDesc.Quality = 0;
		desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		desc.BufferCount = 2;
		desc.Scaling = DXGI_SCALING_STRETCH;
		desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

		HRESULT hr = m_factory->CreateSwapChainForHwnd(
			m_device.Get(), m_hWnd, &desc, nullptr, nullptr, &m_swapChain);
		if (FAILED(hr))
		{
			Log("display_redirect_sample: CreateSwapChainForHwnd failed for %ux%u format %u: 0x%08x",
				srcDesc.Width, srcDesc.Height, static_cast<unsigned>(swapFormat), static_cast<unsigned>(hr));
			return false;
		}

		m_factory->MakeWindowAssociation(m_hWnd, DXGI_MWA_NO_ALT_ENTER);

		m_nSwapChainWidth = srcDesc.Width;
		m_nSwapChainHeight = srcDesc.Height;
		m_swapChainFormat = swapFormat;

		Log("display_redirect_sample: preview swapchain created: %ux%u format=%u",
			m_nSwapChainWidth, m_nSwapChainHeight, static_cast<unsigned>(m_swapChainFormat));
		return true;
	}

	void PreviewWindow::RenderFrame(const PendingFrame& frame)
	{
		if (!OpenSharedTexture(frame.hTexture))
		{
			return;
		}

		D3D11_TEXTURE2D_DESC srcDesc{};
		m_sharedTexture->GetDesc(&srcDesc);

		if (!EnsureSwapChain(srcDesc))
		{
			return;
		}

		ComPtr< IDXGIKeyedMutex > keyedMutex;
		const bool bHasKeyedMutex = SUCCEEDED(m_sharedTexture.As(&keyedMutex));
		if (bHasKeyedMutex)
		{
			HRESULT hrAcquire = keyedMutex->AcquireSync(0, 10);
			if (hrAcquire != S_OK)
			{
				static uint32_t s_nAcquireFailures = 0;
				if ((++s_nAcquireFailures % 120) == 1)
				{
					Log("display_redirect_sample: IDXGIKeyedMutex::AcquireSync failed/timed out: 0x%08x",
						static_cast<unsigned>(hrAcquire));
				}
				return;
			}
		}

		ComPtr< ID3D11Texture2D > backBuffer;
		HRESULT hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
		if (SUCCEEDED(hr))
		{
			if (srcDesc.SampleDesc.Count > 1)
			{
				m_context->ResolveSubresource(backBuffer.Get(), 0, m_sharedTexture.Get(), 0, m_swapChainFormat);
			}
			else
			{
				m_context->CopySubresourceRegion(backBuffer.Get(), 0, 0, 0, 0, m_sharedTexture.Get(), 0, nullptr);
			}

			m_context->Flush();
		}

		if (bHasKeyedMutex)
		{
			keyedMutex->ReleaseSync(0);
		}

		if (FAILED(hr))
		{
			Log("display_redirect_sample: IDXGISwapChain::GetBuffer failed: 0x%08x", static_cast<unsigned>(hr));
			return;
		}

		hr = m_swapChain->Present(0, 0);
		if (FAILED(hr))
		{
			Log("display_redirect_sample: preview Present failed: 0x%08x", static_cast<unsigned>(hr));
			return;
		}

		if ((frame.nFrameId % 60) == 0 && m_hWnd)
		{
			wchar_t title[256]{};
			swprintf_s(title,
				L"OpenVR Display Redirect Preview - frame %llu - %ux%u",
				static_cast<unsigned long long>(frame.nFrameId),
				srcDesc.Width,
				srcDesc.Height);
			SetWindowTextW(m_hWnd, title);
		}
	}

	void PreviewWindow::ReleaseFrameResources()
	{
		m_swapChain.Reset();
		m_sharedTexture.Reset();
		m_context.Reset();
		m_device.Reset();
		m_adapter.Reset();
		m_factory.Reset();
		m_hOpenedTexture = INVALID_SHARED_TEXTURE_HANDLE;
	}
} // namespace sample
