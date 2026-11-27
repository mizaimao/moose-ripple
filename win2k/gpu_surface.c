/* The miniport serializes this bounded resource table with the GPU probe and
 * mode changes. Applications reach it through the display DLL's DirectDraw
 * callbacks. Every byte returned by READ belongs to the requested surface. */
#include <miniport.h>
#include <devioctl.h>
#include <winerror.h>
#include <ntddvdeo.h>
#include <video.h>
#include "gpu_transport.h"
#include "gpu_memory.h"
#include <virgl_hw.h>

static VP_STATUS query(RB2K_GPU *gpu, PVIDEO_REQUEST_PACKET packet)
{
    RB2K_DD_CAPS *caps = packet->OutputBuffer;
    RB2K_GPU_RESULT result;
    VP_STATUS status, cleanup;
    if (packet->InputBufferLength || packet->OutputBufferLength < sizeof(*caps))
        return ERROR_INVALID_PARAMETER;
    if (!gpu->ready && !gpu->tested) {
        status = rb_gpu_start(gpu, &result);
        cleanup = rb_gpu_stop(gpu);
        if (status) return status;
        if (cleanup) return cleanup;
        gpu->tested = TRUE;
    }
    caps->maxBytes = RB2K_DD_MAX_BYTES;
    caps->slots = RB2K_DD_SLOTS;
    caps->hostFills = gpu->hostFills;
    caps->readbacks = gpu->readbacks;
    caps->uploads = gpu->uploads;
    caps->hostCopies = gpu->hostCopies;
    caps->presents = gpu->presents;
    caps->presentReadbacks = gpu->presentReadbacks;
    caps->presentBytes = gpu->presentBytes;
    /* The features switched on for this adapter. They use 32-bit pixels, so
     * the display driver offers them only in its 32-bit modes: during a mode
     * change it may ask before the miniport has set the new mode. */
    caps->gpuSurfaces = 0;
    if (gpu->ddGpuSurfaces) {
        caps->gpuSurfaces = RB2K_DD_HOST_SURFACES;
        if (gpu->plainSurfaces) caps->gpuSurfaces |= RB2K_DD_PLAIN_SURFACES;
        if (gpu->nativeD3D7)
            caps->gpuSurfaces |= RB2K_DD_NATIVE_D3D7 | RB2K_DD_Z16_STORAGE | RB2K_DD_Z16_RENDER |
                                 (gpu->d3d8 ? RB2K_DD_D3D8 : 0) | (gpu->d3d8 && gpu->vertexShaders ? RB2K_DD_VERTEX_SHADERS : 0) |
                                 (gpu->d3d8 && gpu->vertexShaders && gpu->d3d9Ddi ? RB2K_DD_D3D9_DDI : 0) |
                                 (gpu->d3d8 && gpu->vertexShaders && gpu->hostBuffers ? RB2K_DD_HOST_BUFFERS : 0) |
                                 RB2K_DD_Z24S8;
    }
    packet->StatusBlock->Information = sizeof(*caps);
    return NO_ERROR;
}

static VP_STATUS create(RB2K_GPU *gpu, PVIDEO_REQUEST_PACKET packet)
{
    static const char name[] = "rb2k-ddraw";
    RB2K_DD_CREATE *size = packet->InputBuffer;
    RB2K_GPU_RESULT result;
    RB2K_IMAGE *image;
    VP_STATUS status;
    ULONG slot;
    BOOLEAN stencil8 = packet->IoControlCode == IOCTL_RB2K_DD_CREATE_Z24S8;
    BOOLEAN depth16 = packet->IoControlCode == IOCTL_RB2K_DD_CREATE_Z16 || stencil8;
    if (packet->InputBufferLength != sizeof(*size) || packet->OutputBufferLength < sizeof(ULONG))
        return ERROR_INVALID_PARAMETER;
    if (gpu->primary.bitsPerPixel != 32 || (depth16 && (!gpu->nativeD3D7 || !gpu->ddGpuSurfaces)))
        return ERROR_NOT_SUPPORTED;
    if (!size->width || size->width > RB2K_DD_TARGET_SIDE ||
        !size->height || size->height > RB2K_DD_TARGET_SIDE)
        return ERROR_INVALID_PARAMETER;
    if (gpu->nextId == 0xffffffffUL) return ERROR_NO_MORE_ITEMS;
    for (slot = 0; slot < RB2K_DD_SLOTS; ++slot) if (!gpu->images[slot].id) break;
    if (slot == RB2K_DD_SLOTS) return ERROR_NOT_ENOUGH_MEMORY;
    /* OpenGL contexts (gpu_rvd.c) may already hold the transport. */
    if (!gpu->ready) {
        status = rb_gpu_start(gpu, &result);
        if (status) {
            rb_gpu_stop(gpu);
            return status;
        }
    }
    image = &gpu->images[slot];
    image->id = ++gpu->nextId;
    image->width = size->width;
    image->height = size->height;
    image->depth16 = depth16;
    image->stencil8 = stencil8;
    if (!rb_gpu_create_image(gpu, &result, image, name, sizeof(name) - 1, FALSE)) {
        VideoPortZeroMemory(image, sizeof(*image));
        if (!gpu->ready || (!gpu->imageCount && !rb_gpu_held(gpu))) rb_gpu_stop(gpu);
        return result.status;
    }
    ++gpu->imageCount;
    *(PULONG)packet->OutputBuffer = image->id;
    packet->StatusBlock->Information = sizeof(ULONG);
    return NO_ERROR;
}

static RB2K_IMAGE *find_image(RB2K_GPU *gpu, ULONG id)
{
    ULONG i;
    if (id) {
        for (i = 0; i < RB2K_DD_SLOTS; ++i)
            if (gpu->images[i].id == id) return &gpu->images[i];
    }
    return NULL;
}

static VP_STATUS command_error(RB2K_GPU *gpu, const RB2K_GPU_RESULT *result)
{
    if (!gpu->d7Profile.commandFailures++) {
        gpu->d7Profile.firstCommandStage = result->stage;
        gpu->d7Profile.firstCommandStatus = result->status;
    }
    /* Staging allocation precedes submission and leaves other images usable. */
    /* A late completion must not touch recycled storage. A host error
     * reply leaves no command outstanding, so OpenGL contexts may stay. */
    if ((result->stage != RB2K_STAGE_BACKING || result->status != ERROR_NOT_ENOUGH_MEMORY) &&
        (!rb_gpu_held(gpu) || !gpu->ready))
        rb_gpu_stop(gpu);
    return result->status;
}

/* Frame pacing (RB2K_PACE_SEGMENT): one present has reached the screen. */
static void pace(RB2K_GPU *gpu)
{
    RB2K_D7_PROFILE *p = &gpu->d7Profile;
    RB2K_PACE_SEGMENT *s = &p->pace[p->paceSegment];
    LONGLONG second, now = rb_counter_ticks(&second), dt = now - p->paceLast;
    ULONG high, bucket = 0, draws = p->paceRecords - p->paceDraws;
    BOOLEAN light = draws < RB2K_PACE_SCENE_DRAWS;
    ULONG uploads, shaders;
    p->paceDraws = p->paceRecords;
    uploads = p->op[RB2K_D7_UPLOAD].calls - p->paceUploads;
    shaders = p->shaders - p->paceShaders;
    p->paceUploads = p->op[RB2K_D7_UPLOAD].calls;
    p->paceShaders = p->shaders;
    if (!p->paceLast || dt < 0 || (dt >> RB2K_PACE_GAP_SHIFT)) {
        /* The first present of a stretch. */
        if (s->frames && p->paceSegment + 1 < RB2K_PACE_SEGMENTS) s = &p->pace[++p->paceSegment];
        if (!s->frames) s->start = now;
    } else if (light || p->paceLight) {
        ++s->lightFrames;
        s->lightTotal += dt;
    } else {
        for (high = RB2K_PACE_GAP_SHIFT - 1; high >= RB2K_PACE_FIRST_SHIFT && !(dt >> high); --high) {}
        if (high >= RB2K_PACE_FIRST_SHIFT)
            bucket = 1 + 2 * (high - RB2K_PACE_FIRST_SHIFT) + (ULONG)((dt >> (high - 1)) & 1);
        ++s->buckets[bucket < RB2K_PACE_BUCKETS ? bucket : RB2K_PACE_BUCKETS - 1];
        s->total += dt;
        if (dt > s->longest) s->longest = dt;
        if (dt * 10 >= second) {
            if (uploads) ++s->longUploads;
            if (shaders) ++s->longShaders;
            if (!uploads && !shaders) ++s->longOther;
        }
    }
    ++s->frames;
    p->paceLast = now;
    p->paceLight = light;
}

