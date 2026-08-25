#include <GL/glew.h>
#include <SDL.h>
#include <openvr.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr float kNearClip = 0.05f;
constexpr float kFarClip = 100.0f;
constexpr int kMirrorWidth = 960;
constexpr int kMirrorHeight = 540;

struct Mat4 {
    std::array<float, 16> m{};

    static Mat4 Identity() {
        Mat4 result;
        result.m[0] = 1.0f;
        result.m[5] = 1.0f;
        result.m[10] = 1.0f;
        result.m[15] = 1.0f;
        return result;
    }
};

Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 result;
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            float value = 0.0f;
            for (int k = 0; k < 4; ++k) {
                value += a.m[k * 4 + row] * b.m[column * 4 + k];
            }
            result.m[column * 4 + row] = value;
        }
    }
    return result;
}

Mat4 Convert(const vr::HmdMatrix44_t& source) {
    Mat4 result;
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            result.m[column * 4 + row] = source.m[row][column];
        }
    }
    return result;
}

Mat4 Convert(const vr::HmdMatrix34_t& source) {
    Mat4 result = Mat4::Identity();
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 4; ++column) {
            result.m[column * 4 + row] = source.m[row][column];
        }
    }
    return result;
}

Mat4 InverseRigidBody(const Mat4& transform) {
    // Rigid transform inverse: [R t]^-1 = [R^T -R^T t].
    Mat4 result = Mat4::Identity();

    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            result.m[column * 4 + row] = transform.m[row * 4 + column];
        }
    }

    const float tx = transform.m[12];
    const float ty = transform.m[13];
    const float tz = transform.m[14];

    result.m[12] = -(result.m[0] * tx + result.m[4] * ty + result.m[8] * tz);
    result.m[13] = -(result.m[1] * tx + result.m[5] * ty + result.m[9] * tz);
    result.m[14] = -(result.m[2] * tx + result.m[6] * ty + result.m[10] * tz);
    return result;
}

struct Vertex {
    float x;
    float y;
    float z;
    float r;
    float g;
    float b;
    float a;
};

Vertex MakeVertex(const vr::HmdVector3_t& point, float r, float g, float b, float a, float yOffset = 0.0f) {
    return Vertex{point.v[0], point.v[1] + yOffset, point.v[2], r, g, b, a};
}

Vertex MakeVertex(float x, float y, float z, float r, float g, float b, float a) {
    return Vertex{x, y, z, r, g, b, a};
}

void AddLine(std::vector<Vertex>& vertices, const Vertex& a, const Vertex& b) {
    vertices.push_back(a);
    vertices.push_back(b);
}

void AddQuadTriangles(std::vector<Vertex>& vertices,
                      const std::array<vr::HmdVector3_t, 4>& corners,
                      float r,
                      float g,
                      float b,
                      float a,
                      float yOffset = 0.0f) {
    const Vertex v0 = MakeVertex(corners[0], r, g, b, a, yOffset);
    const Vertex v1 = MakeVertex(corners[1], r, g, b, a, yOffset);
    const Vertex v2 = MakeVertex(corners[2], r, g, b, a, yOffset);
    const Vertex v3 = MakeVertex(corners[3], r, g, b, a, yOffset);

    vertices.insert(vertices.end(), {v0, v1, v2, v0, v2, v3});
}

std::array<vr::HmdVector3_t, 4> QuadCorners(const vr::HmdQuad_t& quad) {
    return {quad.vCorners[0], quad.vCorners[1], quad.vCorners[2], quad.vCorners[3]};
}

class GlMesh {
  public:
    GlMesh() = default;
    GlMesh(const GlMesh&) = delete;
    GlMesh& operator=(const GlMesh&) = delete;

    ~GlMesh() { Destroy(); }

    void Destroy() {
        if (vbo_ != 0) {
            glDeleteBuffers(1, &vbo_);
            vbo_ = 0;
        }
        if (vao_ != 0) {
            glDeleteVertexArrays(1, &vao_);
            vao_ = 0;
        }
        count_ = 0;
    }

