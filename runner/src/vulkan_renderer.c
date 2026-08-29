#include "vulkan_renderer.h"
#include "saturn.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VK_CHECK(call) do { VkResult _r = (call); if (_r != VK_SUCCESS) { \
    set_error(error, error_size, "%s failed (%d)", #call, (int)_r); goto fail; \
} } while (0)

#define MAX_VDP1_OPS 65536u
#define PARAM_WORDS  272u
#define FB_PIXELS    (512u * 256u)
#define VDP1_TILE_W  16u
#define VDP1_TILES_X 32u
#define VDP1_TILES_Y 16u
#define VDP1_TILES   (VDP1_TILES_X * VDP1_TILES_Y)
#define MAX_TILE_REFS 4194304u

static void set_error(char *out, size_t size, const char *fmt, ...);

typedef struct vkbuf {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void *map;
    VkDeviceSize size;
} vkbuf;

struct saturn_vk_renderer {
    SDL_Window *window;
    saturn *system;
    VkInstance instance;
    VkSurfaceKHR surface;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    char device_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];

    VkSwapchainKHR swapchain;
    VkFormat swap_format;
    VkExtent2D swap_extent;
    VkImage *swap_images;
    uint32_t swap_count;

    VkCommandPool command_pool;
    VkCommandBuffer command;
    VkSemaphore acquired, rendered;
    VkFence fence;

    VkDescriptorSetLayout desc_layout;
    VkDescriptorPool desc_pool;
    VkDescriptorSet desc_set;
    VkPipelineLayout pipeline_layout;
    VkPipeline vdp1_pipeline, vdp2_pipeline;

    vkbuf v1ram, v2ram, cram, params, ops, fb, mesh, tile_headers, tile_refs;
    VkImage output;
    VkDeviceMemory output_memory;
    VkImageView output_view;
    int output_ready;

    saturn_vk_vdp1_op *pending;
    uint32_t pending_count, pending_cap;
    int queue_overflow_reported;
    uint32_t present_index;
    int present_pending;
};

static int op_bounds(const saturn_vk_vdp1_op *o, int *x0, int *y0, int *x1, int *y1)
{
    if (o->kind == SATURN_VK_VDP1_ERASE) {
        *x0=o->xy[0];*y0=o->xy[1];*x1=o->xy[2]-1;*y1=o->xy[3];
    } else if (o->kind == SATURN_VK_VDP1_LINE) {
        *x0=o->xy[0]<o->xy[2]?o->xy[0]:o->xy[2];*x1=o->xy[0]>o->xy[2]?o->xy[0]:o->xy[2];
        *y0=o->xy[1]<o->xy[3]?o->xy[1]:o->xy[3];*y1=o->xy[1]>o->xy[3]?o->xy[1]:o->xy[3];
        --*x0;--*y0;++*x1;++*y1;
    } else {
        *x0=*x1=o->xy[0];*y0=*y1=o->xy[1];
        for(int k=1;k<4;k++){int x=o->xy[k*2],y=o->xy[k*2+1];if(x<*x0)*x0=x;if(x>*x1)*x1=x;if(y<*y0)*y0=y;if(y>*y1)*y1=y;}
    }
    if(o->kind!=SATURN_VK_VDP1_ERASE){if(*x1>o->sys_x1)*x1=o->sys_x1;if(*y1>o->sys_y1)*y1=o->sys_y1;}
    if(*x0<0)*x0=0;if(*y0<0)*y0=0;if(*x1>511)*x1=511;if(*y1>255)*y1=255;
    return *x0<=*x1&&*y0<=*y1;
}

static int build_tiles(saturn_vk_renderer *r, char *error, size_t error_size)
{
    uint32_t counts[VDP1_TILES]={0},cursor[VDP1_TILES];
    uint64_t total=0;
    for(uint32_t i=0;i<r->pending_count;i++){
        int x0,y0,x1,y1;if(!op_bounds(&r->pending[i],&x0,&y0,&x1,&y1))continue;
        for(uint32_t ty=(uint32_t)y0/VDP1_TILE_W;ty<=(uint32_t)y1/VDP1_TILE_W;ty++)
            for(uint32_t tx=(uint32_t)x0/VDP1_TILE_W;tx<=(uint32_t)x1/VDP1_TILE_W;tx++)counts[ty*VDP1_TILES_X+tx]++;
    }
    uint32_t *head=r->tile_headers.map;
    for(uint32_t t=0;t<VDP1_TILES;t++){head[t*2]=(uint32_t)total;head[t*2+1]=counts[t];cursor[t]=(uint32_t)total;total+=counts[t];}
    if(total>MAX_TILE_REFS){set_error(error,error_size,"VDP1 tile reference overflow: %llu",(unsigned long long)total);return 0;}
    uint32_t *ref=r->tile_refs.map;
    for(uint32_t i=0;i<r->pending_count;i++){
        int x0,y0,x1,y1;if(!op_bounds(&r->pending[i],&x0,&y0,&x1,&y1))continue;
        for(uint32_t ty=(uint32_t)y0/VDP1_TILE_W;ty<=(uint32_t)y1/VDP1_TILE_W;ty++)
            for(uint32_t tx=(uint32_t)x0/VDP1_TILE_W;tx<=(uint32_t)x1/VDP1_TILE_W;tx++){uint32_t t=ty*VDP1_TILES_X+tx;ref[cursor[t]++]=i;}
    }
    return 1;
}