static VP_STATUS present(RB2K_GPU *gpu, const RB2K_IMAGE *image, const RB2K_DD_PRESENT *p)
{
    RB2K_PRIMARY *primary = &gpu->primary;
    RB2K_GPU_RESULT result;
    BOOLEAN solid = !image->defined || image->uniform;
    ULONG span[64], offset, y;
    const ULONG segmentPixels = RB2K_SEGMENT_BYTES / sizeof(ULONG);
    ULONG rows;     /* in the whole mapping: a flip chain's second page lies below the first */
    if (primary->bitsPerPixel != 32 || !primary->pixels || !primary->height || !primary->width ||
        primary->pitchPixels < primary->width ||
        primary->pitchPixels > primary->bytes / sizeof(ULONG) / primary->height)
        return ERROR_NOT_READY;
    rows = primary->bytes / sizeof(ULONG) / primary->pitchPixels;
    if (!p->width || !p->height || p->width > image->width || p->height > image->height ||
        p->srcX > image->width - p->width || p->srcY > image->height - p->height ||
        p->width > primary->width || p->height > primary->height ||
        p->dstX > primary->width - p->width || p->dstY > rows - p->height)
        return ERROR_INVALID_PARAMETER;
    if (gpu->memoryWidth && !solid) {
        /* The desktop in guest memory: the host copies the frame (gpu_primary.c). */
        RB2K_RECT to;
        VP_STATUS status;
        to.left = p->dstX;
        to.top = p->dstY;
        to.right = p->dstX + p->width;
        to.bottom = p->dstY + p->height;
        status = rb_memory_primary_present(gpu, image->id, p->srcX, p->srcY, &to);
        if (!status) ++gpu->presents;
        return status;
    }
    if (solid) {
        ULONG i, color = image->defined ? image->color : 0;
        for (i = 0; i < sizeof(span) / sizeof(span[0]); ++i) span[i] = color;
    } else if (gpu->presentCache != image->id) {
        /* Windows presents each visible rectangle separately. Read the whole
         * image back once; the following rectangles copy from staging. */
        LONGLONG read = rb_counter_ticks(NULL);
        if (!rb_gpu_transfer_box(gpu, &result, image->id, 0, 0,
                                 image->width, image->height, NULL)) return command_error(gpu, &result);
        if (gpu->d7Measure) gpu->d7Measure->fenceTicks += rb_counter_ticks(NULL) - read;
        gpu->presentCache = image->id;
        ++gpu->presentReadbacks;
        gpu->presentBytes += image->width * image->height * sizeof(ULONG);
    }
    for (y = 0; y < p->height; ++y) {
        PULONG dst = primary->pixels + (p->dstY + y) * primary->pitchPixels + p->dstX;
        ULONG remaining = p->width;
        offset = (p->srcY + y) * image->width + p->srcX;
        while (remaining) {
            PULONG src = span;
            ULONG count = sizeof(span) / sizeof(span[0]);
            if (!solid) {
                ULONG inSegment = offset % segmentPixels;
                src = (PULONG)gpu->staging[offset / segmentPixels].data + inSegment;
                count = segmentPixels - inSegment;
            }
            if (count > remaining) count = remaining;
            VideoPortWriteRegisterBufferUlong(dst, src, count);
            dst += count;
            offset += count;
            remaining -= count;
        }
    }
    ++gpu->presents;
    if (gpu->memoryWidth) {
        /* The desktop in guest memory shows what the host has been sent. A flip
         * chain's second page lies below the first. */
        ULONG page = p->dstY >= gpu->memoryHeight ? 1 : 0;
        RB2K_RECT r;
        r.left = p->dstX;
        r.top = p->dstY - page * gpu->memoryHeight;
        r.right = p->dstX + p->width;
        r.bottom = r.top + p->height;
        return rb_memory_primary_upload(gpu, page, &r);
    }
    return NO_ERROR;
}

/* Record a refused draw for d7draw: the step (RB2K_D7_SITE_*), its status
 * and a detail (a stage, see RB2K_D7_REFUSE_IMAGE, or a vertex index). */
static BOOLEAN refuse(RB2K_GPU *gpu, ULONG site, VP_STATUS status, ULONG stage)
{
    gpu->d7Profile.failSite = site;
    gpu->d7Profile.failStatus = status;
    gpu->d7Profile.failStage = stage;
    if (!gpu->d7Profile.failCount++) {
        gpu->d7Profile.firstSite = site;
        gpu->d7Profile.firstStatus = status;
        gpu->d7Profile.firstStage = stage;
    }
    return FALSE;
}

/* A texture id naming an image (a render target texture): the image's
 * resource seen as a texture of one level, attached to the target's context.
 * Its own draws are submitted first, as a frame for another target. On a
 * refusal, *why says which step (RB2K_D7_IMAGE_*). */
static RB2K_TEXTURE *image_texture(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, const RB2K_IMAGE *target, ULONG id,
                                   RB2K_TEXTURE *alias, PULONG why)
{
    RB2K_IMAGE *source = find_image(gpu, id & ~RB2K_TEXTURE_IMAGE);
    *why = RB2K_D7_IMAGE_MISSING;
    if (!source || source == target || source->depth16) return NULL;
    *why = RB2K_D7_IMAGE_FAILED;
    if (source->frameFailed) return NULL;
    *why = RB2K_D7_IMAGE_ATTACH;
    if (!rb_gpu_image_attach(gpu, result, source, target)) return NULL;
    *why = RB2K_D7_IMAGE_FLUSH;
    if (gpu->d7Frame.target == source && !rb_gpu_frame_flush(gpu, result)) return NULL;
    VideoPortZeroMemory(alias, sizeof(*alias));
    alias->id = source->id;
    alias->width = source->width;
    alias->height = source->height;
    alias->levels = alias->faces = 1;
    ++gpu->imageSamples;
    return alias;
}

/* One native draw to the image: validate it, find its depth buffer and
 * textures, and queue its commands. length is the record's byte count. */
static BOOLEAN draw_one(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, RB2K_IMAGE *image,
                        const RB2K_D3D7_DRAW *d, ULONG length, VP_STATUS *status)
{
    RB2K_IMAGE *depth = NULL;
    RB2K_TEXTURE *texture = NULL, *texture1 = NULL, alias[2];
    ULONG i, why, header = FIELD_OFFSET(RB2K_D3D7_DRAW, vertices);
    *status = ERROR_INVALID_PARAMETER;
    if (length < header || !d->count || d->count > RB2K_D3D7_VERTICES ||
        length != header + d->count*sizeof(d->vertices[0]) || d->target != image->id ||
        !rb_gpu_d7_valid(d) || image->depth16 || ((d->depthState || d->stencil) && !d->depth))
        return refuse(gpu, RB2K_D7_SITE_RECORD, *status, 0);
    *status = ERROR_INVALID_HANDLE;
    if (d->depth) {
        depth = find_image(gpu, d->depth);
        if (!depth || !depth->depth16 || (d->stencil && !depth->stencil8))
            return refuse(gpu, RB2K_D7_SITE_DEPTH, *status, 0);
    }
    for (i = 0; i < 2; ++i) {
        RB2K_TEXTURE *t;
        if (!rb_gpu_d7_samples(d, i)) continue;
        if ((i ? d->texture1 : d->texture) & RB2K_TEXTURE_IMAGE) {
            t = image_texture(gpu, result, image, i ? d->texture1 : d->texture, &alias[i], &why);
            if (!t) return refuse(gpu, RB2K_D7_SITE_TEXTURE, *status, RB2K_D7_REFUSE_IMAGE(why, i));
            if (i) texture1 = t; else texture = t;
            continue;
        }
        t = rb_gpu_texture(gpu, i ? d->texture1 : d->texture);
        if (!t) return refuse(gpu, RB2K_D7_SITE_TEXTURE, *status, i);
        /* Attached to this target's context on its first use here. */
        if (!rb_gpu_texture_attach(gpu, result, t, image)) {
            refuse(gpu, RB2K_D7_SITE_TEXTURE, result->status, 16 + i);
            *status = command_error(gpu, result);
            return FALSE;
        }
        if (i) texture1 = t; else texture = t;
    }
    *status = ERROR_INVALID_PARAMETER;
    for (i = 0; i < d->count; ++i) {
        const RB2K_D3D7_VERTEX *v = d->vertices+i;
        if ((v->x & 0x7fffffff) >= 0x7f800000 || (v->y & 0x7fffffff) >= 0x7f800000 ||
            (v->z & 0x7fffffff) >= 0x7f800000 || !(v->rhw & 0x7fffffff) ||
            (v->rhw & 0x7fffffff) >= 0x7f800000)
            return refuse(gpu, RB2K_D7_SITE_VERTEX, *status, i);
    }
    *status = NO_ERROR;
    if ((depth && !rb_gpu_depth_target(gpu, result, image, depth)) ||
        !rb_gpu_d7_draw(gpu, result, image, d, texture, texture1)) {
        refuse(gpu, RB2K_D7_SITE_COMMAND, result->status, result->stage);
        *status = command_error(gpu, result);
        return FALSE;
    }
    gpu->d7Frame.pending.triangles += d->count/3;
    ++gpu->d7Profile.paceRecords;
    return TRUE;
}

