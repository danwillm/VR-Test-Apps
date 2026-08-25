#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <openvr.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;

static void CheckHR(HRESULT hr, const char* what)
{
    if (FAILED(hr))
    {
        char buffer[128];
        sprintf_s(buffer, "%s failed (HRESULT 0x%08X)", what, static_cast<unsigned>(hr));
        throw std::runtime_error(buffer);
    }
}

static ComPtr<ID3DBlob> CompileShader(const char* source, const char* entryPoint, const char* target)
{
    ComPtr<ID3DBlob> shader;
    ComPtr<ID3DBlob> errors;

    const UINT flags =
#ifdef _DEBUG
        D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    const HRESULT hr = D3DCompile(
        source,
        strlen(source),
        nullptr,
        nullptr,
        nullptr,
        entryPoint,
        target,
        flags,
        0,
        &shader,
        &errors);

    if (FAILED(hr))
    {
        if (errors)
            std::cerr << static_cast<const char*>(errors->GetBufferPointer()) << '\n';

        CheckHR(hr, "D3DCompile");
    }

    return shader;
}

struct EyeTarget
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> rtv;
};

static EyeTarget CreateEyeTarget(ID3D11Device* device, uint32_t width, uint32_t height)
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    EyeTarget target;
    CheckHR(device->CreateTexture2D(&desc, nullptr, &target.texture), "CreateTexture2D");
    CheckHR(device->CreateRenderTargetView(target.texture.Get(), nullptr, &target.rtv),
            "CreateRenderTargetView");
    return target;
}

static void RenderEye(
    ID3D11DeviceContext* context,
    ID3D11VertexShader* vertexShader,
    ID3D11PixelShader* pixelShader,
    ID3D11RenderTargetView* rtv,
    uint32_t width,
    uint32_t height)
{
    const float clearColor[4] = { 0.02f, 0.02f, 0.04f, 1.0f };
    context->ClearRenderTargetView(rtv, clearColor);

    ID3D11RenderTargetView* renderTargets[] = { rtv };
    context->OMSetRenderTargets(1, renderTargets, nullptr);

    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    context->RSSetViewports(1, &viewport);

    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertexShader, nullptr, 0);
    context->PSSetShader(pixelShader, nullptr, 0);

    // The vertex shader generates all three vertices from SV_VertexID.
    context->Draw(3, 0);
}