    void Upload(const std::vector<Vertex>& vertices) {
        if (vao_ == 0) {
            glGenVertexArrays(1, &vao_);
            glGenBuffers(1, &vbo_);
        }

        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(vertices.size() * sizeof(Vertex)),
                     vertices.empty() ? nullptr : vertices.data(),
                     GL_DYNAMIC_DRAW);

        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(0));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1,
                              4,
                              GL_FLOAT,
                              GL_FALSE,
                              sizeof(Vertex),
                              reinterpret_cast<void*>(3 * sizeof(float)));

        glBindVertexArray(0);
        count_ = static_cast<GLsizei>(vertices.size());
    }

    void Draw(GLenum primitive) const {
        if (vao_ == 0 || count_ == 0) {
            return;
        }
        glBindVertexArray(vao_);
        glDrawArrays(primitive, 0, count_);
        glBindVertexArray(0);
    }

  private:
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLsizei count_ = 0;
};

GLuint CompileShader(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_FALSE) {
        GLint length = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
        std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
        glGetShaderInfoLog(shader, length, nullptr, log.data());
        glDeleteShader(shader);
        throw std::runtime_error("Shader compilation failed:\n" + log);
    }
    return shader;
}

GLuint CreateProgram() {
    constexpr const char* vertexSource = R"GLSL(
        #version 330 core
        layout(location = 0) in vec3 inPosition;
        layout(location = 1) in vec4 inColor;

        uniform mat4 uMvp;
        out vec4 vertexColor;

        void main() {
            gl_Position = uMvp * vec4(inPosition, 1.0);
            vertexColor = inColor;
        }
    )GLSL";

    constexpr const char* fragmentSource = R"GLSL(
        #version 330 core
        in vec4 vertexColor;
        out vec4 outColor;

        void main() {
            outColor = vertexColor;
        }
    )GLSL";

    const GLuint vertexShader = CompileShader(GL_VERTEX_SHADER, vertexSource);
    const GLuint fragmentShader = CompileShader(GL_FRAGMENT_SHADER, fragmentSource);

    const GLuint program = glCreateProgram();
    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);
    glLinkProgram(program);
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked == GL_FALSE) {
        GLint length = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
        std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
        glGetProgramInfoLog(program, length, nullptr, log.data());
        glDeleteProgram(program);
        throw std::runtime_error("Program link failed:\n" + log);
    }

    return program;
}

struct EyeFramebuffer {
    GLuint framebuffer = 0;
    GLuint colorTexture = 0;
    GLuint depthBuffer = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    void Create(std::uint32_t newWidth, std::uint32_t newHeight) {
        width = newWidth;
        height = newHeight;

        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);

        glGenTextures(1, &colorTexture);
        glBindTexture(GL_TEXTURE_2D, colorTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D,
                     0,
                     GL_RGBA8,
                     static_cast<GLsizei>(width),
                     static_cast<GLsizei>(height),
                     0,
                     GL_RGBA,
                     GL_UNSIGNED_BYTE,
                     nullptr);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTexture, 0);

        glGenRenderbuffers(1, &depthBuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, depthBuffer);
        glRenderbufferStorage(GL_RENDERBUFFER,
                              GL_DEPTH24_STENCIL8,
                              static_cast<GLsizei>(width),
                              static_cast<GLsizei>(height));
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depthBuffer);

        const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            throw std::runtime_error("Could not create eye framebuffer");
        }
    }

    void Destroy() {
        if (depthBuffer != 0) {
            glDeleteRenderbuffers(1, &depthBuffer);
            depthBuffer = 0;
        }
        if (colorTexture != 0) {
            glDeleteTextures(1, &colorTexture);
            colorTexture = 0;
        }
        if (framebuffer != 0) {
            glDeleteFramebuffers(1, &framebuffer);
            framebuffer = 0;
        }
    }
};

class Application {
  public:
    ~Application() { Shutdown(); }

    void Run() {
        InitializeOpenVr();
        InitializeWindowAndOpenGl();
        InitializeRenderer();
        RefreshChaperoneGeometry();

        auto nextRefresh = std::chrono::steady_clock::now();
        while (running_) {
            ProcessEvents();
            if (!running_) {
                break;
            }

            const auto now = std::chrono::steady_clock::now();
            if (now >= nextRefresh) {
                RefreshChaperoneGeometry();
                nextRefresh = now + std::chrono::seconds(1);
            }

            RenderFrame();
        }
    }

