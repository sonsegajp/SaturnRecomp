/* Synthetic, game-independent checks of the actual presentation shaders.
 * Includes renderer internals only to verify native-buffer isolation and
 * indexed sprite codes; all pictures use the production Vulkan pipelines. */
#define SDL_MAIN_HANDLED
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <vulkan/vulkan.h>

/* Fault the real allocation calls used by interpolation initialization, not
 * a second implementation of that path. Disabled for ordinary rendering. */
static int fault_after=-1,fault_fired;
static int allocation_fails(void) {
    if(fault_after<0)return 0;
    if(fault_after--==0){fault_fired=1;return 1;}
    return 0;
}
static void *fault_malloc(size_t size){return allocation_fails()?NULL:malloc(size);}
static void *fault_calloc(size_t count,size_t size){return allocation_fails()?NULL:calloc(count,size);}
static VkResult VKAPI_CALL fault_create_buffer(VkDevice device,const VkBufferCreateInfo *info,
    const VkAllocationCallbacks *allocator,VkBuffer *buffer) {
    return allocation_fails()?VK_ERROR_OUT_OF_HOST_MEMORY:vkCreateBuffer(device,info,allocator,buffer);
}
static VkResult VKAPI_CALL fault_allocate_memory(VkDevice device,const VkMemoryAllocateInfo *info,
    const VkAllocationCallbacks *allocator,VkDeviceMemory *memory) {
    return allocation_fails()?VK_ERROR_OUT_OF_DEVICE_MEMORY:vkAllocateMemory(device,info,allocator,memory);
}
static VkResult VKAPI_CALL fault_bind_memory(VkDevice device,VkBuffer buffer,VkDeviceMemory memory,VkDeviceSize offset) {
    return allocation_fails()?VK_ERROR_OUT_OF_DEVICE_MEMORY:vkBindBufferMemory(device,buffer,memory,offset);
}
static VkResult VKAPI_CALL fault_map_memory(VkDevice device,VkDeviceMemory memory,VkDeviceSize offset,
    VkDeviceSize size,VkMemoryMapFlags flags,void **data) {
    return allocation_fails()?VK_ERROR_MEMORY_MAP_FAILED:vkMapMemory(device,memory,offset,size,flags,data);
}
#define malloc fault_malloc
#define calloc fault_calloc
#define vkCreateBuffer fault_create_buffer
#define vkAllocateMemory fault_allocate_memory
#define vkBindBufferMemory fault_bind_memory
#define vkMapMemory fault_map_memory
#include "../runner/src/vulkan_renderer.c"
#undef malloc
#undef calloc
#undef vkCreateBuffer
#undef vkAllocateMemory
#undef vkBindBufferMemory
#undef vkMapMemory

enum { W=320,H=224,PIXELS=W*H };
static saturn state,guest_before;
static uint32_t native[PIXELS],result[PIXELS*16],twice[PIXELS*4];
static uint32_t native_fb[FB_PIXELS*2],native_mesh[FB_PIXELS*2];
static uint32_t quality_framebuffer[FB_PIXELS];
static char error[512];
static unsigned checks;
#define REQUIRE(c,msg) do {checks++;if(!(c)){fprintf(stderr,"FAIL: %s (%s)\n",msg,error);return 0;}}while(0)

