#include <vulkan/vulkan.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

void CheckXr(XrResult result, const char* what) {
    if (XR_FAILED(result)) {
        throw std::runtime_error(std::string(what) + " failed with XrResult " + std::to_string(result));
    }
}

void CheckVk(VkResult result, const char* what) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(what) + " failed with VkResult " + std::to_string(result));
    }
}

template <typename T>
T GetXrProc(XrInstance instance, const char* name) {
    PFN_xrVoidFunction fn = nullptr;
    CheckXr(xrGetInstanceProcAddr(instance, name, &fn), name);
    if (!fn) throw std::runtime_error(std::string("Missing OpenXR function: ") + name);
    return reinterpret_cast<T>(fn);
}

// These are tiny precompiled SPIR-V shaders. The source they represent is essentially:
//
// vertex:   gl_Position = vec4(inPos, 0, 1); outColor = inColor;
// fragment: outColor = vec4(inColor, 1);
//
// Keeping SPIR-V here means main.cpp is the only source/code file in the project.
static const uint32_t kVertexShader[] = {
    0x07230203u, 0x00010000u, 0x00000000u, 0x00000018u, 0x00000000u, 0x00020011u, 0x00000001u, 0x0003000eu,
    0x00000000u, 0x00000001u, 0x0009000fu, 0x00000000u, 0x00000011u, 0x6e69616du, 0x00000000u, 0x0000000bu,
    0x0000000cu, 0x0000000du, 0x0000000eu, 0x00040047u, 0x0000000bu, 0x0000001eu, 0x00000000u, 0x00040047u,
    0x0000000cu, 0x0000001eu, 0x00000001u, 0x00040047u, 0x0000000du, 0x0000001eu, 0x00000000u, 0x00040047u,
    0x0000000eu, 0x0000000bu, 0x00000000u, 0x00020013u, 0x00000001u, 0x00030016u, 0x00000002u, 0x00000020u,
    0x00040017u, 0x00000003u, 0x00000002u, 0x00000002u, 0x00040017u, 0x00000004u, 0x00000002u, 0x00000003u,
    0x00040017u, 0x00000005u, 0x00000002u, 0x00000004u, 0x00040020u, 0x00000006u, 0x00000001u, 0x00000003u,
    0x00040020u, 0x00000007u, 0x00000001u, 0x00000004u, 0x00040020u, 0x00000008u, 0x00000003u, 0x00000004u,
    0x00040020u, 0x00000009u, 0x00000003u, 0x00000005u, 0x00030021u, 0x0000000au, 0x00000001u, 0x0004002bu,
    0x00000002u, 0x0000000fu, 0x00000000u, 0x0004002bu, 0x00000002u, 0x00000010u, 0x3f800000u, 0x0004003bu,
    0x00000006u, 0x0000000bu, 0x00000001u, 0x0004003bu, 0x00000007u, 0x0000000cu, 0x00000001u, 0x0004003bu,
    0x00000008u, 0x0000000du, 0x00000003u, 0x0004003bu, 0x00000009u, 0x0000000eu, 0x00000003u, 0x00050036u,
    0x00000001u, 0x00000011u, 0x00000000u, 0x0000000au, 0x000200f8u, 0x00000012u, 0x0004003du, 0x00000003u,
    0x00000013u, 0x0000000bu, 0x00050051u, 0x00000002u, 0x00000014u, 0x00000013u, 0x00000000u, 0x00050051u,
    0x00000002u, 0x00000015u, 0x00000013u, 0x00000001u, 0x00070050u, 0x00000005u, 0x00000016u, 0x00000014u,
    0x00000015u, 0x0000000fu, 0x00000010u, 0x0003003eu, 0x0000000eu, 0x00000016u, 0x0004003du, 0x00000004u,
    0x00000017u, 0x0000000cu, 0x0003003eu, 0x0000000du, 0x00000017u, 0x000100fdu, 0x00010038u
};

