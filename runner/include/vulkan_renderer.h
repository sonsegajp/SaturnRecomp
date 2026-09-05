#ifndef SATURN_VULKAN_RENDERER_H
#define SATURN_VULKAN_RENDERER_H

#include <stdint.h>
#include <stddef.h>

typedef struct SDL_Window SDL_Window;
typedef struct saturn saturn;
typedef struct saturn_vk_renderer saturn_vk_renderer;

/* Resolved VDP1 operation. Command walking, clipping-register updates and
 * draw-end timing remain emulated by vdp1.c; Vulkan owns rasterization and
 * framebuffer colour calculation when this stream is bound. */
enum {
    SATURN_VK_VDP1_QUAD  = 1,
    SATURN_VK_VDP1_LINE  = 2,
    SATURN_VK_VDP1_ERASE = 3,
    /* Guest SH-2/SCU writes to the memory-mapped VDP1 framebuffer. These
     * must stay ordered against draw and erase operations; otherwise games
     * which upload cutscene cells directly leave Vulkan's persistent copy
     * frozen on the first image. */
    SATURN_VK_VDP1_FB_WRITE = 4
};

typedef struct saturn_vk_vdp1_op {
    uint32_t kind;
    uint32_t target;
    int32_t  xy[8];
    uint32_t chr;
    uint32_t tw, th;
    uint32_t colr, pmod, grda;
    uint32_t flat, textured, flip;
    int32_t  sys_x1, sys_y1;
    int32_t  usr_x0, usr_y0, usr_x1, usr_y1;
} saturn_vk_vdp1_op;

typedef struct saturn_vdp1_gpu_sink {
    void *userdata;
    int  (*enqueue)(void *userdata, const saturn_vk_vdp1_op *op);
    void (*reset)(void *userdata);
} saturn_vdp1_gpu_sink;

/* Implemented by vdp1.c so non-window tests do not link against Vulkan. */
void vdp1_gpu_bind(saturn *s, const saturn_vdp1_gpu_sink *sink);
int  vdp1_gpu_is_bound(const saturn *s);
void vdp1_gpu_fb_write(saturn *s, unsigned target, uint32_t byte_offset,
                       unsigned size, uint32_t value);

/* Complete Vulkan renderer: VDP1 compute raster, VDP2 compute composition,
 * then swapchain transfer/presentation. */
saturn_vk_renderer *saturn_vk_create(SDL_Window *window, saturn *s,
                                     char *error, size_t error_size);
int  saturn_vk_render(saturn_vk_renderer *r, saturn *s, int width, int height,
                      char *error, size_t error_size);
int  saturn_vk_present(saturn_vk_renderer *r, char *error, size_t error_size);
/* Diagnostic readback of the actual compute output, in ARGB8888. */
int saturn_vk_readback(saturn_vk_renderer *r, uint32_t *pixels, int width, int height,
                       char *error, size_t error_size);
int saturn_vk_replay_geometry(saturn_vk_renderer *r, saturn *s,
    const saturn_vk_vdp1_op *ops,unsigned count,int w,int h,char *error,size_t error_size);
int saturn_vk_interpolation_enable(saturn_vk_renderer *r);
void saturn_vk_interpolation_disable(saturn_vk_renderer *r);
int saturn_vk_interpolation_begin(saturn_vk_renderer *r,saturn *s,int w,int h,char *error,size_t size);
int saturn_vk_interpolation_render(saturn_vk_renderer *r,saturn *s,float alpha,int w,int h,char *error,size_t size);
void saturn_vk_destroy(saturn_vk_renderer *r);
const char *saturn_vk_device_name(const saturn_vk_renderer *r);

#endif
