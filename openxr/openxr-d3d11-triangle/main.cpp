#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

void CheckXr(XrResult result, const char* what)
{
    if (XR_FAILED(result))
        throw std::runtime_error(std::string(what) + " failed with XrResult " + std::to_string(result));
}

void CheckHr(HRESULT hr, const char* what)
{
    if (FAILED(hr)) {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "0x%08X", static_cast<unsigned>(hr));
        throw std::runtime_error(std::string(what) + " failed with HRESULT " + buffer);
    }
}

bool SameLuid(const LUID& a, const LUID& b)
{
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

struct Vertex
{
    float position[2];
    float color[3];
};

struct Swapchain
{
    XrSwapchain handle = XR_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<XrSwapchainImageD3D11KHR> images;
    std::vector<ComPtr<ID3D11RenderTargetView>> renderTargetViews;
};

class App
{
public:
    ~App()
    {
        Cleanup();
    }

    void Run()
    {
        CreateInstance();
        GetSystem();
        CreateD3D11Device();
        CreateSession();
        CreateReferenceSpace();
        CreateSwapchains();
        CreateGraphicsPipeline();
        SelectBlendMode();

        std::cout << "OpenXR D3D11 triangle running.\n";
        std::cout << "Close the OpenXR session/runtime to exit.\n";

        while (!exitRequested_) {
            PollEvents();

            if (!sessionRunning_) {
                Sleep(10);
                continue;
            }

            RenderFrame();
        }
    }

private:
    void CreateInstance()
    {
        uint32_t extensionCount = 0;
        CheckXr(
            xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr),
            "xrEnumerateInstanceExtensionProperties");

        std::vector<XrExtensionProperties> extensions(
            extensionCount, {XR_TYPE_EXTENSION_PROPERTIES});
        CheckXr(
            xrEnumerateInstanceExtensionProperties(
                nullptr, extensionCount, &extensionCount, extensions.data()),
            "xrEnumerateInstanceExtensionProperties");

        const bool hasD3D11 = std::any_of(
            extensions.begin(), extensions.end(),
            [](const XrExtensionProperties& ext) {
                return std::strcmp(ext.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0;
            });

        if (!hasD3D11)
            throw std::runtime_error("The active OpenXR runtime does not support XR_KHR_D3D11_enable.");

        const char* enabledExtensions[] = {
            XR_KHR_D3D11_ENABLE_EXTENSION_NAME,
        };

        XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
        std::strncpy(
            createInfo.applicationInfo.applicationName,
            "OpenXR D3D11 Triangle",
            XR_MAX_APPLICATION_NAME_SIZE - 1);
        createInfo.applicationInfo.applicationVersion = 1;
        std::strncpy(
            createInfo.applicationInfo.engineName,
            "None",
            XR_MAX_ENGINE_NAME_SIZE - 1);
        createInfo.applicationInfo.engineVersion = 1;
        createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
        createInfo.enabledExtensionCount = static_cast<uint32_t>(std::size(enabledExtensions));
        createInfo.enabledExtensionNames = enabledExtensions;

        CheckXr(xrCreateInstance(&createInfo, &instance_), "xrCreateInstance");
    }

    void GetSystem()
    {
        XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
        systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

        CheckXr(xrGetSystem(instance_, &systemInfo, &systemId_), "xrGetSystem");

        XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
        CheckXr(xrGetSystemProperties(instance_, systemId_, &properties), "xrGetSystemProperties");

        std::cout << "OpenXR system: " << properties.systemName << "\n";
    }

    void CreateD3D11Device()
    {
        PFN_xrGetD3D11GraphicsRequirementsKHR getRequirements = nullptr;
        CheckXr(
            xrGetInstanceProcAddr(
                instance_,
                "xrGetD3D11GraphicsRequirementsKHR",
                reinterpret_cast<PFN_xrVoidFunction*>(&getRequirements)),
            "xrGetInstanceProcAddr(xrGetD3D11GraphicsRequirementsKHR)");

        if (!getRequirements)
            throw std::runtime_error("xrGetD3D11GraphicsRequirementsKHR was not returned by the loader.");

        XrGraphicsRequirementsD3D11KHR requirements{
            XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        CheckXr(
            getRequirements(instance_, systemId_, &requirements),
            "xrGetD3D11GraphicsRequirementsKHR");

        ComPtr<IDXGIFactory1> factory;
        CheckHr(
            CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())),
            "CreateDXGIFactory1");

        for (UINT adapterIndex = 0;; ++adapterIndex) {
            ComPtr<IDXGIAdapter1> candidate;
            const HRESULT hr = factory->EnumAdapters1(
                adapterIndex, candidate.ReleaseAndGetAddressOf());

            if (hr == DXGI_ERROR_NOT_FOUND)
                break;
            CheckHr(hr, "IDXGIFactory1::EnumAdapters1");

            DXGI_ADAPTER_DESC1 desc{};
            CheckHr(candidate->GetDesc1(&desc), "IDXGIAdapter1::GetDesc1");

            if (SameLuid(desc.AdapterLuid, requirements.adapterLuid)) {
                adapter_ = candidate;
                std::wcout << L"D3D11 adapter: " << desc.Description << L"\n";
                break;
            }
        }

        if (!adapter_)
            throw std::runtime_error("Could not find the DXGI adapter requested by the OpenXR runtime.");

        const std::array<D3D_FEATURE_LEVEL, 6> allFeatureLevels = {
            D3D_FEATURE_LEVEL_12_1,
            D3D_FEATURE_LEVEL_12_0,
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
        };

        std::vector<D3D_FEATURE_LEVEL> requestedFeatureLevels;
        for (D3D_FEATURE_LEVEL level : allFeatureLevels) {
            if (level >= requirements.minFeatureLevel)
                requestedFeatureLevels.push_back(level);
        }

        if (requestedFeatureLevels.empty())
            throw std::runtime_error("OpenXR requested a D3D feature level unsupported by this sample.");

        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

        D3D_FEATURE_LEVEL createdFeatureLevel{};
        HRESULT hr = D3D11CreateDevice(
            adapter_.Get(),
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            flags,
            requestedFeatureLevels.data(),
            static_cast<UINT>(requestedFeatureLevels.size()),
            D3D11_SDK_VERSION,
            device_.GetAddressOf(),
            &createdFeatureLevel,
            context_.GetAddressOf());

#if defined(_DEBUG)
        // The debug layer may not be installed. Retry without it.
        if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG)) {
            flags &= ~D3D11_CREATE_DEVICE_DEBUG;
            hr = D3D11CreateDevice(
                adapter_.Get(),
                D3D_DRIVER_TYPE_UNKNOWN,
                nullptr,
                flags,
                requestedFeatureLevels.data(),
                static_cast<UINT>(requestedFeatureLevels.size()),
                D3D11_SDK_VERSION,
                device_.GetAddressOf(),
                &createdFeatureLevel,
                context_.GetAddressOf());
        }