static int interpolation_allocation_failures(saturn_vk_renderer *r) {
    unsigned failures=0;
    for(int point=0;point<32;point++) {
        fault_after=point;fault_fired=0;
        int enabled=saturn_vk_interpolation_enable(r);
        fault_after=-1;
        if(!fault_fired) {
            REQUIRE(enabled,"initialization succeeds after the final injected allocation point");
            free_interpolation_resources(r);break;
        }
        failures++;
        REQUIRE(!enabled&&!r->interpolate,"an allocation failure never enables interpolation");
        REQUIRE(!r->history&&!r->blend&&!r->history_vram&&!r->picture_vram&&
            !r->rotation_history&&!r->rotation_previous&&!r->cram_history&&!r->cram_previous,
            "failed initialization releases every partial CPU allocation");
        REQUIRE(!r->saved_fb.buffer&&!r->saved_fb.memory&&!r->saved_fb.map&&
            !r->saved_mesh.buffer&&!r->saved_mesh.memory&&!r->saved_mesh.map,
            "failed initialization releases every partial Vulkan allocation");
        REQUIRE(saturn_vk_interpolation_enable(r),"retry after allocation failure succeeds");
        REQUIRE(r->history&&r->blend&&r->history_vram&&r->picture_vram&&
            r->rotation_history&&r->rotation_previous&&r->cram_history&&r->cram_previous&&
            r->saved_fb.buffer&&r->saved_fb.map&&r->saved_mesh.buffer&&r->saved_mesh.map,
            "successful retry has all resources needed by the first picture");
        free_interpolation_resources(r);
    }
    REQUIRE(failures==16,"all eight CPU and eight Vulkan creation/binding/mapping points were injected");
    printf("interpolation initialization: %u allocation failures recovered on retry\n",failures);
    return 1;
}

