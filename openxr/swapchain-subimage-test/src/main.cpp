#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#define GLFW_EXPOSE_NATIVE_WGL
#elif defined(__linux__)
#include <X11/Xlib.h>
#include <GL/glx.h>
#define GLFW_EXPOSE_NATIVE_X11
#define GLFW_EXPOSE_NATIVE_GLX
#else
#error This sample currently supports Windows and Linux/X11.
#endif

#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#define XR_USE_GRAPHICS_API_OPENGL
#ifdef _WIN32
#define XR_USE_PLATFORM_WIN32
#elif defined(__linux__)
#define XR_USE_PLATFORM_XLIB
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace {

    constexpr int kSwapchainWidth = 1024;
    constexpr int kSwapchainHeight = 1024;
    constexpr int kDefaultTestHeight = ( kSwapchainHeight * 3 ) / 4;
    constexpr int kStep = 32;

    // Values from the OpenGL registry. Defining them here avoids needing an
    // extension-function loader just to name the formats.
    constexpr int64_t kGlRgba8 = 0x8058;
    constexpr int64_t kGlSrgb8Alpha8 = 0x8C43;

    struct Rgba
    {
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint8_t a;
    };

    [[noreturn]] void Fail( const std::string &message )
    {
        throw std::runtime_error( message );
    }

    void CheckXr( XrResult result, const char *what )
    {
        if ( XR_FAILED( result ) )
        {
            Fail( std::string( what ) + " failed with XrResult " + std::to_string( result ) );
        }
    }

    bool HasExtension( const char *name )
    {
        uint32_t count = 0;
        CheckXr( xrEnumerateInstanceExtensionProperties( nullptr, 0, &count, nullptr ),
                 "xrEnumerateInstanceExtensionProperties(count)" );

        std::vector<XrExtensionProperties> extensions( count, { XR_TYPE_EXTENSION_PROPERTIES, nullptr } );
        CheckXr( xrEnumerateInstanceExtensionProperties( nullptr, count, &count, extensions.data() ),
                 "xrEnumerateInstanceExtensionProperties" );

        for ( const XrExtensionProperties &extension : extensions )
        {
            if ( std::strcmp( extension.extensionName, name ) == 0 )
                return true;
        }
        return false;
    }

    XrInstance CreateInstance()
    {
        if ( !HasExtension( XR_KHR_OPENGL_ENABLE_EXTENSION_NAME ) )
            Fail( "Active OpenXR runtime does not expose XR_KHR_opengl_enable" );

        const char *extensions[] = { XR_KHR_OPENGL_ENABLE_EXTENSION_NAME };

        XrInstanceCreateInfo createInfo{ XR_TYPE_INSTANCE_CREATE_INFO };
        std::strncpy( createInfo.applicationInfo.applicationName,
                      "OpenGL Subimage Origin Repro",
                      XR_MAX_APPLICATION_NAME_SIZE - 1 );
        createInfo.applicationInfo.applicationVersion = 1;
        std::strncpy( createInfo.applicationInfo.engineName,
                      "none",
                      XR_MAX_ENGINE_NAME_SIZE - 1 );
        createInfo.applicationInfo.engineVersion = 1;
        createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
        createInfo.enabledExtensionCount = 1;
        createInfo.enabledExtensionNames = extensions;

        XrInstance instance = XR_NULL_HANDLE;
        CheckXr( xrCreateInstance( &createInfo, &instance ), "xrCreateInstance" );
        return instance;
    }

    XrSystemId GetSystem( XrInstance instance )
    {
        XrSystemGetInfo systemInfo{ XR_TYPE_SYSTEM_GET_INFO };
        systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

        XrSystemId systemId = XR_NULL_SYSTEM_ID;
        CheckXr( xrGetSystem( instance, &systemInfo, &systemId ), "xrGetSystem" );
        return systemId;
    }

    XrGraphicsRequirementsOpenGLKHR GetOpenGlRequirements( XrInstance instance, XrSystemId systemId )
    {
        PFN_xrGetOpenGLGraphicsRequirementsKHR getRequirements = nullptr;
        CheckXr( xrGetInstanceProcAddr(
            instance,
            "xrGetOpenGLGraphicsRequirementsKHR",
            reinterpret_cast<PFN_xrVoidFunction *>( &getRequirements ) ),
                 "xrGetInstanceProcAddr(xrGetOpenGLGraphicsRequirementsKHR)" );

        if ( getRequirements == nullptr )
            Fail( "xrGetOpenGLGraphicsRequirementsKHR was null" );

        XrGraphicsRequirementsOpenGLKHR requirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
        CheckXr( getRequirements( instance, systemId, &requirements ),
                 "xrGetOpenGLGraphicsRequirementsKHR" );
        return requirements;
    }

    GLFWwindow *CreateWindowAndContext( const XrGraphicsRequirementsOpenGLKHR &requirements )
    {
        if ( glfwInit() != GLFW_TRUE )
            Fail( "glfwInit failed" );

        const int major = static_cast<int>( XR_VERSION_MAJOR( requirements.minApiVersionSupported ) );
        const int minor = static_cast<int>( XR_VERSION_MINOR( requirements.minApiVersionSupported ) );

        glfwWindowHint( GLFW_CONTEXT_VERSION_MAJOR, std::max( major, 1 ) );
        glfwWindowHint( GLFW_CONTEXT_VERSION_MINOR, minor );
        glfwWindowHint( GLFW_VISIBLE, GLFW_TRUE );

        // Do not request a core-only profile. The actual repro path only uses core
        // texture calls, but leaving the profile unconstrained is friendlier to
        // older desktop OpenGL drivers/runtime requirements.
        GLFWwindow *window = glfwCreateWindow( 900, 180, "OpenXR OpenGL Subimage Origin Repro", nullptr, nullptr );
        if ( window == nullptr )
            Fail( "glfwCreateWindow failed" );

        glfwMakeContextCurrent( window );
        glfwSwapInterval( 0 );

        int glMajor = 0;
        int glMinor = 0;
        const char *versionString = reinterpret_cast<const char *>( glGetString( GL_VERSION ) );
        if ( versionString == nullptr || std::sscanf( versionString, "%d.%d", &glMajor, &glMinor ) != 2 )
            Fail( "Could not parse GL_VERSION" );

        const XrVersion actualVersion = XR_MAKE_VERSION( glMajor, glMinor, 0 );
        if ( actualVersion < requirements.minApiVersionSupported )
        {
            Fail( "Created OpenGL context is older than the runtime's minimum supported version" );
        }

        std::cout << "OpenGL context: " << glMajor << '.' << glMinor << '\n';
        std::cout << "Runtime OpenGL minimum: "
        << XR_VERSION_MAJOR( requirements.minApiVersionSupported ) << '.'
        << XR_VERSION_MINOR( requirements.minApiVersionSupported ) << '\n';

        return window;
    }

    #ifdef __linux__
    struct LinuxGlxBindingData
    {
        XrGraphicsBindingOpenGLXlibKHR binding{ XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR };
    };

    LinuxGlxBindingData MakeLinuxGlxBinding( GLFWwindow *window )
    {
        LinuxGlxBindingData result;

        Display *display = glfwGetX11Display();
        GLXContext context = glfwGetGLXContext( window );
        GLXWindow drawable = glfwGetGLXWindow( window );

        if ( display == nullptr || context == nullptr || drawable == 0 )
            Fail( "Could not obtain GLFW X11/GLX native handles" );

        int fbConfigId = 0;
        if ( glXQueryContext( display, context, GLX_FBCONFIG_ID, &fbConfigId ) != Success )
            Fail( "glXQueryContext(GLX_FBCONFIG_ID) failed" );

        int configCount = 0;
        GLXFBConfig *configs = glXGetFBConfigs( display, DefaultScreen( display ), &configCount );
        if ( configs == nullptr || configCount < 1 )
            Fail( "Could not enumerate GLXFBConfigs" );

        GLXFBConfig selectedConfig = nullptr;
        for ( int i = 0; i < configCount; ++i )
        {
            int candidateId = 0;
            if ( glXGetFBConfigAttrib( display, configs[ i ], GLX_FBCONFIG_ID, &candidateId ) == Success &&
                candidateId == fbConfigId )
            {
                selectedConfig = configs[ i ];
                break;
            }
        }

        if ( selectedConfig == nullptr )
        {
            XFree( configs );
            Fail( "Could not recover GLXFBConfig for GLFW context" );
        }

        XVisualInfo *visualInfo = glXGetVisualFromFBConfig( display, selectedConfig );
        if ( visualInfo == nullptr )
        {
            XFree( configs );
            Fail( "glXGetVisualFromFBConfig failed" );
        }

        result.binding.xDisplay = display;
        result.binding.visualid = visualInfo->visualid;
        result.binding.glxFBConfig = selectedConfig;
        result.binding.glxDrawable = drawable;
        result.binding.glxContext = context;

        XFree( visualInfo );
        XFree( configs );
        return result;
    }
    #endif

    XrSession CreateSession( XrInstance instance, XrSystemId systemId, GLFWwindow *window )
    {
        XrSessionCreateInfo createInfo{ XR_TYPE_SESSION_CREATE_INFO };
        createInfo.systemId = systemId;

        #ifdef _WIN32
        XrGraphicsBindingOpenGLWin32KHR binding{ XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR };
        HWND hwnd = glfwGetWin32Window( window );
        binding.hDC = GetDC( hwnd );
        binding.hGLRC = glfwGetWGLContext( window );
        if ( binding.hDC == nullptr || binding.hGLRC == nullptr )
            Fail( "Could not obtain GLFW Win32/WGL native handles" );
        createInfo.next = &binding;

        XrSession session = XR_NULL_HANDLE;
        CheckXr( xrCreateSession( instance, &createInfo, &session ), "xrCreateSession" );
        return session;
        #elif defined(__linux__)
        LinuxGlxBindingData native = MakeLinuxGlxBinding( window );
        createInfo.next = &native.binding;

        XrSession session = XR_NULL_HANDLE;
        CheckXr( xrCreateSession( instance, &createInfo, &session ), "xrCreateSession" );
        return session;
        #endif
    }

    XrSpace CreateViewSpace( XrSession session )
    {
        XrReferenceSpaceCreateInfo createInfo{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
        createInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        createInfo.poseInReferenceSpace.orientation.w = 1.0f;

        XrSpace space = XR_NULL_HANDLE;
        CheckXr( xrCreateReferenceSpace( session, &createInfo, &space ), "xrCreateReferenceSpace(VIEW)" );
        return space;
    }

    XrEnvironmentBlendMode ChooseBlendMode( XrInstance instance, XrSystemId systemId )
    {
        uint32_t count = 0;
        CheckXr( xrEnumerateEnvironmentBlendModes(
            instance,
            systemId,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            0,
            &count,
            nullptr ),
            "xrEnumerateEnvironmentBlendModes(count)" );

        std::vector<XrEnvironmentBlendMode> modes( count );
        CheckXr( xrEnumerateEnvironmentBlendModes(
            instance,
            systemId,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            count,
            &count,
            modes.data() ),
                 "xrEnumerateEnvironmentBlendModes" );

        for ( XrEnvironmentBlendMode mode : modes )
        {
            if ( mode == XR_ENVIRONMENT_BLEND_MODE_OPAQUE )
                return mode;
        }

        if ( modes.empty() )
            Fail( "Runtime returned no environment blend modes" );
        return modes.front();
    }

    int64_t ChooseSwapchainFormat( XrSession session )
    {
        uint32_t count = 0;
        CheckXr( xrEnumerateSwapchainFormats( session, 0, &count, nullptr ),
                 "xrEnumerateSwapchainFormats(count)" );

        std::vector<int64_t> formats( count );
        CheckXr( xrEnumerateSwapchainFormats( session, count, &count, formats.data() ),
                 "xrEnumerateSwapchainFormats" );

        const std::array<int64_t, 2> preferred = { kGlRgba8, kGlSrgb8Alpha8 };
        for ( int64_t wanted : preferred )
        {
            if ( std::find( formats.begin(), formats.end(), wanted ) != formats.end() )
                return wanted;
        }

        std::cerr << "Runtime swapchain formats:";
        for ( int64_t format : formats )
            std::cerr << " 0x" << std::hex << format << std::dec;
        std::cerr << '\n';
        Fail( "Runtime did not advertise GL_RGBA8 or GL_SRGB8_ALPHA8" );
    }

    struct Swapchain
    {
        XrSwapchain handle = XR_NULL_HANDLE;
        std::vector<XrSwapchainImageOpenGLKHR> images;
    };

    Swapchain CreateSwapchain( XrSession session )
    {
        XrSwapchainCreateInfo createInfo{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
        createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        createInfo.format = ChooseSwapchainFormat( session );
        createInfo.sampleCount = 1;
        createInfo.width = kSwapchainWidth;
        createInfo.height = kSwapchainHeight;
        createInfo.faceCount = 1;
        createInfo.arraySize = 1;
        createInfo.mipCount = 1;

        Swapchain swapchain;
        CheckXr( xrCreateSwapchain( session, &createInfo, &swapchain.handle ), "xrCreateSwapchain" );

        uint32_t count = 0;
        CheckXr( xrEnumerateSwapchainImages( swapchain.handle, 0, &count, nullptr ),
                 "xrEnumerateSwapchainImages(count)" );

        swapchain.images.resize( count );
        for ( XrSwapchainImageOpenGLKHR &image : swapchain.images )
            image = { XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR };

        CheckXr( xrEnumerateSwapchainImages(
            swapchain.handle,
            count,
            &count,
            reinterpret_cast<XrSwapchainImageBaseHeader *>( swapchain.images.data() ) ),
                 "xrEnumerateSwapchainImages" );

        return swapchain;
    }

    void PutPixel( std::vector<Rgba> &pixels, int x, int y, Rgba color )
    {
        if ( x < 0 || x >= kSwapchainWidth || y < 0 || y >= kSwapchainHeight )
            return;
        pixels[ static_cast<size_t>( y ) * kSwapchainWidth + static_cast<size_t>( x ) ] = color;
    }

    using Glyph = std::array<uint8_t, 7>;

    Glyph GetGlyph( char ch )
    {
        // 5x7 glyphs, most-significant five bits used.
        switch ( ch )
        {
            case 'T': return { 0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100 };
            case 'O': return { 0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110 };
            case 'P': return { 0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000 };
            case 'B': return { 0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110 };
            case 'M': return { 0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001 };
            default:  return { 0, 0, 0, 0, 0, 0, 0 };
        }
    }

    void DrawText( std::vector<Rgba> &pixels,
                   const std::string &text,
                   int x,
                   int y,
                   int scale,
                   Rgba color )
    {
        int cursor = x;
        for ( char ch : text )
        {
            if ( ch == ' ' )
            {
                cursor += 4 * scale;
                continue;
            }

            const Glyph glyph = GetGlyph( ch );
            for ( int row = 0; row < 7; ++row )
            {
                for ( int col = 0; col < 5; ++col )
                {
                    if ( ( glyph[ row ] & ( 1u << ( 4 - col ) ) ) == 0 )
                        continue;

                    for ( int yy = 0; yy < scale; ++yy )
                    {
                        for ( int xx = 0; xx < scale; ++xx )
                        {
                            // Text coordinates are specified bottom-up here.
                            PutPixel( pixels,
                                      cursor + col * scale + xx,
                                      y + ( 6 - row ) * scale + yy,
                                      color );
                        }
                    }
                }
            }
            cursor += 6 * scale;
        }
    }

    std::vector<Rgba> BuildDiagnosticImage()
    {
        std::vector<Rgba> pixels( static_cast<size_t>( kSwapchainWidth ) * kSwapchainHeight );

        const Rgba blue{ 25, 90, 240, 255 };
        const Rgba green{ 25, 200, 85, 255 };
        const Rgba yellow{ 240, 210, 35, 255 };
        const Rgba red{ 225, 45, 45, 255 };
        const Rgba white{ 255, 255, 255, 255 };
        const Rgba black{ 0, 0, 0, 255 };

        for ( int y = 0; y < kSwapchainHeight; ++y )
        {
            Rgba band = blue;
            if ( y >= kSwapchainHeight * 3 / 4 )
                band = red;
            else if ( y >= kSwapchainHeight / 2 )
                band = yellow;
            else if ( y >= kSwapchainHeight / 4 )
                band = green;

            for ( int x = 0; x < kSwapchainWidth; ++x )
            {
                Rgba color = band;

                // A vertical checker rail makes the overall texture orientation
                // obvious without affecting the Y-origin test.
                if ( x < 44 )
                {
                    const bool checker = ( ( y / 32 ) & 1 ) != 0;
                    color = checker ? white : black;
                }

                pixels[ static_cast<size_t>( y ) * kSwapchainWidth + static_cast<size_t>( x ) ] = color;
            }
        }

        // White separators at each quarter boundary.
        for ( int boundary : { kSwapchainHeight / 4, kSwapchainHeight / 2, kSwapchainHeight * 3 / 4 } )
        {
            for ( int y = boundary - 3; y <= boundary + 3; ++y )
            {
                for ( int x = 44; x < kSwapchainWidth; ++x )
                    PutPixel( pixels, x, y, white );
            }
        }

        DrawText( pixels, "BOTTOM", 300, 70, 12, white );
        DrawText( pixels, "TOP", 390, kSwapchainHeight - 160, 12, white );
        return pixels;
    }

    void UploadDiagnosticImage( const XrSwapchainImageOpenGLKHR &image, const std::vector<Rgba> &pixels )
    {
        glBindTexture( GL_TEXTURE_2D, image.image );
        glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
        glTexSubImage2D(
            GL_TEXTURE_2D,
            0,
            0,
            0,
            kSwapchainWidth,
            kSwapchainHeight,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            pixels.data() );
        glBindTexture( GL_TEXTURE_2D, 0 );

        // Keep the sample maximally deterministic: the runtime only sees a released
        // swapchain image after the upload has completed.
        glFinish();
    }

    struct TestRect
    {
        int logicalY = 0;
        int height = kDefaultTestHeight;
        bool applyTopLeftWorkaround = false;
    };

    int SubmittedY( const TestRect &rect )
    {
        if ( rect.applyTopLeftWorkaround )
            return kSwapchainHeight - rect.height - rect.logicalY;
        return rect.logicalY;
    }

    void ClampTestRect( TestRect &rect )
    {
        rect.height = std::clamp( rect.height, kStep, kSwapchainHeight );
        rect.logicalY = std::clamp( rect.logicalY, 0, kSwapchainHeight - rect.height );
    }

    void PrintRect( const TestRect &rect )
    {
        const int submittedY = SubmittedY( rect );
        std::cout << "Test rect: logical OpenGL y=" << rect.logicalY
        << ", height=" << rect.height
        << ", submitted y=" << submittedY
        << ( rect.applyTopLeftWorkaround ? " [top-left workaround ON]" : " [spec coordinates]" )
        << '\n';

        if ( !rect.applyTopLeftWorkaround && rect.logicalY == 0 && rect.height == kDefaultTestHeight )
        {
            std::cout << "Expected by XR_KHR_opengl_enable: RIGHT panel contains the BOTTOM blue band and omits TOP red.\n"
            "Buggy top-left interpretation: RIGHT panel contains TOP red and omits BOTTOM blue.\n";
        }
    }

    void UpdateWindowTitle( GLFWwindow *window, const TestRect &rect )
    {
        const int submittedY = SubmittedY( rect );
        std::string title = "OpenXR GL subimage | RIGHT: y=" + std::to_string( submittedY ) +
        " h=" + std::to_string( rect.height );
        if ( rect.applyTopLeftWorkaround )
            title += " | workaround ON";
        title += " | Up/Down=y  PgUp/PgDn=height  W=workaround  R=reset  Esc=quit";
        glfwSetWindowTitle( window, title.c_str() );
    }

    void HandleKeyboard( GLFWwindow *window, TestRect &rect )
    {
        struct KeyState
        {
            int key;
            bool down = false;
        };

        static std::array<KeyState, 7> keys = {{
            { GLFW_KEY_UP },
            { GLFW_KEY_DOWN },
            { GLFW_KEY_PAGE_UP },
            { GLFW_KEY_PAGE_DOWN },
            { GLFW_KEY_W },
            { GLFW_KEY_R },
            { GLFW_KEY_SPACE },
        }};

        bool changed = false;
        for ( KeyState &key : keys )
        {
            const bool nowDown = glfwGetKey( window, key.key ) == GLFW_PRESS;
            if ( nowDown && !key.down )
            {
                switch ( key.key )
                {
                    case GLFW_KEY_UP:
                        rect.logicalY += kStep;
                        changed = true;
                        break;
                    case GLFW_KEY_DOWN:
                        rect.logicalY -= kStep;
                        changed = true;
                        break;
                    case GLFW_KEY_PAGE_UP:
                        rect.height += kStep;
                        changed = true;
                        break;
                    case GLFW_KEY_PAGE_DOWN:
                        rect.height -= kStep;
                        changed = true;
                        break;
                    case GLFW_KEY_W:
                        rect.applyTopLeftWorkaround = !rect.applyTopLeftWorkaround;
                        changed = true;
                        break;
                    case GLFW_KEY_R:
                        rect = {};
                        changed = true;
                        break;
                    case GLFW_KEY_SPACE:
                        rect.logicalY = ( rect.logicalY == 0 ) ? ( kSwapchainHeight - rect.height ) : 0;
                        changed = true;
                        break;
                    default:
                        break;
                }
            }
            key.down = nowDown;
        }

        if ( glfwGetKey( window, GLFW_KEY_ESCAPE ) == GLFW_PRESS )
            glfwSetWindowShouldClose( window, GLFW_TRUE );

        if ( changed )
        {
            ClampTestRect( rect );
            PrintRect( rect );
            UpdateWindowTitle( window, rect );
        }
    }

    struct SessionState
    {
        bool running = false;
        bool exitRequested = false;
    };

    void PollXrEvents( XrInstance instance, XrSession session, SessionState &state )
    {
        for ( ;; )
        {
            XrEventDataBuffer event{ XR_TYPE_EVENT_DATA_BUFFER };
            const XrResult result = xrPollEvent( instance, &event );
            if ( result == XR_EVENT_UNAVAILABLE )
                break;
            CheckXr( result, "xrPollEvent" );

            if ( event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING )
            {
                state.exitRequested = true;
                continue;
            }

            if ( event.type != XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED )
                continue;

            const auto *changed = reinterpret_cast<const XrEventDataSessionStateChanged *>( &event );
            std::cout << "OpenXR session state: " << changed->state << '\n';

            if ( changed->state == XR_SESSION_STATE_READY )
            {
                XrSessionBeginInfo beginInfo{ XR_TYPE_SESSION_BEGIN_INFO, nullptr };
                beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                CheckXr( xrBeginSession( session, &beginInfo ), "xrBeginSession" );
                state.running = true;
            }
            else if ( changed->state == XR_SESSION_STATE_STOPPING )
            {
                if ( state.running )
                {
                    CheckXr( xrEndSession( session ), "xrEndSession" );
                    state.running = false;
                }
            }
            else if ( changed->state == XR_SESSION_STATE_EXITING ||
                changed->state == XR_SESSION_STATE_LOSS_PENDING )
            {
                state.exitRequested = true;
            }
        }
    }

    XrCompositionLayerQuad MakeQuad( XrSpace viewSpace,
                                     XrSwapchain swapchain,
                                     float x,
                                     XrRect2Di imageRect )
    {
        XrCompositionLayerQuad quad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
        quad.space = viewSpace;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = swapchain;
        quad.subImage.imageRect = imageRect;
        quad.subImage.imageArrayIndex = 0;
        quad.pose.orientation.w = 1.0f;
        quad.pose.position = { x, 0.0f, -1.45f };
        quad.size = { 0.76f, 0.76f };
        return quad;
    }

    void RenderFrame( XrSession session,
                      XrSpace viewSpace,
                      const Swapchain &swapchain,
                      XrEnvironmentBlendMode blendMode,
                      const std::vector<Rgba> &pixels,
                      const TestRect &testRect )
    {
        XrFrameWaitInfo waitInfo{ XR_TYPE_FRAME_WAIT_INFO };
        XrFrameState frameState{ XR_TYPE_FRAME_STATE };
        CheckXr( xrWaitFrame( session, &waitInfo, &frameState ), "xrWaitFrame" );

        XrFrameBeginInfo beginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
        CheckXr( xrBeginFrame( session, &beginInfo ), "xrBeginFrame" );

        std::array<XrCompositionLayerQuad, 2> quads{};
        std::array<const XrCompositionLayerBaseHeader *, 2> layers{};
        uint32_t layerCount = 0;

        if ( frameState.shouldRender )
        {
            uint32_t imageIndex = 0;
            XrSwapchainImageAcquireInfo acquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
            CheckXr( xrAcquireSwapchainImage( swapchain.handle, &acquireInfo, &imageIndex ),
                     "xrAcquireSwapchainImage" );

            XrSwapchainImageWaitInfo imageWait{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
            imageWait.timeout = XR_INFINITE_DURATION;
            CheckXr( xrWaitSwapchainImage( swapchain.handle, &imageWait ), "xrWaitSwapchainImage" );

            UploadDiagnosticImage( swapchain.images.at( imageIndex ), pixels );

            XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            CheckXr( xrReleaseSwapchainImage( swapchain.handle, &releaseInfo ), "xrReleaseSwapchainImage" );

            const XrRect2Di fullRect{
                { 0, 0 },
                { kSwapchainWidth, kSwapchainHeight }
            };

            const XrRect2Di croppedRect{
                { 0, SubmittedY( testRect ) },
                { kSwapchainWidth, testRect.height }
            };

            // LEFT: full source image, always the reference.
            // RIGHT: identical swapchain image, but with imageRect cropped.
            quads[ 0 ] = MakeQuad( viewSpace, swapchain.handle, -0.41f, fullRect );
            quads[ 1 ] = MakeQuad( viewSpace, swapchain.handle, +0.41f, croppedRect );
            layers[ 0 ] = reinterpret_cast<const XrCompositionLayerBaseHeader *>( &quads[ 0 ] );
            layers[ 1 ] = reinterpret_cast<const XrCompositionLayerBaseHeader *>( &quads[ 1 ] );
            layerCount = 2;
        }

        XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
        endInfo.displayTime = frameState.predictedDisplayTime;
        endInfo.environmentBlendMode = blendMode;
        endInfo.layerCount = layerCount;
        endInfo.layers = layerCount > 0 ? layers.data() : nullptr;
        CheckXr( xrEndFrame( session, &endInfo ), "xrEndFrame" );
    }

} // namespace

int main()
{
    int exitCode = 0;
    XrInstance instance = XR_NULL_HANDLE;
    XrSession session = XR_NULL_HANDLE;
    XrSpace viewSpace = XR_NULL_HANDLE;
    Swapchain swapchain;
    GLFWwindow *window = nullptr;

    try
    {
        instance = CreateInstance();

        XrInstanceProperties instanceProperties{ XR_TYPE_INSTANCE_PROPERTIES };
        CheckXr( xrGetInstanceProperties( instance, &instanceProperties ), "xrGetInstanceProperties" );
        std::cout << "OpenXR runtime: " << instanceProperties.runtimeName << " "
        << XR_VERSION_MAJOR( instanceProperties.runtimeVersion ) << '.'
        << XR_VERSION_MINOR( instanceProperties.runtimeVersion ) << '.'
        << XR_VERSION_PATCH( instanceProperties.runtimeVersion ) << '\n';

        const XrSystemId systemId = GetSystem( instance );
        const XrGraphicsRequirementsOpenGLKHR requirements = GetOpenGlRequirements( instance, systemId );

        window = CreateWindowAndContext( requirements );
        session = CreateSession( instance, systemId, window );
        viewSpace = CreateViewSpace( session );
        const XrEnvironmentBlendMode blendMode = ChooseBlendMode( instance, systemId );
        swapchain = CreateSwapchain( session );
        const std::vector<Rgba> pixels = BuildDiagnosticImage();

        TestRect testRect;
        PrintRect( testRect );
        UpdateWindowTitle( window, testRect );

        std::cout << "\nLEFT panel: full 1024x1024 source image.\n"
        "RIGHT panel: XrSwapchainSubImage crop from the SAME OpenGL swapchain.\n"
        "Default RIGHT imageRect is offset.y=0, extent.height=768.\n\n"
        "OpenGL/OpenXR expected result:\n"
        "  RIGHT keeps the bottom BLUE band and loses the top RED band.\n"
        "Top-left/+Y-down interpretation:\n"
        "  RIGHT keeps the top RED band and loses the bottom BLUE band.\n\n"
        "Controls:\n"
        "  Up/Down       change logical OpenGL offset.y by 32\n"
        "  PageUp/Down   change extent.height by 32\n"
        "  Space         jump crop between bottom and top\n"
        "  W             toggle y = H - h - y workaround\n"
        "  R             reset\n"
        "  Escape        quit\n\n";

        SessionState xrState;
        while ( !glfwWindowShouldClose( window ) && !xrState.exitRequested )
        {
            glfwPollEvents();
            HandleKeyboard( window, testRect );
            PollXrEvents( instance, session, xrState );

            // The window only exists to own the OpenGL context and provide
            // controls. Keep it visually neutral; the actual diagnostic image
            // is shown by the OpenXR compositor.
            glViewport( 0, 0, 900, 180 );
            glClearColor( 0.08f, 0.08f, 0.08f, 1.0f );
            glClear( GL_COLOR_BUFFER_BIT );
            glfwSwapBuffers( window );

            if ( xrState.running )
                RenderFrame( session, viewSpace, swapchain, blendMode, pixels, testRect );
        }
    }
    catch ( const std::exception &e )
    {
        std::cerr << "ERROR: " << e.what() << '\n';
        exitCode = 1;
    }

    if ( swapchain.handle != XR_NULL_HANDLE )
        xrDestroySwapchain( swapchain.handle );
    if ( viewSpace != XR_NULL_HANDLE )
        xrDestroySpace( viewSpace );
    if ( session != XR_NULL_HANDLE )
        xrDestroySession( session );
    if ( instance != XR_NULL_HANDLE )
        xrDestroyInstance( instance );

    if ( window != nullptr )
        glfwDestroyWindow( window );
    glfwTerminate();

    return exitCode;
}