static void upload_cpu_framebuffers(saturn_vk_renderer *r, const saturn *s)
{
    uint32_t *gf=r->fb.map,*gm=r->mesh.map;
    for(uint32_t b=0;b<2;b++)for(uint32_t i=0;i<FB_PIXELS;i++){
        uint32_t o=i*2u;
        gf[b*FB_PIXELS+i]=((uint32_t)s->vdp1_fb[b][o]<<8)|s->vdp1_fb[b][o+1];
        gm[b*FB_PIXELS+i]=((uint32_t)s->vdp1_meshfb[b][o]<<8)|s->vdp1_meshfb[b][o+1];
    }
}

static void set_error(char *out, size_t size, const char *fmt, ...)
{
    va_list ap;
    if (!out || !size) return;
    va_start(ap, fmt);
    vsnprintf(out, size, fmt, ap);
    va_end(ap);
}

static uint32_t find_memory(saturn_vk_renderer *r, uint32_t bits,
                            VkMemoryPropertyFlags wanted)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(r->physical, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & wanted) == wanted)
            return i;
    return UINT32_MAX;
}

static int make_buffer(saturn_vk_renderer *r, vkbuf *b, VkDeviceSize size,
                       char *error, size_t error_size)
{
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    uint32_t mt;
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(r->device, &bi, NULL, &b->buffer));
    vkGetBufferMemoryRequirements(r->device, b->buffer, &mr);
    mt = find_memory(r, mr.memoryTypeBits,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt == UINT32_MAX) {
        set_error(error, error_size, "Vulkan device has no coherent host-visible storage memory");
        goto fail;
    }
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = mt;
    VK_CHECK(vkAllocateMemory(r->device, &ai, NULL, &b->memory));
    VK_CHECK(vkBindBufferMemory(r->device, b->buffer, b->memory, 0));
    VK_CHECK(vkMapMemory(r->device, b->memory, 0, size, 0, &b->map));
    b->size = size;
    memset(b->map, 0, (size_t)size);
    return 1;
fail:
    return 0;
}

static void free_buffer(saturn_vk_renderer *r, vkbuf *b)
{
    if (!r->device) return;
    if (b->map) vkUnmapMemory(r->device, b->memory);
    if (b->buffer) vkDestroyBuffer(r->device, b->buffer, NULL);
    if (b->memory) vkFreeMemory(r->device, b->memory, NULL);
    memset(b, 0, sizeof *b);
}

static int has_device_extension(VkPhysicalDevice p, const char *name)
{
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(p, NULL, &n, NULL);
    VkExtensionProperties *e = n ? malloc((size_t)n * sizeof *e) : NULL;
    if (n && !e) return 0;
    vkEnumerateDeviceExtensionProperties(p, NULL, &n, e);
    int found = 0;
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) found = 1;
    free(e);
    return found;
}