/* A draw from host buffers: every element of the vertex program reads a stream that is given,
 * from a vertex buffer that holds the vertices the draw names; the indices are in an index
 * buffer. (What the indices hold is the display driver's to check; the host's vertex fetch is
 * bounded by the buffers.) */
static BOOLEAN buffer_draw_valid(RB2K_GPU *gpu, const RB2K_D8_BUFFER_DRAW *b, ULONG count, ULONG vertex)
{
    static const UCHAR size[8] = { 4, 8, 12, 16, 4, 4, 4, 8 };
    ULONG k, last;
    if (!gpu->programs[vertex].elementCount || b->count != count || b->mode > 6 || b->mode == 2 ||
        b->streams >> RB2K_D8_STREAMS || b->start > 0x7fffffff || count > 0x7fffffff - b->start ||
        (b->indexSize && b->start)) return FALSE;
    if (b->indexSize) {
        RB2K_BUFFER *ib = rb_gpu_buffer(gpu, b->indexBuffer);
        if ((b->indexSize != 2 && b->indexSize != 4) || !ib || ib->kind != RB2K_D8_BUFFER_INDEX ||
            b->indexOffset > ib->bytes || b->start + count > (ib->bytes - b->indexOffset) / b->indexSize ||
            b->minIndex > b->maxIndex || b->maxIndex > 0xffffff || b->bias > 0xffffff || b->bias < -0xffffff ||
            (b->bias < 0 && b->minIndex < (ULONG)-b->bias)) return FALSE;
        last = b->maxIndex + b->bias;
    } else last = b->start + count - 1;
    for (k = 0; k < RB2K_D8_STREAMS; ++k) {
        RB2K_BUFFER *vb;
        if (!(b->streams & (1UL << k))) continue;
        vb = rb_gpu_buffer(gpu, b->stream[k].buffer);
        if (!vb || vb->kind != RB2K_D8_BUFFER_VERTEX || b->stream[k].stride > 255 || b->stream[k].offset > vb->bytes) return FALSE;
    }
    for (k = 0; k < gpu->programs[vertex].elementCount; ++k) {
        ULONG e = gpu->programs[vertex].elements[k], s = RB2K_D8_ELEMENT_STREAM(e), end, room;
        RB2K_BUFFER *vb;
        if (!(b->streams & (1UL << s))) return FALSE;
        vb = rb_gpu_buffer(gpu, b->stream[s].buffer);
        /* The last vertex's element ends inside the buffer. */
        end = RB2K_D8_ELEMENT_OFFSET(e) + size[RB2K_D8_ELEMENT_TYPE(e)];
        room = vb->bytes - b->stream[s].offset;
        if (end > room || (b->stream[s].stride && last > (room - end) / b->stream[s].stride)) return FALSE;
    }
    return TRUE;
}

/* One pixel shader draw: validate it, find its program, depth buffer and
 * textures, and queue its commands. */
static BOOLEAN draw_one8(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, RB2K_IMAGE *image,
                         const RB2K_D3D8_DRAW *d, ULONG length, VP_STATUS *status)
{
    RB2K_IMAGE *depth = NULL;
    RB2K_TEXTURE *textures[RB2K_D8_SAMPLERS], alias[RB2K_D8_SAMPLERS];
    ULONG i, program, vertex = 0, header = FIELD_OFFSET(RB2K_D3D8_DRAW, vertices), words = 0;
    *status = ERROR_INVALID_PARAMETER;
    if (length >= header) {
        /* Vertices as the stream held them after the vertex shader's
         * constants, or the fixed layout of RB2K_D3D8_VERTEX. */
        if (d->transform == 3)   /* an RB2K_D8_BUFFER_DRAW after the constants */
            words = d->vertexConstants <= RB2K_D8_VS_CONSTANTS ? 4 * d->vertexConstants + RB2K_D8_BUFFER_DRAW_WORDS : 0;
        else if (d->transform == 2)
            words = d->vertexConstants <= RB2K_D8_VS_CONSTANTS && d->vertexWords <= RB2K_D8_VS_VERTEX_WORDS &&
                    d->count <= RB2K_D8_VERTICES ? 4 * d->vertexConstants + d->count * d->vertexWords : 0;
        else words = d->coordSets <= RB2K_D8_STAGES && d->count <= RB2K_D8_VERTICES ?
                     d->count * RB2K_D8_VERTEX_WORDS(d->coordSets) : 0;
    }
    if (!gpu->d3d8 || length < header || !d->count || (d->transform != 3 && d->count > RB2K_D8_VERTICES) || !words ||
        length != header + words * sizeof(ULONG) || d->target != image->id ||
        !rb_gpu_d8_valid(d) || image->depth16 || ((d->depthState || d->stencil) && !d->depth))
        return refuse(gpu, RB2K_D7_SITE_RECORD, *status, 0);
    *status = ERROR_INVALID_HANDLE;
    program = RB2K_D8_PROGRAM_SLOT(d->program);
    if (!d->program || program >= RB2K_D8_PROGRAMS || gpu->programs[program].id != d->program ||
        gpu->programs[program].text[0] != 'F')
        return refuse(gpu, RB2K_D7_SITE_PROGRAM, *status, d->program);
    if (d->transform >= 2) {
        vertex = RB2K_D8_PROGRAM_SLOT(d->vertexProgram);
        if (!d->vertexProgram || vertex >= RB2K_D8_PROGRAMS || gpu->programs[vertex].id != d->vertexProgram ||
            gpu->programs[vertex].text[0] != 'V')
            return refuse(gpu, RB2K_D7_SITE_PROGRAM, *status, d->vertexProgram);
    }
    /* A program that reads attributes draws only from host buffers, and the other kind never. */
    if (d->transform == 3 ?
        !buffer_draw_valid(gpu, (const RB2K_D8_BUFFER_DRAW *)(d->vertices + 4 * d->vertexConstants), d->count, vertex) :
        d->transform == 2 && gpu->programs[vertex].elementCount)
        return refuse(gpu, RB2K_D7_SITE_PROGRAM, *status, 0x80000000UL | d->vertexProgram);
    if (d->depth) {
        depth = find_image(gpu, d->depth);
        if (!depth || !depth->depth16 || (d->stencil && !depth->stencil8)) return refuse(gpu, RB2K_D7_SITE_DEPTH, *status, 0);
    }
    for (i = 0; i < RB2K_D8_SAMPLERS; ++i) {
        RB2K_TEXTURE *t;
        textures[i] = NULL;
        if (!(gpu->programs[program].samplers & (1UL << i))) continue;
        if (d->textures[i] & RB2K_TEXTURE_IMAGE) {
            ULONG why;
            textures[i] = image_texture(gpu, result, image, d->textures[i], &alias[i], &why);
            if (!textures[i]) return refuse(gpu, RB2K_D7_SITE_TEXTURE, *status, RB2K_D7_REFUSE_IMAGE(why, i));
            continue;
        }
        t = rb_gpu_texture(gpu, d->textures[i]);
        if (!t) return refuse(gpu, RB2K_D7_SITE_TEXTURE, *status, i);
        if (!rb_gpu_texture_attach(gpu, result, t, image)) {
            refuse(gpu, RB2K_D7_SITE_TEXTURE, result->status, 16 + i);
            *status = command_error(gpu, result);
            return FALSE;
        }
        textures[i] = t;
    }
    *status = ERROR_INVALID_PARAMETER;
    for (i = 0; d->transform < 2 && i < d->count; ++i) {
        const RB2K_D3D8_VERTEX *v = (const RB2K_D3D8_VERTEX *)(d->vertices + i * RB2K_D8_VERTEX_WORDS(d->coordSets));
        if ((v->x & 0x7fffffff) >= 0x7f800000 || (v->y & 0x7fffffff) >= 0x7f800000 ||
            (v->z & 0x7fffffff) >= 0x7f800000 || !(v->rhw & 0x7fffffff) ||
            (v->rhw & 0x7fffffff) >= 0x7f800000)
            return refuse(gpu, RB2K_D7_SITE_VERTEX, *status, i);
    }
    *status = NO_ERROR;
    gpu->d8Step = 0;
    if ((depth && !rb_gpu_depth_target(gpu, result, image, depth)) ||
        !rb_gpu_d8_draw(gpu, result, image, d, gpu->programs[program].text, gpu->programs[program].bytes,
                        d->transform >= 2 ? gpu->programs[vertex].text : NULL,
                        d->transform >= 2 ? gpu->programs[vertex].bytes : 0, textures)) {
        refuse(gpu, RB2K_D7_SITE_STEP + gpu->d8Step, result->status, result->stage);
        *status = command_error(gpu, result);
        return FALSE;
    }
    gpu->d7Frame.pending.triangles += d->count/3;
    ++gpu->d7Profile.paceRecords;
    return TRUE;
}

