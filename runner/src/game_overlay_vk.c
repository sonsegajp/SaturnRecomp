#include "game_overlay_vk.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct overlay_buffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void *mapped;
    VkDeviceSize size;
} overlay_buffer;

struct game_overlay_vk {
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    unsigned queue_family;
    VkRenderPass render_pass;
    VkDescriptorSetLayout descriptor_layout;
    VkDescriptorPool descriptor_pool;
    VkDescriptorSet descriptor;
    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;
    VkShaderModule vertex_shader, fragment_shader;
    VkImage atlas;
    VkDeviceMemory atlas_memory;
    VkImageView atlas_view;
    VkSampler sampler;
    overlay_buffer vertices, indices;
    VkImage *images;
    VkImageView *views;
    VkFramebuffer *framebuffers;
    unsigned image_count;
    VkExtent2D extent;
    VkFormat format;
};

static void set_error(char *error, size_t size, const char *format, ...)
{
    if (!error || !size) return;
    va_list args;
    va_start(args, format);
    vsnprintf(error, size, format, args);
    va_end(args);
}

#define CHECK(call) do { VkResult result_ = (call); if (result_ != VK_SUCCESS) { \
    set_error(error, error_size, "%s failed (%d)", #call, (int)result_); goto fail; \
} } while (0)

static uint32_t memory_type(game_overlay_vk *o, uint32_t bits, VkMemoryPropertyFlags flags)
{
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(o->physical, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    return UINT32_MAX;
}

static void free_buffer(game_overlay_vk *o, overlay_buffer *b)
{
    if (b->mapped) vkUnmapMemory(o->device, b->memory);
    if (b->buffer) vkDestroyBuffer(o->device, b->buffer, NULL);
    if (b->memory) vkFreeMemory(o->device, b->memory, NULL);
    memset(b, 0, sizeof *b);
}

static int make_buffer(game_overlay_vk *o, overlay_buffer *b, VkDeviceSize size,
    VkBufferUsageFlags usage, char *error, size_t error_size)
{
    VkBufferCreateInfo info = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    CHECK(vkCreateBuffer(o->device, &info, NULL, &b->buffer));
    VkMemoryRequirements needs;
    vkGetBufferMemoryRequirements(o->device, b->buffer, &needs);
    uint32_t type = memory_type(o, needs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) { set_error(error, error_size, "No coherent overlay upload memory"); goto fail; }
    VkMemoryAllocateInfo allocation = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = needs.size, .memoryTypeIndex = type };
    CHECK(vkAllocateMemory(o->device, &allocation, NULL, &b->memory));
    CHECK(vkBindBufferMemory(o->device, b->buffer, b->memory, 0));
    CHECK(vkMapMemory(o->device, b->memory, 0, size, 0, &b->mapped));
    b->size = size;
    return 1;
fail:
    free_buffer(o, b);
    return 0;
}

static void image_barrier(VkCommandBuffer command, VkImage image,
    VkImageLayout before, VkImageLayout after,
    VkAccessFlags source_access, VkAccessFlags destination_access,
    VkPipelineStageFlags source_stage, VkPipelineStageFlags destination_stage)
{
    VkImageMemoryBarrier barrier = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = source_access, .dstAccessMask = destination_access,
        .oldLayout = before, .newLayout = after,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(command, source_stage, destination_stage, 0,
        0, NULL, 0, NULL, 1, &barrier);
}