static int overlay_format_changes(saturn_vk_renderer *r) {
    static const unsigned char white[]={255,255,255,255};
    static const game_overlay_vertex vertices[]={
        {{0,0},{.5f,.5f},{240,60,30,255}},
        {{16,0},{.5f,.5f},{240,60,30,255}},
        {{0,16},{.5f,.5f},{240,60,30,255}}
    };
    static const uint16_t indices[]={0,1,2};
    static const game_overlay_draw_command draw_command={3,0,0,0,16,16};
    const game_overlay_draw_data draw={vertices,indices,&draw_command,sizeof vertices,sizeof indices,1,16,16};
    game_overlay_vk *overlay=game_overlay_vk_create(r->physical,r->device,r->queue,r->queue_family,
        VK_FORMAT_R8G8B8A8_UNORM,white,1,1,"runner/shaders",error,sizeof error);
    REQUIRE(overlay!=NULL,"create the real overlay pipeline for a synthetic atlas");
    const VkFormat formats[]={VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_R8G8B8A8_UNORM};
    for(unsigned stage=0;stage<4;stage++) {
        unsigned side=stage==3?32:16;
        VkImage image=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;
        VkImageCreateInfo ci={VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType=VK_IMAGE_TYPE_2D;ci.format=formats[stage];ci.extent=(VkExtent3D){side,side,1};
        ci.mipLevels=ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;
        ci.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        REQUIRE(vkCreateImage(r->device,&ci,NULL,&image)==VK_SUCCESS,"create a target in the new pixel format");
        VkMemoryRequirements requirements;vkGetImageMemoryRequirements(r->device,image,&requirements);
        uint32_t type=find_memory(r,requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        REQUIRE(type!=UINT32_MAX,"find overlay target memory");
        VkMemoryAllocateInfo allocation={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize=requirements.size;allocation.memoryTypeIndex=type;
        REQUIRE(vkAllocateMemory(r->device,&allocation,NULL,&memory)==VK_SUCCESS,"allocate overlay target memory");
        REQUIRE(vkBindImageMemory(r->device,image,memory,0)==VK_SUCCESS,"bind overlay target memory");
        REQUIRE(game_overlay_vk_set_format(overlay,formats[stage],error,sizeof error),"switch overlay renderpass and pipeline format");
        REQUIRE(game_overlay_vk_set_targets(overlay,&image,1,(VkExtent2D){side,side},error,sizeof error),"attach new format/extent targets");
        REQUIRE(!game_overlay_vk_set_format(overlay,VK_FORMAT_UNDEFINED,error,sizeof error),"invalid format leaves the working pipeline and targets intact");
        error[0]=0;
        vkbuf readback={0};
        REQUIRE(make_buffer(r,&readback,side*side*4,error,sizeof error),"allocate overlay pixel readback");
        REQUIRE(vkWaitForFences(r->device,1,&r->fence,VK_TRUE,UINT64_MAX)==VK_SUCCESS,"wait before reusing overlay draw buffers");
        REQUIRE(vkResetCommandBuffer(r->command,0)==VK_SUCCESS,"reset overlay test command buffer");
        VkCommandBufferBeginInfo begin={VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        REQUIRE(vkBeginCommandBuffer(r->command,&begin)==VK_SUCCESS,"begin overlay format test");
        image_barrier(r->command,image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue black={{0,0,0,1}};VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdClearColorImage(r->command,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&black,1,&range);
        REQUIRE(game_overlay_vk_record(overlay,r->command,0,&draw,error,sizeof error),"record an actual UI triangle after format change");
        image_barrier(r->command,image,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy={0};copy.imageSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        copy.imageExtent=(VkExtent3D){side,side,1};
        vkCmdCopyImageToBuffer(r->command,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback.buffer,1,&copy);
        REQUIRE(vkEndCommandBuffer(r->command)==VK_SUCCESS,"end overlay format test");
        REQUIRE(vkResetFences(r->device,1,&r->fence)==VK_SUCCESS,"reset overlay test fence");
        VkSubmitInfo submit={VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&r->command;
        REQUIRE(vkQueueSubmit(r->queue,1,&submit,r->fence)==VK_SUCCESS,"submit overlay format test");
        REQUIRE(vkWaitForFences(r->device,1,&r->fence,VK_TRUE,UINT64_MAX)==VK_SUCCESS,"read completed overlay pixels");
        const uint8_t *pixel=(const uint8_t *)readback.map+(side*2u+2u)*4u;
        unsigned red=formats[stage]==VK_FORMAT_B8G8R8A8_UNORM?2:0,blue=2-red;
        REQUIRE(pixel[red]==240&&pixel[1]==60&&pixel[blue]==30&&pixel[3]==255,
            "format changes and same-format resize preserve exact UI colour");
        game_overlay_vk_clear_targets(overlay);free_buffer(r,&readback);
        vkDestroyImage(r->device,image,NULL);vkFreeMemory(r->device,memory,NULL);
    }
    game_overlay_vk_destroy(overlay);
    puts("overlay lifecycle: RGBA/BGRA/RGBA, invalid-format recovery and resize preserve pixels");
    return 1;
}

static void put_word(unsigned address,unsigned value) {
    state.vdp1_vram[address]=(uint8_t)(value>>8);
    state.vdp1_vram[address+1]=(uint8_t)value;
}
static saturn_vk_vdp1_op quad(int x,int y,int width,int height) {
    saturn_vk_vdp1_op o={0};o.kind=SATURN_VK_VDP1_QUAD;o.target=1;
    o.xy[0]=o.xy[6]=x;o.xy[2]=o.xy[4]=x+width;
    o.xy[1]=o.xy[3]=y;o.xy[5]=o.xy[7]=y+height;
    o.sys_x1=511;o.sys_y1=255;o.flat=0xffff;o.flip=4u<<8;
    return o;
}
static int scene(saturn_vk_renderer *r) {
    saturn_vk_vdp1_op erase={0};erase.kind=SATURN_VK_VDP1_ERASE;erase.target=1;
    erase.xy[2]=W;erase.xy[3]=H-1;
    REQUIRE(queue_vdp1(r,&erase),"queue native clear");
    saturn_vk_vdp1_op solid=quad(15,20,60,55);
    solid.xy[3]=45;solid.xy[4]=60;solid.xy[5]=100;solid.xy[6]=5;
    REQUIRE(queue_vdp1(r,&solid),"queue diagonal geometry");
    for(unsigned y=0;y<8;y++)for(unsigned x=0;x<8;x++) {
        put_word(0x1000+(y*8+x)*2,x<4?0x801f:0xfc00);
        state.vdp1_vram[0x2000+(y*8+x)/2]=0x12;
        state.vdp1_vram[0x3000+(y*8+x)/2]=x<4?0x11:0x22;
    }
    put_word(0x4000+2,0x83e0);put_word(0x4000+4,0xfc00);
    saturn_vk_vdp1_op rgb=quad(100,20,80,80);
    rgb.textured=1;rgb.tw=rgb.th=8;rgb.chr=0x1000;rgb.pmod=5u<<3;rgb.flip=2u<<8;
    REQUIRE(queue_vdp1(r,&rgb),"queue RGB texture");
    saturn_vk_vdp1_op indexed=rgb;indexed.chr=0x2000;indexed.pmod=0;indexed.colr=0x4000;
    indexed.xy[0]=indexed.xy[6]=210;indexed.xy[2]=indexed.xy[4]=290;
    REQUIRE(queue_vdp1(r,&indexed),"queue indexed texture carrying priority codes");
    saturn_vk_vdp1_op clut=rgb;clut.chr=0x3000;clut.pmod=1u<<3;clut.colr=0x4000/8;
    clut.xy[1]=clut.xy[3]=130;clut.xy[5]=clut.xy[7]=210;
    REQUIRE(queue_vdp1(r,&clut),"queue RGB CLUT texture");
    state.fb_draw=0;state.frames=1;
    REQUIRE(saturn_vk_interpolation_begin(r,&state,W,H,error,sizeof error),"build canonical scene");
    REQUIRE(saturn_vk_readback(r,native,W,H,error,sizeof error),"read canonical scene");
    REQUIRE(r->history_valid,"scene permits geometry rerasterization");
    memcpy(native_fb,r->fb.map,sizeof native_fb);memcpy(native_mesh,r->mesh.map,sizeof native_mesh);
    guest_before=state;r->rotation_pair=0;
    return 1;
}
static int present(saturn_vk_renderer *r,saturn_runtime_settings *settings,uint32_t *pixels) {
    REQUIRE(saturn_vk_set_quality(r,settings,error,sizeof error),"apply quality options");
    REQUIRE(saturn_vk_interpolation_render(r,&state,1,W,H,error,sizeof error),"present quality picture");
    REQUIRE(saturn_vk_readback_presented(r,pixels,W*settings->internal_scale,H*settings->internal_scale,error,sizeof error),"read actual presented resolution");
    REQUIRE(!memcmp(native_fb,r->fb.map,sizeof native_fb),"quality leaves native framebuffer pair unchanged");
    REQUIRE(!memcmp(native_mesh,r->mesh.map,sizeof native_mesh),"quality leaves native mesh buffers unchanged");
    REQUIRE(!memcmp(&state,&guest_before,sizeof state),"quality leaves all guest state and RAM unchanged");
    return 1;
}
static unsigned changed_in(const uint32_t *a,const uint32_t *b,int x0,int y0,int x1,int y1) {
    unsigned n=0;for(int y=y0;y<y1;y++)for(int x=x0;x<x1;x++)n+=a[y*W+x]!=b[y*W+x];return n;
}
static int read_quality_framebuffer(saturn_vk_renderer *r) {
    vkbuf staging={0};
    REQUIRE(make_buffer(r,&staging,sizeof quality_framebuffer,error,sizeof error),"allocate indexed-code staging readback");
    REQUIRE(vkWaitForFences(r->device,1,&r->fence,VK_TRUE,UINT64_MAX)==VK_SUCCESS,"wait for quality output");
    REQUIRE(vkResetCommandBuffer(r->command,0)==VK_SUCCESS,"reset readback commands");
    VkCommandBufferBeginInfo begin={VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    REQUIRE(vkBeginCommandBuffer(r->command,&begin)==VK_SUCCESS,"begin indexed-code readback");
    VkBufferMemoryBarrier barrier={VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer=r->quality_fb.buffer;barrier.size=VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(r->command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,1,&barrier,0,NULL);
    VkBufferCopy copy={FB_PIXELS*sizeof(uint32_t),0,sizeof quality_framebuffer};
    vkCmdCopyBuffer(r->command,r->quality_fb.buffer,staging.buffer,1,&copy);
    REQUIRE(vkEndCommandBuffer(r->command)==VK_SUCCESS,"end indexed-code readback");
    REQUIRE(vkResetFences(r->device,1,&r->fence)==VK_SUCCESS,"reset readback fence");
    VkSubmitInfo submit={VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&r->command;
    REQUIRE(vkQueueSubmit(r->queue,1,&submit,r->fence)==VK_SUCCESS,"submit indexed-code readback");
    REQUIRE(vkWaitForFences(r->device,1,&r->fence,VK_TRUE,UINT64_MAX)==VK_SUCCESS,"finish indexed-code readback");
    memcpy(quality_framebuffer,staging.map,sizeof quality_framebuffer);free_buffer(r,&staging);
    return 1;
}
static int uniform_regions_unchanged(const uint32_t *filtered) {
    unsigned uniform=0;
    for(int y=8;y<H-8;y++)for(int x=8;x<W-8;x++) {
        uint32_t center=native[y*W+x];int flat=1;
        for(int j=-8;j<=8&&flat;j++)for(int i=-8;i<=8;i++)if(native[(y+j)*W+x+i]!=center){flat=0;break;}
        if(flat){uniform++;REQUIRE(filtered[y*W+x]==center,"uniform interior colour is preserved");}
    }
    REQUIRE(uniform>1000,"uniform-region check covers meaningful image area");return 1;
}
static int regression(saturn_vk_renderer *r) {
    saturn_runtime_settings settings;saturn_settings_defaults(&settings);settings.internal_scale=2;
    REQUIRE(saturn_vk_set_quality(r,&settings,error,sizeof error),"enable enhanced rendering without interpolation");
    REQUIRE(saturn_vk_presentation_active(r)&&!r->interpolate,"quality uses presentation path independently of interpolation");
    REQUIRE(scene(r),"create synthetic scene");
    REQUIRE(present(r,&settings,twice),"render at two times native resolution");
    unsigned subpixels=0,different=0;
    for(int y=10;y<115;y++)for(int x=0;x<85;x++) {
        uint32_t a=twice[(y*2)*(W*2)+x*2];
        subpixels+=a!=twice[(y*2)*(W*2)+x*2+1]||a!=twice[(y*2+1)*(W*2)+x*2]||a!=twice[(y*2+1)*(W*2)+x*2+1];
        for(int j=0;j<2;j++)for(int i=0;i<2;i++)different+=twice[(y*2+j)*(W*2)+x*2+i]!=native[y*W+x];
    }
    REQUIRE(subpixels>50&&different>50,"2x resolution rerasterizes diagonal geometry instead of enlarging native pixels");
    printf("2x geometry: %u subpixel blocks, %u pixels differ from nearest enlargement\n",subpixels,different);

    settings.internal_scale=1;settings.texture_filter=1;
    REQUIRE(present(r,&settings,result),"enable RGB texture filtering");
    unsigned rgb=changed_in(native,result,101,21,180,100),clut=changed_in(native,result,101,131,180,210);
    REQUIRE(rgb>100&&clut>100,"filter blends RGB texture and RGB CLUT samples");
    unsigned mixed=0;
    for(int y=25;y<95;y++)for(int x=105;x<175;x++) {
        uint32_t p=result[y*W+x];mixed+=((p>>16)&255)>0&&((p>>16)&255)<255&&(p&255)>0&&(p&255)<255;
    }
    REQUIRE(mixed>100,"smooth filtering produces intermediate RGB colours");
    REQUIRE(read_quality_framebuffer(r),"read indexed codes from device-local quality buffer");
    for(int y=21;y<100;y++)for(int x=211;x<290;x++) {
        unsigned offset=FB_PIXELS+(unsigned)y*512u+(unsigned)x;
        REQUIRE(quality_framebuffer[offset-FB_PIXELS]==native_fb[offset],"indexed pixels retain exact priority and palette codes");
    }
    REQUIRE(!changed_in(native,result,211,21,290,100),"indexed texture appearance stays native");
    printf("texture filtering: %u RGB pixels, %u RGB-CLUT pixels changed; indexed codes unchanged\n",rgb,clut);

    settings.texture_filter=0;settings.model_smoothing=1;
    REQUIRE(present(r,&settings,result),"resolve model smoothing at native output dimensions");
    REQUIRE(changed_in(native,result,0,10,85,115)>30,"model smoothing changes diagonal edge coverage");
    for(int y=0;y<H;y++)for(int x=0;x<W;x++)for(unsigned channel=0;channel<24;channel+=8) {
        unsigned total=0;
        for(int j=0;j<2;j++)for(int i=0;i<2;i++)total+=(twice[(y*2+j)*(W*2)+x*2+i]>>channel)&255u;
        int actual=(int)((result[y*W+x]>>channel)&255u),expected=(int)((total+2)/4);
        REQUIRE(abs(actual-expected)<=1,"model smoothing resolves four rerasterized samples");
    }
    REQUIRE(uniform_regions_unchanged(result),"SSAA preserves flat regions");

    settings.model_smoothing=0;settings.antialiasing=1;
    REQUIRE(present(r,&settings,result),"enable FXAA");
    REQUIRE(changed_in(native,result,0,10,85,115)>30,"FXAA modifies aliased geometry edges");
    REQUIRE(uniform_regions_unchanged(result),"FXAA preserves flat regions");

    settings.internal_scale=4;settings.antialiasing=0;
    REQUIRE(present(r,&settings,result),"grow resources to 4x output");
    settings.model_smoothing=1;settings.texture_filter=1;settings.antialiasing=1;
    REQUIRE(present(r,&settings,result),"combine 4x output, 8x sampling, filtering and FXAA");
    REQUIRE(quality_scale(r)==8&&r->quality_capacity>=8,"maximum combined quality allocates eight-times sampling");
    REQUIRE(result[(60*4)*(W*4)+40*4]==native[60*W+40],"maximum quality preserves a uniform polygon interior");
    settings.model_smoothing=0;settings.texture_filter=0;settings.antialiasing=0;

    /* A new source picture three fields later must remain at its current
     * geometry when quality is enabled but interpolation is disabled. */
    settings.internal_scale=2;
    REQUIRE(saturn_vk_set_quality(r,&settings,error,sizeof error),"retain quality only");
    saturn_vk_vdp1_op moved[8];unsigned count=r->history_count;
    REQUIRE(count<=8&&count>1,"retain source command history");
    memcpy(moved,r->history,count*sizeof *moved);
    for(unsigned i=0;i<count;i++) {
        if(moved[i].kind==SATURN_VK_VDP1_QUAD)for(unsigned k=0;k<8;k+=2)moved[i].xy[k]+=4;
        REQUIRE(queue_vdp1(r,&moved[i]),"queue a moved source picture");
    }
    state.frames+=3;
    REQUIRE(saturn_vk_interpolation_begin(r,&state,W,H,error,sizeof error),"build a held-span source pair");
    REQUIRE(saturn_vk_readback(r,native,W,H,error,sizeof error),"read current source endpoint");
    memcpy(native_fb,r->fb.map,sizeof native_fb);memcpy(native_mesh,r->mesh.map,sizeof native_mesh);guest_before=state;
    REQUIRE(r->geometry_span==3&&!r->matched,"disabled interpolation does not build a held-span motion blend");
    REQUIRE(present(r,&settings,twice),"quality-only presentation with an available motion pair");
    r->matched=0;r->rotation_pair=0;
    REQUIRE(present(r,&settings,result),"explicit current-geometry reference");
    REQUIRE(!memcmp(twice,result,PIXELS*4*sizeof *result),"disabled interpolation does not blend held-span geometry");
    settings.internal_scale=1;
    REQUIRE(present(r,&settings,result),"disable enhancement after resource growth");
    REQUIRE(!saturn_vk_presentation_active(r),"presentation enhancement is disabled");
    REQUIRE(!memcmp(native,result,sizeof native),"native output is restored exactly");
    return 1;
}
static int high_resolution_viewport(saturn_vk_renderer *r) {
    /* Real VDP2 composition doubles VDP1 coordinates in this mode. A
     * 352x224 erase therefore covers the complete 704x448 output. */
#ifdef _WIN32
    _putenv("SATURN_VK_VDP1_ONLY=");
#else
    unsetenv("SATURN_VK_VDP1_ONLY");
#endif
    saturn_runtime_settings settings;saturn_settings_defaults(&settings);settings.internal_scale=2;
    REQUIRE(saturn_vk_set_quality(r,&settings,error,sizeof error),"enable high-resolution compositor quality");
    state.vdp2_reg[0]=0x00c0;state.vdp2_reg[0xe0/2]=0x20;state.vdp2_reg[0xf0/2]=1;
    saturn_vk_vdp1_op erase={0};erase.kind=SATURN_VK_VDP1_ERASE;erase.target=1;
    erase.xy[2]=352;erase.xy[3]=223;
    saturn_vk_vdp1_op solid=quad(10,10,70,70);solid.flat=0xfc00;
    REQUIRE(queue_vdp1(r,&erase)&&queue_vdp1(r,&solid),"queue a complete VDP1 viewport clear");
    state.frames++;
    REQUIRE(saturn_vk_interpolation_begin(r,&state,704,448,error,sizeof error),"compose 704x448 source picture");
    REQUIRE(saturn_vk_readback(r,result,704,448,error,sizeof error),"read high-resolution native composite");
    REQUIRE((result[60*704+60]&0xffffffu)!=0,"real compositor displays the RGB polygon");
    REQUIRE(r->history_valid,"352x224 clear covers 704x448 high-resolution interlaced display");
    memcpy(native_fb,r->fb.map,sizeof native_fb);memcpy(native_mesh,r->mesh.map,sizeof native_mesh);guest_before=state;
    uint32_t *large=malloc(1408u*896u*sizeof *large);
    REQUIRE(large!=NULL,"allocate high-resolution readback");
    int ok=saturn_vk_interpolation_render(r,&state,1,704,448,error,sizeof error)&&
        saturn_vk_readback_presented(r,large,1408,896,error,sizeof error);
    free(large);REQUIRE(ok,"high-resolution viewport rerasterizes at 1408x896");
    REQUIRE(!memcmp(native_fb,r->fb.map,sizeof native_fb)&&!memcmp(native_mesh,r->mesh.map,sizeof native_mesh),"high-resolution quality preserves native buffers");
    REQUIRE(!memcmp(&state,&guest_before,sizeof state),"high-resolution quality preserves guest state");
    for(unsigned edge=0;edge<2;edge++) {
        erase.xy[2]=edge?352:351;erase.xy[3]=edge?222:223;
        REQUIRE(queue_vdp1(r,&erase)&&queue_vdp1(r,&solid),"queue an incomplete viewport clear");
        state.frames++;
        REQUIRE(saturn_vk_interpolation_begin(r,&state,704,448,error,sizeof error),"compose partial high-resolution viewport");
        REQUIRE(!r->history_valid,"one uncovered VDP1 row or column still rejects replay");
    }
    return 1;
}
int main(void) {
    SDL_SetMainReady();
#ifdef _WIN32
    _putenv("SATURN_VK_VDP1_ONLY=1");
#else
    setenv("SATURN_VK_VDP1_ONLY","1",1);
#endif
    if(SDL_Init(SDL_INIT_VIDEO)){fprintf(stderr,"SDL: %s\n",SDL_GetError());return 77;}
    SDL_Window *window=SDL_CreateWindow("Render quality regression",0,0,W,H,SDL_WINDOW_VULKAN|SDL_WINDOW_HIDDEN);
    if(!window){fprintf(stderr,"Vulkan window: %s\n",SDL_GetError());SDL_Quit();return 77;}
    saturn_vk_renderer *r=saturn_vk_create(window,&state,error,sizeof error);
    int ok=r&&interpolation_allocation_failures(r)&&overlay_format_changes(r)&&regression(r)&&high_resolution_viewport(r);
    if(!r)fprintf(stderr,"Vulkan renderer: %s\n",error);
    saturn_vk_destroy(r);SDL_DestroyWindow(window);SDL_Quit();
    if(ok)printf("render quality: %u checks passed (actual Vulkan)\n",checks);
    return ok?0:1;
}