  private:
    void InitializeOpenVr() {
        if (!vr::VR_IsRuntimeInstalled()) {
            throw std::runtime_error("SteamVR/OpenVR runtime is not installed");
        }
        if (!vr::VR_IsHmdPresent()) {
            throw std::runtime_error("No OpenVR HMD is currently present");
        }

        vr::EVRInitError error = vr::VRInitError_None;
        vrSystem_ = vr::VR_Init(&error, vr::VRApplication_Scene);
        if (error != vr::VRInitError_None || vrSystem_ == nullptr) {
            throw std::runtime_error(std::string("VR_Init failed: ") + vr::VR_GetVRInitErrorAsEnglishDescription(error));
        }

        vrCompositor_ = vr::VRCompositor();
        if (vrCompositor_ == nullptr) {
            throw std::runtime_error("OpenVR compositor interface is unavailable");
        }

        vrChaperone_ = vr::VRChaperone();
        vrChaperoneSetup_ = vr::VRChaperoneSetup();
        vrCompositor_->SetTrackingSpace(vr::TrackingUniverseStanding);
        vrSystem_->GetRecommendedRenderTargetSize(&renderWidth_, &renderHeight_);

        std::cout << "Recommended eye target: " << renderWidth_ << " x " << renderHeight_ << '\n';
    }

    void InitializeWindowAndOpenGl() {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
            throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
        }

        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

        window_ = SDL_CreateWindow("OpenVR Chaperone Visualizer",
                                   SDL_WINDOWPOS_CENTERED,
                                   SDL_WINDOWPOS_CENTERED,
                                   kMirrorWidth,
                                   kMirrorHeight,
                                   SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
        if (window_ == nullptr) {
            throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
        }

        glContext_ = SDL_GL_CreateContext(window_);
        if (glContext_ == nullptr) {
            throw std::runtime_error(std::string("SDL_GL_CreateContext failed: ") + SDL_GetError());
        }

        SDL_GL_SetSwapInterval(0);

        glewExperimental = GL_TRUE;
        const GLenum glewError = glewInit();
        glGetError();
        if (glewError != GLEW_OK) {
            throw std::runtime_error(std::string("glewInit failed: ") +
                                     reinterpret_cast<const char*>(glewGetErrorString(glewError)));
        }
        glReady_ = true;
    }

    void InitializeRenderer() {
        shaderProgram_ = CreateProgram();
        mvpLocation_ = glGetUniformLocation(shaderProgram_, "uMvp");
        if (mvpLocation_ < 0) {
            throw std::runtime_error("uMvp uniform was not found");
        }

        leftEye_.Create(renderWidth_, renderHeight_);
        rightEye_.Create(renderWidth_, renderHeight_);

        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDisable(GL_CULL_FACE);
    }

    void ProcessEvents() {
        SDL_Event event{};
        while (SDL_PollEvent(&event) != 0) {
            if (event.type == SDL_QUIT) {
                running_ = false;
            } else if (event.type == SDL_KEYDOWN) {
                if (event.key.keysym.sym == SDLK_ESCAPE) {
                    running_ = false;
                } else if (event.key.keysym.sym == SDLK_r && event.key.repeat == 0) {
                    RefreshChaperoneGeometry();
                }
            }
        }

        if (vrSystem_ != nullptr) {
            vr::VREvent_t eventVr{};
            while (vrSystem_->PollNextEvent(&eventVr, sizeof(eventVr))) {
                if (eventVr.eventType == vr::VREvent_Quit) {
                    vrSystem_->AcknowledgeQuit_Exiting();
                    running_ = false;
                }
            }
        }
    }