static int make_atlas(game_overlay_vk *o, const unsigned char *pixels, int width,
    int height, char *error, size_t error_size)
{
    overlay_buffer staging = {0};
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    int success = 0;
    VkDeviceSize bytes = (VkDeviceSize)width * (unsigned)height * 4;
    if (!make_buffer(o, &staging, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, error, error_size)) goto fail;
    memcpy(staging.mapped, pixels, (size_t)bytes);
    VkImageCreateInfo image = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { (unsigned)width, (unsigned)height, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    CHECK(vkCreateImage(o->device, &image, NULL, &o->atlas));
    VkMemoryRequirements needs;
    vkGetImageMemoryRequirements(o->device, o->atlas, &needs);
    uint32_t type = memory_type(o, needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) { set_error(error, error_size, "No overlay texture memory"); goto fail; }
    VkMemoryAllocateInfo allocation = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = needs.size, .memoryTypeIndex = type };
    CHECK(vkAllocateMemory(o->device, &allocation, NULL, &o->atlas_memory));
    CHECK(vkBindImageMemory(o->device, o->atlas, o->atlas_memory, 0));
    VkImageViewCreateInfo view = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = o->atlas, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = image.format,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    CHECK(vkCreateImageView(o->device, &view, NULL, &o->atlas_view));
    VkSamplerCreateInfo sampler = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0 };
    CHECK(vkCreateSampler(o->device, &sampler, NULL, &o->sampler));
    VkCommandPoolCreateInfo pool_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, .queueFamilyIndex = o->queue_family };
    CHECK(vkCreateCommandPool(o->device, &pool_info, NULL, &pool));
    VkCommandBuffer command;
    VkCommandBufferAllocateInfo command_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    CHECK(vkAllocateCommandBuffers(o->device, &command_info, &command));
    VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    CHECK(vkBeginCommandBuffer(command, &begin));
    image_barrier(command, o->atlas, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { (unsigned)width, (unsigned)height, 1 } };
    vkCmdCopyBufferToImage(command, staging.buffer, o->atlas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    image_barrier(command, o->atlas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    CHECK(vkEndCommandBuffer(command));
    VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    CHECK(vkCreateFence(o->device, &fence_info, NULL, &fence));
    VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &command };
    CHECK(vkQueueSubmit(o->queue, 1, &submit, fence));
    CHECK(vkWaitForFences(o->device, 1, &fence, VK_TRUE, UINT64_MAX));
    success = 1;
fail:
    if (fence) vkDestroyFence(o->device, fence, NULL);
    if (pool) vkDestroyCommandPool(o->device, pool, NULL);
    free_buffer(o, &staging);
    return success;
}

static VkShaderModule load_shader(game_overlay_vk *o, const char *directory,
    const char *name, char *error, size_t error_size)
{
    char path[2048];
    int length = snprintf(path, sizeof path, "%s/%s", directory, name);
    if (length < 0 || length >= (int)sizeof path) { set_error(error, error_size, "Overlay shader path is too long"); return VK_NULL_HANDLE; }
    FILE *file = fopen(path, "rb");
    if (!file) { set_error(error, error_size, "Could not open overlay shader %s", path); return VK_NULL_HANDLE; }
    if (fseek(file, 0, SEEK_END)) { fclose(file); return VK_NULL_HANDLE; }
    long size = ftell(file);
    rewind(file);
    if (size < 20 || size % 4 || size > 1024 * 1024) { fclose(file); set_error(error, error_size, "Invalid overlay shader %s", name); return VK_NULL_HANDLE; }
    uint32_t *code = malloc((size_t)size);
    if (!code) { fclose(file); set_error(error, error_size, "Could not allocate shader data"); return VK_NULL_HANDLE; }
    size_t read = fread(code, 1, (size_t)size, file);
    fclose(file);
    if (read != (size_t)size) { free(code); set_error(error, error_size, "Could not read overlay shader %s", name); return VK_NULL_HANDLE; }
    VkShaderModuleCreateInfo info = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = (size_t)size, .pCode = code };
    VkShaderModule module = VK_NULL_HANDLE;
    VkResult result = vkCreateShaderModule(o->device, &info, NULL, &module);
    free(code);
    if (result != VK_SUCCESS) set_error(error, error_size, "Overlay shader creation failed (%d)", (int)result);
    return module;
}

