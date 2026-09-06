/* Presentation resources; included after the renderer's Vulkan helpers. */
static int quality_make_buffer(saturn_vk_renderer *r,vkbuf *buffer,VkDeviceSize size,
    char *error,size_t error_size) {
    /* These persistent supersampled pixels never need CPU access. Keeping
     * them on the GPU avoids repeated PCIe reads during shader blending. */
    VkBufferCreateInfo ci={VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};ci.size=size;
    ci.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(r->device,&ci,NULL,&buffer->buffer));
    VkMemoryRequirements requirements;vkGetBufferMemoryRequirements(r->device,buffer->buffer,&requirements);
    uint32_t type=find_memory(r,requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if(type==UINT32_MAX)type=find_memory(r,requirements.memoryTypeBits,0);
    if(type==UINT32_MAX){set_error(error,error_size,"No memory for enhanced framebuffers");goto fail;}
    VkMemoryAllocateInfo ai={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize=requirements.size;ai.memoryTypeIndex=type;
    VK_CHECK(vkAllocateMemory(r->device,&ai,NULL,&buffer->memory));
    VK_CHECK(vkBindBufferMemory(r->device,buffer->buffer,buffer->memory,0));
    buffer->size=size;return 1;
fail:return 0;
}

static void quality_free_image(saturn_vk_renderer *r,VkImage image,VkImageView view,VkDeviceMemory memory) {
    if(view)vkDestroyImageView(r->device,view,NULL);
    if(image)vkDestroyImage(r->device,image,NULL);
    if(memory)vkFreeMemory(r->device,memory,NULL);
}

static int quality_make_image(saturn_vk_renderer *r,unsigned scale,VkImage *image,
    VkImageView *view,VkDeviceMemory *memory,char *error,size_t error_size) {
    VkImageCreateInfo ci={VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType=VK_IMAGE_TYPE_2D;ci.format=VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent=(VkExtent3D){704u*scale,512u*scale,1};ci.mipLevels=ci.arrayLayers=1;
    ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;
    ci.usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VK_CHECK(vkCreateImage(r->device,&ci,NULL,image));
    VkMemoryRequirements req;vkGetImageMemoryRequirements(r->device,*image,&req);
    uint32_t type=find_memory(r,req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if(type==UINT32_MAX)type=find_memory(r,req.memoryTypeBits,0);
    if(type==UINT32_MAX){set_error(error,error_size,"No memory for enhanced rendering");goto fail;}
    VkMemoryAllocateInfo ai={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize=req.size;ai.memoryTypeIndex=type;
    VK_CHECK(vkAllocateMemory(r->device,&ai,NULL,memory));
    VK_CHECK(vkBindImageMemory(r->device,*image,*memory,0));
    VkImageViewCreateInfo vi={VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image=*image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=ci.format;
    vi.subresourceRange=(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VK_CHECK(vkCreateImageView(r->device,&vi,NULL,view));
    return 1;
fail:return 0;
}

static void quality_write_descriptor(saturn_vk_renderer *r,VkDescriptorSet set,int native_fb) {
    vkbuf *buffers[]={&r->v1ram,&r->v2ram,&r->cram,&r->params,&r->ops,
        native_fb?&r->fb:&r->quality_fb,native_fb?&r->mesh:&r->quality_mesh,
        &r->tile_headers,&r->tile_refs};
    const unsigned bindings[]={0,1,2,3,4,5,6,8,9};
    VkDescriptorBufferInfo bi[9];VkWriteDescriptorSet writes[10];memset(writes,0,sizeof writes);
    for(unsigned i=0;i<9;i++) {
        bi[i]=(VkDescriptorBufferInfo){buffers[i]->buffer,0,buffers[i]->size};
        writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[i].dstSet=set;
        writes[i].dstBinding=bindings[i];writes[i].descriptorCount=1;
        writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[i].pBufferInfo=&bi[i];
    }
    VkDescriptorImageInfo ii={VK_NULL_HANDLE,r->quality_view,VK_IMAGE_LAYOUT_GENERAL};
    writes[9].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[9].dstSet=set;
    writes[9].dstBinding=7;writes[9].descriptorCount=1;
    writes[9].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;writes[9].pImageInfo=&ii;
    vkUpdateDescriptorSets(r->device,10,writes,0,NULL);
}

static int quality_resources(saturn_vk_renderer *r,unsigned scale,char *error,size_t error_size) {
    if(r->quality_capacity>=scale)return 1;
    vkbuf fb={0},mesh={0};VkImage image=VK_NULL_HANDLE,resolved=VK_NULL_HANDLE;
    VkImageView view=VK_NULL_HANDLE,resolved_view=VK_NULL_HANDLE;
    VkDeviceMemory memory=VK_NULL_HANDLE,resolved_memory=VK_NULL_HANDLE;
    VkDeviceSize bytes=(VkDeviceSize)FB_PIXELS*2u*4u*scale*scale;
    if(!quality_make_buffer(r,&fb,bytes,error,error_size)||!quality_make_buffer(r,&mesh,bytes,error,error_size)||
       !quality_make_image(r,scale,&image,&view,&memory,error,error_size)||
       !quality_make_image(r,scale,&resolved,&resolved_view,&resolved_memory,error,error_size))goto fail;
    if(!r->post_desc_layout) {
        VkDescriptorSetLayoutBinding bindings[2]={{0},{0}};
        for(unsigned i=0;i<2;i++){bindings[i].binding=i;bindings[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;bindings[i].descriptorCount=1;bindings[i].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;}
        VkDescriptorSetLayoutCreateInfo di={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};di.bindingCount=2;di.pBindings=bindings;
        VK_CHECK(vkCreateDescriptorSetLayout(r->device,&di,NULL,&r->post_desc_layout));
    }
    if(!r->post_layout) {
        VkPushConstantRange range={VK_SHADER_STAGE_COMPUTE_BIT,0,5u*sizeof(int32_t)};
        VkPipelineLayoutCreateInfo pi={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pi.setLayoutCount=1;pi.pSetLayouts=&r->post_desc_layout;pi.pushConstantRangeCount=1;pi.pPushConstantRanges=&range;
        VK_CHECK(vkCreatePipelineLayout(r->device,&pi,NULL,&r->post_layout));
    }
    if(!r->post_pipeline&&!create_compute(r,"postprocess.comp.spv",r->post_layout,&r->post_pipeline,error,error_size))goto fail;
    if(!r->post_desc_pool) {
        VkDescriptorPoolSize size={VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,2};
        VkDescriptorPoolCreateInfo pi={VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pi.maxSets=1;pi.poolSizeCount=1;pi.pPoolSizes=&size;
        VK_CHECK(vkCreateDescriptorPool(r->device,&pi,NULL,&r->post_desc_pool));
    }
    if(!r->post_desc) {
        VkDescriptorSetAllocateInfo ai={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=r->post_desc_pool;ai.descriptorSetCount=1;ai.pSetLayouts=&r->post_desc_layout;
        VK_CHECK(vkAllocateDescriptorSets(r->device,&ai,&r->post_desc));
    }
    if(!r->quality_desc) {
        VkDescriptorSetLayout layouts[2]={r->desc_layout,r->desc_layout};VkDescriptorSet sets[2];
        VkDescriptorSetAllocateInfo ai={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=r->desc_pool;ai.descriptorSetCount=2;ai.pSetLayouts=layouts;
        VK_CHECK(vkAllocateDescriptorSets(r->device,&ai,sets));r->quality_desc=sets[0];r->quality_hold_desc=sets[1];
    }
    VK_CHECK(vkDeviceWaitIdle(r->device));
    free_buffer(r,&r->quality_fb);free_buffer(r,&r->quality_mesh);
    quality_free_image(r,r->quality_output,r->quality_view,r->quality_memory);
    quality_free_image(r,r->quality_resolved,r->resolved_view,r->resolved_memory);
    r->quality_fb=fb;r->quality_mesh=mesh;r->quality_output=image;r->quality_view=view;r->quality_memory=memory;
    r->quality_resolved=resolved;r->resolved_view=resolved_view;r->resolved_memory=resolved_memory;
    r->quality_capacity=scale;r->quality_ready=r->resolved_ready=0;
    quality_write_descriptor(r,r->quality_desc,0);quality_write_descriptor(r,r->quality_hold_desc,1);
    VkDescriptorImageInfo images[2]={{VK_NULL_HANDLE,view,VK_IMAGE_LAYOUT_GENERAL},{VK_NULL_HANDLE,resolved_view,VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[2];memset(writes,0,sizeof writes);
    for(unsigned i=0;i<2;i++){writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[i].dstSet=r->post_desc;writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;writes[i].pImageInfo=&images[i];}
    vkUpdateDescriptorSets(r->device,2,writes,0,NULL);return 1;
fail:
    free_buffer(r,&fb);free_buffer(r,&mesh);
    quality_free_image(r,image,view,memory);quality_free_image(r,resolved,resolved_view,resolved_memory);
    return 0;
}

int saturn_vk_set_quality(saturn_vk_renderer *r,const saturn_runtime_settings *settings,char *error,size_t size) {
    if(!r||!settings)return 0;
    unsigned scale=(unsigned)settings->internal_scale*(settings->model_smoothing?2u:1u);
    if(settings->internal_scale<1||settings->internal_scale>4||scale>8u){set_error(error,size,"Invalid internal resolution");return 0;}
    int active=settings->internal_scale>1||settings->texture_filter||settings->antialiasing||settings->model_smoothing;
    if(active) {
        if(!quality_resources(r,scale,error,size))return 0;
        if(!r->history || !saturn_vk_presentation_active(r)) {
            int interpolating=r->interpolate;
            if(!saturn_vk_interpolation_enable(r)){set_error(error,size,"Presentation history allocation failed");return 0;}
            r->interpolate=interpolating;
        }
    }
    r->quality=*settings;return 1;
}

int saturn_vk_presentation_active(const saturn_vk_renderer *r){return r&&(r->interpolate||quality_active(r));}
void saturn_vk_interpolation_stats(const saturn_vk_renderer *r,unsigned *matched,unsigned *span) {
    if(matched)*matched=r?r->matched:0;if(span)*span=r?r->geometry_span:1;
}

int saturn_vk_attach_overlay(saturn_vk_renderer *r,game_overlay *overlay,char *error,size_t size) {
    if(!r||!overlay)return 0;
    int width,height;const unsigned char *atlas=game_overlay_font_atlas(overlay,&width,&height);
    if(vkDeviceWaitIdle(r->device)!=VK_SUCCESS){set_error(error,size,"Overlay synchronization failed");return 0;}
    char directory[1024];char *base=SDL_GetBasePath();
    snprintf(directory,sizeof directory,"%sshaders",base?base:"");if(base)SDL_free(base);
    char probe[1100];snprintf(probe,sizeof probe,"%s/game_overlay.vert.spv",directory);
    FILE *file=fopen(probe,"rb");
    if(file)fclose(file);else snprintf(directory,sizeof directory,"runner/shaders");
    r->overlay=game_overlay_vk_create(r->physical,r->device,r->queue,r->queue_family,r->swap_format,atlas,width,height,directory,error,size);
    if(!r->overlay)return 0;
    return game_overlay_vk_set_targets(r->overlay,r->swap_images,r->swap_count,r->swap_extent,error,size);
}
void saturn_vk_overlay_draw(saturn_vk_renderer *r,const game_overlay_draw_data *draw){if(r)r->overlay_draw=draw;}