/* An upload record of the draw queue: bytes for a host buffer, written in order with the draws. */
static BOOLEAN upload_one(RB2K_GPU *gpu, RB2K_GPU_RESULT *result, RB2K_IMAGE *image,
                          const RB2K_D8_UPLOAD_HEADER *u, ULONG length, VP_STATUS *status)
{
    RB2K_BUFFER *b;
    *status = ERROR_INVALID_PARAMETER;
    if (!gpu->d3d8 || length < sizeof(*u) || u->target != image->id || !u->bytes ||
        length != sizeof(*u) + ((u->bytes + 3) & ~3UL)) return refuse(gpu, RB2K_D7_SITE_RECORD, *status, 1);
    *status = ERROR_INVALID_HANDLE;
    b = rb_gpu_buffer(gpu, u->buffer);
    if (!b || u->offset > b->bytes || u->bytes > b->bytes - u->offset) return refuse(gpu, RB2K_D7_SITE_RECORD, *status, 2);
    *status = NO_ERROR;
    if (!rb_gpu_buffer_write(gpu, result, image, b, u->offset, u->bytes, (const UCHAR *)(u + 1))) {
        refuse(gpu, RB2K_D7_SITE_COMMAND, result->status, result->stage);
        *status = command_error(gpu, result);
        return FALSE;
    }
    gpu->d7Profile.bufferBytes += u->bytes;
    ++gpu->d7Profile.bufferUploads;
    return TRUE;
}

/* Keep or forget the TGSI text of a vertex or pixel program. */
static VP_STATUS program_request(RB2K_GPU *gpu, PVIDEO_REQUEST_PACKET packet)
{
    const RB2K_D3D8_PROGRAM *in = packet->InputBuffer;
    const RB2K_D3D8_VPROGRAM *vin = NULL;
    const char *text;
    ULONG i, bytes, free = RB2K_D8_PROGRAMS;
    if (!gpu->nativeD3D7 || !gpu->ddGpuSurfaces || !gpu->d3d8) return ERROR_NOT_SUPPORTED;
    if (packet->IoControlCode == IOCTL_RB2K_D3D8_PROGRAM_FREE) {
        ULONG id;
        if (packet->InputBufferLength != sizeof(ULONG) || packet->OutputBufferLength) return ERROR_INVALID_PARAMETER;
        id = *(PULONG)packet->InputBuffer;
        i = RB2K_D8_PROGRAM_SLOT(id);
        if (id && i < RB2K_D8_PROGRAMS && gpu->programs[i].id == id) {
            /* A colour context that made a host shader of it destroys that when it draws the
             * slot's next program (rb_gpu_d8_draw). */
            rb_pool_free(gpu->programs[i].text);
            gpu->programs[i].text = NULL;
            gpu->programs[i].id = 0;
        }
        return NO_ERROR;
    }
    if (packet->IoControlCode == IOCTL_RB2K_D3D8_VPROGRAM) {
        /* A vertex program for host buffers: the elements its inputs are fetched from, then the text. */
        vin = packet->InputBuffer;
        if (packet->InputBufferLength <= FIELD_OFFSET(RB2K_D3D8_VPROGRAM, text) || packet->InputBufferLength > sizeof(*vin) ||
            packet->OutputBufferLength < sizeof(ULONG) || !vin->elementCount || vin->elementCount > RB2K_D8_ELEMENTS)
            return ERROR_INVALID_PARAMETER;
        for (i = 0; i < vin->elementCount; ++i)
            if (RB2K_D8_ELEMENT_TYPE(vin->elements[i]) > 7 || RB2K_D8_ELEMENT_STREAM(vin->elements[i]) >= RB2K_D8_STREAMS ||
                RB2K_D8_ELEMENT_OFFSET(vin->elements[i]) > 252) return ERROR_INVALID_PARAMETER;
        text = vin->text;
        bytes = packet->InputBufferLength - FIELD_OFFSET(RB2K_D3D8_VPROGRAM, text);
        if (text[0] != 'V') return ERROR_INVALID_PARAMETER;
    } else {
        if (packet->InputBufferLength <= sizeof(ULONG) || packet->InputBufferLength > sizeof(*in) ||
            packet->OutputBufferLength < sizeof(ULONG) || in->samplers >> RB2K_D8_SAMPLERS) return ERROR_INVALID_PARAMETER;
        text = in->text;
        bytes = packet->InputBufferLength - sizeof(ULONG);
    }
    for (i = 0; i < bytes && text[i]; ++i) {}
    if (i == bytes || i < 4) return ERROR_INVALID_PARAMETER;
    bytes = i + 1;
    /* The first free slot at or after the last one given: a short search while slots remain. */
    for (i = 0; i < RB2K_D8_PROGRAMS && free == RB2K_D8_PROGRAMS; ++i) {
        ULONG slot = (gpu->programNext + i) % RB2K_D8_PROGRAMS;
        if (!gpu->programs[slot].id) free = slot;
    }
    if (free == RB2K_D8_PROGRAMS) return ERROR_NOT_ENOUGH_MEMORY;
    gpu->programs[free].text = rb_pool_alloc(bytes);
    if (!gpu->programs[free].text) return ERROR_NOT_ENOUGH_MEMORY;
    gpu->programNext = (free + 1) % RB2K_D8_PROGRAMS;
    if (!(++gpu->programSerial & (0xffffffffUL >> RB2K_D8_PROGRAM_SLOT_BITS))) ++gpu->programSerial;
    gpu->programs[free].id = (gpu->programSerial << RB2K_D8_PROGRAM_SLOT_BITS) | (free + 1);
    gpu->programs[free].samplers = vin ? 0 : in->samplers;
    gpu->programs[free].bytes = bytes;
    gpu->programs[free].elementCount = vin ? vin->elementCount : 0;
    if (vin) VideoPortMoveMemory(gpu->programs[free].elements, (PVOID)vin->elements, sizeof(vin->elements));
    VideoPortMoveMemory(gpu->programs[free].text, (PVOID)text, bytes);
    *(PULONG)packet->OutputBuffer = gpu->programs[free].id;
    packet->StatusBlock->Information = sizeof(ULONG);
    return NO_ERROR;
}