static const uint32_t kFragmentShader[] = {
    0x07230203u, 0x00010000u, 0x00000000u, 0x00000012u, 0x00000000u, 0x00020011u, 0x00000001u, 0x0003000eu,
    0x00000000u, 0x00000001u, 0x0007000fu, 0x00000004u, 0x0000000bu, 0x6e69616du, 0x00000000u, 0x00000008u,
    0x00000009u, 0x00030010u, 0x0000000bu, 0x00000007u, 0x00040047u, 0x00000008u, 0x0000001eu, 0x00000000u,
    0x00040047u, 0x00000009u, 0x0000001eu, 0x00000000u, 0x00020013u, 0x00000001u, 0x00030016u, 0x00000002u,
    0x00000020u, 0x00040017u, 0x00000003u, 0x00000002u, 0x00000003u, 0x00040017u, 0x00000004u, 0x00000002u,
    0x00000004u, 0x00040020u, 0x00000005u, 0x00000001u, 0x00000003u, 0x00040020u, 0x00000006u, 0x00000003u,
    0x00000004u, 0x00030021u, 0x00000007u, 0x00000001u, 0x0004002bu, 0x00000002u, 0x0000000au, 0x3f800000u,
    0x0004003bu, 0x00000005u, 0x00000008u, 0x00000001u, 0x0004003bu, 0x00000006u, 0x00000009u, 0x00000003u,
    0x00050036u, 0x00000001u, 0x0000000bu, 0x00000000u, 0x00000007u, 0x000200f8u, 0x0000000cu, 0x0004003du,
    0x00000003u, 0x0000000du, 0x00000008u, 0x00050051u, 0x00000002u, 0x0000000eu, 0x0000000du, 0x00000000u,
    0x00050051u, 0x00000002u, 0x0000000fu, 0x0000000du, 0x00000001u, 0x00050051u, 0x00000002u, 0x00000010u,
    0x0000000du, 0x00000002u, 0x00070050u, 0x00000004u, 0x00000011u, 0x0000000eu, 0x0000000fu, 0x00000010u,
    0x0000000au, 0x0003003eu, 0x00000009u, 0x00000011u, 0x000100fdu, 0x00010038u
};

struct Vertex {
    float position[2];
    float color[3];
};

struct EyeSwapchain {
    XrSwapchain handle{XR_NULL_HANDLE};
    uint32_t width{};
    uint32_t height{};
    std::vector<XrSwapchainImageVulkanKHR> images;
    std::vector<VkImageView> imageViews;
    std::vector<VkFramebuffer> framebuffers;
};

uint32_t FindMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeBits, VkMemoryPropertyFlags wanted) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & wanted) == wanted)
            return i;
    }
    throw std::runtime_error("No suitable Vulkan memory type found");
}

VkShaderModule CreateShaderModule(VkDevice device, const uint32_t* words, size_t wordCount) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = wordCount * sizeof(uint32_t);
    ci.pCode = words;
    VkShaderModule module = VK_NULL_HANDLE;
    CheckVk(vkCreateShaderModule(device, &ci, nullptr, &module), "vkCreateShaderModule");
    return module;
}

XrEnvironmentBlendMode ChooseBlendMode(XrInstance instance, XrSystemId systemId) {
    uint32_t count = 0;
    CheckXr(xrEnumerateEnvironmentBlendModes(instance, systemId,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr),
        "xrEnumerateEnvironmentBlendModes");
    std::vector<XrEnvironmentBlendMode> modes(count);
    CheckXr(xrEnumerateEnvironmentBlendModes(instance, systemId,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count, &count, modes.data()),
        "xrEnumerateEnvironmentBlendModes");
    auto it = std::find(modes.begin(), modes.end(), XR_ENVIRONMENT_BLEND_MODE_OPAQUE);
    return it != modes.end() ? *it : modes.at(0);
}

} // namespace