#endif

        CheckHr(hr, "D3D11CreateDevice");

        if (createdFeatureLevel < requirements.minFeatureLevel)
            throw std::runtime_error("Created D3D11 device does not meet the OpenXR minimum feature level.");
    }

    void CreateSession()
    {
        XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
        binding.device = device_.Get();

        XrSessionCreateInfo createInfo{XR_TYPE_SESSION_CREATE_INFO};
        createInfo.next = &binding;
        createInfo.systemId = systemId_;

        CheckXr(xrCreateSession(instance_, &createInfo, &session_), "xrCreateSession");
    }

    void CreateReferenceSpace()
    {
        XrReferenceSpaceCreateInfo createInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        createInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        createInfo.poseInReferenceSpace.orientation.w = 1.0f;

        CheckXr(
            xrCreateReferenceSpace(session_, &createInfo, &appSpace_),
            "xrCreateReferenceSpace");
    }

    int64_t ChooseSwapchainFormat(const std::vector<int64_t>& formats)
    {
        const std::array<int64_t, 4> preferred = {
            DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
            DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            DXGI_FORMAT_B8G8R8A8_UNORM,
        };

        for (int64_t candidate : preferred) {
            if (std::find(formats.begin(), formats.end(), candidate) != formats.end())
                return candidate;
        }

        throw std::runtime_error("Runtime did not expose a color format this sample supports.");
    }

    void CreateSwapchains()
    {
        uint32_t viewCount = 0;
        CheckXr(
            xrEnumerateViewConfigurationViews(
                instance_,
                systemId_,
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                0,
                &viewCount,
                nullptr),
            "xrEnumerateViewConfigurationViews");

        if (viewCount == 0)
            throw std::runtime_error("Runtime returned zero stereo views.");

        viewConfigViews_.assign(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
        CheckXr(
            xrEnumerateViewConfigurationViews(
                instance_,
                systemId_,
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                viewCount,
                &viewCount,
                viewConfigViews_.data()),
            "xrEnumerateViewConfigurationViews");

        views_.assign(viewCount, {XR_TYPE_VIEW});
        projectionViews_.assign(viewCount, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});

        uint32_t formatCount = 0;
        CheckXr(
            xrEnumerateSwapchainFormats(session_, 0, &formatCount, nullptr),
            "xrEnumerateSwapchainFormats");

        std::vector<int64_t> formats(formatCount);
        CheckXr(
            xrEnumerateSwapchainFormats(
                session_, formatCount, &formatCount, formats.data()),
            "xrEnumerateSwapchainFormats");

        const int64_t colorFormat = ChooseSwapchainFormat(formats);

        swapchains_.resize(viewCount);

        for (uint32_t i = 0; i < viewCount; ++i) {
            Swapchain& swapchain = swapchains_[i];
            swapchain.width = viewConfigViews_[i].recommendedImageRectWidth;
            swapchain.height = viewConfigViews_[i].recommendedImageRectHeight;

            XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            createInfo.usageFlags =
                XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
            createInfo.format = colorFormat;
            createInfo.sampleCount = 1;
            createInfo.width = swapchain.width;
            createInfo.height = swapchain.height;
            createInfo.faceCount = 1;
            createInfo.arraySize = 1;
            createInfo.mipCount = 1;

            CheckXr(
                xrCreateSwapchain(session_, &createInfo, &swapchain.handle),
                "xrCreateSwapchain");

            uint32_t imageCount = 0;
            CheckXr(
                xrEnumerateSwapchainImages(
                    swapchain.handle, 0, &imageCount, nullptr),
                "xrEnumerateSwapchainImages");

            swapchain.images.assign(
                imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});

            CheckXr(
                xrEnumerateSwapchainImages(
                    swapchain.handle,
                    imageCount,
                    &imageCount,
                    reinterpret_cast<XrSwapchainImageBaseHeader*>(
                        swapchain.images.data())),
                "xrEnumerateSwapchainImages");

            swapchain.renderTargetViews.resize(imageCount);

            for (uint32_t imageIndex = 0; imageIndex < imageCount; ++imageIndex) {
                CheckHr(
                    device_->CreateRenderTargetView(
                        swapchain.images[imageIndex].texture,
                        nullptr,
                        swapchain.renderTargetViews[imageIndex].GetAddressOf()),
                    "ID3D11Device::CreateRenderTargetView");
            }
        }

        std::cout << "Created " << viewCount << " eye swapchains.\n";
    }

    void CreateGraphicsPipeline()
    {
        static const char* shaderSource = R"(
struct VSInput {
    float2 position : POSITION;
    float3 color    : COLOR0;
};

struct VSOutput {
    float4 position : SV_Position;
    float3 color    : COLOR0;
};

VSOutput VSMain(VSInput input) {
    VSOutput output;
    output.position = float4(input.position, 0.0, 1.0);
    output.color = input.color;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target {
    return float4(input.color, 1.0);
}
)";

        UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
        compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

        ComPtr<ID3DBlob> vsBlob;
        ComPtr<ID3DBlob> psBlob;
        ComPtr<ID3DBlob> errors;

        HRESULT hr = D3DCompile(
            shaderSource,
            std::strlen(shaderSource),
            nullptr,
            nullptr,
            nullptr,
            "VSMain",
            "vs_4_0",
            compileFlags,
            0,
            vsBlob.GetAddressOf(),
            errors.GetAddressOf());

        if (FAILED(hr)) {
            const char* message =
                errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown shader compile error";
            throw std::runtime_error(std::string("Vertex shader compilation failed: ") + message);
        }

        errors.Reset();
        hr = D3DCompile(
            shaderSource,
            std::strlen(shaderSource),
            nullptr,
            nullptr,
            nullptr,
            "PSMain",
            "ps_4_0",
            compileFlags,
            0,
            psBlob.GetAddressOf(),
            errors.GetAddressOf());

        if (FAILED(hr)) {
            const char* message =
                errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown shader compile error";
            throw std::runtime_error(std::string("Pixel shader compilation failed: ") + message);
        }

        CheckHr(
            device_->CreateVertexShader(
                vsBlob->GetBufferPointer(),
                vsBlob->GetBufferSize(),
                nullptr,
                vertexShader_.GetAddressOf()),
            "ID3D11Device::CreateVertexShader");

        CheckHr(
            device_->CreatePixelShader(
                psBlob->GetBufferPointer(),
                psBlob->GetBufferSize(),
                nullptr,
                pixelShader_.GetAddressOf()),
            "ID3D11Device::CreatePixelShader");

        const D3D11_INPUT_ELEMENT_DESC inputElements[] = {
            {
                "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0,
                static_cast<UINT>(offsetof(Vertex, position)),
                D3D11_INPUT_PER_VERTEX_DATA, 0
            },
            {
                "COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
                static_cast<UINT>(offsetof(Vertex, color)),
                D3D11_INPUT_PER_VERTEX_DATA, 0
            },
        };

        CheckHr(
            device_->CreateInputLayout(
                inputElements,
                static_cast<UINT>(std::size(inputElements)),
                vsBlob->GetBufferPointer(),
                vsBlob->GetBufferSize(),
                inputLayout_.GetAddressOf()),
            "ID3D11Device::CreateInputLayout");

        const Vertex vertices[] = {
            {{ 0.0f,  0.60f}, {1.0f, 0.1f, 0.1f}},
            {{ 0.60f, -0.55f}, {0.1f, 1.0f, 0.1f}},
            {{-0.60f, -0.55f}, {0.1f, 0.3f, 1.0f}},
        };

        D3D11_BUFFER_DESC bufferDesc{};
        bufferDesc.ByteWidth = sizeof(vertices);
        bufferDesc.Usage = D3D11_USAGE_IMMUTABLE;
        bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

        D3D11_SUBRESOURCE_DATA initialData{};
        initialData.pSysMem = vertices;

        CheckHr(
            device_->CreateBuffer(
                &bufferDesc, &initialData, vertexBuffer_.GetAddressOf()),
            "ID3D11Device::CreateBuffer");
    }

    void SelectBlendMode()
    {
        uint32_t blendModeCount = 0;
        CheckXr(
            xrEnumerateEnvironmentBlendModes(
                instance_,
                systemId_,
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                0,
                &blendModeCount,
                nullptr),
            "xrEnumerateEnvironmentBlendModes");

        std::vector<XrEnvironmentBlendMode> blendModes(blendModeCount);
        CheckXr(
            xrEnumerateEnvironmentBlendModes(
                instance_,
                systemId_,
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                blendModeCount,
                &blendModeCount,
                blendModes.data()),
            "xrEnumerateEnvironmentBlendModes");

        const auto opaque = std::find(
            blendModes.begin(), blendModes.end(), XR_ENVIRONMENT_BLEND_MODE_OPAQUE);

        if (opaque != blendModes.end())
            blendMode_ = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        else if (!blendModes.empty())
            blendMode_ = blendModes.front();
        else
            throw std::runtime_error("Runtime returned no environment blend modes.");
    }

    void PollEvents()
    {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};

        for (;;) {
            const XrResult result = xrPollEvent(instance_, &event);

            if (result == XR_EVENT_UNAVAILABLE)
                break;

            CheckXr(result, "xrPollEvent");

            const auto* header =
                reinterpret_cast<const XrEventDataBaseHeader*>(&event);

            if (header->type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const auto* stateChanged =
                    reinterpret_cast<const XrEventDataSessionStateChanged*>(header);

                sessionState_ = stateChanged->state;

                switch (sessionState_) {
                case XR_SESSION_STATE_READY: {
                    XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
                    beginInfo.primaryViewConfigurationType =
                        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    CheckXr(xrBeginSession(session_, &beginInfo), "xrBeginSession");
                    sessionRunning_ = true;
                    break;
                }

                case XR_SESSION_STATE_STOPPING:
                    if (sessionRunning_) {
                        CheckXr(xrEndSession(session_), "xrEndSession");
                        sessionRunning_ = false;
                    }
                    break;

                case XR_SESSION_STATE_EXITING:
                case XR_SESSION_STATE_LOSS_PENDING:
                    exitRequested_ = true;
                    break;

                default:
                    break;
                }
            } else if (header->type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
                exitRequested_ = true;
            }

            event = {XR_TYPE_EVENT_DATA_BUFFER};
        }
    }

    void RenderFrame()
    {
        XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState frameState{XR_TYPE_FRAME_STATE};
        CheckXr(xrWaitFrame(session_, &waitInfo, &frameState), "xrWaitFrame");

        XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
        CheckXr(xrBeginFrame(session_, &beginInfo), "xrBeginFrame");

        std::vector<const XrCompositionLayerBaseHeader*> layers;

        XrCompositionLayerProjection projectionLayer{
            XR_TYPE_COMPOSITION_LAYER_PROJECTION};

        if (frameState.shouldRender) {
            XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
            locateInfo.viewConfigurationType =
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            locateInfo.displayTime = frameState.predictedDisplayTime;
            locateInfo.space = appSpace_;

            XrViewState viewState{XR_TYPE_VIEW_STATE};
            uint32_t viewCountOutput = 0;

            CheckXr(
                xrLocateViews(
                    session_,
                    &locateInfo,
                    &viewState,
                    static_cast<uint32_t>(views_.size()),
                    &viewCountOutput,
                    views_.data()),
                "xrLocateViews");

            const XrViewStateFlags requiredFlags =
                XR_VIEW_STATE_POSITION_VALID_BIT |
                XR_VIEW_STATE_ORIENTATION_VALID_BIT;

            if ((viewState.viewStateFlags & requiredFlags) == requiredFlags &&
                viewCountOutput == swapchains_.size()) {

                for (uint32_t i = 0; i < viewCountOutput; ++i) {
                    RenderEye(i);

                    XrCompositionLayerProjectionView& projectionView =
                        projectionViews_[i];

                    projectionView = {
                        XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                    projectionView.pose = views_[i].pose;
                    projectionView.fov = views_[i].fov;
                    projectionView.subImage.swapchain = swapchains_[i].handle;
                    projectionView.subImage.imageRect.offset = {0, 0};
                    projectionView.subImage.imageRect.extent = {
                        static_cast<int32_t>(swapchains_[i].width),
                        static_cast<int32_t>(swapchains_[i].height)};
                    projectionView.subImage.imageArrayIndex = 0;
                }

                projectionLayer.space = appSpace_;
                projectionLayer.viewCount = viewCountOutput;
                projectionLayer.views = projectionViews_.data();

                layers.push_back(
                    reinterpret_cast<const XrCompositionLayerBaseHeader*>(
                        &projectionLayer));
            }
        }

        XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
        endInfo.displayTime = frameState.predictedDisplayTime;
        endInfo.environmentBlendMode = blendMode_;
        endInfo.layerCount = static_cast<uint32_t>(layers.size());
        endInfo.layers = layers.empty() ? nullptr : layers.data();

        CheckXr(xrEndFrame(session_, &endInfo), "xrEndFrame");
    }

    void RenderEye(uint32_t eyeIndex)
    {
        Swapchain& swapchain = swapchains_[eyeIndex];

        XrSwapchainImageAcquireInfo acquireInfo{
            XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};

        uint32_t imageIndex = 0;
        CheckXr(
            xrAcquireSwapchainImage(
                swapchain.handle, &acquireInfo, &imageIndex),
            "xrAcquireSwapchainImage");

        XrSwapchainImageWaitInfo waitInfo{
            XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        waitInfo.timeout = XR_INFINITE_DURATION;

        CheckXr(
            xrWaitSwapchainImage(swapchain.handle, &waitInfo),
            "xrWaitSwapchainImage");

        ID3D11RenderTargetView* rtv =
            swapchain.renderTargetViews[imageIndex].Get();

        const float clearColor[4] = {0.02f, 0.02f, 0.04f, 1.0f};
        context_->ClearRenderTargetView(rtv, clearColor);
        context_->OMSetRenderTargets(1, &rtv, nullptr);

        D3D11_VIEWPORT viewport{};
        viewport.TopLeftX = 0.0f;
        viewport.TopLeftY = 0.0f;
        viewport.Width = static_cast<float>(swapchain.width);
        viewport.Height = static_cast<float>(swapchain.height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        context_->RSSetViewports(1, &viewport);

        const UINT stride = sizeof(Vertex);
        const UINT offset = 0;
        ID3D11Buffer* vertexBuffer = vertexBuffer_.Get();

        context_->IASetInputLayout(inputLayout_.Get());
        context_->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
        context_->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
        context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
        context_->Draw(3, 0);

        // Make the rendering commands visible to the runtime before release.
        context_->Flush();

        XrSwapchainImageReleaseInfo releaseInfo{
            XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        CheckXr(
            xrReleaseSwapchainImage(swapchain.handle, &releaseInfo),
            "xrReleaseSwapchainImage");
    }

    void Cleanup()
    {
        if (context_)
            context_->ClearState();

        for (Swapchain& swapchain : swapchains_) {
            swapchain.renderTargetViews.clear();
            swapchain.images.clear();

            if (swapchain.handle != XR_NULL_HANDLE) {
                xrDestroySwapchain(swapchain.handle);
                swapchain.handle = XR_NULL_HANDLE;
            }
        }

        if (appSpace_ != XR_NULL_HANDLE) {
            xrDestroySpace(appSpace_);
            appSpace_ = XR_NULL_HANDLE;
        }

        if (session_ != XR_NULL_HANDLE) {
            xrDestroySession(session_);
            session_ = XR_NULL_HANDLE;
        }

        context_.Reset();
        device_.Reset();
        adapter_.Reset();

        if (instance_ != XR_NULL_HANDLE) {
            xrDestroyInstance(instance_);
            instance_ = XR_NULL_HANDLE;
        }
    }

private:
    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId systemId_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace appSpace_ = XR_NULL_HANDLE;

    XrSessionState sessionState_ = XR_SESSION_STATE_UNKNOWN;
    bool sessionRunning_ = false;
    bool exitRequested_ = false;

    XrEnvironmentBlendMode blendMode_ = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

    ComPtr<IDXGIAdapter1> adapter_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;

    ComPtr<ID3D11VertexShader> vertexShader_;
    ComPtr<ID3D11PixelShader> pixelShader_;
    ComPtr<ID3D11InputLayout> inputLayout_;
    ComPtr<ID3D11Buffer> vertexBuffer_;

    std::vector<XrViewConfigurationView> viewConfigViews_;
    std::vector<XrView> views_;
    std::vector<XrCompositionLayerProjectionView> projectionViews_;
    std::vector<Swapchain> swapchains_;
};

} // namespace

int main()
{
    try {
        App app;
        app.Run();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
}
