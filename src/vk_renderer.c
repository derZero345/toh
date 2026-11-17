#include "vk_renderer.h"

#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <stdio.h>

#define MAX_FRAMES      2
#define MAX_SWAP_IMAGES 8
#define MAX_TEXTURE_SETS 4096

static const uint32_t vert_spv[] =
#include "ui.vert.inc"
;
static const uint32_t frag_spv[] =
#include "ui.frag.inc"
;

#define VK_CHECK(expr) do {                                                    \
        VkResult r_ = (expr);                                                  \
        if (r_ != VK_SUCCESS) {                                                \
            fprintf(stderr, "Vulkan error %d at %s:%d: %s\n",                  \
                    (int)r_, __FILE__, __LINE__, #expr);                       \
            abort();                                                           \
        }                                                                      \
    } while (0)

struct Texture {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkDescriptorSet set;
    int w, h;
    bool alpha_only;
    int refs;
    uint64_t id;      /* unique, never reused (save file fingerprint) */
    uint8_t *pixels;  /* optional RGBA copy, see vkr_texture_create_kept */
    uint8_t *blob;    /* optional encoded copy, see vkr_texture_set_blob */
    size_t blob_len;
    int blob_kind, blob_w, blob_h;
};

typedef struct {
    VkCommandBuffer cmd;
    VkSemaphore image_available;
    VkFence in_flight;
    VkBuffer vbuf;
    VkDeviceMemory vmem;
    void *vmapped;
    VkDeviceSize vcap;
} Frame;

static struct {
    SDL_Window *window;
    VkInstance instance;
    VkSurfaceKHR surface;
    VkPhysicalDevice phys;
    VkDevice device;
    uint32_t qfamily;
    VkQueue queue;
    VkCommandPool pool;

    VkSurfaceFormatKHR format;
    VkSampleCountFlagBits samples;
    VkRenderPass render_pass;
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout pipe_layout;
    VkPipeline pipeline;
    VkDescriptorPool desc_pool;
    VkSampler sampler;

    VkSwapchainKHR swapchain;
    VkExtent2D extent;
    uint32_t nimages;
    VkImage images[MAX_SWAP_IMAGES];
    VkImageView views[MAX_SWAP_IMAGES];
    VkFramebuffer framebuffers[MAX_SWAP_IMAGES];
    VkSemaphore render_finished[MAX_SWAP_IMAGES];
    bool can_capture;

    VkImage msaa_image;
    VkDeviceMemory msaa_memory;
    VkImageView msaa_view;

    Frame frames[MAX_FRAMES];
    uint32_t frame;
    bool need_resize;
    bool has_swapchain;
} R;

/* ---- helpers ------------------------------------------------------------ */

static uint32_t find_memory_type(uint32_t bits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(R.phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    fprintf(stderr, "No suitable Vulkan memory type\n");
    abort();
}

static void create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                          VkBuffer *buf, VkDeviceMemory *mem) {
    VkBufferCreateInfo bi = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VK_CHECK(vkCreateBuffer(R.device, &bi, NULL, buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(R.device, *buf, &req);
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = find_memory_type(req.memoryTypeBits, props),
    };
    VK_CHECK(vkAllocateMemory(R.device, &ai, NULL, mem));
    VK_CHECK(vkBindBufferMemory(R.device, *buf, *mem, 0));
}

static void create_image(uint32_t w, uint32_t h, VkFormat fmt, VkSampleCountFlagBits samples,
                         VkImageUsageFlags usage, VkImage *img, VkDeviceMemory *mem) {
    VkImageCreateInfo ii = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt,
        .extent = { w, h, 1 },
        .mipLevels = 1, .arrayLayers = 1,
        .samples = samples,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VK_CHECK(vkCreateImage(R.device, &ii, NULL, img));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(R.device, *img, &req);
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    VK_CHECK(vkAllocateMemory(R.device, &ai, NULL, mem));
    VK_CHECK(vkBindImageMemory(R.device, *img, *mem, 0));
}

static VkImageView create_view(VkImage img, VkFormat fmt, bool alpha_swizzle) {
    VkImageViewCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = fmt,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    if (alpha_swizzle) {
        vi.components = (VkComponentMapping){
            VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE,
            VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_R,
        };
    }
    VkImageView view;
    VK_CHECK(vkCreateImageView(R.device, &vi, NULL, &view));
    return view;
}

static VkCommandBuffer begin_one_time(void) {
    VkCommandBufferAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = R.pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
    };
    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(R.device, &ai, &cmd));
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    return cmd;
}

