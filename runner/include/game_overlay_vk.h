#ifndef SATURN_GAME_OVERLAY_VK_H
#define SATURN_GAME_OVERLAY_VK_H

#include <vulkan/vulkan.h>
#include "game_overlay.h"

typedef struct game_overlay_vk game_overlay_vk;
/* Device/queue/images are borrowed. Call with the presentation fence signaled. */
game_overlay_vk *game_overlay_vk_create(VkPhysicalDevice physical, VkDevice device,
    VkQueue queue, unsigned queue_family, VkFormat target_format,
    const unsigned char *atlas, int atlas_width, int atlas_height,
    const char *shader_directory, char *error, size_t error_size);
int game_overlay_vk_set_targets(game_overlay_vk *overlay, const VkImage *images,
    unsigned image_count, VkExtent2D extent, char *error, size_t error_size);
/* Rebuilds format-dependent resources transactionally. On success, old
 * targets are cleared; the caller must attach images in the new format. */
int game_overlay_vk_set_format(game_overlay_vk *overlay, VkFormat format,
    char *error, size_t error_size);
void game_overlay_vk_clear_targets(game_overlay_vk *overlay);
/* Target enters in TRANSFER_DST_OPTIMAL; leaves in COLOR_ATTACHMENT_OPTIMAL.
 * Caller performs the final PRESENT transition and submits the existing command. */
int game_overlay_vk_record(game_overlay_vk *overlay, VkCommandBuffer command,
    unsigned image_index, const game_overlay_draw_data *draw,
    char *error, size_t error_size);
void game_overlay_vk_destroy(game_overlay_vk *overlay);

#endif
