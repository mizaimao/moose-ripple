// Based on virglrenderer/src/virgl_hw.h
#pragma once

// Resource bind flags for kernel allocations
#define VIRGL_BIND_RENDER_TARGET            (1 << 1)
#define VIRGL_BIND_SAMPLER_VIEW             (1 << 3)
#define VIRGL_BIND_DISPLAY_TARGET           (1 << 7)
#define VIRGL_BIND_DISPLAY_TARGET           (1 << 7)
#define VIRGL_BIND_SCANOUT                  (1 << 18)

// Blt command specification for kernel BLTs
#define VIRGL_CMD0(cmd, obj, len)           ((cmd) | ((obj) << 8) | ((len) << 16))
#define VIRGL_CCMD_RESOURCE_COPY_REGION     17
#define VIRGL_CMD_RESOURCE_COPY_REGION_SIZE 13

// Flags for the driver about resource behaviour
#define VIRGL_RESOURCE_Y_0_TOP              (1 << 0)
#define VIRGL_RESOURCE_FLAG_MAP_PERSISTENT  (1 << 1)
#define VIRGL_RESOURCE_FLAG_MAP_COHERENT    (1 << 2)

// What the driver's own drawing needs (viogpu_gamma.cpp): commands, objects, formats, binds.
// Numbers as in virglrenderer's virgl_protocol.h and virgl_hw.h.
#define VIRGL_CCMD_CREATE_OBJECT            1
#define VIRGL_CCMD_BIND_OBJECT              2
#define VIRGL_CCMD_DESTROY_OBJECT           3
#define VIRGL_CCMD_SET_VIEWPORT_STATE       4
#define VIRGL_CCMD_SET_FRAMEBUFFER_STATE    5
#define VIRGL_CCMD_SET_VERTEX_BUFFERS       6
#define VIRGL_CCMD_DRAW_VBO                 8
#define VIRGL_CCMD_RESOURCE_INLINE_WRITE    9
#define VIRGL_CCMD_SET_SAMPLER_VIEWS        10
#define VIRGL_CCMD_BIND_SAMPLER_STATES      18
#define VIRGL_CCMD_BIND_SHADER              31

#define VIRGL_OBJECT_BLEND                  1
#define VIRGL_OBJECT_RASTERIZER             2
#define VIRGL_OBJECT_DSA                    3
#define VIRGL_OBJECT_SHADER                 4
#define VIRGL_OBJECT_VERTEX_ELEMENTS        5
#define VIRGL_OBJECT_SAMPLER_VIEW           6
#define VIRGL_OBJECT_SAMPLER_STATE          7
#define VIRGL_OBJECT_SURFACE                8

#define VIRGL_FORMAT_R32G32_FLOAT           29
#define VIRGL_FORMAT_R8_UNORM               64
#define VIRGL_FORMAT_R8G8B8A8_UNORM         67

#define VIRGL_BIND_VERTEX_BUFFER            (1 << 4)

#define VIRGL_TARGET_BUFFER                 0
#define VIRGL_TARGET_TEXTURE_2D             2
#define VIRGL_SHADER_VERTEX                 0
#define VIRGL_SHADER_FRAGMENT               1
#define VIRGL_PRIM_TRIANGLE_STRIP           5