game_overlay_vk *game_overlay_vk_create(VkPhysicalDevice physical, VkDevice device,
    VkQueue queue, unsigned queue_family, VkFormat target_format,
    const unsigned char *atlas, int atlas_width, int atlas_height,
    const char *shader_directory, char *error, size_t error_size)
{
    if (!physical || !device || !queue || !atlas || atlas_width <= 0 || atlas_height <= 0 || !shader_directory) {
        set_error(error, error_size, "Invalid overlay renderer arguments"); return NULL;
    }
    game_overlay_vk *o = calloc(1, sizeof *o);
    if (!o) { set_error(error, error_size, "Could not allocate overlay renderer"); return NULL; }
    o->physical = physical; o->device = device; o->queue = queue;
    o->queue_family = queue_family; o->format = target_format;
    if (!make_atlas(o, atlas, atlas_width, atlas_height, error, error_size)) goto fail;
    VkDescriptorSetLayoutBinding binding = { .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT };
    VkDescriptorSetLayoutCreateInfo layout = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &binding };
    CHECK(vkCreateDescriptorSetLayout(device, &layout, NULL, &o->descriptor_layout));
    VkDescriptorPoolSize pool_size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo pool = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &pool_size };
    CHECK(vkCreateDescriptorPool(device, &pool, NULL, &o->descriptor_pool));
    VkDescriptorSetAllocateInfo allocation = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = o->descriptor_pool, .descriptorSetCount = 1, .pSetLayouts = &o->descriptor_layout };
    CHECK(vkAllocateDescriptorSets(device, &allocation, &o->descriptor));
    VkDescriptorImageInfo image = { o->sampler, o->atlas_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet write = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = o->descriptor, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &image };
    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
    VkPushConstantRange push = { VK_SHADER_STAGE_VERTEX_BIT, 0, 2 * sizeof(float) };
    VkPipelineLayoutCreateInfo pipeline_layout = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &o->descriptor_layout,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &push };
    CHECK(vkCreatePipelineLayout(device, &pipeline_layout, NULL, &o->pipeline_layout));
    /* Retain the tiny shader modules so a new swapchain format never requires
     * file I/O or rebuilding the immutable font atlas. */
    o->vertex_shader = load_shader(o, shader_directory, "game_overlay.vert.spv", error, error_size);
    o->fragment_shader = load_shader(o, shader_directory, "game_overlay.frag.spv", error, error_size);
    if (!o->vertex_shader || !o->fragment_shader ||
        !game_overlay_vk_set_format(o, target_format, error, error_size)) goto fail;
    return o;
fail:
    game_overlay_vk_destroy(o);
    return NULL;
}

int game_overlay_vk_set_format(game_overlay_vk *o, VkFormat target_format,
    char *error, size_t error_size)
{
    if (!o || target_format == VK_FORMAT_UNDEFINED) {
        set_error(error, error_size, "Invalid overlay target format"); return 0;
    }
    if (o->pipeline && o->format == target_format) return 1;
    VkDevice device = o->device;
    VkRenderPass next_render_pass = VK_NULL_HANDLE;
    VkPipeline next_pipeline = VK_NULL_HANDLE;
    VkAttachmentDescription attachment = { .format = target_format, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference reference = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &reference };
    VkRenderPassCreateInfo render_pass = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &attachment, .subpassCount = 1, .pSubpasses = &subpass };
    CHECK(vkCreateRenderPass(device, &render_pass, NULL, &next_render_pass));
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = o->vertex_shader, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = o->fragment_shader, .pName = "main" }
    };
    VkVertexInputBindingDescription vertex_binding = { 0, sizeof(game_overlay_vertex), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attributes[] = {
        { 0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(game_overlay_vertex, position) },
        { 1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(game_overlay_vertex, uv) },
        { 2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(game_overlay_vertex, color) }
    };
    VkPipelineVertexInputStateCreateInfo vertex_input = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vertex_binding,
        .vertexAttributeDescriptionCount = 3, .pVertexAttributeDescriptions = attributes };
    VkPipelineInputAssemblyStateCreateInfo assembly = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo viewport = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo raster = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1 };
    VkPipelineMultisampleStateCreateInfo multisample = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState blend = { .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD, .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT };
    VkPipelineColorBlendStateCreateInfo color = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &blend };
    VkDynamicState states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = states };
    VkGraphicsPipelineCreateInfo pipeline = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &assembly, .pViewportState = &viewport,
        .pRasterizationState = &raster, .pMultisampleState = &multisample,
        .pColorBlendState = &color, .pDynamicState = &dynamic,
        .layout = o->pipeline_layout, .renderPass = next_render_pass };
    CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline, NULL, &next_pipeline));
    game_overlay_vk_clear_targets(o);
    if (o->pipeline) vkDestroyPipeline(device, o->pipeline, NULL);
    if (o->render_pass) vkDestroyRenderPass(device, o->render_pass, NULL);
    o->pipeline = next_pipeline; o->render_pass = next_render_pass; o->format = target_format;
    return 1;
