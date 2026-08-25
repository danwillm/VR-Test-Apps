#include <vulkan/vulkan.h>
#include <openvr.h>

#include <atomic>
#include <csignal>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace
{
std::atomic_bool g_bRunning{true};

void OnSignal(int) { g_bRunning = false; }

void VkCheck(VkResult result, const char *what)
{
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(what) + " failed: " + std::to_string(result));
}

std::vector<std::string> SplitExtensions(const char *text)
{
    std::vector<std::string> result;
    std::istringstream stream(text ? text : "");
    for (std::string s; stream >> s;)
        result.push_back(std::move(s));
    return result;
}

std::vector<std::string> GetInstanceExtensions()
{
    auto *compositor = vr::VRCompositor();
    const uint32_t size = compositor->GetVulkanInstanceExtensionsRequired(nullptr, 0);
    if (!size)
        return {};

    std::vector<char> text(size);
    compositor->GetVulkanInstanceExtensionsRequired(text.data(), size);
    return SplitExtensions(text.data());
}

std::vector<std::string> GetDeviceExtensions(VkPhysicalDevice physicalDevice)
{
    auto *compositor = vr::VRCompositor();
    const uint32_t size = compositor->GetVulkanDeviceExtensionsRequired(
        reinterpret_cast<VkPhysicalDevice_T *>(physicalDevice), nullptr, 0);
    if (!size)
        return {};

    std::vector<char> text(size);
    compositor->GetVulkanDeviceExtensionsRequired(
        reinterpret_cast<VkPhysicalDevice_T *>(physicalDevice), text.data(), size);
    return SplitExtensions(text.data());
}

std::vector<const char *> CStrings(const std::vector<std::string> &strings)
{
    std::vector<const char *> result;
    result.reserve(strings.size());
    for (const auto &s : strings)
        result.push_back(s.c_str());
    return result;
}

uint32_t FindMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeBits,
                        VkMemoryPropertyFlags required)
{
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &props);

    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((typeBits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & required) == required)
            return i;

    throw std::runtime_error("No suitable Vulkan memory type");
}

std::vector<uint32_t> ReadSpirV(const char *path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        throw std::runtime_error(std::string("Could not open ") + path);

    const auto size = file.tellg();
    if (size <= 0 || (size % 4) != 0)
        throw std::runtime_error(std::string("Invalid SPIR-V file ") + path);

    std::vector<uint32_t> code(static_cast<size_t>(size) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char *>(code.data()), size);
    return code;
}

VkShaderModule CreateShader(VkDevice device, const char *path)
{
    const auto code = ReadSpirV(path);

    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = code.size() * sizeof(uint32_t);
    info.pCode = code.data();

    VkShaderModule shader = VK_NULL_HANDLE;
    VkCheck(vkCreateShaderModule(device, &info, nullptr, &shader), "vkCreateShaderModule");
    return shader;
}

template <typename T>
uint64_t Handle64(T handle)
{
    if constexpr (std::is_pointer_v<T>)
        return reinterpret_cast<uint64_t>(handle);
    else
        return static_cast<uint64_t>(handle);
}

struct Target
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
};

VkImageMemoryBarrier ImageBarrier(VkImage image, VkImageLayout oldLayout,
                                  VkImageLayout newLayout, VkAccessFlags srcAccess,
                                  VkAccessFlags dstAccess)
{
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    return b;
}

Target CreateTarget(VkPhysicalDevice physicalDevice, VkDevice device,
                    VkRenderPass renderPass, uint32_t width, uint32_t height,
                    VkFormat format)
{
    Target t;

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkCheck(vkCreateImage(device, &imageInfo, nullptr, &t.image), "vkCreateImage");

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device, t.image, &req);

    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = FindMemoryType(
        physicalDevice, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkCheck(vkAllocateMemory(device, &alloc, nullptr, &t.memory), "vkAllocateMemory");
    VkCheck(vkBindImageMemory(device, t.image, t.memory, 0), "vkBindImageMemory");

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = t.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    VkCheck(vkCreateImageView(device, &viewInfo, nullptr, &t.view), "vkCreateImageView");

    VkFramebufferCreateInfo fbInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fbInfo.renderPass = renderPass;
    fbInfo.attachmentCount = 1;
    fbInfo.pAttachments = &t.view;
    fbInfo.width = width;
    fbInfo.height = height;
    fbInfo.layers = 1;
    VkCheck(vkCreateFramebuffer(device, &fbInfo, nullptr, &t.framebuffer),
            "vkCreateFramebuffer");

    return t;
}