static void end_one_time(VkCommandBuffer cmd) {
    VK_CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &cmd,
    };
    VK_CHECK(vkQueueSubmit(R.queue, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(R.queue));
    vkFreeCommandBuffers(R.device, R.pool, 1, &cmd);
}

static void image_barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to,
                          VkAccessFlags src_access, VkAccessFlags dst_access,
                          VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access, .dstAccessMask = dst_access,
        .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

static VkShaderModule create_shader(const uint32_t *code, size_t size) {
    VkShaderModuleCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size, .pCode = code,
    };
    VkShaderModule m;
    VK_CHECK(vkCreateShaderModule(R.device, &ci, NULL, &m));
    return m;
}

/* ---- device setup ---------------------------------------------------------- */

static bool create_instance(void) {
    Uint32 next = 0;
    const char *const *sdl_exts = SDL_Vulkan_GetInstanceExtensions(&next);
    if (!sdl_exts) {
        fprintf(stderr, "SDL_Vulkan_GetInstanceExtensions: %s\n", SDL_GetError());
        return false;
    }
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "pathfinder",
        .applicationVersion = VK_MAKE_VERSION(0, 1, 0),
        .pEngineName = "pathfinder",
        .apiVersion = VK_API_VERSION_1_0,
    };
    const char *layers[] = { "VK_LAYER_KHRONOS_validation" };
    const char *env = SDL_getenv("PATHFINDER_VK_VALIDATION");
    bool validation = env && env[0] == '1';

    VkInstanceCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
        .enabledExtensionCount = next,
        .ppEnabledExtensionNames = sdl_exts,
        .enabledLayerCount = validation ? 1 : 0,
        .ppEnabledLayerNames = layers,
    };
    VkResult res = vkCreateInstance(&ci, NULL, &R.instance);
    if (res != VK_SUCCESS && validation) {
        fprintf(stderr, "Validation layer unavailable, continuing without it\n");
        ci.enabledLayerCount = 0;
        res = vkCreateInstance(&ci, NULL, &R.instance);
    }
    if (res != VK_SUCCESS) {
        fprintf(stderr, "vkCreateInstance failed (%d)\n", (int)res);
        return false;
    }
    return true;
}

static bool device_suitable(VkPhysicalDevice pd, uint32_t *family) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL);
    VkExtensionProperties *exts = malloc(sizeof(*exts) * (n ? n : 1));
    vkEnumerateDeviceExtensionProperties(pd, NULL, &n, exts);
    bool has_swapchain = false;
    for (uint32_t i = 0; i < n; i++)
        if (strcmp(exts[i].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) has_swapchain = true;
    free(exts);
    if (!has_swapchain) return false;

    vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, NULL);
    VkQueueFamilyProperties *qf = malloc(sizeof(*qf) * (n ? n : 1));
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, qf);
    bool found = false;
    for (uint32_t i = 0; i < n && !found; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, R.surface, &present);
        if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
            *family = i;
            found = true;
        }
    }
    free(qf);
    if (!found) return false;

    uint32_t nfmt = 0, npm = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, R.surface, &nfmt, NULL);
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd, R.surface, &npm, NULL);
    return nfmt > 0 && npm > 0;
}

