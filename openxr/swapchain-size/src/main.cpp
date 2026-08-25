#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

namespace
{

    constexpr int64_t kGlRgba8 = 0x8058;
    constexpr int64_t kGlSrgb8Alpha8 = 0x8C43;
    constexpr float kMinScale = 0.25f;
    constexpr float kMaxScale = 1.50f;
    constexpr float kScaleStep = 0.10f;

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

        std::vector<XrExtensionProperties> extensions( count, { XR_TYPE_EXTENSION_PROPERTIES } );
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
                      "OpenGL Swapchain Resize Repro",
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

    XrSystemProperties GetSystemProperties( XrInstance instance, XrSystemId systemId )
    {
        XrSystemProperties properties{ XR_TYPE_SYSTEM_PROPERTIES };
        CheckXr( xrGetSystemProperties( instance, systemId, &properties ), "xrGetSystemProperties" );
        return properties;
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

        GLFWwindow *window = glfwCreateWindow(
            1100, 180, "OpenXR OpenGL swapchain resize repro", nullptr, nullptr );
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
            Fail( "Created OpenGL context is older than the runtime's minimum supported version" );

        std::cout << "OpenGL context: " << glMajor << '.' << glMinor << '\n';
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

    XrSpace CreateLocalSpace( XrSession session )
    {
        XrReferenceSpaceCreateInfo createInfo{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
        createInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        createInfo.poseInReferenceSpace.orientation.w = 1.0f;

        XrSpace space = XR_NULL_HANDLE;
        CheckXr( xrCreateReferenceSpace( session, &createInfo, &space ), "xrCreateReferenceSpace(LOCAL)" );
        return space;
    }

    std::vector<XrViewConfigurationView> GetViewConfigurationViews( XrInstance instance, XrSystemId systemId )
    {
        uint32_t count = 0;
        CheckXr( xrEnumerateViewConfigurationViews(
            instance,
            systemId,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            0,
            &count,
            nullptr ),
            "xrEnumerateViewConfigurationViews(count)" );

        std::vector<XrViewConfigurationView> views( count, { XR_TYPE_VIEW_CONFIGURATION_VIEW } );
        CheckXr( xrEnumerateViewConfigurationViews(
            instance,
            systemId,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            count,
            &count,
            views.data() ),
                 "xrEnumerateViewConfigurationViews" );

        if ( views.size() < 2 )
            Fail( "PRIMARY_STEREO returned fewer than two views" );

        return views;
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

    void PutPixel( std::vector<Rgba> &pixels, uint32_t width, uint32_t height, int x, int y, Rgba color )
    {
        if ( x < 0 || y < 0 || x >= static_cast<int>( width ) || y >= static_cast<int>( height ) )
            return;

        pixels[ static_cast<size_t>( y ) * width + static_cast<size_t>( x ) ] = color;
    }

    void FillRect( std::vector<Rgba> &pixels,
                   uint32_t width,
                   uint32_t height,
                   int x0,
                   int y0,
                   int x1,
                   int y1,
                   Rgba color )
    {
        x0 = std::clamp( x0, 0, static_cast<int>( width ) );
        x1 = std::clamp( x1, 0, static_cast<int>( width ) );
        y0 = std::clamp( y0, 0, static_cast<int>( height ) );
        y1 = std::clamp( y1, 0, static_cast<int>( height ) );

        for ( int y = y0; y < y1; ++y )
        {
            for ( int x = x0; x < x1; ++x )
                PutPixel( pixels, width, height, x, y, color );
        }
    }

    void DrawEyeMarker( std::vector<Rgba> &pixels,
                        uint32_t width,
                        uint32_t height,
                        uint32_t eyeIndex,
                        Rgba color )
    {
        const int cx = static_cast<int>( width / 2 );
        const int cy = static_cast<int>( height / 2 );
        const int w = std::max( 8, static_cast<int>( width / 24 ) );
        const int h = std::max( 12, static_cast<int>( height / 10 ) );
        const int t = std::max( 3, std::min( w, h ) / 5 );

        if ( eyeIndex == 0 )
        {
            // Big L.
            FillRect( pixels, width, height, cx - w, cy - h, cx - w + t, cy + h, color );
            FillRect( pixels, width, height, cx - w, cy - h, cx + w, cy - h + t, color );
        }
        else
        {
            // Big R: stem, upper/lower bars, right stem for upper bowl and a diagonal-ish leg.
            FillRect( pixels, width, height, cx - w, cy - h, cx - w + t, cy + h, color );
            FillRect( pixels, width, height, cx - w, cy + h - t, cx + w, cy + h, color );
            FillRect( pixels, width, height, cx - w, cy - t / 2, cx + w, cy + t / 2 + 1, color );
            FillRect( pixels, width, height, cx + w - t, cy, cx + w, cy + h, color );
            for ( int i = 0; i < h; ++i )
            {
                const int x = cx + ( i * w ) / h;
                FillRect( pixels, width, height, x, cy - i - t, x + t, cy - i + t, color );
            }
        }
    }

    std::vector<Rgba> BuildDiagnosticImage( uint32_t width, uint32_t height, uint32_t eyeIndex )
    {
        std::vector<Rgba> pixels( static_cast<size_t>( width ) * height );

        // Pixel storage is bottom-up, matching OpenGL's image coordinate convention.
        const Rgba topLeft{ 220, 55, 55, 255 };
        const Rgba topRight{ 45, 185, 80, 255 };
        const Rgba bottomLeft{ 35, 90, 225, 255 };
        const Rgba bottomRight{ 230, 195, 45, 255 };
        const Rgba white{ 255, 255, 255, 255 };
        const Rgba black{ 8, 8, 8, 255 };

        const uint32_t halfW = width / 2;
        const uint32_t halfH = height / 2;

        for ( uint32_t y = 0; y < height; ++y )
        {
            for ( uint32_t x = 0; x < width; ++x )
            {
                Rgba color;
                if ( y >= halfH )
                    color = ( x < halfW ) ? topLeft : topRight;
                else
                    color = ( x < halfW ) ? bottomLeft : bottomRight;

                pixels[ static_cast<size_t>( y ) * width + x ] = color;
            }
        }

        // A thick white outer border is the easiest way to spot any crop/zoom.
        const int border = std::max( 4, static_cast<int>( std::min( width, height ) / 80 ) );
        FillRect( pixels, width, height, 0, 0, static_cast<int>( width ), border, white );
        FillRect( pixels, width, height, 0, static_cast<int>( height ) - border,
                  static_cast<int>( width ), static_cast<int>( height ), white );
        FillRect( pixels, width, height, 0, 0, border, static_cast<int>( height ), white );
        FillRect( pixels, width, height, static_cast<int>( width ) - border, 0,
                  static_cast<int>( width ), static_cast<int>( height ), white );

        // Normalized 10x10 grid. It should occupy exactly the same field of view
        // regardless of the physical swapchain resolution.
        const int gridThickness = std::max( 1, border / 3 );
        for ( int i = 1; i < 10; ++i )
        {
            const int x = static_cast<int>( ( static_cast<uint64_t>( width ) * i ) / 10 );
            const int y = static_cast<int>( ( static_cast<uint64_t>( height ) * i ) / 10 );
            FillRect( pixels, width, height, x - gridThickness, 0, x + gridThickness + 1,
                      static_cast<int>( height ), black );
            FillRect( pixels, width, height, 0, y - gridThickness, static_cast<int>( width ),
                      y + gridThickness + 1, black );
        }

        // Center cross.
        const int cross = std::max( 2, border / 2 );
        FillRect( pixels, width, height, static_cast<int>( halfW ) - cross, 0,
                  static_cast<int>( halfW ) + cross + 1, static_cast<int>( height ), white );
        FillRect( pixels, width, height, 0, static_cast<int>( halfH ) - cross,
                  static_cast<int>( width ), static_cast<int>( halfH ) + cross + 1, white );

        // A fine checker patch in the middle makes the resolution change itself visible,
        // while the rest of the normalized pattern should not change size on screen.
        const int patchHalfW = std::max( 16, static_cast<int>( width / 10 ) );
        const int patchHalfH = std::max( 16, static_cast<int>( height / 10 ) );
        const int checker = std::max( 2, static_cast<int>( std::min( width, height ) / 100 ) );
        for ( int y = static_cast<int>( halfH ) - patchHalfH; y < static_cast<int>( halfH ) + patchHalfH; ++y )
        {
            for ( int x = static_cast<int>( halfW ) - patchHalfW; x < static_cast<int>( halfW ) + patchHalfW; ++x )
            {
                const bool on = ( ( x / checker ) + ( y / checker ) ) & 1;
                PutPixel( pixels, width, height, x, y, on ? white : black );
            }
        }

        DrawEyeMarker( pixels, width, height, eyeIndex, white );
        return pixels;
    }

    struct EyeSwapchain
    {
        XrSwapchain handle = XR_NULL_HANDLE;
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<XrSwapchainImageOpenGLKHR> images;
        std::vector<Rgba> diagnosticPixels;
    };

    void DestroySwapchain( EyeSwapchain &swapchain )
    {
        if ( swapchain.handle != XR_NULL_HANDLE )
            xrDestroySwapchain( swapchain.handle );

        swapchain = {};
    }

    EyeSwapchain CreateSwapchain( XrSession session,
                                  int64_t format,
                                  uint32_t width,
                                  uint32_t height,
                                  uint32_t eyeIndex )
    {
        XrSwapchainCreateInfo createInfo{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
        createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        createInfo.format = format;
        createInfo.sampleCount = 1;
        createInfo.width = width;
        createInfo.height = height;
        createInfo.faceCount = 1;
        createInfo.arraySize = 1;
        createInfo.mipCount = 1;

        EyeSwapchain swapchain;
        swapchain.width = width;
        swapchain.height = height;

        CheckXr( xrCreateSwapchain( session, &createInfo, &swapchain.handle ), "xrCreateSwapchain" );

        uint32_t imageCount = 0;
        CheckXr( xrEnumerateSwapchainImages( swapchain.handle, 0, &imageCount, nullptr ),
                 "xrEnumerateSwapchainImages(count)" );

        swapchain.images.resize( imageCount );
        for ( XrSwapchainImageOpenGLKHR &image : swapchain.images )
            image = { XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR };

        CheckXr( xrEnumerateSwapchainImages(
            swapchain.handle,
            imageCount,
            &imageCount,
            reinterpret_cast<XrSwapchainImageBaseHeader *>( swapchain.images.data() ) ),
                 "xrEnumerateSwapchainImages" );

        swapchain.diagnosticPixels = BuildDiagnosticImage( width, height, eyeIndex );
        return swapchain;
    }

    uint32_t ScaledDimension( uint32_t recommended,
                              uint32_t viewMaximum,
                              uint32_t systemMaximum,
                              float scale )
    {
        const uint32_t maximum = std::min( viewMaximum, systemMaximum );
        const uint32_t requested = static_cast<uint32_t>(
            std::max( 1.0, std::round( static_cast<double>( recommended ) * scale ) ) );
        return std::clamp( requested, 1u, maximum );
    }

    struct SwapchainSet
    {
        std::vector<EyeSwapchain> eyes;
        std::vector<uint32_t> largestWidth;
        std::vector<uint32_t> largestHeight;
        float scale = 0.50f;
    };

    void DestroySwapchains( SwapchainSet &set )
    {
        for ( EyeSwapchain &eye : set.eyes )
            DestroySwapchain( eye );
        set.eyes.clear();
    }

    void RecreateSwapchains( SwapchainSet &set,
                             XrSession session,
                             int64_t format,
                             const std::vector<XrViewConfigurationView> &viewConfigs,
                             const XrSystemProperties &systemProperties,
                             float requestedScale )
    {
        requestedScale = std::clamp( requestedScale, kMinScale, kMaxScale );

        // All swapchain images have been released before this is called. Destroying and
        // creating new XrSwapchain objects is the behavior this repro is specifically testing.
        DestroySwapchains( set );
        set.eyes.resize( viewConfigs.size() );

        if ( set.largestWidth.size() != viewConfigs.size() )
        {
            set.largestWidth.assign( viewConfigs.size(), 0 );
            set.largestHeight.assign( viewConfigs.size(), 0 );
        }

        bool smallerThanPreviousLargest = false;
        std::cout << "\nRecreating projection swapchains at "
        << static_cast<int>( std::round( requestedScale * 100.0f ) ) << "% recommended:\n";

        for ( size_t i = 0; i < viewConfigs.size(); ++i )
        {
            const XrViewConfigurationView &view = viewConfigs[ i ];
            const uint32_t width = ScaledDimension(
                view.recommendedImageRectWidth,
                view.maxImageRectWidth,
                systemProperties.graphicsProperties.maxSwapchainImageWidth,
                requestedScale );
            const uint32_t height = ScaledDimension(
                view.recommendedImageRectHeight,
                view.maxImageRectHeight,
                systemProperties.graphicsProperties.maxSwapchainImageHeight,
                requestedScale );

            if ( width < set.largestWidth[ i ] || height < set.largestHeight[ i ] )
                smallerThanPreviousLargest = true;

            set.eyes[ i ] = CreateSwapchain( session, format, width, height, static_cast<uint32_t>( i ) );
            set.largestWidth[ i ] = std::max( set.largestWidth[ i ], width );
            set.largestHeight[ i ] = std::max( set.largestHeight[ i ], height );

            std::cout << "  view " << i << ": " << width << 'x' << height
            << " (recommended " << view.recommendedImageRectWidth << 'x'
            << view.recommendedImageRectHeight << ", largest used "
            << set.largestWidth[ i ] << 'x' << set.largestHeight[ i ] << ")\n";
        }

        if ( smallerThanPreviousLargest )
        {
            std::cout << "  *** REPRO CONDITION: the newly-created swapchain is smaller than one previously submitted. ***\n"
            "  Correct behavior: the white border, 10x10 grid and all four quadrants still fill each eye.\n"
            "  Reported SteamVR bug: the image becomes zoomed/cropped toward a corner.\n";
        }

        set.scale = requestedScale;
    }

    void UploadDiagnosticImage( const EyeSwapchain &swapchain, uint32_t imageIndex )
    {
        const XrSwapchainImageOpenGLKHR &image = swapchain.images.at( imageIndex );

        glBindTexture( GL_TEXTURE_2D, image.image );
        glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
        glTexSubImage2D(
            GL_TEXTURE_2D,
            0,
            0,
            0,
            static_cast<GLsizei>( swapchain.width ),
                        static_cast<GLsizei>( swapchain.height ),
                        GL_RGBA,
                        GL_UNSIGNED_BYTE,
                        swapchain.diagnosticPixels.data() );
        glBindTexture( GL_TEXTURE_2D, 0 );
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
                XrSessionBeginInfo beginInfo{ XR_TYPE_SESSION_BEGIN_INFO };
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

    void RenderFrame( XrSession session,
                      XrSpace localSpace,
                      XrEnvironmentBlendMode blendMode,
                      const SwapchainSet &swapchains )
    {
        XrFrameWaitInfo waitInfo{ XR_TYPE_FRAME_WAIT_INFO };
        XrFrameState frameState{ XR_TYPE_FRAME_STATE };
        CheckXr( xrWaitFrame( session, &waitInfo, &frameState ), "xrWaitFrame" );

        XrFrameBeginInfo beginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
        CheckXr( xrBeginFrame( session, &beginInfo ), "xrBeginFrame" );

        XrCompositionLayerProjection projection{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
        std::vector<XrCompositionLayerProjectionView> projectionViews;
        const XrCompositionLayerBaseHeader *layer = nullptr;
        uint32_t layerCount = 0;

        if ( frameState.shouldRender )
        {
            std::vector<XrView> locatedViews( swapchains.eyes.size(), { XR_TYPE_VIEW } );
            XrViewState viewState{ XR_TYPE_VIEW_STATE };
            XrViewLocateInfo locateInfo{ XR_TYPE_VIEW_LOCATE_INFO };
            locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            locateInfo.displayTime = frameState.predictedDisplayTime;
            locateInfo.space = localSpace;

            uint32_t viewCountOutput = 0;
            CheckXr( xrLocateViews(
                session,
                &locateInfo,
                &viewState,
                static_cast<uint32_t>( locatedViews.size() ),
                                   &viewCountOutput,
                                   locatedViews.data() ),
                     "xrLocateViews" );

            if ( viewCountOutput != swapchains.eyes.size() )
                Fail( "xrLocateViews returned a different view count than xrEnumerateViewConfigurationViews" );

            projectionViews.resize( viewCountOutput );

            for ( uint32_t i = 0; i < viewCountOutput; ++i )
            {
                const EyeSwapchain &eye = swapchains.eyes[ i ];

                uint32_t imageIndex = 0;
                XrSwapchainImageAcquireInfo acquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
                CheckXr( xrAcquireSwapchainImage( eye.handle, &acquireInfo, &imageIndex ),
                         "xrAcquireSwapchainImage" );

                XrSwapchainImageWaitInfo imageWait{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
                imageWait.timeout = XR_INFINITE_DURATION;
                CheckXr( xrWaitSwapchainImage( eye.handle, &imageWait ), "xrWaitSwapchainImage" );

                UploadDiagnosticImage( eye, imageIndex );

                // Keep this repro deliberately conservative about GL/OpenXR synchronization.
                // All commands touching the swapchain image are complete before it is released.
                glFinish();

                XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                CheckXr( xrReleaseSwapchainImage( eye.handle, &releaseInfo ), "xrReleaseSwapchainImage" );

                XrCompositionLayerProjectionView &projectionView = projectionViews[ i ];
                projectionView = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
                projectionView.pose = locatedViews[ i ].pose;
                projectionView.fov = locatedViews[ i ].fov;
                projectionView.subImage.swapchain = eye.handle;
                projectionView.subImage.imageRect.offset = { 0, 0 };
                projectionView.subImage.imageRect.extent = {
                    static_cast<int32_t>( eye.width ),
                    static_cast<int32_t>( eye.height )
                };
                projectionView.subImage.imageArrayIndex = 0;
            }

            projection.space = localSpace;
            projection.viewCount = static_cast<uint32_t>( projectionViews.size() );
            projection.views = projectionViews.data();
            layer = reinterpret_cast<const XrCompositionLayerBaseHeader *>( &projection );
            layerCount = 1;
        }

        XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
        endInfo.displayTime = frameState.predictedDisplayTime;
        endInfo.environmentBlendMode = blendMode;
        endInfo.layerCount = layerCount;
        endInfo.layers = layerCount > 0 ? &layer : nullptr;
        CheckXr( xrEndFrame( session, &endInfo ), "xrEndFrame" );
    }

    struct KeyboardResult
    {
        bool recreate = false;
        float requestedScale = 0.50f;
    };

    KeyboardResult HandleKeyboard( GLFWwindow *window, float currentScale )
    {
        struct KeyState
        {
            int key;
            bool down = false;
        };

        static std::array<KeyState, 8> keys = {{
            { GLFW_KEY_1 },
            { GLFW_KEY_2 },
            { GLFW_KEY_3 },
            { GLFW_KEY_4 },
            { GLFW_KEY_LEFT_BRACKET },
            { GLFW_KEY_RIGHT_BRACKET },
            { GLFW_KEY_R },
            { GLFW_KEY_P },
        }};

        KeyboardResult result;
        result.requestedScale = currentScale;

        for ( KeyState &key : keys )
        {
            const bool nowDown = glfwGetKey( window, key.key ) == GLFW_PRESS;
            if ( nowDown && !key.down )
            {
                switch ( key.key )
                {
                    case GLFW_KEY_1:
                        result.requestedScale = 0.50f;
                        result.recreate = true;
                        break;
                    case GLFW_KEY_2:
                        result.requestedScale = 1.00f;
                        result.recreate = true;
                        break;
                    case GLFW_KEY_3:
                        result.requestedScale = 1.25f;
                        result.recreate = true;
                        break;
                    case GLFW_KEY_4:
                        result.requestedScale = 1.50f;
                        result.recreate = true;
                        break;
                    case GLFW_KEY_LEFT_BRACKET:
                        result.requestedScale = std::max( kMinScale, currentScale - kScaleStep );
                        result.recreate = true;
                        break;
                    case GLFW_KEY_RIGHT_BRACKET:
                        result.requestedScale = std::min( kMaxScale, currentScale + kScaleStep );
                        result.recreate = true;
                        break;
                    case GLFW_KEY_R:
                        // Destroy and recreate at the exact same requested scale.
                        result.requestedScale = currentScale;
                        result.recreate = true;
                        break;
                    case GLFW_KEY_P:
                        std::cout << "Current requested scale: "
                        << static_cast<int>( std::round( currentScale * 100.0f ) ) << "%\n";
                        break;
                    default:
                        break;
                }
            }
            key.down = nowDown;
        }

        if ( glfwGetKey( window, GLFW_KEY_ESCAPE ) == GLFW_PRESS )
            glfwSetWindowShouldClose( window, GLFW_TRUE );

        return result;
    }

    void UpdateWindowTitle( GLFWwindow *window, const SwapchainSet &swapchains )
    {
        std::string title = "OpenXR GL swapchain resize | ";
        title += std::to_string( static_cast<int>( std::round( swapchains.scale * 100.0f ) ) );
        title += "%";

        if ( !swapchains.eyes.empty() )
        {
            title += " | eye0=" + std::to_string( swapchains.eyes[ 0 ].width ) + "x" +
            std::to_string( swapchains.eyes[ 0 ].height );
            title += " | largest=" + std::to_string( swapchains.largestWidth[ 0 ] ) + "x" +
            std::to_string( swapchains.largestHeight[ 0 ] );
        }

        title += " | 1=50% 2=100% 3=125% 4=150% [ ]=step R=recreate Esc=quit";
        glfwSetWindowTitle( window, title.c_str() );
    }

} // namespace

int main()
{
    int exitCode = 0;
    XrInstance instance = XR_NULL_HANDLE;
    XrSession session = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE;
    GLFWwindow *window = nullptr;
    SwapchainSet swapchains;

    try
    {
        instance = CreateInstance();

        XrInstanceProperties instanceProperties{ XR_TYPE_INSTANCE_PROPERTIES };
        CheckXr( xrGetInstanceProperties( instance, &instanceProperties ), "xrGetInstanceProperties" );
        std::cout << "OpenXR runtime: " << instanceProperties.runtimeName << ' '
        << XR_VERSION_MAJOR( instanceProperties.runtimeVersion ) << '.'
        << XR_VERSION_MINOR( instanceProperties.runtimeVersion ) << '.'
        << XR_VERSION_PATCH( instanceProperties.runtimeVersion ) << '\n';

        const XrSystemId systemId = GetSystem( instance );
        const XrSystemProperties systemProperties = GetSystemProperties( instance, systemId );
        const XrGraphicsRequirementsOpenGLKHR requirements = GetOpenGlRequirements( instance, systemId );
        const std::vector<XrViewConfigurationView> viewConfigs = GetViewConfigurationViews( instance, systemId );

        std::cout << "View configuration:\n";
        for ( size_t i = 0; i < viewConfigs.size(); ++i )
        {
            const XrViewConfigurationView &view = viewConfigs[ i ];
            std::cout << "  view " << i
            << ": recommended=" << view.recommendedImageRectWidth << 'x'
            << view.recommendedImageRectHeight
            << " max=" << view.maxImageRectWidth << 'x' << view.maxImageRectHeight
            << '\n';
        }

        window = CreateWindowAndContext( requirements );
        session = CreateSession( instance, systemId, window );
        localSpace = CreateLocalSpace( session );

        const XrEnvironmentBlendMode blendMode = ChooseBlendMode( instance, systemId );
        const int64_t swapchainFormat = ChooseSwapchainFormat( session );

        RecreateSwapchains(
            swapchains,
            session,
            swapchainFormat,
            viewConfigs,
            systemProperties,
            0.50f );
        UpdateWindowTitle( window, swapchains );

        std::cout <<
        "\nPurpose:\n"
        "  Reproduce a runtime bug where a newly-created OpenGL projection swapchain\n"
        "  smaller than the largest swapchain previously submitted is treated as if it\n"
        "  still had the old/larger dimensions, producing a zoomed/cropped image.\n\n"
        "The diagnostic image is normalized to every newly-created swapchain:\n"
        "  red    = top-left quadrant\n"
        "  green  = top-right quadrant\n"
        "  blue   = bottom-left quadrant\n"
        "  yellow = bottom-right quadrant\n"
        "  thick white outer border + 10x10 grid should always fill each eye\n\n"
        "Recommended repro sequence:\n"
        "  1. Start at 50% (key 1). Observe the full pattern.\n"
        "  2. Press 3 or 4 to create/use a larger swapchain.\n"
        "  3. Press 1 to destroy it and create a new smaller swapchain.\n"
        "  4. The projection view imageRect is still exactly {0,0,currentWidth,currentHeight}.\n"
        "     Correct: same complete pattern, only lower resolution.\n"
        "     Reported bug: zoom/crop into part of the pattern.\n\n"
        "Controls:\n"
        "  1             recreate at 50% recommended\n"
        "  2             recreate at 100% recommended\n"
        "  3             recreate at 125% recommended\n"
        "  4             recreate at 150% recommended (clamped to runtime limits)\n"
        "  [ / ]         decrease/increase requested scale by 10%\n"
        "  R             destroy/recreate at the current size\n"
        "  P             print current scale\n"
        "  Escape        quit\n\n";

        SessionState xrState;
        while ( !glfwWindowShouldClose( window ) && !xrState.exitRequested )
        {
            glfwPollEvents();
            PollXrEvents( instance, session, xrState );

            const KeyboardResult keyboard = HandleKeyboard( window, swapchains.scale );
            if ( keyboard.recreate )
            {
                RecreateSwapchains(
                    swapchains,
                    session,
                    swapchainFormat,
                    viewConfigs,
                    systemProperties,
                    keyboard.requestedScale );
                UpdateWindowTitle( window, swapchains );
            }

            // The desktop window only owns the GL context and receives controls.
            glViewport( 0, 0, 1100, 180 );
            glClearColor( 0.07f, 0.07f, 0.07f, 1.0f );
            glClear( GL_COLOR_BUFFER_BIT );
            glfwSwapBuffers( window );

            if ( xrState.running )
                RenderFrame( session, localSpace, blendMode, swapchains );
        }
    }
    catch ( const std::exception &e )
    {
        std::cerr << "ERROR: " << e.what() << '\n';
        exitCode = 1;
    }

    DestroySwapchains( swapchains );

    if ( localSpace != XR_NULL_HANDLE )
        xrDestroySpace( localSpace );
    if ( session != XR_NULL_HANDLE )
        xrDestroySession( session );
    if ( instance != XR_NULL_HANDLE )
        xrDestroyInstance( instance );

    if ( window != nullptr )
        glfwDestroyWindow( window );
    glfwTerminate();

    return exitCode;
}