void DestroyTarget(VkDevice device, Target &t)
{
    if (t.framebuffer) vkDestroyFramebuffer(device, t.framebuffer, nullptr);
    if (t.view) vkDestroyImageView(device, t.view, nullptr);
    if (t.image) vkDestroyImage(device, t.image, nullptr);
    if (t.memory) vkFreeMemory(device, t.memory, nullptr);
    t = {};
}
} // namespace

int main()
{
    std::signal(SIGINT, OnSignal);

    vr::IVRSystem *hmd = nullptr;
    bool vrInitialized = false;

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    Target target;

    try
    {
        // ---------------- OpenVR ----------------
        vr::EVRInitError vrError = vr::VRInitError_None;
        hmd = vr::VR_Init(&vrError, vr::VRApplication_Scene);
        if (!hmd || vrError != vr::VRInitError_None)
            throw std::runtime_error(
                std::string("VR_Init failed: ") +
                vr::VR_GetVRInitErrorAsEnglishDescription(vrError));
        vrInitialized = true;

        auto *compositor = vr::VRCompositor();
        if (!compositor)
            throw std::runtime_error("VRCompositor() returned null");

        uint32_t width = 0, height = 0;
        hmd->GetRecommendedRenderTargetSize(&width, &height);
        std::cout << "Eye target: " << width << "x" << height << '\n';

        // ---------------- Vulkan instance ----------------
        const auto instanceExtStrings = GetInstanceExtensions();
        const auto instanceExts = CStrings(instanceExtStrings);

        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "vulkan_openvr_triangle";
        app.apiVersion = VK_API_VERSION_1_0;

        VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instanceInfo.pApplicationInfo = &app;
        instanceInfo.enabledExtensionCount = static_cast<uint32_t>(instanceExts.size());
        instanceInfo.ppEnabledExtensionNames =
            instanceExts.empty() ? nullptr : instanceExts.data();
        VkCheck(vkCreateInstance(&instanceInfo, nullptr, &instance), "vkCreateInstance");

        // Prefer the GPU OpenVR says owns the HMD.
        uint32_t gpuCount = 0;
        VkCheck(vkEnumeratePhysicalDevices(instance, &gpuCount, nullptr),
                "vkEnumeratePhysicalDevices");
        if (!gpuCount)
            throw std::runtime_error("No Vulkan GPUs");

        std::vector<VkPhysicalDevice> gpus(gpuCount);
        VkCheck(vkEnumeratePhysicalDevices(instance, &gpuCount, gpus.data()),
                "vkEnumeratePhysicalDevices");

        uint64_t outputDevice = 0;
        hmd->GetOutputDevice(&outputDevice, vr::TextureType_Vulkan,
                             reinterpret_cast<VkInstance_T *>(instance));
        const VkPhysicalDevice openVrGpu =
            reinterpret_cast<VkPhysicalDevice>(outputDevice);

        for (auto gpu : gpus)
            if (gpu == openVrGpu)
                physicalDevice = gpu;

        if (!physicalDevice)
        {
            std::cerr << "OpenVR GPU not found; using Vulkan GPU 0\n";
            physicalDevice = gpus[0];
        }

        VkPhysicalDeviceProperties gpuProps{};
        vkGetPhysicalDeviceProperties(physicalDevice, &gpuProps);
        std::cout << "GPU: " << gpuProps.deviceName << '\n';

        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(
            physicalDevice, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(
            physicalDevice, &queueFamilyCount, queueFamilies.data());

        uint32_t queueFamily = UINT32_MAX;
        for (uint32_t i = 0; i < queueFamilyCount; ++i)
            if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            {
                queueFamily = i;
                break;
            }
        if (queueFamily == UINT32_MAX)
            throw std::runtime_error("No graphics queue");

        // ---------------- Vulkan device ----------------
        const auto deviceExtStrings = GetDeviceExtensions(physicalDevice);
        const auto deviceExts = CStrings(deviceExtStrings);

        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = queueFamily;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;

        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExts.size());
        deviceInfo.ppEnabledExtensionNames =
            deviceExts.empty() ? nullptr : deviceExts.data();
        VkCheck(vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &device),
                "vkCreateDevice");
        vkGetDeviceQueue(device, queueFamily, 0, &queue);

        // ---------------- Render target + render pass ----------------
        constexpr VkFormat format = VK_FORMAT_R8G8B8A8_SRGB;

        VkAttachmentDescription attachment{};
        attachment.format = format;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;

        VkRenderPassCreateInfo rpInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpInfo.attachmentCount = 1;
        rpInfo.pAttachments = &attachment;
        rpInfo.subpassCount = 1;
        rpInfo.pSubpasses = &subpass;
        VkCheck(vkCreateRenderPass(device, &rpInfo, nullptr, &renderPass),
                "vkCreateRenderPass");

        target = CreateTarget(physicalDevice, device, renderPass, width, height, format);

        // ---------------- Graphics pipeline ----------------
        VkShaderModule vs = CreateShader(device, TRIANGLE_VERT_SPV);
        VkShaderModule fs = CreateShader(device, TRIANGLE_FRAG_SPV);

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs;
        stages[0].pName = "main";
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fs;
        stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vertexInput{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkViewport viewport{0.0f, 0.0f, static_cast<float>(width),
                            static_cast<float>(height), 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, {width, height}};
        VkPipelineViewportStateCreateInfo viewportState{
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewportState.viewportCount = 1;
        viewportState.pViewports = &viewport;
        viewportState.scissorCount = 1;
        viewportState.pScissors = &scissor;

        VkPipelineRasterizationStateCreateInfo raster{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo msaa{
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        msaa.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo blend{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &blendAttachment;

        VkPipelineLayoutCreateInfo layoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        VkCheck(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout),
                "vkCreatePipelineLayout");

        VkGraphicsPipelineCreateInfo pipelineInfo{
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = stages;
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &raster;
        pipelineInfo.pMultisampleState = &msaa;
        pipelineInfo.pColorBlendState = &blend;
        pipelineInfo.layout = pipelineLayout;
        pipelineInfo.renderPass = renderPass;
        VkCheck(vkCreateGraphicsPipelines(
                    device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline),
                "vkCreateGraphicsPipelines");

        vkDestroyShaderModule(device, vs, nullptr);
        vkDestroyShaderModule(device, fs, nullptr);

        // ---------------- Command buffer ----------------
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamily;
        VkCheck(vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool),
                "vkCreateCommandPool");

        VkCommandBufferAllocateInfo cmdInfo{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cmdInfo.commandPool = commandPool;
        cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdInfo.commandBufferCount = 1;
        VkCheck(vkAllocateCommandBuffers(device, &cmdInfo, &cmd),
                "vkAllocateCommandBuffers");

        // OpenVR requires submitted Vulkan images in TRANSFER_SRC_OPTIMAL.
        {
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            VkCheck(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");
            auto barrier = ImageBarrier(
                target.image, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                0, VK_ACCESS_TRANSFER_READ_BIT);
            vkCmdPipelineBarrier(
                cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &barrier);
            VkCheck(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd;
            VkCheck(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
            VkCheck(vkQueueWaitIdle(queue), "vkQueueWaitIdle");
        }

        // The same image is submitted to both eyes because this sample has no
        // stereoscopic scene; it only demonstrates the Vulkan/OpenVR path.
        vr::VRVulkanTextureData_t textureData{};
        textureData.m_nImage = Handle64(target.image);
        textureData.m_pDevice = reinterpret_cast<VkDevice_T *>(device);
        textureData.m_pPhysicalDevice =
            reinterpret_cast<VkPhysicalDevice_T *>(physicalDevice);
        textureData.m_pInstance = reinterpret_cast<VkInstance_T *>(instance);
        textureData.m_pQueue = reinterpret_cast<VkQueue_T *>(queue);
        textureData.m_nQueueFamilyIndex = queueFamily;
        textureData.m_nWidth = width;
        textureData.m_nHeight = height;
        textureData.m_nFormat = static_cast<uint32_t>(format);
        textureData.m_nSampleCount = 1;

        vr::Texture_t texture{
            &textureData, vr::TextureType_Vulkan, vr::ColorSpace_Gamma};

        std::cout << "Rendering. Ctrl+C exits.\n";

        // ---------------- Frame loop ----------------
        while (g_bRunning)
        {
            vr::VREvent_t event{};
            while (hmd->PollNextEvent(&event, sizeof(event)))
                if (event.eventType == vr::VREvent_Quit)
                {
                    hmd->AcknowledgeQuit_Exiting();
                    g_bRunning = false;
                }
            if (!g_bRunning)
                break;

            vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]{};
            const auto poseError = compositor->WaitGetPoses(
                poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
            if (poseError != vr::VRCompositorError_None)
                throw std::runtime_error("WaitGetPoses failed");

            VkCheck(vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer");

            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            VkCheck(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");

            auto toColor = ImageBarrier(
                target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            vkCmdPipelineBarrier(
                cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                0, 0, nullptr, 0, nullptr, 1, &toColor);

            VkClearValue clear{};
            clear.color.float32[0] = 0.015f;
            clear.color.float32[1] = 0.015f;
            clear.color.float32[2] = 0.020f;
            clear.color.float32[3] = 1.0f;

            VkRenderPassBeginInfo renderBegin{
                VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            renderBegin.renderPass = renderPass;
            renderBegin.framebuffer = target.framebuffer;
            renderBegin.renderArea = {{0, 0}, {width, height}};
            renderBegin.clearValueCount = 1;
            renderBegin.pClearValues = &clear;

            vkCmdBeginRenderPass(cmd, &renderBegin, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRenderPass(cmd);

            auto toTransfer = ImageBarrier(
                target.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            vkCmdPipelineBarrier(
                cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &toTransfer);

            VkCheck(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd;
            VkCheck(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");

            // Intentionally simple: rendering finishes before OpenVR uses this queue.
            VkCheck(vkQueueWaitIdle(queue), "vkQueueWaitIdle");

            const auto left = compositor->Submit(vr::Eye_Left, &texture);
            const auto right = compositor->Submit(vr::Eye_Right, &texture);
            if (left != vr::VRCompositorError_None ||
                right != vr::VRCompositorError_None)
                throw std::runtime_error(
                    "OpenVR Submit failed: " +
                    std::to_string(static_cast<int>(left)) + ", " +
                    std::to_string(static_cast<int>(right)));

            // No desktop VkSwapchainKHR exists, so hand off explicitly.
            compositor->PostPresentHandoff();
        }

        VkCheck(vkDeviceWaitIdle(device), "vkDeviceWaitIdle");

        // OpenVR says submitted Vulkan resources must remain alive until shutdown.
        vr::VR_Shutdown();
        vrInitialized = false;

        vkDestroyCommandPool(device, commandPool, nullptr);
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        DestroyTarget(device, target);
        vkDestroyRenderPass(device, renderPass, nullptr);
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Fatal: " << e.what() << '\n';

        if (device)
            vkDeviceWaitIdle(device);

        if (vrInitialized)
            vr::VR_Shutdown();

        if (device)
        {
            if (commandPool) vkDestroyCommandPool(device, commandPool, nullptr);
            if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
            if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
            DestroyTarget(device, target);
            if (renderPass) vkDestroyRenderPass(device, renderPass, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (instance)
            vkDestroyInstance(instance, nullptr);
        return 1;
    }
}