int main()
{
    vr::IVRSystem* vrSystem = nullptr;

    try
    {
        // ---------------------------------------------------------------------
        // OpenVR
        // ---------------------------------------------------------------------
        vr::EVRInitError initError = vr::VRInitError_None;
        vrSystem = vr::VR_Init(&initError, vr::VRApplication_Scene);

        if (initError != vr::VRInitError_None || !vrSystem)
        {
            throw std::runtime_error(
                std::string("VR_Init failed: ") +
                vr::VR_GetVRInitErrorAsEnglishDescription(initError));
        }

        vr::IVRCompositor* compositor = vr::VRCompositor();
        if (!compositor)
            throw std::runtime_error("VRCompositor() returned nullptr");

        uint32_t eyeWidth = 0;
        uint32_t eyeHeight = 0;
        vrSystem->GetRecommendedRenderTargetSize(&eyeWidth, &eyeHeight);

        std::cout << "OpenVR eye render size: "
                  << eyeWidth << " x " << eyeHeight << '\n';

        // OpenVR tells D3D10/11 applications which DXGI adapter to use.
        int32_t adapterIndex = -1;
        vrSystem->GetDXGIOutputInfo(&adapterIndex);

        if (adapterIndex < 0)
            throw std::runtime_error("OpenVR did not provide a valid DXGI adapter index");

        // ---------------------------------------------------------------------
        // D3D11 device
        // ---------------------------------------------------------------------
        ComPtr<IDXGIFactory1> factory;
        CheckHR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");

        ComPtr<IDXGIAdapter> adapter;
        CheckHR(factory->EnumAdapters(static_cast<UINT>(adapterIndex), &adapter),
                "IDXGIFactory::EnumAdapters");

        DXGI_ADAPTER_DESC adapterDesc = {};
        if (SUCCEEDED(adapter->GetDesc(&adapterDesc)))
            std::wcout << L"Using adapter: " << adapterDesc.Description << L'\n';

        const D3D_FEATURE_LEVEL requestedLevels[] = {
            D3D_FEATURE_LEVEL_11_0
        };

        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL createdLevel = {};

        CheckHR(
            D3D11CreateDevice(
                adapter.Get(),
                D3D_DRIVER_TYPE_UNKNOWN,
                nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                requestedLevels,
                _countof(requestedLevels),
                D3D11_SDK_VERSION,
                &device,
                &createdLevel,
                &context),
            "D3D11CreateDevice");

        EyeTarget leftEye = CreateEyeTarget(device.Get(), eyeWidth, eyeHeight);
        EyeTarget rightEye = CreateEyeTarget(device.Get(), eyeWidth, eyeHeight);

        // ---------------------------------------------------------------------
        // Very small shaders: no vertex buffer or constant buffers are needed.
        // ---------------------------------------------------------------------
        static const char* shaderSource = R"(
struct VSOutput
{
    float4 position : SV_Position;
    float3 color    : COLOR0;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    const float2 positions[3] =
    {
        float2( 0.00,  0.65),
        float2( 0.65, -0.55),
        float2(-0.65, -0.55)
    };

    const float3 colors[3] =
    {
        float3(1.0, 0.1, 0.1),
        float3(0.1, 1.0, 0.1),
        float3(0.1, 0.2, 1.0)
    };

    VSOutput output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    output.color = colors[vertexId];
    return output;
}

float4 PSMain(VSOutput input) : SV_Target
{
    return float4(input.color, 1.0);
}
)";

        const ComPtr<ID3DBlob> vsBlob = CompileShader(shaderSource, "VSMain", "vs_5_0");
        const ComPtr<ID3DBlob> psBlob = CompileShader(shaderSource, "PSMain", "ps_5_0");

        ComPtr<ID3D11VertexShader> vertexShader;
        ComPtr<ID3D11PixelShader> pixelShader;

        CheckHR(
            device->CreateVertexShader(
                vsBlob->GetBufferPointer(),
                vsBlob->GetBufferSize(),
                nullptr,
                &vertexShader),
            "CreateVertexShader");

        CheckHR(
            device->CreatePixelShader(
                psBlob->GetBufferPointer(),
                psBlob->GetBufferSize(),
                nullptr,
                &pixelShader),
            "CreatePixelShader");

        std::cout << "Running. Press Escape to quit.\n";

        // ---------------------------------------------------------------------
        // Main loop
        // ---------------------------------------------------------------------
        bool running = true;

        while (running)
        {
            vr::VREvent_t event = {};
            while (vrSystem->PollNextEvent(&event, sizeof(event)))
            {
                if (event.eventType == vr::VREvent_Quit)
                {
                    vrSystem->AcknowledgeQuit_Exiting();
                    running = false;
                }
            }

            if (!running || (GetAsyncKeyState(VK_ESCAPE) & 0x8000))
                break;

            vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount] = {};
            const vr::EVRCompositorError waitError =
                compositor->WaitGetPoses(
                    poses,
                    vr::k_unMaxTrackedDeviceCount,
                    nullptr,
                    0);

            if (waitError != vr::VRCompositorError_None)
            {
                std::cerr << "WaitGetPoses failed: " << static_cast<int>(waitError) << '\n';
                continue;
            }

            // This intentionally ignores the poses. The goal is only to show the
            // smallest possible D3D11 -> OpenVR rendering/submission path.
            RenderEye(
                context.Get(),
                vertexShader.Get(),
                pixelShader.Get(),
                leftEye.rtv.Get(),
                eyeWidth,
                eyeHeight);

            RenderEye(
                context.Get(),
                vertexShader.Get(),
                pixelShader.Get(),
                rightEye.rtv.Get(),
                eyeWidth,
                eyeHeight);

            // Ensure our immediate-context work has been submitted before the
            // textures are handed to the compositor.
            context->Flush();

            vr::Texture_t leftTexture = {
                leftEye.texture.Get(),
                vr::TextureType_DirectX,
                vr::ColorSpace_Gamma
            };

            vr::Texture_t rightTexture = {
                rightEye.texture.Get(),
                vr::TextureType_DirectX,
                vr::ColorSpace_Gamma
            };

            const vr::EVRCompositorError leftError =
                compositor->Submit(vr::Eye_Left, &leftTexture);

            const vr::EVRCompositorError rightError =
                compositor->Submit(vr::Eye_Right, &rightTexture);

            if (leftError != vr::VRCompositorError_None ||
                rightError != vr::VRCompositorError_None)
            {
                std::cerr
                    << "Submit failed. Left=" << static_cast<int>(leftError)
                    << " Right=" << static_cast<int>(rightError) << '\n';
            }

            compositor->PostPresentHandoff();
        }

        vr::VR_Shutdown();
        vrSystem = nullptr;
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Fatal error: " << e.what() << '\n';

        if (vrSystem)
            vr::VR_Shutdown();

        return 1;
    }
}