static bool pick_device(void) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(R.instance, &n, NULL);
    if (n == 0) { fprintf(stderr, "No Vulkan devices found\n"); return false; }
    VkPhysicalDevice *devs = malloc(sizeof(*devs) * n);
    vkEnumeratePhysicalDevices(R.instance, &n, devs);
    int best_score = -1;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t fam;
        if (!device_suitable(devs[i], &fam)) continue;
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU   ? 3
                  : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if (score > best_score) {
            best_score = score;
            R.phys = devs[i];
            R.qfamily = fam;
        }
    }
    free(devs);
    if (best_score < 0) { fprintf(stderr, "No suitable Vulkan device\n"); return false; }

    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(R.phys, &p);
    VkSampleCountFlags counts = p.limits.framebufferColorSampleCounts;
    R.samples = (counts & VK_SAMPLE_COUNT_4_BIT) ? VK_SAMPLE_COUNT_4_BIT
              : (counts & VK_SAMPLE_COUNT_2_BIT) ? VK_SAMPLE_COUNT_2_BIT
              : VK_SAMPLE_COUNT_1_BIT;
    return true;
}

static void create_device(void) {
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = R.qfamily, .queueCount = 1, .pQueuePriorities = &prio,
    };
    const char *exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = exts,
    };
    VK_CHECK(vkCreateDevice(R.phys, &ci, NULL, &R.device));
    vkGetDeviceQueue(R.device, R.qfamily, 0, &R.queue);

    VkCommandPoolCreateInfo pi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = R.qfamily,
    };
    VK_CHECK(vkCreateCommandPool(R.device, &pi, NULL, &R.pool));
}

static void choose_surface_format(void) {
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(R.phys, R.surface, &n, NULL);
    VkSurfaceFormatKHR *f = malloc(sizeof(*f) * n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(R.phys, R.surface, &n, f);
    /* UNORM (not sRGB) on purpose: blending then matches Java2D's behaviour. */
    R.format = f[0];
    for (uint32_t i = 0; i < n; i++) {
        if ((f[i].format == VK_FORMAT_B8G8R8A8_UNORM || f[i].format == VK_FORMAT_R8G8B8A8_UNORM) &&
            f[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            R.format = f[i];
            break;
        }
    }
    free(f);
}

static void create_render_pass(void) {
    bool msaa = R.samples != VK_SAMPLE_COUNT_1_BIT;
    VkAttachmentDescription att[2] = {
        {   /* colour (multisampled if msaa) */
            .format = R.format.format,
            .samples = R.samples,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = msaa ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = msaa ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        },
        {   /* resolve target = swapchain image */
            .format = R.format.format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        },
    };
    VkAttachmentReference color_ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference resolve_ref = { 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_ref,
        .pResolveAttachments = msaa ? &resolve_ref : NULL,
    };
    VkSubpassDependency dep = {
        .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    };
    VkRenderPassCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = msaa ? 2 : 1, .pAttachments = att,
        .subpassCount = 1, .pSubpasses = &sub,
        .dependencyCount = 1, .pDependencies = &dep,
    };
    VK_CHECK(vkCreateRenderPass(R.device, &ci, NULL, &R.render_pass));
}

static void create_pipeline(void) {
    VkDescriptorSetLayoutBinding binding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    VkDescriptorSetLayoutCreateInfo dl = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &binding,
    };
    VK_CHECK(vkCreateDescriptorSetLayout(R.device, &dl, NULL, &R.set_layout));

    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float) * 2 };
    VkPipelineLayoutCreateInfo pl = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &R.set_layout,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr,
    };
    VK_CHECK(vkCreatePipelineLayout(R.device, &pl, NULL, &R.pipe_layout));

    VkShaderModule vs = create_shader(vert_spv, sizeof(vert_spv));
    VkShaderModule fs = create_shader(frag_spv, sizeof(frag_spv));
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" },
    };
    VkVertexInputBindingDescription vb = { 0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[3] = {
        { 0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, x) },
        { 1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, u) },
        { 2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(Vertex, color) },
    };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
        .vertexAttributeDescriptionCount = 3, .pVertexAttributeDescriptions = va,
    };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1,
    };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = R.samples,
    };
    VkPipelineColorBlendAttachmentState blend = {
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &blend,
    };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn,
    };
    VkGraphicsPipelineCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages,
        .pVertexInputState = &vi,
        .pInputAssemblyState = &ia,
        .pViewportState = &vp,
        .pRasterizationState = &rs,
        .pMultisampleState = &ms,
        .pColorBlendState = &cb,
        .pDynamicState = &ds,
        .layout = R.pipe_layout,
        .renderPass = R.render_pass,
        .subpass = 0,
    };
    VK_CHECK(vkCreateGraphicsPipelines(R.device, VK_NULL_HANDLE, 1, &ci, NULL, &R.pipeline));
    vkDestroyShaderModule(R.device, vs, NULL);
    vkDestroyShaderModule(R.device, fs, NULL);

    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_TEXTURE_SETS };
    VkDescriptorPoolCreateInfo dp = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = MAX_TEXTURE_SETS, .poolSizeCount = 1, .pPoolSizes = &ps,
    };
    VK_CHECK(vkCreateDescriptorPool(R.device, &dp, NULL, &R.desc_pool));

    VkSamplerCreateInfo si = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = 0.0f,
    };
    VK_CHECK(vkCreateSampler(R.device, &si, NULL, &R.sampler));
}