static int choose_device(saturn_vk_renderer *r, char *error, size_t error_size)
{
    uint32_t n = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(r->instance, &n, NULL));
    if (!n) { set_error(error, error_size, "No Vulkan physical device found"); goto fail; }
    VkPhysicalDevice *list = malloc((size_t)n * sizeof *list);
    if (!list) { set_error(error, error_size, "out of memory"); goto fail; }
    VK_CHECK(vkEnumeratePhysicalDevices(r->instance, &n, list));
    int best_score = -1;
    for (uint32_t d = 0; d < n; d++) {
        uint32_t qn = 0;
        VkPhysicalDeviceProperties pp;
        vkGetPhysicalDeviceProperties(list[d], &pp);
        if (!has_device_extension(list[d], VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;
        vkGetPhysicalDeviceQueueFamilyProperties(list[d], &qn, NULL);
        VkQueueFamilyProperties *qp = malloc((size_t)qn * sizeof *qp);
        if (!qp) continue;
        vkGetPhysicalDeviceQueueFamilyProperties(list[d], &qn, qp);
        for (uint32_t q = 0; q < qn; q++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(list[d], q, r->surface, &present);
            if (!present || !(qp[q].queueFlags & VK_QUEUE_COMPUTE_BIT) ||
                !(qp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            int score = pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 300
                      : pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 200 : 100;
            if (score > best_score) {
                best_score = score; r->physical = list[d]; r->queue_family = q;
                strncpy(r->device_name, pp.deviceName, sizeof r->device_name - 1);
            }
        }
        free(qp);
    }
    free(list);
    if (!r->physical) { set_error(error, error_size, "No Vulkan graphics/compute/present queue found"); goto fail; }
    return 1;
fail:
    return 0;
}

static void destroy_swapchain(saturn_vk_renderer *r)
{
    free(r->swap_images); r->swap_images = NULL; r->swap_count = 0;
    if (r->swapchain) vkDestroySwapchainKHR(r->device, r->swapchain, NULL);
    r->swapchain = VK_NULL_HANDLE;
}

static int create_swapchain(saturn_vk_renderer *r, char *error, size_t error_size)
{
    VkSurfaceCapabilitiesKHR caps;
    uint32_t fn = 0, pn = 0;
    VkSurfaceFormatKHR *formats = NULL;
    VkPresentModeKHR *modes = NULL;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(r->physical, r->surface, &caps));
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(r->physical, r->surface, &fn, NULL));
    formats = fn ? malloc((size_t)fn * sizeof *formats) : NULL;
    if (!formats) { set_error(error, error_size, "Vulkan surface has no formats"); goto fail; }
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(r->physical, r->surface, &fn, formats));
    VkSurfaceFormatKHR chosen = formats[0];
    for (uint32_t i = 0; i < fn; i++)
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) chosen = formats[i];
    free(formats); formats = NULL;

    VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(r->physical, r->surface, &pn, NULL));
    modes = pn ? malloc((size_t)pn * sizeof *modes) : NULL;
    if (!modes) { set_error(error, error_size, "Vulkan surface has no present modes"); goto fail; }
    VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(r->physical, r->surface, &pn, modes));
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t i = 0; i < pn; i++) if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = modes[i];
    for (uint32_t i = 0; i < pn; i++) if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = modes[i];
    free(modes); modes = NULL;

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        int w = 0, h = 0; SDL_Vulkan_GetDrawableSize(r->window, &w, &h);
        extent.width = (uint32_t)(w > 0 ? w : 1); extent.height = (uint32_t)(h > 0 ? h : 1);
        if (extent.width < caps.minImageExtent.width) extent.width = caps.minImageExtent.width;
        if (extent.height < caps.minImageExtent.height) extent.height = caps.minImageExtent.height;
        if (extent.width > caps.maxImageExtent.width) extent.width = caps.maxImageExtent.width;
        if (extent.height > caps.maxImageExtent.height) extent.height = caps.maxImageExtent.height;
    }
    uint32_t count = caps.minImageCount + 1u;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    ci.surface = r->surface; ci.minImageCount = count;
    ci.imageFormat = chosen.format; ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = extent; ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                      ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                      : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    ci.presentMode = mode; ci.clipped = VK_TRUE;
    VK_CHECK(vkCreateSwapchainKHR(r->device, &ci, NULL, &r->swapchain));
    r->swap_format = chosen.format; r->swap_extent = extent;
    VK_CHECK(vkGetSwapchainImagesKHR(r->device, r->swapchain, &r->swap_count, NULL));
    r->swap_images = malloc((size_t)r->swap_count * sizeof *r->swap_images);
    if (!r->swap_images) { set_error(error, error_size, "out of memory"); goto fail; }
    VK_CHECK(vkGetSwapchainImagesKHR(r->device, r->swapchain, &r->swap_count, r->swap_images));
    return 1;
fail:
    free(formats); free(modes); destroy_swapchain(r); return 0;
}

static unsigned char *load_shader(const char *name, size_t *size)
{
    char path[1024]; FILE *f = NULL;
    char *base = SDL_GetBasePath();
    if (base) {
        snprintf(path, sizeof path, "%sshaders\\%s", base, name);
        f = fopen(path, "rb"); SDL_free(base);
    }
    if (!f) { snprintf(path, sizeof path, "runner\\shaders\\%s", name); f = fopen(path, "rb"); }
    if (!f) { snprintf(path, sizeof path, "shaders\\%s", name); f = fopen(path, "rb"); }
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    if (n <= 0 || (n & 3)) { fclose(f); return NULL; }
    unsigned char *data = malloc((size_t)n);
    if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) { free(data); fclose(f); return NULL; }
    fclose(f); *size = (size_t)n; return data;
}