int main() {
    try {
        // ---------------- OpenXR instance/system ----------------
        const char* xrExtensions[] = { XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME };

        XrInstanceCreateInfo xrInstanceCI{XR_TYPE_INSTANCE_CREATE_INFO};
        std::strncpy(xrInstanceCI.applicationInfo.applicationName, "Vulkan OpenXR Triangle",
                     XR_MAX_APPLICATION_NAME_SIZE - 1);
        xrInstanceCI.applicationInfo.applicationVersion = 1;
        std::strncpy(xrInstanceCI.applicationInfo.engineName, "None", XR_MAX_ENGINE_NAME_SIZE - 1);
        xrInstanceCI.applicationInfo.engineVersion = 1;
        xrInstanceCI.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);
        xrInstanceCI.enabledExtensionCount = 1;
        xrInstanceCI.enabledExtensionNames = xrExtensions;

        XrInstance xrInstance = XR_NULL_HANDLE;
        CheckXr(xrCreateInstance(&xrInstanceCI, &xrInstance), "xrCreateInstance");

        XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
        systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        XrSystemId systemId = XR_NULL_SYSTEM_ID;
        CheckXr(xrGetSystem(xrInstance, &systemInfo, &systemId), "xrGetSystem");

        auto xrGetVulkanGraphicsRequirements2KHR =
            GetXrProc<PFN_xrGetVulkanGraphicsRequirements2KHR>(xrInstance, "xrGetVulkanGraphicsRequirements2KHR");
        auto xrCreateVulkanInstanceKHR =
            GetXrProc<PFN_xrCreateVulkanInstanceKHR>(xrInstance, "xrCreateVulkanInstanceKHR");
        auto xrGetVulkanGraphicsDevice2KHR =
            GetXrProc<PFN_xrGetVulkanGraphicsDevice2KHR>(xrInstance, "xrGetVulkanGraphicsDevice2KHR");
        auto xrCreateVulkanDeviceKHR =
            GetXrProc<PFN_xrCreateVulkanDeviceKHR>(xrInstance, "xrCreateVulkanDeviceKHR");

        XrGraphicsRequirementsVulkan2KHR graphicsReq{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
        CheckXr(xrGetVulkanGraphicsRequirements2KHR(xrInstance, systemId, &graphicsReq),
                "xrGetVulkanGraphicsRequirements2KHR");

        // ---------------- Vulkan instance/device ----------------
        // Use the runtime's minimum supported Vulkan major/minor to maximize compatibility.
        const uint32_t vkApiVersion = VK_MAKE_VERSION(
            XR_VERSION_MAJOR(graphicsReq.minApiVersionSupported),
            XR_VERSION_MINOR(graphicsReq.minApiVersionSupported), 0);

        VkApplicationInfo vkAppInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        vkAppInfo.pApplicationName = "Vulkan OpenXR Triangle";
        vkAppInfo.applicationVersion = 1;
        vkAppInfo.pEngineName = "None";
        vkAppInfo.engineVersion = 1;
        vkAppInfo.apiVersion = vkApiVersion;

        VkInstanceCreateInfo vkInstanceCI{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        vkInstanceCI.pApplicationInfo = &vkAppInfo;

        XrVulkanInstanceCreateInfoKHR xrVkInstanceCI{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
        xrVkInstanceCI.systemId = systemId;
        xrVkInstanceCI.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
        xrVkInstanceCI.vulkanCreateInfo = &vkInstanceCI;

        VkInstance vkInstance = VK_NULL_HANDLE;
        VkResult vkResult = VK_SUCCESS;
        CheckXr(xrCreateVulkanInstanceKHR(xrInstance, &xrVkInstanceCI, &vkInstance, &vkResult),
                "xrCreateVulkanInstanceKHR");
        CheckVk(vkResult, "vkCreateInstance (through OpenXR)");

        XrVulkanGraphicsDeviceGetInfoKHR deviceGetInfo{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
        deviceGetInfo.systemId = systemId;
        deviceGetInfo.vulkanInstance = vkInstance;

        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        CheckXr(xrGetVulkanGraphicsDevice2KHR(xrInstance, &deviceGetInfo, &physicalDevice),
                "xrGetVulkanGraphicsDevice2KHR");

        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilies.data());

        uint32_t queueFamilyIndex = UINT32_MAX;
        for (uint32_t i = 0; i < queueFamilyCount; ++i) {
            if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                queueFamilyIndex = i;
                break;
            }
        }
        if (queueFamilyIndex == UINT32_MAX) throw std::runtime_error("No Vulkan graphics queue found");

        float queuePriority = 1.0f;
        VkDeviceQueueCreateInfo queueCI{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueCI.queueFamilyIndex = queueFamilyIndex;
        queueCI.queueCount = 1;
        queueCI.pQueuePriorities = &queuePriority;

        VkDeviceCreateInfo vkDeviceCI{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        vkDeviceCI.queueCreateInfoCount = 1;
        vkDeviceCI.pQueueCreateInfos = &queueCI;

        XrVulkanDeviceCreateInfoKHR xrVkDeviceCI{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
        xrVkDeviceCI.systemId = systemId;
        xrVkDeviceCI.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
        xrVkDeviceCI.vulkanPhysicalDevice = physicalDevice;
        xrVkDeviceCI.vulkanCreateInfo = &vkDeviceCI;

        VkDevice device = VK_NULL_HANDLE;
        CheckXr(xrCreateVulkanDeviceKHR(xrInstance, &xrVkDeviceCI, &device, &vkResult),
                "xrCreateVulkanDeviceKHR");
        CheckVk(vkResult, "vkCreateDevice (through OpenXR)");

        VkQueue queue = VK_NULL_HANDLE;
        vkGetDeviceQueue(device, queueFamilyIndex, 0, &queue);

        // ---------------- OpenXR session/space ----------------
        XrGraphicsBindingVulkan2KHR graphicsBinding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
        graphicsBinding.instance = vkInstance;
        graphicsBinding.physicalDevice = physicalDevice;
        graphicsBinding.device = device;
        graphicsBinding.queueFamilyIndex = queueFamilyIndex;
        graphicsBinding.queueIndex = 0;

        XrSessionCreateInfo sessionCI{XR_TYPE_SESSION_CREATE_INFO};
        sessionCI.next = &graphicsBinding;
        sessionCI.systemId = systemId;

        XrSession session = XR_NULL_HANDLE;
        CheckXr(xrCreateSession(xrInstance, &sessionCI, &session), "xrCreateSession");

        XrReferenceSpaceCreateInfo spaceCI{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        spaceCI.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        spaceCI.poseInReferenceSpace.orientation.w = 1.0f;
        XrSpace localSpace = XR_NULL_HANDLE;
        CheckXr(xrCreateReferenceSpace(session, &spaceCI, &localSpace), "xrCreateReferenceSpace");

        const XrEnvironmentBlendMode blendMode = ChooseBlendMode(xrInstance, systemId);

        // ---------------- View configuration/swapchains ----------------
        uint32_t viewCount = 0;
        CheckXr(xrEnumerateViewConfigurationViews(xrInstance, systemId,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr),
            "xrEnumerateViewConfigurationViews");
        if (viewCount == 0) throw std::runtime_error("Runtime returned no stereo views");

        std::vector<XrViewConfigurationView> configViews(viewCount,
            XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW});
        CheckXr(xrEnumerateViewConfigurationViews(xrInstance, systemId,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, configViews.data()),
            "xrEnumerateViewConfigurationViews");

        uint32_t formatCount = 0;
        CheckXr(xrEnumerateSwapchainFormats(session, 0, &formatCount, nullptr),
                "xrEnumerateSwapchainFormats");
        std::vector<int64_t> formats(formatCount);
        CheckXr(xrEnumerateSwapchainFormats(session, formatCount, &formatCount, formats.data()),
                "xrEnumerateSwapchainFormats");

        const std::array<VkFormat, 4> preferredFormats = {
            VK_FORMAT_R8G8B8A8_SRGB,
            VK_FORMAT_B8G8R8A8_SRGB,
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_FORMAT_B8G8R8A8_UNORM,
        };
        VkFormat colorFormat = VK_FORMAT_UNDEFINED;
        for (VkFormat preferred : preferredFormats) {
            if (std::find(formats.begin(), formats.end(), static_cast<int64_t>(preferred)) != formats.end()) {
                colorFormat = preferred;
                break;
            }
        }
        if (colorFormat == VK_FORMAT_UNDEFINED)
            throw std::runtime_error("Runtime offered no simple RGBA/BGRA Vulkan color format");

        // ---------------- Vulkan render pass ----------------
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = colorFormat;
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentReference colorRef{};
        colorRef.attachment = 0;
        colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;

        VkRenderPassCreateInfo renderPassCI{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        renderPassCI.attachmentCount = 1;
        renderPassCI.pAttachments = &colorAttachment;
        renderPassCI.subpassCount = 1;
        renderPassCI.pSubpasses = &subpass;

        VkRenderPass renderPass = VK_NULL_HANDLE;
        CheckVk(vkCreateRenderPass(device, &renderPassCI, nullptr, &renderPass), "vkCreateRenderPass");

        // ---------------- Vulkan pipeline ----------------
        VkShaderModule vertModule = CreateShaderModule(device, kVertexShader, std::size(kVertexShader));
        VkShaderModule fragModule = CreateShaderModule(device, kFragmentShader, std::size(kFragmentShader));

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertModule;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragModule;
        stages[1].pName = "main";

        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(Vertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        VkVertexInputAttributeDescription attributes[2]{};
        attributes[0].location = 0;
        attributes[0].binding = 0;
        attributes[0].format = VK_FORMAT_R32G32_SFLOAT;
        attributes[0].offset = offsetof(Vertex, position);
        attributes[1].location = 1;
        attributes[1].binding = 0;
        attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributes[1].offset = offsetof(Vertex, color);

        VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = 2;
        vertexInput.pVertexAttributeDescriptions = attributes;

        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &blendAttachment;

        const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamicStates;

        VkPipelineLayoutCreateInfo layoutCI{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        CheckVk(vkCreatePipelineLayout(device, &layoutCI, nullptr, &pipelineLayout), "vkCreatePipelineLayout");

        VkGraphicsPipelineCreateInfo pipelineCI{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pipelineCI.stageCount = 2;
        pipelineCI.pStages = stages;
        pipelineCI.pVertexInputState = &vertexInput;
        pipelineCI.pInputAssemblyState = &assembly;
        pipelineCI.pViewportState = &viewportState;
        pipelineCI.pRasterizationState = &raster;
        pipelineCI.pMultisampleState = &multisample;
        pipelineCI.pColorBlendState = &blend;
        pipelineCI.pDynamicState = &dynamic;
        pipelineCI.layout = pipelineLayout;
        pipelineCI.renderPass = renderPass;
        pipelineCI.subpass = 0;

        VkPipeline pipeline = VK_NULL_HANDLE;
        CheckVk(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineCI, nullptr, &pipeline),
                "vkCreateGraphicsPipelines");
        vkDestroyShaderModule(device, vertModule, nullptr);
        vkDestroyShaderModule(device, fragModule, nullptr);

        // ---------------- Vertex buffer ----------------
        const Vertex vertices[3] = {
            {{-0.65f,  0.55f}, {1.0f, 0.1f, 0.1f}},
            {{ 0.65f,  0.55f}, {0.1f, 1.0f, 0.1f}},
            {{ 0.00f, -0.65f}, {0.1f, 0.3f, 1.0f}},
        };

        VkBufferCreateInfo bufferCI{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferCI.size = sizeof(vertices);
        bufferCI.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        bufferCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer vertexBuffer = VK_NULL_HANDLE;
        CheckVk(vkCreateBuffer(device, &bufferCI, nullptr, &vertexBuffer), "vkCreateBuffer");

        VkMemoryRequirements bufferReq{};
        vkGetBufferMemoryRequirements(device, vertexBuffer, &bufferReq);
        VkMemoryAllocateInfo bufferAlloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        bufferAlloc.allocationSize = bufferReq.size;
        bufferAlloc.memoryTypeIndex = FindMemoryType(physicalDevice, bufferReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
        CheckVk(vkAllocateMemory(device, &bufferAlloc, nullptr, &vertexMemory), "vkAllocateMemory");
        CheckVk(vkBindBufferMemory(device, vertexBuffer, vertexMemory, 0), "vkBindBufferMemory");
        void* mapped = nullptr;
        CheckVk(vkMapMemory(device, vertexMemory, 0, sizeof(vertices), 0, &mapped), "vkMapMemory");
        std::memcpy(mapped, vertices, sizeof(vertices));
        vkUnmapMemory(device, vertexMemory);

        // ---------------- Command pool/buffer ----------------
        VkCommandPoolCreateInfo poolCI{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolCI.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolCI.queueFamilyIndex = queueFamilyIndex;
        VkCommandPool commandPool = VK_NULL_HANDLE;
        CheckVk(vkCreateCommandPool(device, &poolCI, nullptr, &commandPool), "vkCreateCommandPool");

        VkCommandBufferAllocateInfo cmdAlloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cmdAlloc.commandPool = commandPool;
        cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdAlloc.commandBufferCount = 1;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        CheckVk(vkAllocateCommandBuffers(device, &cmdAlloc, &commandBuffer), "vkAllocateCommandBuffers");

        // ---------------- OpenXR swapchains + Vulkan image views/framebuffers ----------------
        std::vector<EyeSwapchain> eyes(viewCount);
        for (uint32_t eye = 0; eye < viewCount; ++eye) {
            EyeSwapchain& e = eyes[eye];
            e.width = configViews[eye].recommendedImageRectWidth;
            e.height = configViews[eye].recommendedImageRectHeight;

            XrSwapchainCreateInfo swapchainCI{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            swapchainCI.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
            swapchainCI.format = static_cast<int64_t>(colorFormat);
            swapchainCI.sampleCount = 1;
            swapchainCI.width = e.width;
            swapchainCI.height = e.height;
            swapchainCI.faceCount = 1;
            swapchainCI.arraySize = 1;
            swapchainCI.mipCount = 1;
            CheckXr(xrCreateSwapchain(session, &swapchainCI, &e.handle), "xrCreateSwapchain");

            uint32_t imageCount = 0;
            CheckXr(xrEnumerateSwapchainImages(e.handle, 0, &imageCount, nullptr),
                    "xrEnumerateSwapchainImages");
            e.images.resize(imageCount, XrSwapchainImageVulkanKHR{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
            CheckXr(xrEnumerateSwapchainImages(e.handle, imageCount, &imageCount,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(e.images.data())),
                "xrEnumerateSwapchainImages");

            e.imageViews.resize(imageCount);
            e.framebuffers.resize(imageCount);
            for (uint32_t i = 0; i < imageCount; ++i) {
                VkImageViewCreateInfo viewCI{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                viewCI.image = e.images[i].image;
                viewCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
                viewCI.format = colorFormat;
                viewCI.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                viewCI.subresourceRange.levelCount = 1;
                viewCI.subresourceRange.layerCount = 1;
                CheckVk(vkCreateImageView(device, &viewCI, nullptr, &e.imageViews[i]), "vkCreateImageView");

                VkFramebufferCreateInfo framebufferCI{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
                framebufferCI.renderPass = renderPass;
                framebufferCI.attachmentCount = 1;
                framebufferCI.pAttachments = &e.imageViews[i];
                framebufferCI.width = e.width;
                framebufferCI.height = e.height;
                framebufferCI.layers = 1;
                CheckVk(vkCreateFramebuffer(device, &framebufferCI, nullptr, &e.framebuffers[i]),
                        "vkCreateFramebuffer");
            }
        }

        auto RenderEye = [&](EyeSwapchain& eye, uint32_t imageIndex) {
            CheckVk(vkResetCommandBuffer(commandBuffer, 0), "vkResetCommandBuffer");
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            CheckVk(vkBeginCommandBuffer(commandBuffer, &begin), "vkBeginCommandBuffer");

            const VkClearValue clear = {{{0.025f, 0.025f, 0.045f, 1.0f}}};
            VkRenderPassBeginInfo rpBegin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            rpBegin.renderPass = renderPass;
            rpBegin.framebuffer = eye.framebuffers[imageIndex];
            rpBegin.renderArea.extent = {eye.width, eye.height};
            rpBegin.clearValueCount = 1;
            rpBegin.pClearValues = &clear;

            vkCmdBeginRenderPass(commandBuffer, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

            VkViewport viewport{};
            viewport.width = static_cast<float>(eye.width);
            viewport.height = static_cast<float>(eye.height);
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

            VkRect2D scissor{{0, 0}, {eye.width, eye.height}};
            vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer, &offset);
            vkCmdDraw(commandBuffer, 3, 1, 0, 0);
            vkCmdEndRenderPass(commandBuffer);
            CheckVk(vkEndCommandBuffer(commandBuffer), "vkEndCommandBuffer");

            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &commandBuffer;
            CheckVk(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");

            // Deliberately simple for a sample: wait before releasing the image.
            // A real app would use fences and overlap CPU/GPU work.
            CheckVk(vkQueueWaitIdle(queue), "vkQueueWaitIdle");
        };

        // ---------------- Main OpenXR event/frame loop ----------------
        bool exitRequested = false;
        bool sessionRunning = false;
        XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
        std::vector<XrView> views(viewCount, XrView{XR_TYPE_VIEW});
        std::vector<XrCompositionLayerProjectionView> projectionViews(viewCount);

        std::cout << "OpenXR/Vulkan initialized. Waiting for the session...\n";

        while (!exitRequested) {
            XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
            while (xrPollEvent(xrInstance, &event) == XR_SUCCESS) {
                if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                    const auto* changed = reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
                    sessionState = changed->state;

                    if (sessionState == XR_SESSION_STATE_READY && !sessionRunning) {
                        XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
                        beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                        CheckXr(xrBeginSession(session, &beginInfo), "xrBeginSession");
                        sessionRunning = true;
                    } else if (sessionState == XR_SESSION_STATE_STOPPING && sessionRunning) {
                        CheckXr(xrEndSession(session), "xrEndSession");
                        sessionRunning = false;
                    } else if (sessionState == XR_SESSION_STATE_EXITING ||
                               sessionState == XR_SESSION_STATE_LOSS_PENDING) {
                        exitRequested = true;
                    }
                }
                event = {XR_TYPE_EVENT_DATA_BUFFER};
            }

            if (!sessionRunning) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
            XrFrameState frameState{XR_TYPE_FRAME_STATE};
            CheckXr(xrWaitFrame(session, &waitInfo, &frameState), "xrWaitFrame");

            XrFrameBeginInfo frameBegin{XR_TYPE_FRAME_BEGIN_INFO};
            CheckXr(xrBeginFrame(session, &frameBegin), "xrBeginFrame");

            bool submitProjection = false;
            XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};

            if (frameState.shouldRender) {
                for (auto& view : views) view = {XR_TYPE_VIEW};

                XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
                locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                locateInfo.displayTime = frameState.predictedDisplayTime;
                locateInfo.space = localSpace;

                XrViewState viewState{XR_TYPE_VIEW_STATE};
                uint32_t locatedCount = 0;
                CheckXr(xrLocateViews(session, &locateInfo, &viewState, viewCount, &locatedCount, views.data()),
                        "xrLocateViews");

                const XrViewStateFlags requiredFlags =
                    XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
                submitProjection = locatedCount == viewCount &&
                    (viewState.viewStateFlags & requiredFlags) == requiredFlags;

                if (submitProjection) {
                    for (uint32_t eyeIndex = 0; eyeIndex < viewCount; ++eyeIndex) {
                        EyeSwapchain& eye = eyes[eyeIndex];

                        XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                        uint32_t imageIndex = 0;
                        CheckXr(xrAcquireSwapchainImage(eye.handle, &acquireInfo, &imageIndex),
                                "xrAcquireSwapchainImage");

                        XrSwapchainImageWaitInfo imageWait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                        imageWait.timeout = XR_INFINITE_DURATION;
                        CheckXr(xrWaitSwapchainImage(eye.handle, &imageWait), "xrWaitSwapchainImage");

                        RenderEye(eye, imageIndex);

                        XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                        CheckXr(xrReleaseSwapchainImage(eye.handle, &releaseInfo),
                                "xrReleaseSwapchainImage");

                        auto& pv = projectionViews[eyeIndex];
                        pv = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                        pv.pose = views[eyeIndex].pose;
                        pv.fov = views[eyeIndex].fov;
                        pv.subImage.swapchain = eye.handle;
                        pv.subImage.imageRect.offset = {0, 0};
                        pv.subImage.imageRect.extent = {
                            static_cast<int32_t>(eye.width), static_cast<int32_t>(eye.height)};
                        pv.subImage.imageArrayIndex = 0;
                    }

                    projection.space = localSpace;
                    projection.viewCount = viewCount;
                    projection.views = projectionViews.data();
                }
            }

            const XrCompositionLayerBaseHeader* layers[] = {
                reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)
            };
            XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
            endInfo.displayTime = frameState.predictedDisplayTime;
            endInfo.environmentBlendMode = blendMode;
            endInfo.layerCount = submitProjection ? 1u : 0u;
            endInfo.layers = submitProjection ? layers : nullptr;
            CheckXr(xrEndFrame(session, &endInfo), "xrEndFrame");
        }

        // ---------------- Cleanup ----------------
        vkDeviceWaitIdle(device);
        for (auto& eye : eyes) {
            for (VkFramebuffer fb : eye.framebuffers) vkDestroyFramebuffer(device, fb, nullptr);
            for (VkImageView view : eye.imageViews) vkDestroyImageView(device, view, nullptr);
            if (eye.handle != XR_NULL_HANDLE) xrDestroySwapchain(eye.handle);
        }
        vkDestroyCommandPool(device, commandPool, nullptr);
        vkDestroyBuffer(device, vertexBuffer, nullptr);
        vkFreeMemory(device, vertexMemory, nullptr);
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        vkDestroyRenderPass(device, renderPass, nullptr);
        xrDestroySpace(localSpace);
        xrDestroySession(session);
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        xrDestroyInstance(xrInstance);

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << '\n';
        return 1;
    }
}