/* Make or free a host vertex or index buffer (IOCTL_RB2K_D8_BUFFER_CREATE, _FREE). */
static VP_STATUS buffer_request(RB2K_GPU *gpu, PVIDEO_REQUEST_PACKET packet)
{
    RB2K_GPU_RESULT result;
    const RB2K_D8_BUFFER_CREATE *in = packet->InputBuffer;
    RB2K_BUFFER *b;
    ULONG i, free = RB2K_D8_BUFFERS;
    if (!gpu->nativeD3D7 || !gpu->ddGpuSurfaces || !gpu->d3d8) return ERROR_NOT_SUPPORTED;
    if (packet->IoControlCode == IOCTL_RB2K_D8_BUFFER_FREE) {
        if (packet->InputBufferLength != sizeof(ULONG) || packet->OutputBufferLength) return ERROR_INVALID_PARAMETER;
        b = rb_gpu_buffer(gpu, *(PULONG)packet->InputBuffer);
        /* A transport reset has already released it. */
        if (!b) return NO_ERROR;
        ++gpu->d7Profile.buffersFreed;
        return rb_gpu_buffer_free(gpu, &result, b, TRUE) ? NO_ERROR : command_error(gpu, &result);
    }
    if (packet->InputBufferLength != sizeof(*in) || packet->OutputBufferLength < sizeof(ULONG) ||
        !in->bytes || in->bytes > RB2K_D8_BUFFER_BYTES || in->kind >= RB2K_D8_BUFFER_KINDS) return ERROR_INVALID_PARAMETER;
    if (!gpu->ready) return ERROR_NOT_READY;
    if (gpu->nextId == 0xffffffffUL) return ERROR_NO_MORE_ITEMS;
    for (i = 0; i < RB2K_D8_BUFFERS && free == RB2K_D8_BUFFERS; ++i) {
        ULONG slot = (gpu->bufferNext + i) % RB2K_D8_BUFFERS;
        if (!gpu->buffers[slot].handle) free = slot;
    }
    if (free == RB2K_D8_BUFFERS) return ERROR_NOT_ENOUGH_MEMORY;
    b = &gpu->buffers[free];
    VideoPortZeroMemory(b, sizeof(*b));
    b->id = ++gpu->nextId;
    b->bytes = in->bytes;
    b->kind = in->kind;
    if (!rb_gpu_buffer_create(gpu, &result, b)) {
        VideoPortZeroMemory(b, sizeof(*b));
        return command_error(gpu, &result);
    }
    gpu->bufferNext = (free + 1) % RB2K_D8_BUFFERS;
    if (!(++gpu->bufferSerial & (0xffffffffUL >> RB2K_D8_BUFFER_SLOT_BITS))) ++gpu->bufferSerial;
    b->handle = (gpu->bufferSerial << RB2K_D8_BUFFER_SLOT_BITS) | (free + 1);
    ++gpu->d7Profile.buffersMade;
    *(PULONG)packet->OutputBuffer = b->handle;
    packet->StatusBlock->Information = sizeof(ULONG);
    return NO_ERROR;
}