    void RefreshChaperoneGeometry() {
        std::vector<Vertex> triangles;
        std::vector<Vertex> lines;

        constexpr int halfGrid = 10;
        constexpr float gridY = -0.004f;
        for (int coordinate = -halfGrid; coordinate <= halfGrid; ++coordinate) {
            const float intensity = coordinate == 0 ? 0.34f : 0.16f;
            AddLine(lines,
                    MakeVertex(static_cast<float>(coordinate), gridY, -halfGrid, intensity, intensity, intensity, 1.0f),
                    MakeVertex(static_cast<float>(coordinate), gridY, halfGrid, intensity, intensity, intensity, 1.0f));
            AddLine(lines,
                    MakeVertex(-halfGrid, gridY, static_cast<float>(coordinate), intensity, intensity, intensity, 1.0f),
                    MakeVertex(halfGrid, gridY, static_cast<float>(coordinate), intensity, intensity, intensity, 1.0f));
        }

        AddLine(lines, MakeVertex(0.0f, 0.02f, 0.0f, 1.0f, 0.22f, 0.22f, 1.0f),
                MakeVertex(0.6f, 0.02f, 0.0f, 1.0f, 0.22f, 0.22f, 1.0f));
        AddLine(lines, MakeVertex(0.0f, 0.02f, 0.0f, 0.30f, 1.0f, 0.30f, 1.0f),
                MakeVertex(0.0f, 0.62f, 0.0f, 0.30f, 1.0f, 0.30f, 1.0f));
        AddLine(lines, MakeVertex(0.0f, 0.02f, 0.0f, 0.24f, 0.46f, 1.0f, 1.0f),
                MakeVertex(0.0f, 0.02f, 0.6f, 0.24f, 0.46f, 1.0f, 1.0f));

        bool havePlayArea = false;
        vr::HmdQuad_t playArea{};
        if (vrChaperone_ != nullptr) {
            havePlayArea = vrChaperone_->GetPlayAreaRect(&playArea);
        }

        if (havePlayArea) {
            const auto corners = QuadCorners(playArea);
            AddQuadTriangles(triangles, corners, 0.16f, 0.92f, 0.34f, 0.13f, 0.008f);

            for (std::size_t i = 0; i < corners.size(); ++i) {
                const std::size_t next = (i + 1) % corners.size();
                AddLine(lines,
                        MakeVertex(corners[i], 0.18f, 1.0f, 0.38f, 1.0f, 0.018f),
                        MakeVertex(corners[next], 0.18f, 1.0f, 0.38f, 1.0f, 0.018f));
            }
        }

        std::uint32_t boundCount = 0;
        std::vector<vr::HmdQuad_t> bounds;

        if (vrChaperoneSetup_ != nullptr) {
            vrChaperoneSetup_->GetLiveCollisionBoundsInfo(nullptr, &boundCount);

            if (boundCount > 0) {
                bounds.resize(boundCount);

                std::uint32_t writtenCount = boundCount;

                if (vrChaperoneSetup_->GetLiveCollisionBoundsInfo(
                    bounds.data(), &writtenCount)) {
                    bounds.resize(std::min<std::uint32_t>(
                        writtenCount,
                        static_cast<std::uint32_t>(bounds.size())));
                }
                else {
                    std::cerr
                        << "GetLiveCollisionBoundsInfo failed while reading "
                        << boundCount << " collision quads\n";
                    bounds.clear();
                }
            }
        }

        for (const vr::HmdQuad_t& bound : bounds) {
            const auto corners = QuadCorners(bound);
            AddQuadTriangles(triangles, corners, 0.05f, 0.77f, 1.0f, 0.18f);

            for (std::size_t i = 0; i < corners.size(); ++i) {
                const std::size_t next = (i + 1) % corners.size();
                AddLine(lines,
                        MakeVertex(corners[i], 0.16f, 0.88f, 1.0f, 0.95f),
                        MakeVertex(corners[next], 0.16f, 0.88f, 1.0f, 0.95f));
            }
        }

        triangleMesh_.Upload(triangles);
        lineMesh_.Upload(lines);

        std::cout << "Chaperone refresh: play area=" << (havePlayArea ? "yes" : "no")
                  << ", collision quads=" << bounds.size() << '\n';
    }

    Mat4 EyeViewProjection(vr::EVREye eye, const Mat4& deviceToWorld) const {
        const Mat4 projection = Convert(vrSystem_->GetProjectionMatrix(eye, kNearClip, kFarClip));
        const Mat4 eyeToHead = Convert(vrSystem_->GetEyeToHeadTransform(eye));
        const Mat4 headToEye = InverseRigidBody(eyeToHead);
        const Mat4 worldToHead = InverseRigidBody(deviceToWorld);
        return projection * headToEye * worldToHead;
    }