static VkShaderModule shader_module(saturn_vk_renderer *r, const char *name,
                                    char *error, size_t error_size)
{
    size_t size = 0; unsigned char *data = load_shader(name, &size);
    if (!data) { set_error(error, error_size, "cannot load Vulkan shader %s", name); return VK_NULL_HANDLE; }
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = size; ci.pCode = (const uint32_t *)data;
    VkShaderModule m = VK_NULL_HANDLE;
    VkResult vr = vkCreateShaderModule(r->device, &ci, NULL, &m);
    free(data);
    if (vr != VK_SUCCESS) set_error(error, error_size, "vkCreateShaderModule(%s) failed (%d)", name, (int)vr);
    return m;
}

static int create_compute(saturn_vk_renderer *r, const char *name, VkPipeline *out,
                          char *error, size_t error_size)
{
    VkShaderModule sm = shader_module(r, name, error, error_size);
    if (!sm) return 0;
    VkPipelineShaderStageCreateInfo ss = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    ss.stage = VK_SHADER_STAGE_COMPUTE_BIT; ss.module = sm; ss.pName = "main";
    VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    ci.stage = ss; ci.layout = r->pipeline_layout;
    VkResult vr = vkCreateComputePipelines(r->device, VK_NULL_HANDLE, 1, &ci, NULL, out);
    vkDestroyShaderModule(r->device, sm, NULL);
    if (vr != VK_SUCCESS) { set_error(error, error_size, "compute pipeline %s failed (%d)", name, (int)vr); return 0; }
    return 1;
}

static int queue_vdp1(void *userdata, const saturn_vk_vdp1_op *op)
{
    saturn_vk_renderer *r = userdata;
    if (r->pending_count >= MAX_VDP1_OPS) {
        if (!r->queue_overflow_reported) {
            fprintf(stderr, "[vulkan] VDP1 operation queue overflow (%u)\n", MAX_VDP1_OPS);
            r->queue_overflow_reported = 1;
        }
        return 0;
    }
    if (r->pending_count == r->pending_cap) {
        uint32_t cap = r->pending_cap ? r->pending_cap * 2u : 4096u;
        if (cap > MAX_VDP1_OPS) cap = MAX_VDP1_OPS;
        void *p = realloc(r->pending, (size_t)cap * sizeof *r->pending);
        if (!p) return 0;
        r->pending = p; r->pending_cap = cap;
    }
    r->pending[r->pending_count++] = *op;
    return 1;
}

static void reset_vdp1(void *userdata) { (void)userdata; }