static VP_STATUS image_request(RB2K_GPU *gpu, PVIDEO_REQUEST_PACKET packet)
{
    ULONG id, bytes;
    RB2K_IMAGE *image;
    RB2K_GPU_RESULT result;
    BOOLEAN success;
    if (packet->InputBufferLength < sizeof(id)) return ERROR_INVALID_PARAMETER;
    id = *(PULONG)packet->InputBuffer;
    image = find_image(gpu, id);
    if (!image) return ERROR_INVALID_HANDLE;
    /* Depth with stencil is never read or written by the processor. */
    if (image->stencil8 && packet->IoControlCode != IOCTL_RB2K_DD_DESTROY) return ERROR_NOT_SUPPORTED;
    if (image->depth16 && packet->IoControlCode != IOCTL_RB2K_DD_READ &&
        packet->IoControlCode != IOCTL_RB2K_DD_WRITE &&
        packet->IoControlCode != IOCTL_RB2K_DD_DESTROY)
        return ERROR_NOT_SUPPORTED;
    if (packet->IoControlCode == IOCTL_RB2K_D3D7_FLUSH) {
        if (packet->InputBufferLength != sizeof(id) || packet->OutputBufferLength)
            return ERROR_INVALID_PARAMETER;
        if (image->frameFailed) return ERROR_INVALID_DATA;
        if (gpu->d7Frame.target == image && !rb_gpu_frame_flush(gpu, &result))
            return command_error(gpu, &result);
        return NO_ERROR;
    }
    /* A DirectDraw lock, blit, present or release can observe this image
     * before EndScene. Preserve the ordering of both sides of a blit. A
     * present reads the image back in its own context after the frame in
     * queue order, so it only queues the frame and does not wait for it. */
    /* Anything but a present may change an image the cached readback holds. */
    if (packet->IoControlCode != IOCTL_RB2K_DD_PRESENT) gpu->presentCache = 0;
    if (packet->IoControlCode == IOCTL_RB2K_DD_PRESENT) {
        if (!rb_gpu_order_image(gpu, &result, image)) return command_error(gpu, &result);
    } else if (packet->IoControlCode != IOCTL_RB2K_D3D7_CLEAR && packet->IoControlCode != IOCTL_RB2K_D3D7_DRAW &&
               packet->IoControlCode != IOCTL_RB2K_D3D7_DRAWS &&
               packet->IoControlCode != IOCTL_RB2K_D3D7_ZCLEAR) {
        if (!rb_gpu_sync_image(gpu, &result, image)) return command_error(gpu, &result);
    }
    if ((image->frameFailed || (image->depthOwner && image->depthOwner->frameFailed)) &&
        packet->IoControlCode != IOCTL_RB2K_DD_DESTROY) return ERROR_INVALID_DATA;
    bytes = image->width * image->height * (image->depth16 ? sizeof(USHORT) : sizeof(ULONG));
    switch (packet->IoControlCode) {
    case IOCTL_RB2K_D3D7_DRAW:
    case IOCTL_RB2K_D3D7_DRAWS: {
        VP_STATUS status;
        if (!gpu->nativeD3D7 || !gpu->ddGpuSurfaces || gpu->primary.bitsPerPixel != 32)
            return ERROR_NOT_SUPPORTED;
        if (packet->OutputBufferLength) return ERROR_INVALID_PARAMETER;
        if (packet->IoControlCode == IOCTL_RB2K_D3D7_DRAW) {
            if (!draw_one(gpu, &result, image, packet->InputBuffer, packet->InputBufferLength, &status))
                return status;
        } else {
            /* After the target id: byte count, record, until the end. */
            const UCHAR *p = (const UCHAR *)packet->InputBuffer + sizeof(ULONG);
            ULONG left = packet->InputBufferLength - sizeof(ULONG);
            if (packet->InputBufferLength > RB2K_D7_DRAWS_BYTES + sizeof(ULONG) || !left)
                return ERROR_INVALID_PARAMETER;
            while (left) {
                ULONG length, kind;
                if (left < sizeof(ULONG)) return ERROR_INVALID_PARAMETER;
                kind = *(const ULONG *)p & (RB2K_D8_RECORD | RB2K_D8_UPLOAD);
                length = *(const ULONG *)p & ~(RB2K_D8_RECORD | RB2K_D8_UPLOAD);
                if (length > left - sizeof(ULONG) || length & 3) return ERROR_INVALID_PARAMETER;
                if (kind == RB2K_D8_UPLOAD ? !upload_one(gpu, &result, image, (const RB2K_D8_UPLOAD_HEADER *)(p + sizeof(ULONG)), length, &status) :
                    kind ? !draw_one8(gpu, &result, image, (const RB2K_D3D8_DRAW *)(p + sizeof(ULONG)), length, &status)
                         : !draw_one(gpu, &result, image, (const RB2K_D3D7_DRAW *)(p + sizeof(ULONG)), length, &status))
                    return status;
                p += sizeof(ULONG) + length;
                left -= sizeof(ULONG) + length;
            }
        }
        success = TRUE;
        break;
    }
    case IOCTL_RB2K_DD_UNIFORM: {
        RB2K_DD_UNIFORM *answer = packet->OutputBuffer;
        if (packet->InputBufferLength != sizeof(id) || packet->OutputBufferLength < sizeof(*answer))
            return ERROR_INVALID_PARAMETER;
        answer->known = !image->defined || image->uniform;
        answer->color = image->defined && image->uniform ? image->color : 0;
        packet->StatusBlock->Information = sizeof(*answer);
        return NO_ERROR;
    }
    case IOCTL_RB2K_D3D7_ZCLEAR: {
        const RB2K_D3D7_ZCLEAR *z = packet->InputBuffer;
        RB2K_IMAGE *depth;
        if (!gpu->nativeD3D7 || !gpu->ddGpuSurfaces) return ERROR_NOT_SUPPORTED;
        if (packet->OutputBufferLength || packet->InputBufferLength != sizeof(*z) ||
            !z->flags || (z->flags & ~(RB2K_CLEAR_COLOR|RB2K_CLEAR_DEPTH|RB2K_CLEAR_STENCIL)) ||
            ((z->flags & RB2K_CLEAR_DEPTH) && z->depthBits > 0x3f800000 && z->depthBits != 0x80000000))
            return ERROR_INVALID_PARAMETER;
        depth = find_image(gpu, z->depth);
        if (!depth || !depth->depth16) return ERROR_INVALID_HANDLE;
        if (!rb_gpu_depth_target(gpu, &result, image, depth)) return command_error(gpu, &result);
        success = rb_gpu_clear_buffers(gpu, &result, image, z->flags, z->color, z->depthBits, z->stencil);
        if (success) ++gpu->d7Frame.pending.clears;
        break;
    }
    case IOCTL_RB2K_D3D7_CLEAR:
        if (!gpu->nativeD3D7 || !gpu->ddGpuSurfaces) return ERROR_NOT_SUPPORTED;
        /* Both paths use the same virgl clear encoding. */
    case IOCTL_RB2K_DD_FILL: {
        RB2K_DD_FILL *fill = packet->InputBuffer;
        if (packet->InputBufferLength != sizeof(*fill) || packet->OutputBufferLength)
            return ERROR_INVALID_PARAMETER;
        success = rb_gpu_draw_image(gpu, &result, id, fill->color, (BOOLEAN)!image->hasSurface,
                                    packet->IoControlCode == IOCTL_RB2K_D3D7_CLEAR ? image : NULL,
                                    image->depthBuffer ? RB2K_D7_DEPTH_VIEW : 0);
        if (success) {
            if (packet->IoControlCode == IOCTL_RB2K_D3D7_CLEAR) ++gpu->d7Frame.pending.clears;
            else ++gpu->hostFills;
            image->hasSurface = image->defined = image->uniform = TRUE;
            image->color = fill->color;
        }
        break;
    }
    case IOCTL_RB2K_DD_READ:
        if (packet->InputBufferLength != sizeof(id) || packet->OutputBufferLength < bytes)
            return ERROR_INVALID_PARAMETER;
        /* Never expose uninitialized renderer memory or a reused DMA page.
         * Every supported write defines the entire image, including uploads. */
        if (!image->defined) {
            VideoPortZeroMemory(packet->OutputBuffer, bytes);
            packet->StatusBlock->Information = bytes;
            return NO_ERROR;
        }
        success = rb_gpu_transfer_image(gpu, &result, image, packet->OutputBuffer, TRUE);
        if (success) {
            packet->StatusBlock->Information = bytes;
            ++gpu->readbacks;
        }
        break;
    case IOCTL_RB2K_DD_WRITE:
        if (packet->InputBufferLength != sizeof(id) + bytes || packet->OutputBufferLength)
            return ERROR_INVALID_PARAMETER;
        success = rb_gpu_transfer_image(gpu, &result, image, (PUCHAR)packet->InputBuffer + sizeof(id), FALSE);
        if (success) {
            image->defined = TRUE;
            image->uniform = FALSE;
            ++gpu->uploads;
        }
        break;
    case IOCTL_RB2K_DD_COPY: {
        RB2K_DD_COPY *copy = packet->InputBuffer;
        RB2K_IMAGE *destination;
        RB2K_RECT region = {0};
        if (packet->InputBufferLength != sizeof(*copy) || packet->OutputBufferLength)
            return ERROR_INVALID_PARAMETER;
        destination = find_image(gpu, copy->destination);
        if (!destination) return ERROR_INVALID_HANDLE;
        if (destination->depth16) return ERROR_NOT_SUPPORTED;
        if (gpu->d7Frame.target == destination && !rb_gpu_frame_flush(gpu, &result))
            return command_error(gpu, &result);
        result.status = rb_gpu_wait_image(gpu, destination);
        if (result.status) {
            result.stage = RB2K_STAGE_TRANSFER;
            return command_error(gpu, &result);
        }
        if (destination->frameFailed) return ERROR_INVALID_DATA;
        if (image->width != destination->width || image->height != destination->height)
            return ERROR_INVALID_PARAMETER;
        if (image == destination) return NO_ERROR;
        /* A fresh surface is logically zero until its first complete write. */
        if (!image->defined) {
            destination->defined = FALSE;
            return NO_ERROR;
        }
        region.right = image->width;
        region.bottom = image->height;
        success = rb_gpu_copy_rects(gpu, &result, destination->id, image->id, &region, 1, 0, 0, NULL);
        if (success) {
            destination->defined = TRUE;
            destination->uniform = image->uniform;
            destination->color = image->color;
            ++gpu->hostCopies;
        }
        break;
    }
    case IOCTL_RB2K_DD_STRETCH: {
        RB2K_DD_STRETCH *s = packet->InputBuffer;
        RB2K_IMAGE *destination;
        if (packet->InputBufferLength != sizeof(*s) || packet->OutputBufferLength)
            return ERROR_INVALID_PARAMETER;
        destination = find_image(gpu, s->destination);
        if (!destination) return ERROR_INVALID_HANDLE;
        if (destination->depth16 || destination == image) return ERROR_NOT_SUPPORTED;
        if (s->from.left >= s->from.right || s->from.top >= s->from.bottom || s->from.right > image->width ||
            s->from.bottom > image->height || s->to.left >= s->to.right || s->to.top >= s->to.bottom ||
            s->to.right > destination->width || s->to.bottom > destination->height)
            return ERROR_INVALID_PARAMETER;
        if (gpu->d7Frame.target == destination && !rb_gpu_frame_flush(gpu, &result))
            return command_error(gpu, &result);
        result.status = rb_gpu_wait_image(gpu, destination);
        if (result.status) {
            result.stage = RB2K_STAGE_TRANSFER;
            return command_error(gpu, &result);
        }
        if (destination->frameFailed) return ERROR_INVALID_DATA;
        success = rb_gpu_blit_rect(gpu, &result, destination->id, image->id, &s->to, &s->from, s->linear != 0);
        if (success) {
            destination->defined = TRUE;
            destination->uniform = FALSE;
            ++gpu->hostCopies;
        }
        break;
    }
    case IOCTL_RB2K_DD_PRESENT:
        if (packet->InputBufferLength != sizeof(RB2K_DD_PRESENT) || packet->OutputBufferLength)
            return ERROR_INVALID_PARAMETER;
        {
            VP_STATUS shown = present(gpu, image, packet->InputBuffer);
            if (!shown) pace(gpu);
            return shown;
        }
    case IOCTL_RB2K_DD_DESTROY:
        if (packet->InputBufferLength != sizeof(id) || packet->OutputBufferLength)
            return ERROR_INVALID_PARAMETER;
        if (image->depthOwner && !rb_gpu_depth_target(gpu, &result, image->depthOwner, NULL))
            return command_error(gpu, &result);
        if (image->depthBuffer && !rb_gpu_depth_target(gpu, &result, image, NULL))
            return command_error(gpu, &result);
        /* Another target's open frame may sample this image as a texture: its draws go to the
         * host before the image does (as in rb_gpu_texture_free). */
        if (gpu->d7Frame.target && gpu->d7Frame.target != image && !rb_gpu_frame_flush(gpu, &result))
            return command_error(gpu, &result);
        success = rb_gpu_destroy_image(gpu, &result, id);
        if (success) {
            rb_gpu_slot_forget(gpu, image);
            VideoPortZeroMemory(image, sizeof(*image));
            /* The last image has gone (a program closed its Direct3D device): what is presented next
             * starts a new stretch of the frame pacing counts. */
            if (gpu->imageCount == 1) gpu->d7Profile.paceLast = 0;
            if (!--gpu->imageCount && !rb_gpu_held(gpu)) return rb_gpu_stop(gpu);
        }
        break;
    default:
        return ERROR_INVALID_FUNCTION;
    }
    if (success) return NO_ERROR;
    return command_error(gpu, &result);
}

/* Create a Direct3D texture or replace rows of one, attached to the target
 * that will sample it. Texture ids share the image counter. */
/* Host formats of the upload formats of RB2K_D3D7_TEXTURE. */
static const ULONG block_formats[RB2K_D7_UPLOAD_FORMATS] = {
    0, VIRGL_FORMAT_DXT1_RGBA, VIRGL_FORMAT_DXT3_RGBA, VIRGL_FORMAT_DXT5_RGBA,
    VIRGL_FORMAT_R8G8_SNORM, VIRGL_FORMAT_R8G8B8A8_SNORM
};