static void create_frames(void) {
    for (int i = 0; i < MAX_FRAMES; i++) {
        Frame *f = &R.frames[i];
        VkCommandBufferAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = R.pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
        };
        VK_CHECK(vkAllocateCommandBuffers(R.device, &ai, &f->cmd));
        VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VK_CHECK(vkCreateSemaphore(R.device, &si, NULL, &f->image_available));
        VkFenceCreateInfo fi = {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        };
        VK_CHECK(vkCreateFence(R.device, &fi, NULL, &f->in_flight));
    }
}

/* ---- swapchain ------------------------------------------------------------- */

static void destroy_swapchain_resources(void) {
    for (uint32_t i = 0; i < R.nimages; i++) {
        vkDestroyFramebuffer(R.device, R.framebuffers[i], NULL);
        vkDestroyImageView(R.device, R.views[i], NULL);
        vkDestroySemaphore(R.device, R.render_finished[i], NULL);
    }
    R.nimages = 0;
    if (R.msaa_view) {
        vkDestroyImageView(R.device, R.msaa_view, NULL);
        vkDestroyImage(R.device, R.msaa_image, NULL);
        vkFreeMemory(R.device, R.msaa_memory, NULL);
        R.msaa_view = VK_NULL_HANDLE;
    }
}

static bool create_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(R.phys, R.surface, &caps));

    VkExtent2D ext = caps.currentExtent;
    if (ext.width == UINT32_MAX) {
        int w, h;
        SDL_GetWindowSizeInPixels(R.window, &w, &h);
        ext.width = (uint32_t)w;
        ext.height = (uint32_t)h;
        if (ext.width < caps.minImageExtent.width) ext.width = caps.minImageExtent.width;
        if (ext.height < caps.minImageExtent.height) ext.height = caps.minImageExtent.height;
        if (ext.width > caps.maxImageExtent.width) ext.width = caps.maxImageExtent.width;
        if (ext.height > caps.maxImageExtent.height) ext.height = caps.maxImageExtent.height;
    }
    if (ext.width == 0 || ext.height == 0) return false; /* minimised */

    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > MAX_SWAP_IMAGES) count = MAX_SWAP_IMAGES;

    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) {
        for (uint32_t b = 1; b; b <<= 1)
            if (caps.supportedCompositeAlpha & b) { alpha = (VkCompositeAlphaFlagBitsKHR)b; break; }
    }

    R.can_capture = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    VkSwapchainKHR old = R.swapchain;
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = R.surface,
        .minImageCount = count,
        .imageFormat = R.format.format,
        .imageColorSpace = R.format.colorSpace,
        .imageExtent = ext,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                      (R.can_capture ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0),
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = alpha,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
        .oldSwapchain = old,
    };
    VK_CHECK(vkCreateSwapchainKHR(R.device, &ci, NULL, &R.swapchain));
    if (old) vkDestroySwapchainKHR(R.device, old, NULL);
    R.extent = ext;

    VK_CHECK(vkGetSwapchainImagesKHR(R.device, R.swapchain, &R.nimages, NULL));
    if (R.nimages > MAX_SWAP_IMAGES) R.nimages = MAX_SWAP_IMAGES;
    VkResult r = vkGetSwapchainImagesKHR(R.device, R.swapchain, &R.nimages, R.images);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) VK_CHECK(r);

    bool msaa = R.samples != VK_SAMPLE_COUNT_1_BIT;
    if (msaa) {
        create_image(ext.width, ext.height, R.format.format, R.samples,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                     &R.msaa_image, &R.msaa_memory);
        R.msaa_view = create_view(R.msaa_image, R.format.format, false);
    }

    for (uint32_t i = 0; i < R.nimages; i++) {
        R.views[i] = create_view(R.images[i], R.format.format, false);
        VkImageView atts[2];
        uint32_t natt = 0;
        if (msaa) atts[natt++] = R.msaa_view;
        atts[natt++] = R.views[i];
        VkFramebufferCreateInfo fi = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = R.render_pass,
            .attachmentCount = natt, .pAttachments = atts,
            .width = ext.width, .height = ext.height, .layers = 1,
        };
        VK_CHECK(vkCreateFramebuffer(R.device, &fi, NULL, &R.framebuffers[i]));
        VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VK_CHECK(vkCreateSemaphore(R.device, &si, NULL, &R.render_finished[i]));
    }
    return true;
}

