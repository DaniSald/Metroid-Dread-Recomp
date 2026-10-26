/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The on-screen controls, the game's menu (runtime/platform/ui.h) and the shader loading screen before the game
   (runtime/platform/prepare_screen.h) over a presented frame: the quads they give, blended onto the swapchain image
   in a pass of their own after the frame's blit. Nothing is recorded while none shows */
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/ui_spv.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/overlay.h"
#include "nx/runtime/platform/prepare_screen.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/vpad.h"
#include "snxvk_ui_overlay.h"

#define MAX_QUADS 4096
typedef struct {
    float x, y, u, v;
    u32 rgba;
} Vertex;
static SnxUiQuad g_quads[MAX_QUADS];
static VkDescriptorSetLayout g_dsl;
static VkPipelineLayout g_layout;
static VkPipeline g_pipe;
static VkFormat g_pipe_format;
static VkDescriptorPool g_pool;
static VkDescriptorSet g_sets[2]; /* the atlas blended (sprites), and at its nearest texel (the font, fills) */
static VkSampler g_samplers[2];
static VkImage g_atlas;
static VmaAllocation g_atlas_mem;
static VkImageView g_atlas_view;
static u32 g_atlas_version;
static float g_atlas_w, g_atlas_h;
static VkImage g_sc_img[VK_SC_MAX];
static VkImageView g_sc_view[VK_SC_MAX];
VkOverlayWantedFn g_vk_overlay_wanted;   /* SwitchRecomp: the runtime's in-game menu */
VkOverlayPrepareFn g_vk_overlay_prepare;
VkOverlayFrameFn g_vk_overlay_frame;

static int layout_make(void) {
    VkDescriptorSetLayoutBinding b = {.binding = 0,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                      .descriptorCount = 1,
                                      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
    VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                      .setLayoutCount = 1,
                                      .pSetLayouts = &g_dsl,
                                      .pushConstantRangeCount = 1,
                                      .pPushConstantRanges = &pc};
    VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
    VkDescriptorPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 2, .poolSizeCount = 1, .pPoolSizes = &size};
    VkDescriptorSetLayout layouts[2];
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorSetCount = 2};
    int i;
    if (g_layout) return 1;
    if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &dci, NULL, &g_dsl))) return 0;
    if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &lci, NULL, &g_layout))) return 0;
    if (!VK_OK(vkCreateDescriptorPool(g_vk.dev, &pci, NULL, &g_pool))) return 0;
    layouts[0] = layouts[1] = g_dsl;
    ai.descriptorPool = g_pool;
    ai.pSetLayouts = layouts;
    if (!VK_OK(vkAllocateDescriptorSets(g_vk.dev, &ai, g_sets))) return 0;
    for (i = 0; i < 2; ++i) {
        VkFilter f = i ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        VkSamplerCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                   .magFilter = f,
                                   .minFilter = f,
                                   .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                   .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                   .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                   .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
        if (!VK_OK(vkCreateSampler(g_vk.dev, &sci, NULL, &g_samplers[i]))) return 0;
    }
    return 1;
}

static int pipeline_make(VkFormat format) {
    VkShaderModule vs, fs;
    VkPipelineShaderStageCreateInfo st[2] = {{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                              .stage = VK_SHADER_STAGE_VERTEX_BIT,
                                              .pName = "main"},
                                             {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                              .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                                              .pName = "main"}};
    VkVertexInputBindingDescription vb = {0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription va[3] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32_SFLOAT, 8}, {2, 0, VK_FORMAT_R8G8B8A8_UNORM, 16}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                               .vertexBindingDescriptionCount = 1,
                                               .pVertexBindingDescriptions = &vb,
                                               .vertexAttributeDescriptionCount = 3,
                                               .pVertexAttributeDescriptions = va};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL,
                                                 .cullMode = VK_CULL_MODE_NONE,
                                                 .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    /* alpha premultiplied: the atlas is, and the shader multiplies the colour's in */
    VkPipelineColorBlendAttachmentState ba = {.blendEnable = VK_TRUE,
                                              .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
                                              .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                              .colorBlendOp = VK_BLEND_OP_ADD,
                                              .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                                              .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                              .alphaBlendOp = VK_BLEND_OP_ADD,
                                              .colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ba};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                        .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &format};
    VkGraphicsPipelineCreateInfo gci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                        .pNext = &ri,
                                        .stageCount = 2,
                                        .pStages = st,
                                        .pVertexInputState = &vi,
                                        .pInputAssemblyState = &ia,
                                        .pViewportState = &vp,
                                        .pRasterizationState = &rs,
                                        .pMultisampleState = &ms,
                                        .pColorBlendState = &cb,
                                        .pDynamicState = &ds,
                                        .layout = g_layout};
    int ok;
    if (g_pipe && g_pipe_format == format) return 1;
    if (g_pipe) vkDestroyPipeline(g_vk.dev, g_pipe, NULL); /* vk_finish ran when the swapchain changed format */
    g_pipe = VK_NULL_HANDLE;
    vs = vk_module_spv(snx_ui_vs_spv, sizeof snx_ui_vs_spv / 4);
    fs = vk_module_spv(snx_ui_fs_spv, sizeof snx_ui_fs_spv / 4);
    st[0].module = vs;
    st[1].module = fs;
    ok = vs && fs && VK_OK(vkCreateGraphicsPipelines(g_vk.dev, vk_pipeline_cache(), 1, &gci, NULL, &g_pipe));
    if (vs) vkDestroyShaderModule(g_vk.dev, vs, NULL);
    if (fs) vkDestroyShaderModule(g_vk.dev, fs, NULL);
    g_pipe_format = format;
    if (!ok) g_pipe = VK_NULL_HANDLE;
    return ok;
}