static VP_STATUS texture_request(RB2K_GPU *gpu, PVIDEO_REQUEST_PACKET packet)
{
    RB2K_GPU_RESULT result;
    RB2K_TEXTURE *t;
    const RB2K_D3D7_TEXTURE *in = packet->InputBuffer;
    RB2K_IMAGE *image;
    ULONG bytes, side, block;
    if (!gpu->nativeD3D7 || !gpu->ddGpuSurfaces) return ERROR_NOT_SUPPORTED;
    if (packet->IoControlCode == IOCTL_RB2K_D3D7_TEXTURE_FREE) {
        if (packet->InputBufferLength != sizeof(ULONG) || packet->OutputBufferLength)
            return ERROR_INVALID_PARAMETER;
        t = rb_gpu_texture(gpu, *(PULONG)packet->InputBuffer);
        /* A transport reset has already released it. */
        if (!t) return NO_ERROR;
        return rb_gpu_texture_free(gpu, &result, t, TRUE) ? NO_ERROR : command_error(gpu, &result);
    }
    if (packet->InputBufferLength < sizeof(*in) || packet->OutputBufferLength < sizeof(ULONG))
        return ERROR_INVALID_PARAMETER;
    if (!in->width || in->width > RB2K_D7_TEXTURE_SIDE || !in->height || in->height > RB2K_D7_TEXTURE_SIDE ||
        !in->levels || in->levels > RB2K_D7_LEVELS || in->level >= in->levels ||
        ((in->width | in->height) >> (in->levels - 1)) == 0)
        return ERROR_INVALID_PARAMETER;
    {
        ULONG depth = in->depth > 1 ? in->depth : 1, slices = depth >> in->level ? depth >> in->level : 1;
        if (in->format >= RB2K_D7_UPLOAD_FORMATS || (in->faces != 1 && in->faces != 6) ||
            (in->faces == 6 && in->width != in->height) || depth > RB2K_D7_VOLUME_SIDE ||
            (depth > 1 && (in->faces != 1 || RB2K_D7_BLOCK_SIDE(in->format) > 1 || in->width > RB2K_D7_VOLUME_SIDE ||
                           in->height > RB2K_D7_VOLUME_SIDE)) ||
            in->face >= (depth > 1 ? slices : in->faces)) return ERROR_INVALID_PARAMETER;
    }
    /* Blocks and signed texels as they are only where the host samples them
     * (DXT also only with RippleHostDxt); otherwise the answer is texture id
     * zero (the video port reports every failure alike). */
    if (in->format && ((in->format <= RB2K_D7_BLOCKS_DXT5 && !gpu->hostDxt) ||
                       !((rb_gpu_host_blocks(gpu) >> in->format) & 1))) {
        *(PULONG)packet->OutputBuffer = 0;
        packet->StatusBlock->Information = sizeof(ULONG);
        return NO_ERROR;
    }
    side = RB2K_D7_BLOCK_SIDE(in->format);
    block = in->format ? RB2K_D7_BLOCK_BYTES(in->format) : sizeof(ULONG);
    {
        /* Rows of one level, each level half the one above, at least one. */
        ULONG width = in->width >> in->level ? in->width >> in->level : 1;
        ULONG height = in->height >> in->level ? in->height >> in->level : 1;
        if (!in->rows || in->y >= height || in->rows > height - in->y || in->y % side)
            return ERROR_INVALID_PARAMETER;
        bytes = (width + side - 1) / side * block * ((in->rows + side - 1) / side);
    }
    if (bytes > RB2K_D7_UPLOAD_BYTES || packet->InputBufferLength != sizeof(*in) + bytes)
        return ERROR_INVALID_PARAMETER;
    image = find_image(gpu, in->target);
    if (!image || image->depth16) return ERROR_INVALID_HANDLE;
    if (image->frameFailed) return ERROR_INVALID_DATA;
    if (in->id) {
        t = rb_gpu_texture(gpu, in->id);
        if (!t) return ERROR_INVALID_HANDLE;
        if (t->width != in->width || t->height != in->height || t->levels != in->levels || t->faces != in->faces ||
            t->depth != (in->depth > 1 ? in->depth : 1) ||
            t->format != block_formats[in->format])
            return ERROR_INVALID_PARAMETER;
    } else {
        /* Levels below the top add up to under a third of it. */
        ULONG i, size = (in->width + side - 1) / side * ((in->height + side - 1) / side) * block;
        if (in->levels > 1) size += size / 3 + 4 * sizeof(ULONG);
        if (in->y || in->level || in->face) return ERROR_INVALID_PARAMETER;
        size *= in->faces;
        if (in->depth > 1) size *= in->depth;
        if (gpu->nextId == 0xffffffffUL) return ERROR_NO_MORE_ITEMS;
        for (i = 0; i < RB2K_D7_TEXTURES && gpu->textures[i].id; ++i) {}
        if (i == RB2K_D7_TEXTURES || size > RB2K_D7_TEXEL_BUDGET - gpu->textureBytes)
            return ERROR_NOT_ENOUGH_MEMORY;
        t = &gpu->textures[i];
        t->id = ++gpu->nextId;
        t->width = in->width;
        t->height = in->height;
        t->levels = in->levels;
        t->bytes = size;
        t->format = block_formats[in->format];
        t->faces = in->faces;
        t->depth = in->depth > 1 ? in->depth : 1;
        if (!rb_gpu_texture_create(gpu, &result, t)) {
            VideoPortZeroMemory(t, sizeof(*t));
            return command_error(gpu, &result);
        }
        ++gpu->textureCount;
        gpu->textureBytes += size;
    }
    if (!rb_gpu_texture_attach(gpu, &result, t, image) ||
        !rb_gpu_texture_upload(gpu, &result, t, image, in->level, in->face, in->y, in->rows, (PVOID)(in + 1)))
        return command_error(gpu, &result);
    ++gpu->uploads;
    *(PULONG)packet->OutputBuffer = t->id;
    packet->StatusBlock->Information = sizeof(ULONG);
    return NO_ERROR;
}

/* IOCTL_RB2K_D7_CUBE_SYNC: the host cube texture of a cube map render target, made when asked for
 * the first time, and the faces in mask copied into it from their images. */
static VP_STATUS cube_sync(RB2K_GPU *gpu, PVIDEO_REQUEST_PACKET packet)
{
    const RB2K_D7_CUBE_SYNC *in = packet->InputBuffer;
    RB2K_GPU_RESULT result;
    RB2K_TEXTURE *t;
    RB2K_IMAGE *faces[6];
    ULONG f;
    if (!gpu->nativeD3D7 || !gpu->ddGpuSurfaces) return ERROR_NOT_SUPPORTED;
    if (packet->InputBufferLength != sizeof(*in) || packet->OutputBufferLength < sizeof(ULONG) ||
        !in->side || in->side > RB2K_D7_TEXTURE_SIDE || in->mask > 63) return ERROR_INVALID_PARAMETER;
    for (f = 0; f < 6; ++f) {
        faces[f] = find_image(gpu, in->images[f]);
        if (!faces[f] || faces[f]->depth16 || faces[f]->width != in->side || faces[f]->height != in->side)
            return ERROR_INVALID_HANDLE;
        if (faces[f]->frameFailed) return ERROR_INVALID_DATA;
    }
    if (in->texture) {
        t = rb_gpu_texture(gpu, in->texture);
        if (!t || t->faces != 6 || t->width != in->side || t->levels != 1 || t->format) return ERROR_INVALID_HANDLE;
    } else {
        ULONG i, size = in->side * in->side * 4 * 6;
        if (gpu->nextId == 0xffffffffUL) return ERROR_NO_MORE_ITEMS;
        for (i = 0; i < RB2K_D7_TEXTURES && gpu->textures[i].id; ++i) {}
        if (i == RB2K_D7_TEXTURES || size > RB2K_D7_TEXEL_BUDGET - gpu->textureBytes) return ERROR_NOT_ENOUGH_MEMORY;
        t = &gpu->textures[i];
        t->id = ++gpu->nextId;
        t->width = t->height = in->side;
        t->levels = 1;
        t->bytes = size;
        t->format = 0;   /* B8G8R8A8, as the images */
        t->faces = 6;
        t->depth = 1;
        if (!rb_gpu_texture_create(gpu, &result, t)) {
            VideoPortZeroMemory(t, sizeof(*t));
            return command_error(gpu, &result);
        }
        ++gpu->textureCount;
        gpu->textureBytes += size;
    }
    for (f = 0; f < 6; ++f) {
        if (!((in->mask >> f) & 1)) continue;
        /* What was drawn into the face goes first, in the face's own context. */
        if (gpu->d7Frame.target == faces[f] && !rb_gpu_frame_flush(gpu, &result)) return command_error(gpu, &result);
        if (!rb_gpu_texture_attach(gpu, &result, t, faces[f]) ||
            !rb_gpu_copy_layer(gpu, &result, faces[f]->id, t->id, f, in->side)) return command_error(gpu, &result);
    }
    *(PULONG)packet->OutputBuffer = t->id;
    packet->StatusBlock->Information = sizeof(ULONG);
    return NO_ERROR;
}