static void recreate_swapchain(void) {
    vkDeviceWaitIdle(R.device);
    destroy_swapchain_resources();
    R.has_swapchain = create_swapchain();
    R.need_resize = false;
}

/* ---- public API ------------------------------------------------------------ */

bool vkr_init(SDL_Window *window) {
    memset(&R, 0, sizeof(R));
    R.window = window;
    if (!create_instance()) return false;
    if (!SDL_Vulkan_CreateSurface(window, R.instance, NULL, &R.surface)) {
        fprintf(stderr, "SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
        return false;
    }
    if (!pick_device()) return false;
    create_device();
    choose_surface_format();
    create_render_pass();
    create_pipeline();
    create_frames();
    R.has_swapchain = create_swapchain();
    return true;
}

void vkr_shutdown(void) {
    if (!R.device) return;
    vkDeviceWaitIdle(R.device);
    destroy_swapchain_resources();
    if (R.swapchain) vkDestroySwapchainKHR(R.device, R.swapchain, NULL);
    for (int i = 0; i < MAX_FRAMES; i++) {
        Frame *f = &R.frames[i];
        if (f->vbuf) {
            vkUnmapMemory(R.device, f->vmem);
            vkDestroyBuffer(R.device, f->vbuf, NULL);
            vkFreeMemory(R.device, f->vmem, NULL);
        }
        vkDestroySemaphore(R.device, f->image_available, NULL);
        vkDestroyFence(R.device, f->in_flight, NULL);
    }
    vkDestroySampler(R.device, R.sampler, NULL);
    vkDestroyDescriptorPool(R.device, R.desc_pool, NULL);
    vkDestroyPipeline(R.device, R.pipeline, NULL);
    vkDestroyPipelineLayout(R.device, R.pipe_layout, NULL);
    vkDestroyDescriptorSetLayout(R.device, R.set_layout, NULL);
    vkDestroyRenderPass(R.device, R.render_pass, NULL);
    vkDestroyCommandPool(R.device, R.pool, NULL);
    vkDestroyDevice(R.device, NULL);
    SDL_Vulkan_DestroySurface(R.instance, R.surface, NULL);
    vkDestroyInstance(R.instance, NULL);
    memset(&R, 0, sizeof(R));
}

void vkr_resize(void) { R.need_resize = true; }

static void upload_rows(Texture *t, const uint8_t *pixels, int y0, int y1, VkImageLayout from) {
    int bpp = t->alpha_only ? 1 : 4;
    VkDeviceSize row = (VkDeviceSize)t->w * bpp;
    VkDeviceSize size = row * (VkDeviceSize)(y1 - y0);
    VkBuffer staging;
    VkDeviceMemory smem;
    create_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  &staging, &smem);
    void *map;
    VK_CHECK(vkMapMemory(R.device, smem, 0, size, 0, &map));
    memcpy(map, pixels + row * (VkDeviceSize)y0, (size_t)size);
    vkUnmapMemory(R.device, smem);

    VkCommandBuffer cmd = begin_one_time();
    image_barrier(cmd, t->image, from, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  from == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_SHADER_READ_BIT,
                  VK_ACCESS_TRANSFER_WRITE_BIT,
                  from == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                                    : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageOffset = { 0, y0, 0 },
        .imageExtent = { (uint32_t)t->w, (uint32_t)(y1 - y0), 1 },
    };
    vkCmdCopyBufferToImage(cmd, staging, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    image_barrier(cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    end_one_time(cmd);

    vkDestroyBuffer(R.device, staging, NULL);
    vkFreeMemory(R.device, smem, NULL);
}

Texture *vkr_texture_create(int w, int h, const uint8_t *pixels, bool alpha_only) {
    Texture *t = calloc(1, sizeof(*t));
    if (!t) abort();
    t->w = w;
    t->h = h;
    t->alpha_only = alpha_only;
    t->refs = 1;
    static uint64_t next_id = 1;
    t->id = next_id++;
    VkFormat fmt = alpha_only ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
    create_image((uint32_t)w, (uint32_t)h, fmt, VK_SAMPLE_COUNT_1_BIT,
                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                 &t->image, &t->memory);
    t->view = create_view(t->image, fmt, alpha_only);
    upload_rows(t, pixels, 0, h, VK_IMAGE_LAYOUT_UNDEFINED);

    VkDescriptorSetAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = R.desc_pool, .descriptorSetCount = 1, .pSetLayouts = &R.set_layout,
    };
    VK_CHECK(vkAllocateDescriptorSets(R.device, &ai, &t->set));
    VkDescriptorImageInfo ii = {
        .sampler = R.sampler, .imageView = t->view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet wr = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = t->set, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &ii,
    };
    vkUpdateDescriptorSets(R.device, 1, &wr, 0, NULL);
    return t;
}

void vkr_texture_update(Texture *t, const uint8_t *pixels, int y0, int y1) {
    if (y0 < 0) y0 = 0;
    if (y1 > t->h) y1 = t->h;
    if (y1 <= y0) return;
    vkDeviceWaitIdle(R.device);
    upload_rows(t, pixels, y0, y1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void vkr_texture_retain(Texture *t) { if (t) t->refs++; }

void vkr_texture_release(Texture *t) {
    if (!t || --t->refs > 0) return;
    vkDeviceWaitIdle(R.device);
    vkFreeDescriptorSets(R.device, R.desc_pool, 1, &t->set);
    vkDestroyImageView(R.device, t->view, NULL);
    vkDestroyImage(R.device, t->image, NULL);
    vkFreeMemory(R.device, t->memory, NULL);
    free(t->pixels);
    free(t->blob);
    free(t);
}

Texture *vkr_texture_create_kept(int w, int h, uint8_t *pixels) {
    Texture *t = vkr_texture_create(w, h, pixels, false);
    t->pixels = pixels;
    return t;
}

const uint8_t *vkr_texture_pixels(const Texture *t) { return t->pixels; }

void vkr_texture_set_blob(Texture *t, uint8_t *blob, size_t len, int kind, int w, int h) {
    if (!blob) return;
    free(t->blob);
    t->blob = blob;
    t->blob_len = len;
    t->blob_kind = kind;
    t->blob_w = w;
    t->blob_h = h;
    free(t->pixels); /* the blob is enough to save the texture again */
    t->pixels = NULL;
}

const uint8_t *vkr_texture_blob(const Texture *t, size_t *len, int *kind, int *w, int *h) {
    if (!t->blob) return NULL;
    *len = t->blob_len;
    *kind = t->blob_kind;
    *w = t->blob_w;
    *h = t->blob_h;
    return t->blob;
}

uint64_t vkr_texture_id(const Texture *t) { return t->id; }

int vkr_texture_width(const Texture *t)  { return t->w; }
int vkr_texture_height(const Texture *t) { return t->h; }

static void ensure_vertex_capacity(Frame *f, VkDeviceSize bytes) {
    if (bytes <= f->vcap) return;
    VkDeviceSize cap = f->vcap ? f->vcap : 1 << 20;
    while (cap < bytes) cap *= 2;
    if (f->vbuf) {
        vkUnmapMemory(R.device, f->vmem);
        vkDestroyBuffer(R.device, f->vbuf, NULL);
        vkFreeMemory(R.device, f->vmem, NULL);
    }
    create_buffer(cap, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  &f->vbuf, &f->vmem);
    VK_CHECK(vkMapMemory(R.device, f->vmem, 0, cap, 0, &f->vmapped));
    f->vcap = cap;
}

static void save_capture(VkBuffer buf, VkDeviceMemory mem, const char *path) {
    uint32_t w = R.extent.width, h = R.extent.height;
    void *map;
    VK_CHECK(vkMapMemory(R.device, mem, 0, (VkDeviceSize)w * h * 4, 0, &map));
    uint8_t *px = malloc((size_t)w * h * 4);
    memcpy(px, map, (size_t)w * h * 4);
    vkUnmapMemory(R.device, mem);
    bool bgra = R.format.format == VK_FORMAT_B8G8R8A8_UNORM || R.format.format == VK_FORMAT_B8G8R8A8_SRGB;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        if (bgra) { uint8_t tmp = px[i * 4]; px[i * 4] = px[i * 4 + 2]; px[i * 4 + 2] = tmp; }
        px[i * 4 + 3] = 255;
    }
    SDL_Surface *s = SDL_CreateSurfaceFrom((int)w, (int)h, SDL_PIXELFORMAT_RGBA32, px, (int)w * 4);
    if (!s || !SDL_SavePNG(s, path))
        fprintf(stderr, "Screenshot failed: %s\n", SDL_GetError());
    else
        fprintf(stderr, "Saved screenshot to %s\n", path);
    SDL_DestroySurface(s);
    free(px);
}

void vkr_render(const Vertex *verts, int nverts, const DrawCmd *cmds, int ncmds,
                const float clear[4], float logical_w, float logical_h,
                const char *screenshot_path) {
    if (R.need_resize || !R.has_swapchain) recreate_swapchain();
    if (!R.has_swapchain) return;

    Frame *f = &R.frames[R.frame];
    VK_CHECK(vkWaitForFences(R.device, 1, &f->in_flight, VK_TRUE, UINT64_MAX));

    uint32_t image_index;
    VkResult acq = vkAcquireNextImageKHR(R.device, R.swapchain, UINT64_MAX,
                                         f->image_available, VK_NULL_HANDLE, &image_index);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) { recreate_swapchain(); return; }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) VK_CHECK(acq);

    if (nverts > 0) {
        ensure_vertex_capacity(f, sizeof(Vertex) * (VkDeviceSize)nverts);
        memcpy(f->vmapped, verts, sizeof(Vertex) * (size_t)nverts);
    }
    VK_CHECK(vkResetFences(R.device, 1, &f->in_flight));

    VkBuffer cap_buf = VK_NULL_HANDLE;
    VkDeviceMemory cap_mem = VK_NULL_HANDLE;
    bool capture = screenshot_path && R.can_capture;
    if (capture) {
        create_buffer((VkDeviceSize)R.extent.width * R.extent.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &cap_buf, &cap_mem);
    }

    VkCommandBuffer cmd = f->cmd;
    VK_CHECK(vkResetCommandBuffer(cmd, 0));
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));

    VkClearValue cv[2];
    memset(cv, 0, sizeof(cv));
    for (int i = 0; i < 2; i++)
        memcpy(cv[i].color.float32, clear, sizeof(float) * 4);
    VkRenderPassBeginInfo rp = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = R.render_pass,
        .framebuffer = R.framebuffers[image_index],
        .renderArea = { { 0, 0 }, R.extent },
        .clearValueCount = R.samples != VK_SAMPLE_COUNT_1_BIT ? 2 : 1,
        .pClearValues = cv,
    };
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.pipeline);
    VkViewport vp = { 0, 0, (float)R.extent.width, (float)R.extent.height, 0, 1 };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    float screen[2] = { logical_w, logical_h };
    vkCmdPushConstants(cmd, R.pipe_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(screen), screen);

    if (nverts > 0) {
        VkDeviceSize off = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &f->vbuf, &off);
        float sx = (float)R.extent.width / logical_w, sy = (float)R.extent.height / logical_h;
        Texture *bound = NULL;
        for (int i = 0; i < ncmds; i++) {
            const DrawCmd *c = &cmds[i];
            if (c->count <= 0 || !c->tex) continue;
            VkRect2D sc = { { 0, 0 }, R.extent };
            if (c->scissor) {
                float x0 = c->sx * sx, y0 = c->sy * sy;
                float x1 = (c->sx + c->sw) * sx, y1 = (c->sy + c->sh) * sy;
                if (x0 < 0) x0 = 0;
                if (y0 < 0) y0 = 0;
                if (x1 > (float)R.extent.width) x1 = (float)R.extent.width;
                if (y1 > (float)R.extent.height) y1 = (float)R.extent.height;
                if (x1 <= x0 || y1 <= y0) continue;
                sc.offset.x = (int32_t)x0;
                sc.offset.y = (int32_t)y0;
                sc.extent.width = (uint32_t)(x1 - x0 + 0.5f);
                sc.extent.height = (uint32_t)(y1 - y0 + 0.5f);
            }
            vkCmdSetScissor(cmd, 0, 1, &sc);
            if (c->tex != bound) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.pipe_layout,
                                        0, 1, &c->tex->set, 0, NULL);
                bound = c->tex;
            }
            vkCmdDraw(cmd, (uint32_t)c->count, 1, (uint32_t)c->first, 0);
        }
    }
    vkCmdEndRenderPass(cmd);

    if (capture) {
        VkImage img = R.images[image_index];
        image_barrier(cmd, img, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region = {
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageExtent = { R.extent.width, R.extent.height, 1 },
        };
        vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, cap_buf, 1, &region);
        image_barrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_ACCESS_TRANSFER_READ_BIT, 0,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    }
    VK_CHECK(vkEndCommandBuffer(cmd));

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &f->image_available,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1, .pCommandBuffers = &cmd,
        .signalSemaphoreCount = 1, .pSignalSemaphores = &R.render_finished[image_index],
    };
    VK_CHECK(vkQueueSubmit(R.queue, 1, &si, f->in_flight));

    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &R.render_finished[image_index],
        .swapchainCount = 1, .pSwapchains = &R.swapchain, .pImageIndices = &image_index,
    };
    VkResult pres = vkQueuePresentKHR(R.queue, &pi);
    if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR) R.need_resize = true;
    else if (pres != VK_SUCCESS) VK_CHECK(pres);

    if (capture) {
        VK_CHECK(vkWaitForFences(R.device, 1, &f->in_flight, VK_TRUE, UINT64_MAX));
        save_capture(cap_buf, cap_mem, screenshot_path);
        vkDestroyBuffer(R.device, cap_buf, NULL);
        vkFreeMemory(R.device, cap_mem, NULL);
    } else if (screenshot_path) {
        fprintf(stderr, "Screenshot not supported by this swapchain\n");
    }

    R.frame = (R.frame + 1) % MAX_FRAMES;
}