fail:
    if (next_pipeline) vkDestroyPipeline(device, next_pipeline, NULL);
    if (next_render_pass) vkDestroyRenderPass(device, next_render_pass, NULL);
    return 0;
}

void game_overlay_vk_clear_targets(game_overlay_vk *o)
{
    if (!o) return;
    for (unsigned i = 0; i < o->image_count; ++i) {
        if (o->framebuffers && o->framebuffers[i]) vkDestroyFramebuffer(o->device, o->framebuffers[i], NULL);
        if (o->views && o->views[i]) vkDestroyImageView(o->device, o->views[i], NULL);
    }
    free(o->images); free(o->views); free(o->framebuffers);
    o->images = NULL; o->views = NULL; o->framebuffers = NULL; o->image_count = 0;
}

int game_overlay_vk_set_targets(game_overlay_vk *o, const VkImage *images,
    unsigned image_count, VkExtent2D extent, char *error, size_t error_size)
{
    if (!o || !images || !image_count || !extent.width || !extent.height) {
        set_error(error, error_size, "Invalid overlay presentation targets"); return 0;
    }
    game_overlay_vk_clear_targets(o);
    o->images = calloc(image_count, sizeof *o->images);
    o->views = calloc(image_count, sizeof *o->views);
    o->framebuffers = calloc(image_count, sizeof *o->framebuffers);
    o->image_count = image_count; o->extent = extent;
    if (!o->images || !o->views || !o->framebuffers) { set_error(error, error_size, "Could not allocate overlay targets"); goto fail; }
    memcpy(o->images, images, image_count * sizeof *images);
    for (unsigned i = 0; i < image_count; ++i) {
        VkImageViewCreateInfo view = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = images[i], .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = o->format,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        CHECK(vkCreateImageView(o->device, &view, NULL, &o->views[i]));
        VkFramebufferCreateInfo framebuffer = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = o->render_pass, .attachmentCount = 1, .pAttachments = &o->views[i],
            .width = extent.width, .height = extent.height, .layers = 1 };
        CHECK(vkCreateFramebuffer(o->device, &framebuffer, NULL, &o->framebuffers[i]));
    }
    return 1;
fail:
    game_overlay_vk_clear_targets(o);
    return 0;
}

static int ensure_buffer(game_overlay_vk *o, overlay_buffer *buffer, size_t bytes,
    VkBufferUsageFlags usage, char *error, size_t error_size)
{
    if (buffer->size >= bytes) return 1;
    overlay_buffer replacement = {0};
    VkDeviceSize size = 65536;
    while (size < bytes) size *= 2;
    if (!make_buffer(o, &replacement, size, usage, error, error_size)) return 0;
    free_buffer(o, buffer);
    *buffer = replacement;
    return 1;
}