saturn_vk_renderer *saturn_vk_create(SDL_Window *window, saturn *s,
                                     char *error, size_t error_size)
{
    saturn_vk_renderer *r = calloc(1, sizeof *r);
    uint32_t ext_count = 0;
    const char **extensions = NULL;
    VkShaderModule unused = VK_NULL_HANDLE;
    (void)unused;
    if (!r) { set_error(error, error_size, "out of memory"); return NULL; }
    r->window = window; r->system = s;

    if (!SDL_Vulkan_GetInstanceExtensions(window, &ext_count, NULL) || !ext_count) {
        set_error(error, error_size, "SDL Vulkan extensions: %s", SDL_GetError()); goto fail;
    }
    extensions = malloc((size_t)ext_count * sizeof *extensions);
    if (!extensions || !SDL_Vulkan_GetInstanceExtensions(window, &ext_count, extensions)) {
        set_error(error, error_size, "SDL Vulkan extensions: %s", SDL_GetError()); goto fail;
    }
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "SaturnRecomp"; app.applicationVersion = VK_MAKE_VERSION(1,0,0);
    app.pEngineName = "SaturnRecomp Vulkan"; app.engineVersion = VK_MAKE_VERSION(1,0,0);
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app; ici.enabledExtensionCount = ext_count;
    ici.ppEnabledExtensionNames = extensions;
    VK_CHECK(vkCreateInstance(&ici, NULL, &r->instance));
    free(extensions); extensions = NULL;
    if (!SDL_Vulkan_CreateSurface(window, r->instance, &r->surface)) {
        set_error(error, error_size, "SDL_Vulkan_CreateSurface: %s", SDL_GetError()); goto fail;
    }
    if (!choose_device(r, error, error_size)) goto fail;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = r->queue_family; qci.queueCount = 1; qci.pQueuePriorities = &priority;
    const char *dev_ext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = dev_ext;
    VK_CHECK(vkCreateDevice(r->physical, &dci, NULL, &r->device));
    vkGetDeviceQueue(r->device, r->queue_family, 0, &r->queue);
    if (!create_swapchain(r, error, error_size)) goto fail;

    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.queueFamilyIndex = r->queue_family; pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VK_CHECK(vkCreateCommandPool(r->device, &pci, NULL, &r->command_pool));
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = r->command_pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(r->device, &cai, &r->command));
    VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VK_CHECK(vkCreateSemaphore(r->device, &sci, NULL, &r->acquired));
    VK_CHECK(vkCreateSemaphore(r->device, &sci, NULL, &r->rendered));
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VK_CHECK(vkCreateFence(r->device, &fci, NULL, &r->fence));

    if (!make_buffer(r,&r->v1ram,VDP1_VRAM_SZ,error,error_size) ||
        !make_buffer(r,&r->v2ram,VDP2_VRAM_SZ,error,error_size) ||
        !make_buffer(r,&r->cram,CRAM_SIZE,error,error_size) ||
        !make_buffer(r,&r->params,PARAM_WORDS*4u,error,error_size) ||
        !make_buffer(r,&r->ops,(VkDeviceSize)MAX_VDP1_OPS*sizeof(saturn_vk_vdp1_op),error,error_size) ||
        !make_buffer(r,&r->fb,(VkDeviceSize)FB_PIXELS*2u*4u,error,error_size) ||
        !make_buffer(r,&r->mesh,(VkDeviceSize)FB_PIXELS*2u*4u,error,error_size) ||
        !make_buffer(r,&r->tile_headers,(VkDeviceSize)VDP1_TILES*2u*4u,error,error_size) ||
        !make_buffer(r,&r->tile_refs,(VkDeviceSize)MAX_TILE_REFS*4u,error,error_size)) goto fail;
    upload_cpu_framebuffers(r,s);

    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType=VK_IMAGE_TYPE_2D;ii.format=VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent.width=704;ii.extent.height=512;ii.extent.depth=1;ii.mipLevels=1;ii.arrayLayers=1;
    ii.samples=VK_SAMPLE_COUNT_1_BIT;ii.tiling=VK_IMAGE_TILING_OPTIMAL;
    ii.usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;ii.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(r->device,&ii,NULL,&r->output));
    VkMemoryRequirements imr;vkGetImageMemoryRequirements(r->device,r->output,&imr);
    uint32_t imt=find_memory(r,imr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if(imt==UINT32_MAX)imt=find_memory(r,imr.memoryTypeBits,0);
    VkMemoryAllocateInfo iai={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};iai.allocationSize=imr.size;iai.memoryTypeIndex=imt;
    VK_CHECK(vkAllocateMemory(r->device,&iai,NULL,&r->output_memory));
    VK_CHECK(vkBindImageMemory(r->device,r->output,r->output_memory,0));
    VkImageViewCreateInfo vi={VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=r->output;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=ii.format;
    vi.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;vi.subresourceRange.levelCount=1;vi.subresourceRange.layerCount=1;
    VK_CHECK(vkCreateImageView(r->device,&vi,NULL,&r->output_view));

    VkDescriptorSetLayoutBinding bindings[10]; memset(bindings,0,sizeof bindings);
    for(uint32_t i=0;i<10;i++){bindings[i].binding=i;bindings[i].descriptorCount=1;bindings[i].descriptorType=i==7?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;bindings[i].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;}
    VkDescriptorSetLayoutCreateInfo dl={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};dl.bindingCount=10;dl.pBindings=bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(r->device,&dl,NULL,&r->desc_layout));
    VkPipelineLayoutCreateInfo pl={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&r->desc_layout;
    VK_CHECK(vkCreatePipelineLayout(r->device,&pl,NULL,&r->pipeline_layout));
    VkDescriptorPoolSize ps[2]={{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,9},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1}};
    VkDescriptorPoolCreateInfo dp={VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dp.maxSets=1;dp.poolSizeCount=2;dp.pPoolSizes=ps;
    VK_CHECK(vkCreateDescriptorPool(r->device,&dp,NULL,&r->desc_pool));
    VkDescriptorSetAllocateInfo da={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};da.descriptorPool=r->desc_pool;da.descriptorSetCount=1;da.pSetLayouts=&r->desc_layout;
    VK_CHECK(vkAllocateDescriptorSets(r->device,&da,&r->desc_set));
    vkbuf *bufs[9]={&r->v1ram,&r->v2ram,&r->cram,&r->params,&r->ops,&r->fb,&r->mesh,&r->tile_headers,&r->tile_refs};
    const uint32_t bindno[9]={0,1,2,3,4,5,6,8,9};
    VkDescriptorBufferInfo dbi[9];VkWriteDescriptorSet wr[10];memset(wr,0,sizeof wr);
    for(uint32_t i=0;i<7;i++){dbi[i].buffer=bufs[i]->buffer;dbi[i].offset=0;dbi[i].range=bufs[i]->size;wr[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;wr[i].dstSet=r->desc_set;wr[i].dstBinding=i;wr[i].descriptorCount=1;wr[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;wr[i].pBufferInfo=&dbi[i];}
    for(uint32_t i=7;i<9;i++){dbi[i].buffer=bufs[i]->buffer;dbi[i].offset=0;dbi[i].range=bufs[i]->size;wr[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;wr[i].dstSet=r->desc_set;wr[i].dstBinding=bindno[i];wr[i].descriptorCount=1;wr[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;wr[i].pBufferInfo=&dbi[i];}
    VkDescriptorImageInfo di={VK_NULL_HANDLE,r->output_view,VK_IMAGE_LAYOUT_GENERAL};wr[9].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;wr[9].dstSet=r->desc_set;wr[9].dstBinding=7;wr[9].descriptorCount=1;wr[9].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;wr[9].pImageInfo=&di;
    vkUpdateDescriptorSets(r->device,10,wr,0,NULL);
    if(!create_compute(r,"vdp1.comp.spv",&r->vdp1_pipeline,error,error_size)||!create_compute(r,"vdp2.comp.spv",&r->vdp2_pipeline,error,error_size))goto fail;

    saturn_vdp1_gpu_sink sink={r,queue_vdp1,reset_vdp1};vdp1_gpu_bind(s,&sink);
    fprintf(stderr,"[video] Vulkan: %s (VDP1 compute + VDP2 compute + swapchain)\n",r->device_name);
    return r;
fail:
    free(extensions); saturn_vk_destroy(r); return NULL;
}

static void image_barrier(VkCommandBuffer cb,VkImage image,VkImageLayout oldl,VkImageLayout newl,
                          VkAccessFlags src,VkAccessFlags dst,VkPipelineStageFlags ss,VkPipelineStageFlags ds)
{
    VkImageMemoryBarrier b={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.oldLayout=oldl;b.newLayout=newl;
    b.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=image;
    b.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;b.subresourceRange.levelCount=1;b.subresourceRange.layerCount=1;
    b.srcAccessMask=src;b.dstAccessMask=dst;vkCmdPipelineBarrier(cb,ss,ds,0,0,NULL,0,NULL,1,&b);
}

int saturn_vk_render(saturn_vk_renderer *r, saturn *s, int width, int height,
                     char *error, size_t error_size)
{
    if(!r||!r->device)return 0;
    if(r->present_pending&&!saturn_vk_present(r,error,error_size))return 0;
    if(width<1)width=320;if(height<1)height=224;if(width>704)width=704;if(height>512)height=512;
    VK_CHECK(vkWaitForFences(r->device,1,&r->fence,VK_TRUE,UINT64_MAX));
    int dw=0,dh=0;SDL_Vulkan_GetDrawableSize(r->window,&dw,&dh);
    if(dw<=0||dh<=0)return 1;
    if((uint32_t)dw!=r->swap_extent.width||(uint32_t)dh!=r->swap_extent.height){vkDeviceWaitIdle(r->device);destroy_swapchain(r);if(!create_swapchain(r,error,error_size))goto fail;}
    uint32_t image_index=0;VkResult ar=vkAcquireNextImageKHR(r->device,r->swapchain,UINT64_MAX,r->acquired,VK_NULL_HANDLE,&image_index);
    if(ar==VK_ERROR_OUT_OF_DATE_KHR){vkDeviceWaitIdle(r->device);destroy_swapchain(r);return create_swapchain(r,error,error_size);}if(ar!=VK_SUCCESS&&ar!=VK_SUBOPTIMAL_KHR){set_error(error,error_size,"vkAcquireNextImageKHR failed (%d)",(int)ar);goto fail;}

    memcpy(r->v1ram.map,s->vdp1_vram,VDP1_VRAM_SZ);memcpy(r->v2ram.map,s->vdp2_vram,VDP2_VRAM_SZ);memcpy(r->cram.map,s->cram,CRAM_SIZE);
    uint32_t *rp=r->params.map;for(uint32_t i=0;i<256;i++)rp[i]=s->vdp2_reg[i];rp[256]=(uint32_t)width;rp[257]=(uint32_t)height;rp[258]=(uint32_t)(s->fb_draw^1);rp[259]=r->pending_count;rp[260]=s->layer_mask?s->layer_mask:0x3fu;rp[261]=getenv("SATURN_VK_VDP1_ONLY")?1u:0u;
    memcpy(r->ops.map,r->pending,(size_t)r->pending_count*sizeof *r->pending);
    if(!build_tiles(r,error,error_size))goto fail;
    uint32_t *gf=r->fb.map;

    VK_CHECK(vkResetFences(r->device,1,&r->fence));VK_CHECK(vkResetCommandBuffer(r->command,0));
    VkCommandBufferBeginInfo bi={VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;VK_CHECK(vkBeginCommandBuffer(r->command,&bi));
    image_barrier(r->command,r->output,r->output_ready?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,0,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);r->output_ready=1;
    vkCmdBindDescriptorSets(r->command,VK_PIPELINE_BIND_POINT_COMPUTE,r->pipeline_layout,0,1,&r->desc_set,0,NULL);
    vkCmdBindPipeline(r->command,VK_PIPELINE_BIND_POINT_COMPUTE,r->vdp1_pipeline);vkCmdDispatch(r->command,64,32,1);
    VkBufferMemoryBarrier bb[2];memset(bb,0,sizeof bb);for(int i=0;i<2;i++){bb[i].sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;bb[i].srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;bb[i].dstAccessMask=VK_ACCESS_SHADER_READ_BIT;bb[i].srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;bb[i].dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;bb[i].buffer=i?r->mesh.buffer:r->fb.buffer;bb[i].size=VK_WHOLE_SIZE;}
    vkCmdPipelineBarrier(r->command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,NULL,2,bb,0,NULL);
    vkCmdBindPipeline(r->command,VK_PIPELINE_BIND_POINT_COMPUTE,r->vdp2_pipeline);vkCmdDispatch(r->command,((uint32_t)width+7u)/8u,((uint32_t)height+7u)/8u,1);
    image_barrier(r->command,r->output,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    image_barrier(r->command,r->swap_images[image_index],VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue black={{0,0,0,1}};VkImageSubresourceRange sr={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdClearColorImage(r->command,r->swap_images[image_index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&black,1,&sr);
    int outw=(int)r->swap_extent.width,outh=(int)r->swap_extent.height,x0=0,y0=0,x1=outw,y1=outh;if(outw*3<=outh*4){y1=outw*3/4;y0=(outh-y1)/2;y1+=y0;}else{x1=outh*4/3;x0=(outw-x1)/2;x1+=x0;}
    VkImageBlit bl={0};bl.srcSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;bl.srcSubresource.layerCount=1;bl.srcOffsets[1].x=width;bl.srcOffsets[1].y=height;bl.srcOffsets[1].z=1;bl.dstSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;bl.dstSubresource.layerCount=1;bl.dstOffsets[0].x=x0;bl.dstOffsets[0].y=y0;bl.dstOffsets[1].x=x1;bl.dstOffsets[1].y=y1;bl.dstOffsets[1].z=1;
    vkCmdBlitImage(r->command,r->output,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,r->swap_images[image_index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&bl,VK_FILTER_NEAREST);
    image_barrier(r->command,r->output,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    image_barrier(r->command,r->swap_images[image_index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,VK_ACCESS_TRANSFER_WRITE_BIT,0,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    VK_CHECK(vkEndCommandBuffer(r->command));VkPipelineStageFlags waitstage=VK_PIPELINE_STAGE_TRANSFER_BIT;VkSubmitInfo si={VK_STRUCTURE_TYPE_SUBMIT_INFO};si.waitSemaphoreCount=1;si.pWaitSemaphores=&r->acquired;si.pWaitDstStageMask=&waitstage;si.commandBufferCount=1;si.pCommandBuffers=&r->command;si.signalSemaphoreCount=1;si.pSignalSemaphores=&r->rendered;VK_CHECK(vkQueueSubmit(r->queue,1,&si,r->fence));
    /* Do not serialize the host on the GPU here. The emulated machine writes
     * its own RAM/VRAM while this submission consumes the renderer's upload
     * buffers, so the next Saturn field can execute in parallel with this
     * field's compute and presentation. The fence at the start of the next
     * upload protects every mapped Vulkan buffer before it is reused. Debug
     * readback is the sole exception and waits explicitly below. */
    if(getenv("SATURN_VKLOG") && (r->pending_count || (s->frames % 60u)==0u)) {
        VK_CHECK(vkWaitForFences(r->device,1,&r->fence,VK_TRUE,UINT64_MAX));
        uint32_t nq=0,nl=0,ne=0,n0=0,n1=0;
        for(uint32_t i=0;i<r->pending_count;i++){nq+=r->pending[i].kind==SATURN_VK_VDP1_QUAD;nl+=r->pending[i].kind==SATURN_VK_VDP1_LINE;ne+=r->pending[i].kind==SATURN_VK_VDP1_ERASE;}
        for(uint32_t i=0;i<FB_PIXELS;i++){n0+=gf[i]!=0;n1+=gf[FB_PIXELS+i]!=0;}
        fprintf(stderr,"[vk-vdp1 f%llu] ops=%u q=%u line=%u erase=%u draw=%d display=%d nz=%u,%u\n",(unsigned long long)s->frames,r->pending_count,nq,nl,ne,s->fb_draw,s->fb_draw^1,n0,n1);
        if(r->pending_count){const saturn_vk_vdp1_op *o=&r->pending[0];fprintf(stderr,"[vk-vdp1-op] kind=%u target=%u A=%d,%d B=%d,%d C=%d,%d D=%d,%d tex=%u %ux%u col=%04X pmod=%04X\n",o->kind,o->target,o->xy[0],o->xy[1],o->xy[2],o->xy[3],o->xy[4],o->xy[5],o->xy[6],o->xy[7],o->textured,o->tw,o->th,o->colr,o->pmod);}
    }
    r->pending_count=0;r->present_index=image_index;r->present_pending=1;return 1;
fail:
    return 0;
}

int saturn_vk_present(saturn_vk_renderer *r, char *error, size_t error_size)
{
    if(!r||!r->device)return 0;if(!r->present_pending)return 1;
    VkPresentInfoKHR pi={VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};pi.waitSemaphoreCount=1;pi.pWaitSemaphores=&r->rendered;pi.swapchainCount=1;pi.pSwapchains=&r->swapchain;pi.pImageIndices=&r->present_index;
    VkResult pr=vkQueuePresentKHR(r->queue,&pi);r->present_pending=0;
    if(pr==VK_SUCCESS||pr==VK_SUBOPTIMAL_KHR||pr==VK_ERROR_OUT_OF_DATE_KHR)return 1;
    set_error(error,error_size,"vkQueuePresentKHR failed (%d)",(int)pr);return 0;
}

const char *saturn_vk_device_name(const saturn_vk_renderer *r){return r?r->device_name:"";}

void saturn_vk_destroy(saturn_vk_renderer *r)
{
    if(!r)return;if(r->system)vdp1_gpu_bind(r->system,NULL);if(r->device)vkDeviceWaitIdle(r->device);
    if(r->vdp1_pipeline)vkDestroyPipeline(r->device,r->vdp1_pipeline,NULL);if(r->vdp2_pipeline)vkDestroyPipeline(r->device,r->vdp2_pipeline,NULL);
    if(r->pipeline_layout)vkDestroyPipelineLayout(r->device,r->pipeline_layout,NULL);if(r->desc_pool)vkDestroyDescriptorPool(r->device,r->desc_pool,NULL);if(r->desc_layout)vkDestroyDescriptorSetLayout(r->device,r->desc_layout,NULL);
    if(r->output_view)vkDestroyImageView(r->device,r->output_view,NULL);if(r->output)vkDestroyImage(r->device,r->output,NULL);if(r->output_memory)vkFreeMemory(r->device,r->output_memory,NULL);
    free_buffer(r,&r->tile_refs);free_buffer(r,&r->tile_headers);free_buffer(r,&r->mesh);free_buffer(r,&r->fb);free_buffer(r,&r->ops);free_buffer(r,&r->params);free_buffer(r,&r->cram);free_buffer(r,&r->v2ram);free_buffer(r,&r->v1ram);
    if(r->fence)vkDestroyFence(r->device,r->fence,NULL);if(r->rendered)vkDestroySemaphore(r->device,r->rendered,NULL);if(r->acquired)vkDestroySemaphore(r->device,r->acquired,NULL);if(r->command_pool)vkDestroyCommandPool(r->device,r->command_pool,NULL);
    if(r->device){destroy_swapchain(r);vkDestroyDevice(r->device,NULL);}if(r->surface)vkDestroySurfaceKHR(r->instance,r->surface,NULL);if(r->instance)vkDestroyInstance(r->instance,NULL);
    free(r->pending);free(r);
}