    void RenderEye(vr::EVREye eye, EyeFramebuffer& target, const Mat4& hmdToWorld) {
        glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
        glViewport(0, 0, static_cast<GLsizei>(target.width), static_cast<GLsizei>(target.height));
        glClearColor(0.012f, 0.018f, 0.027f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

        const Mat4 mvp = EyeViewProjection(eye, hmdToWorld);
        glUseProgram(shaderProgram_);
        glUniformMatrix4fv(mvpLocation_, 1, GL_FALSE, mvp.m.data());

        glDepthMask(GL_FALSE);
        triangleMesh_.Draw(GL_TRIANGLES);
        glDepthMask(GL_TRUE);

        glLineWidth(2.0f);
        lineMesh_.Draw(GL_LINES);

        glUseProgram(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    void SubmitEye(vr::EVREye eye, const EyeFramebuffer& target) {
        vr::Texture_t texture{
            reinterpret_cast<void*>(static_cast<std::uintptr_t>(target.colorTexture)),
            vr::TextureType_OpenGL,
            vr::ColorSpace_Gamma,
        };

        const vr::EVRCompositorError result = vrCompositor_->Submit(eye, &texture);
        if (result != vr::VRCompositorError_None) {
            std::cerr << "VRCompositor::Submit failed for eye " << static_cast<int>(eye)
                      << ": " << static_cast<int>(result) << '\n';
        }
    }

    void DrawMirror() {
        int drawableWidth = 0;
        int drawableHeight = 0;
        SDL_GL_GetDrawableSize(window_, &drawableWidth, &drawableHeight);

        glBindFramebuffer(GL_READ_FRAMEBUFFER, leftEye_.framebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0,
                          0,
                          static_cast<GLint>(leftEye_.width),
                          static_cast<GLint>(leftEye_.height),
                          0,
                          0,
                          drawableWidth,
                          drawableHeight,
                          GL_COLOR_BUFFER_BIT,
                          GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        SDL_GL_SwapWindow(window_);
    }

    void RenderFrame() {
        std::array<vr::TrackedDevicePose_t, vr::k_unMaxTrackedDeviceCount> renderPoses{};
        const vr::EVRCompositorError poseError =
            vrCompositor_->WaitGetPoses(renderPoses.data(), static_cast<std::uint32_t>(renderPoses.size()), nullptr, 0);
        if (poseError != vr::VRCompositorError_None) {
            std::cerr << "WaitGetPoses failed: " << static_cast<int>(poseError) << '\n';
            return;
        }

        const vr::TrackedDevicePose_t& hmdPose = renderPoses[vr::k_unTrackedDeviceIndex_Hmd];
        if (!hmdPose.bPoseIsValid) {
            SDL_Delay(1);
            return;
        }

        const Mat4 hmdToWorld = Convert(hmdPose.mDeviceToAbsoluteTracking);
        RenderEye(vr::Eye_Left, leftEye_, hmdToWorld);
        RenderEye(vr::Eye_Right, rightEye_, hmdToWorld);

        SubmitEye(vr::Eye_Left, leftEye_);
        SubmitEye(vr::Eye_Right, rightEye_);
        glFlush();

        DrawMirror();
    }

    void Shutdown() {
        if (glReady_) {
            triangleMesh_.Destroy();
            lineMesh_.Destroy();
            leftEye_.Destroy();
            rightEye_.Destroy();
            if (shaderProgram_ != 0) {
                glDeleteProgram(shaderProgram_);
                shaderProgram_ = 0;
            }
            glReady_ = false;
        }

        if (glContext_ != nullptr) {
            SDL_GL_DeleteContext(glContext_);
            glContext_ = nullptr;
        }
        if (window_ != nullptr) {
            SDL_DestroyWindow(window_);
            window_ = nullptr;
        }
        SDL_Quit();

        if (vrSystem_ != nullptr) {
            vr::VR_Shutdown();
            vrSystem_ = nullptr;
            vrCompositor_ = nullptr;
            vrChaperone_ = nullptr;
            vrChaperoneSetup_ = nullptr;
        }
    }

    bool running_ = true;

    SDL_Window* window_ = nullptr;
    SDL_GLContext glContext_ = nullptr;
    bool glReady_ = false;

    vr::IVRSystem* vrSystem_ = nullptr;
    vr::IVRCompositor* vrCompositor_ = nullptr;
    vr::IVRChaperone* vrChaperone_ = nullptr;
    vr::IVRChaperoneSetup* vrChaperoneSetup_ = nullptr;

    std::uint32_t renderWidth_ = 0;
    std::uint32_t renderHeight_ = 0;

    GLuint shaderProgram_ = 0;
    GLint mvpLocation_ = -1;
    GlMesh triangleMesh_;
    GlMesh lineMesh_;
    EyeFramebuffer leftEye_;
    EyeFramebuffer rightEye_;
};

}  // namespace

int main(int, char**) {
    try {
        Application application;
        application.Run();
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        std::cerr << "Fatal error: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