int game_overlay_vk_record(game_overlay_vk *o, VkCommandBuffer command,
    unsigned image_index, const game_overlay_draw_data *draw, char *error, size_t error_size)
{
    if (!o || !command || !draw || image_index >= o->image_count || draw->width <= 0 || draw->height <= 0) {
        set_error(error, error_size, "Invalid overlay draw arguments"); return 0;
    }
    if (draw->command_count && (!draw->vertices || !draw->indices || !draw->commands)) {
        set_error(error, error_size, "Missing overlay draw buffers"); return 0;
    }
    if (draw->command_count) {
        if (!ensure_buffer(o, &o->vertices, draw->vertex_bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, error, error_size) ||
            !ensure_buffer(o, &o->indices, draw->index_bytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, error, error_size)) return 0;
        memcpy(o->vertices.mapped, draw->vertices, draw->vertex_bytes);
        memcpy(o->indices.mapped, draw->indices, draw->index_bytes);
    }
    image_barrier(command, o->images[image_index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkRenderPassBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = o->render_pass, .framebuffer = o->framebuffers[image_index],
        .renderArea = { {0, 0}, o->extent } };
    vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
    if (draw->command_count) {
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, o->pipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, o->pipeline_layout, 0, 1, &o->descriptor, 0, NULL);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(command, 0, 1, &o->vertices.buffer, &offset);
        vkCmdBindIndexBuffer(command, o->indices.buffer, 0, VK_INDEX_TYPE_UINT16);
        float scale[] = { 2.f / draw->width, 2.f / draw->height };
        vkCmdPushConstants(command, o->pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof scale, scale);
        VkViewport viewport = { 0, 0, (float)o->extent.width, (float)o->extent.height, 0, 1 };
        vkCmdSetViewport(command, 0, 1, &viewport);
        const float sx = (float)o->extent.width / draw->width, sy = (float)o->extent.height / draw->height;
        for (unsigned i = 0; i < draw->command_count; ++i) {
            const game_overlay_draw_command *c = &draw->commands[i];
            int x0 = (int)floorf(fmaxf(c->clip_x * sx, 0));
            int y0 = (int)floorf(fmaxf(c->clip_y * sy, 0));
            int x1 = (int)ceilf(fminf((c->clip_x + c->clip_w) * sx, (float)o->extent.width));
            int y1 = (int)ceilf(fminf((c->clip_y + c->clip_h) * sy, (float)o->extent.height));
            if (x1 <= x0 || y1 <= y0 || (uint64_t)c->first_index + c->element_count > draw->index_bytes / sizeof(uint16_t)) continue;
            VkRect2D scissor = { {x0, y0}, {(unsigned)(x1 - x0), (unsigned)(y1 - y0)} };
            vkCmdSetScissor(command, 0, 1, &scissor);
            vkCmdDrawIndexed(command, c->element_count, 1, c->first_index, 0, 0);
        }
    }
    vkCmdEndRenderPass(command);
    return 1;
}

void game_overlay_vk_destroy(game_overlay_vk *o)
{
    if (!o) return;
    game_overlay_vk_clear_targets(o);
    free_buffer(o, &o->vertices); free_buffer(o, &o->indices);
    if (o->pipeline) vkDestroyPipeline(o->device, o->pipeline, NULL);
    if (o->vertex_shader) vkDestroyShaderModule(o->device, o->vertex_shader, NULL);
    if (o->fragment_shader) vkDestroyShaderModule(o->device, o->fragment_shader, NULL);
    if (o->pipeline_layout) vkDestroyPipelineLayout(o->device, o->pipeline_layout, NULL);
    if (o->render_pass) vkDestroyRenderPass(o->device, o->render_pass, NULL);
    if (o->descriptor_pool) vkDestroyDescriptorPool(o->device, o->descriptor_pool, NULL);
    if (o->descriptor_layout) vkDestroyDescriptorSetLayout(o->device, o->descriptor_layout, NULL);
    if (o->sampler) vkDestroySampler(o->device, o->sampler, NULL);
    if (o->atlas_view) vkDestroyImageView(o->device, o->atlas_view, NULL);
    if (o->atlas) vkDestroyImage(o->device, o->atlas, NULL);
    if (o->atlas_memory) vkFreeMemory(o->device, o->atlas_memory, NULL);
    free(o);
}
