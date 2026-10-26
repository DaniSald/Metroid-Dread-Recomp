/* SwitchRecomp: the in-game menu's triangles for ScarletNX's Vulkan UI pipeline.
   Dear ImGui builds the menu with no renderer backend; ov.cpp hands the draw lists here
   as one vertex/index batch and a range per clipped draw command. This keeps the menu
   on the same pipeline and font atlas as the rest of the runtime's Vulkan UI. */
#ifndef SWR_SNXVK_UI_OVERLAY_H
#define SWR_SNXVK_UI_OVERLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float x, y, u, v;     /* position in the picture's pixels, normalised atlas UV */
    uint32_t rgba;        /* ImGui's R G B A bytes, straight (the shader applies alpha) */
} SnxUiTriVertex;
typedef struct {
    uint32_t first, count, base;   /* index offset, indices, vertex offset */
    float clip[4];                 /* x0, y0, x1, y1 in the picture's pixels */
} SnxUiTriRange;

/* make the UI pipeline and upload the font atlas (outside a rendering pass) */
void vk_ui_triangles_prepare(void* cb);
/* draw the batch inside the dynamic rendering pass vk_ui_overlay opened */
void vk_ui_triangles(void* cb, const SnxUiTriVertex* v, int nv, const uint32_t* idx, int ni,
                     const SnxUiTriRange* r, int nr);

#ifdef __cplusplus
}
#endif
#endif