/* the atlas to the GPU, once each time the frontend gives one (under ui_lock) */
static void atlas_upload(VkCommandBuffer cb) {
    int w, h, i;
    u32 version;
    const u32* px = ui_atlas(&w, &h, &version);
    VkBuffer buf;
    VmaAllocation mem;
    VmaAllocationInfo info;
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = (VkDeviceSize)w * (VkDeviceSize)h * 4,
                              .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo bai = {.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                            VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                   .usage = VMA_MEMORY_USAGE_AUTO};
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {(u32)w, (u32)h, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo iai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                 .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                 .format = VK_FORMAT_R8G8B8A8_UNORM,
                                 .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageExtent = {(u32)w, (u32)h, 1}};
    if (!px || version == g_atlas_version) return;
    if (!VK_OK(vmaCreateBuffer(g_vk.vma, &bci, &bai, &buf, &mem, &info))) return;
    memcpy(info.pMappedData, px, (size_t)w * (size_t)h * 4);
    vmaFlushAllocation(g_vk.vma, mem, 0, VK_WHOLE_SIZE);
    vk_defer_buffer(buf, mem);
    if (g_atlas) vk_defer_image(g_atlas, g_atlas_mem, g_atlas_view, VK_NULL_HANDLE);
    g_atlas = VK_NULL_HANDLE;
    if (!VK_OK(vmaCreateImage(g_vk.vma, &ii, &iai, &g_atlas, &g_atlas_mem, NULL))) {
        g_atlas = VK_NULL_HANDLE;
        return;
    }
    vci.image = g_atlas;
    if (!VK_OK(vkCreateImageView(g_vk.dev, &vci, NULL, &g_atlas_view))) {
        vmaDestroyImage(g_vk.vma, g_atlas, g_atlas_mem);
        g_atlas = VK_NULL_HANDLE;
        return;
    }
    vk_barrier(cb, g_atlas, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    rec_CmdCopyBufferToImage(cb, buf, g_atlas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    vk_barrier(cb, g_atlas, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    for (i = 0; i < 2; ++i) {
        VkDescriptorImageInfo di = {g_samplers[i], g_atlas_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                   .dstSet = g_sets[i],
                                   .descriptorCount = 1,
                                   .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                   .pImageInfo = &di};
        vkUpdateDescriptorSets(g_vk.dev, 1, &wr, 0, NULL);
    }
    g_atlas_version = version;
    g_atlas_w = (float)w;
    g_atlas_h = (float)h;
    ui_atlas_done();
    LOG("vulkan: the on-screen controls' atlas is %dx%d\n", w, h);
}

/* the swapchain image's view, made once per image */
static VkImageView sc_view(VkImage sc) {
    u32 i;
    for (i = 0; i < VK_SC_MAX; ++i)
        if (g_sc_img[i] == sc) return g_sc_view[i];
    for (i = 0; i < VK_SC_MAX; ++i)
        if (!g_sc_img[i]) {
            VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                         .image = sc,
                                         .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                         .format = g_vk.sc_format,
                                         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            if (!VK_OK(vkCreateImageView(g_vk.dev, &vci, NULL, &g_sc_view[i]))) return VK_NULL_HANDLE;
            g_sc_img[i] = sc;
            return g_sc_view[i];
        }
    return VK_NULL_HANDLE;
}
void vk_ui_swapchain_gone(void) {
    u32 i;
    for (i = 0; i < VK_SC_MAX; ++i) {
        if (g_sc_view[i]) vkDestroyImageView(g_vk.dev, g_sc_view[i], NULL);
        g_sc_view[i] = VK_NULL_HANDLE;
        g_sc_img[i] = VK_NULL_HANDLE;
    }
}

static void vertex(Vertex* v, float x, float y, float u, float t, u32 rgba) {
    v->x = x;
    v->y = y;
    v->u = u;
    v->v = t;
    v->rgba = rgba;
    snx_le32_array(v, sizeof *v / 4); /* the device reads the words little-endian: the colour's low byte is R */
}
void vk_ui_draw(VkCommandBuffer cb, VkImage sc, const int safe[4]) {
    SnxUiList list = {g_quads, 0, MAX_QUADS};
    int sw = (int)g_vk.sc_extent.width, sh = (int)g_vk.sc_extent.height, n, i, run;
    VkImageView view;
    Vertex* v;
    VkBuffer buf;
    u64 off;
    struct {
        float w, h;
        u32 srgb, pad;
    } pc = {(float)sw, (float)sh, (u32)g_vk.sc_srgb, 0};
    if (!layout_make()) return;
    ui_lock();
    atlas_upload(cb);
    /* the screen before the game starts, under the menu, hides the on-screen controls: they aren't drawn under it,
       where they would take quads for nothing (nor do they take fingers then, sdl3.c's press_event) */
    if (prepare_screen_shown())
        prepare_screen_draw(&list, sw, sh, safe, overlay_display_percent());
    else {
        vpad_draw(&list, safe);
        vpad_showcase_draw(&list, safe, vpad_showcase_fps(g_vk_present, plat_ns()));
    }
    overlay_draw(&list, sw, sh);
    ui_unlock();
    n = list.n;
    if (!n || !g_atlas || !pipeline_make(g_vk.sc_format) || !(view = sc_view(sc))) return;
    v = (Vertex*)vk_stage((u64)n * 6 * sizeof(Vertex), 4, &buf, &off);
    if (!v) return;
    for (i = 0; i < n; ++i) {
        const SnxUiQuad* q = &g_quads[i];
        /* the texels' corners, clockwise from the top left, turned: a quarter turn shows the bottom left one there */
        float uu[4] = {q->u, q->u + q->uw, q->u + q->uw, q->u}, vv[4] = {q->v, q->v, q->v + q->vh, q->v + q->vh};
        float px[4] = {q->x, q->x + q->w, q->x + q->w, q->x}, py[4] = {q->y, q->y, q->y + q->h, q->y + q->h};
        float cu[4], cv[4];
        int k;
        for (k = 0; k < 4; ++k) {
            cu[k] = uu[(k + 4 - q->turn) % 4] / g_atlas_w;
            cv[k] = vv[(k + 4 - q->turn) % 4] / g_atlas_h;
        }
        vertex(&v[i * 6 + 0], px[0], py[0], cu[0], cv[0], q->rgba);
        vertex(&v[i * 6 + 1], px[1], py[1], cu[1], cv[1], q->rgba);
        vertex(&v[i * 6 + 2], px[2], py[2], cu[2], cv[2], q->rgba);
        vertex(&v[i * 6 + 3], px[0], py[0], cu[0], cv[0], q->rgba);
        vertex(&v[i * 6 + 4], px[2], py[2], cu[2], cv[2], q->rgba);
        vertex(&v[i * 6 + 5], px[3], py[3], cu[3], cv[3], q->rgba);
    }
    vk_barrier(cb, sc, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    {
        VkRenderingAttachmentInfo at = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                        .imageView = view,
                                        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                        .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                              .renderArea = {{0, 0}, g_vk.sc_extent},
                              .layerCount = 1,
                              .colorAttachmentCount = 1,
                              .pColorAttachments = &at};
        VkViewport vp = {0, 0, (float)sw, (float)sh, 0, 1};
        VkRect2D sci = {{0, 0}, g_vk.sc_extent};
        VkDeviceSize vo = off;
        rec_CmdBeginRendering(cb, &ri);
        vk_render_forget_state(); /* another pipeline and layout: a draw after it sets everything again */
        rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipe);
        rec_CmdSetViewport(cb, 0, 1, &vp);
        rec_CmdSetScissor(cb, 0, 1, &sci);
        rec_CmdPushConstants(cb, g_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
        rec_CmdBindVertexBuffers(cb, 0, 1, &buf, &vo);
        /* runs of quads that sample the same way, in their order */
        for (i = 0; i < n; i = run) {
            for (run = i + 1; run < n && g_quads[run].sharp == g_quads[i].sharp; ++run) continue;
            rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, 1,
                                    &g_sets[g_quads[i].sharp ? 1 : 0], 0, NULL);
            rec_CmdDraw(cb, (u32)(run - i) * 6, 1, (u32)i * 6, 0);
        }
        rec_CmdEndRendering(cb);
    }
    vk_barrier(cb, sc, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
}

/* SwitchRecomp: the runtime's in-game menu (Dear ImGui draw data), over the finished picture.
   The image comes in `from` and goes back to `to`; the menu's backend begins no rendering pass itself,
   so this opens one on the swapchain view and gives it the command buffer. */
static void overlay_layout(VkImageLayout l, VkPipelineStageFlags2* stage, VkAccessFlags2* access) {
    switch (l) {
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT;
        *access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        break;
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
        *stage = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
        *access = 0;
        break;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        *access = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        break;
    default:
        *stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        *access = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        break;
    }
}
void vk_ui_overlay(VkCommandBuffer cb, VkImage sc, u32 w, u32 h, VkImageLayout from, VkImageLayout to) {
    VkImageView view;
    VkPipelineStageFlags2 ss, ds;
    VkAccessFlags2 sa, da;
    if (!g_vk_overlay_prepare || !g_vk_overlay_frame || !g_vk_overlay_wanted || !g_vk_overlay_wanted() || !w || !h) return;
    if (!g_vk_overlay_prepare(cb, w, h)) return;
    view = sc_view(sc);
    if (!view) return;
    overlay_layout(from, &ss, &sa);
    overlay_layout(to, &ds, &da);
    vk_barrier(cb, sc, VK_IMAGE_ASPECT_COLOR_BIT, ss, sa, from,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    {
        VkRenderingAttachmentInfo at = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                        .imageView = view,
                                        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                        .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                              .renderArea = {{0, 0}, {w, h}},
                              .layerCount = 1,
                              .colorAttachmentCount = 1,
                              .pColorAttachments = &at};
        VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
        VkRect2D sci = {{0, 0}, {w, h}};
        rec_CmdBeginRendering(cb, &ri);
        rec_CmdSetViewport(cb, 0, 1, &vp);
        rec_CmdSetScissor(cb, 0, 1, &sci);
        g_vk_overlay_frame(cb);
        rec_CmdEndRendering(cb);
    }
    vk_barrier(cb, sc, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, ds, da, to);
    vk_render_forget_state(); /* the menu bound its own pipeline and descriptors */
}

void vk_ui_triangles_prepare(void* cb_) {
    VkCommandBuffer cb = (VkCommandBuffer)cb_;
    if (!layout_make()) return;
    ui_lock();
    atlas_upload(cb);
    ui_unlock();
    pipeline_make(g_vk.sc_format);
}
void vk_ui_triangles(void* cb_, const SnxUiTriVertex* v, int nv, const u32* idx, int ni,
                     const SnxUiTriRange* r, int nr) {
    VkCommandBuffer cb = (VkCommandBuffer)cb_;
    VkBuffer vb, ib;
    u64 voff, ioff;
    Vertex* dst;
    u32* idst;
    int i;
    u32 w = g_vk.sc_extent.width, h = g_vk.sc_extent.height;
    struct { float w, h; u32 srgb, pad; } pc = {(float)w, (float)h, (u32)g_vk.sc_srgb, 0};
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    if (!nv || !ni || !nr || !g_pipe || !g_atlas) return;
    dst = (Vertex*)vk_stage((u64)nv * sizeof(Vertex), 4, &vb, &voff);
    idst = (u32*)vk_stage((u64)ni * 4, 4, &ib, &ioff);
    if (!dst || !idst) return;
    for (i = 0; i < nv; ++i) {
        dst[i].x = v[i].x; dst[i].y = v[i].y;
        dst[i].u = v[i].u; dst[i].v = v[i].v;
        dst[i].rgba = v[i].rgba;
    }
    memcpy(idst, idx, (size_t)ni * 4);
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipe);
    rec_CmdSetViewport(cb, 0, 1, &vp);
    rec_CmdPushConstants(cb, g_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    rec_CmdBindVertexBuffers(cb, 0, 1, &vb, &voff);
    rec_CmdBindIndexBuffer(cb, ib, ioff, VK_INDEX_TYPE_UINT32);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, 1, &g_sets[0], 0, NULL);
    for (i = 0; i < nr; ++i) {
        /* one scissor per clipped ImGui command; a pixel of slack keeps a glyph's first
           column from being cut when the clip lands on a fractional coordinate */
        float x0 = r[i].clip[0] - 1, y0 = r[i].clip[1] - 1, x1 = r[i].clip[2] + 1, y1 = r[i].clip[3] + 1;
        VkRect2D sc;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > (float)w) x1 = (float)w;
        if (y1 > (float)h) y1 = (float)h;
        if (x1 <= x0 || y1 <= y0 || !r[i].count) continue;
        sc.offset.x = (i32)x0; sc.offset.y = (i32)y0;
        sc.extent.width = (u32)(x1 - x0); sc.extent.height = (u32)(y1 - y0);
        rec_CmdSetScissor(cb, 0, 1, &sc);
        rec_CmdDrawIndexed(cb, r[i].count, 1, r[i].first, (i32)r[i].base, 0);
    }
}