VP_STATUS rb_gpu_surface_io(RB2K_GPU *gpu, PVIDEO_REQUEST_PACKET packet)
{
    VP_STATUS status;
    if (VideoPortInterlockedExchange(&gpu->busy, 1)) return ERROR_BUSY;
    if (packet->IoControlCode == IOCTL_RB2K_D3D7_FLIGHT || packet->IoControlCode == IOCTL_RB2K_D3D7_COMPLETED) {
        status = rb_gpu_poll_scenes(gpu);
        if (status) {
            VideoPortInterlockedExchange(&gpu->busy, 0);
            return status;
        }
    }
    if (packet->IoControlCode == IOCTL_RB2K_D3D7_FLIGHT) {
        status = ERROR_INVALID_PARAMETER;
        if (!packet->InputBufferLength && packet->OutputBufferLength == sizeof(gpu->d7Flight)) {
            gpu->d7Flight.limit = gpu->d7WaitCurrent ? 1 : RB2K_D7_FLIGHT_SLOTS;
            rb_counter_ticks(&gpu->d7Flight.frequency);
            *(RB2K_D7_FLIGHT *)packet->OutputBuffer = gpu->d7Flight;
            packet->StatusBlock->Information = sizeof(gpu->d7Flight);
            status = NO_ERROR;
        }
    } else if (packet->IoControlCode == IOCTL_RB2K_D3D7_COMPLETED) {
        status = ERROR_INVALID_PARAMETER;
        if (!packet->InputBufferLength && packet->OutputBufferLength == sizeof(gpu->d7Completed)) {
            *(RB2K_D3D7_COMPLETED *)packet->OutputBuffer = gpu->d7Completed;
            packet->StatusBlock->Information = sizeof(gpu->d7Completed);
            status = NO_ERROR;
        }
    } else if (packet->IoControlCode == IOCTL_RB2K_D3D7_PROFILE) {
        status = ERROR_INVALID_PARAMETER;
        if (!packet->InputBufferLength && packet->OutputBufferLength == sizeof(gpu->d7Profile)) {
            rb_counter_ticks(&gpu->d7Profile.frequency);
            gpu->d7Profile.codeAt = (ULONG)(ULONG_PTR)rb_gpu_frame_reserve;
            gpu->d7Profile.memoryPresents = gpu->memoryPresents;
            gpu->d7Profile.memoryFailures = gpu->memoryFailures;
            gpu->d7Profile.memoryStale = gpu->memoryStale;
            gpu->d7Profile.memoryChain = gpu->memoryChain;
            VideoPortMoveMemory(packet->OutputBuffer, &gpu->d7Profile, sizeof(gpu->d7Profile));
            packet->StatusBlock->Information = sizeof(gpu->d7Profile);
            status = NO_ERROR;
        }
    }
    else if (packet->IoControlCode == IOCTL_RB2K_DD_QUERY) status = query(gpu, packet);
    else if (packet->IoControlCode == IOCTL_RB2K_DD_CREATE ||
             packet->IoControlCode == IOCTL_RB2K_DD_CREATE_Z16 ||
             packet->IoControlCode == IOCTL_RB2K_DD_CREATE_Z24S8) status = create(gpu, packet);
    else {
        ULONG op = RB2K_D7_OPS;
        LONGLONG start = 0;
        switch (packet->IoControlCode) {
        case IOCTL_RB2K_D3D7_FLUSH: op = RB2K_D7_FLUSH; break;
        case IOCTL_RB2K_D3D7_CLEAR:
        case IOCTL_RB2K_D3D7_ZCLEAR: op = RB2K_D7_CLEAR; break;
        case IOCTL_RB2K_D3D7_DRAW:
        case IOCTL_RB2K_D3D7_DRAWS: op = RB2K_D7_DRAW; break;
        case IOCTL_RB2K_DD_READ: op = RB2K_D7_READ; break;
        case IOCTL_RB2K_DD_PRESENT: op = RB2K_D7_PRESENT; break;
        case IOCTL_RB2K_DD_WRITE:
        case IOCTL_RB2K_D3D7_TEXTURE: op = RB2K_D7_UPLOAD; break;
        }
        if (gpu->nativeD3D7 && op < RB2K_D7_OPS) {
            gpu->d7Measure = &gpu->d7Profile.op[op];
            start = rb_counter_ticks(NULL);
        }
        if (packet->IoControlCode == IOCTL_RB2K_D3D7_TEXTURE ||
            packet->IoControlCode == IOCTL_RB2K_D3D7_TEXTURE_FREE) status = texture_request(gpu, packet);
        else if (packet->IoControlCode == IOCTL_RB2K_D7_CUBE_SYNC) status = cube_sync(gpu, packet);
        else if (packet->IoControlCode == IOCTL_RB2K_D3D8_PROGRAM || packet->IoControlCode == IOCTL_RB2K_D3D8_VPROGRAM ||
                 packet->IoControlCode == IOCTL_RB2K_D3D8_PROGRAM_FREE) status = program_request(gpu, packet);
        else if (packet->IoControlCode == IOCTL_RB2K_D8_BUFFER_CREATE ||
                 packet->IoControlCode == IOCTL_RB2K_D8_BUFFER_FREE) status = buffer_request(gpu, packet);
        else status = image_request(gpu, packet);
        if (status && !gpu->d7Profile.requestFailures++) {
            gpu->d7Profile.firstRequest = packet->IoControlCode;
            gpu->d7Profile.firstRequestStatus = status;
            gpu->d7Profile.firstRequestId = packet->InputBufferLength >= sizeof(ULONG) ? *(PULONG)packet->InputBuffer : 0;
        }
        if (gpu->d7Measure) {
            gpu->d7Measure->ticks += rb_counter_ticks(NULL)-start;
            ++gpu->d7Measure->calls;
            if (status) ++gpu->d7Measure->failed;
            gpu->d7Measure = NULL;
        }
    }
    VideoPortInterlockedExchange(&gpu->busy, 0);
    return status;
}

VP_STATUS rb_gpu_surface_reset(RB2K_GPU *gpu)
{
    VP_STATUS status, frameStatus = NO_ERROR;
    if (VideoPortInterlockedExchange(&gpu->busy, 1)) return ERROR_BUSY;
    /* Mode changes lose these DirectDraw surfaces even if the transport
     * survives for an OpenGL client. Finish submitted scenes first. */
    if (gpu->d7Frame.target) {
        RB2K_GPU_RESULT r;
        if (!rb_gpu_frame_flush(gpu, &r)) frameStatus = r.status;
    }
    if (!frameStatus) frameStatus = rb_gpu_wait_image(gpu, NULL);
    rb_cursor_forget(gpu);
    if (rb_gpu_held(gpu) && gpu->ready) {
        /* A mode change loses the DirectDraw surfaces but not OpenGL's
         * contexts or the desktop in memory: free the surfaces one by one
         * and keep the transport. */
        RB2K_GPU_RESULT result;
        ULONG slot;
        for (slot = 0; slot < RB2K_DD_SLOTS && gpu->ready; ++slot) {
            RB2K_IMAGE *image = &gpu->images[slot];
            if (image->depthBuffer && !rb_gpu_depth_target(gpu, &result, image, NULL)) {
                frameStatus = result.status;
                gpu->ready = FALSE;
            }
        }
        for (slot = 0; slot < RB2K_DD_SLOTS; ++slot) {
            if (gpu->ready && gpu->images[slot].id) rb_gpu_destroy_image(gpu, &result, gpu->images[slot].id);
            VideoPortZeroMemory(&gpu->images[slot], sizeof(gpu->images[slot]));
        }
        gpu->imageCount = 0;
        for (slot = 0; slot < RB2K_D7_TEXTURES; ++slot)
            if (gpu->ready && gpu->textures[slot].id) rb_gpu_texture_free(gpu, &result, &gpu->textures[slot], FALSE);
        rb_gpu_texture_forget(gpu);
        for (slot = 0; slot < RB2K_D8_BUFFERS; ++slot)
            if (gpu->ready && gpu->buffers[slot].handle) rb_gpu_buffer_free(gpu, &result, &gpu->buffers[slot], FALSE);
        rb_gpu_buffer_forget(gpu);
        /* Only a transport timeout leaves a reset to do. */
        status = gpu->ready ? NO_ERROR : rb_gpu_stop(gpu);
    } else {
        status = rb_gpu_stop(gpu);
    }
    gpu->primary.width = gpu->primary.height = gpu->primary.pitchPixels = gpu->primary.bitsPerPixel = 0;
    VideoPortInterlockedExchange(&gpu->busy, 0);
    return status ? status : frameStatus;
}
