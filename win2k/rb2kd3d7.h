#ifndef RB2K_D3D7_H
#define RB2K_D3D7_H

/* Read-only display escape for distinguishing native draws from the RGB
 * software device. Counts completed host operations and live D3D objects. */
#define RB2K_D3D7_STATS_ESCAPE 0x52420020
typedef struct {
    ULONG version, contexts, targets, clears, triangles;
} RB2K_D3D7_STATS;
#define RB2K_D3D7_STATS_VERSION 1
#define RB2K_D3D7_PROFILE_ESCAPE 0x52420021
/* A present's fenceTicks is its readback from the host; the rest is the copy
 * to the screen. */
enum { RB2K_D7_CLEAR, RB2K_D7_DRAW, RB2K_D7_READ, RB2K_D7_UPLOAD, RB2K_D7_FLUSH, RB2K_D7_PRESENT, RB2K_D7_OPS };
/* The miniport's host object caches per render target, by kind. */
enum { RB2K_CACHE_DSA, RB2K_CACHE_BLEND, RB2K_CACHE_SHADER, RB2K_CACHE_SAMPLER, RB2K_CACHE_SAMPLER1,
       RB2K_CACHE_VS_PROGRAM, RB2K_CACHE_PS_SHADER, RB2K_CACHE_PS_SAMPLER, RB2K_CACHE_KINDS };
typedef struct {
    LONGLONG ticks, queueTicks, fenceTicks;
    ULONG calls, submits, fences, bytes, failed;
} RB2K_D7_TIME;
typedef struct {
    LONGLONG frequency;
    RB2K_D7_TIME op[RB2K_D7_OPS];
    /* The last refused draw record: the step (RB2K_D7_SITE_*), its status,
     * the transport stage of a command failure, and refusals so far; then
     * the same for the first refusal. */
    ULONG failSite, failStatus, failStage, failCount;
    ULONG firstSite, firstStatus, firstStage;
    /* The first failed surface request (its IOCTL code, status and the image
     * or texture id it named) and the first failed host command (transport
     * stage and status), with the number of each. */
    ULONG requestFailures, firstRequest, firstRequestStatus, firstRequestId;
    ULONG commandFailures, firstCommandStage, firstCommandStatus;
    /* Cache lookups by kind: found, a free handle, an old entry replaced (its
     * host object destroyed and made again); host shaders made and their text. */
    ULONG caches[RB2K_CACHE_KINDS][3];
    ULONG shaders, shaderBytes;
    ULONG codeAt;       /* where rb_gpu_frame_reserve was loaded, to place samples */
} RB2K_D7_PROFILE;
enum { RB2K_D7_SITE_RECORD = 1, RB2K_D7_SITE_PROGRAM, RB2K_D7_SITE_DEPTH, RB2K_D7_SITE_TEXTURE,
       RB2K_D7_SITE_VERTEX, RB2K_D7_SITE_COMMAND, RB2K_D7_SITE_STEP = 16 };   /* STEP + step in the draw */
/* A refused render target texture (site TEXTURE): the detail is 32 + 8*why
 * + stage. why: no such image (or the target itself), the image's own frame
 * failed, attaching it to the target failed, submitting its frame failed. */
enum { RB2K_D7_IMAGE_MISSING, RB2K_D7_IMAGE_FAILED, RB2K_D7_IMAGE_ATTACH, RB2K_D7_IMAGE_FLUSH };
#define RB2K_D7_REFUSE_IMAGE(why, stage) (32 + 8*(why) + (stage))
#define RB2K_D3D7_FLIGHT_ESCAPE 0x52420022
typedef struct {
    LONGLONG frequency, throttleTicks, accessTicks;
    ULONG limit, peak, pending, queued, completed, throttleWaits, accessWaits, failed;
} RB2K_D7_FLIGHT;
/* Read-only counts of what the native path drew or set aside, in triangles
 * unless named otherwise. A skipped draw is consumed and leaves the target
 * unchanged; the reason is the first unsupported feature found. */
#define RB2K_D3D7_DRAW_ESCAPE 0x52420023
enum { RB2K_D7_SKIP_FILL, RB2K_D7_SKIP_STENCIL, RB2K_D7_SKIP_STAGE1, RB2K_D7_SKIP_VERTEX,
       RB2K_D7_SKIP_TEXTURE, RB2K_D7_SKIP_COMBINER, RB2K_D7_SKIP_PRIMITIVE, RB2K_D7_SKIP_CLEAR,
       RB2K_D7_SKIPS };
enum { RB2K_D7_IGNORED_FOG, RB2K_D7_IGNORED_COLORKEY, RB2K_D7_IGNORED_CLIPPLANE,
       RB2K_D7_IGNORED_MIPMAP, RB2K_D7_IGNORED_LIGHTING, RB2K_D7_IGNORED };
#define RB2K_D7_OPCODES 100
typedef struct {
    ULONG version, drawn, drawCalls, skippedCalls;
    ULONG skipped[RB2K_D7_SKIPS], ignored[RB2K_D7_IGNORED];
    ULONG textures, uploads, uploadBytes, checks, uploadFailures, checkFailures;
    ULONG opcodes[RB2K_D7_OPCODES];
    ULONG registered[8];        /* texture registrations by decoded format, zero unsupported */
    ULONG videoTextures, texBlts, texBltFailures, drawFailures;
    ULONG dp2Calls, dp2Failures, lastFailure, lastFailureOp;
    /* Diagnostics: why vertex data was refused, the last refused range,
     * the last host draw error and the distinct drawn state combinations. */
    ULONG badPositions, failSites[8];
    ULONG lastRange[10];        /* op, first, low, high, available, offset, linear, flags, stride, count */
    ULONG lastDrawError, lastDrawState[8];
    ULONG stateOverflow, beyondCalls, beyondVertices;   /* draws past the vertex window, triangles skipped */
    /* The last 32 command buffers: flags, vertex offset, length,
     * kernel surface, its address and size, requested size, lowest start,
     * highest start plus index, result. Recording stops 16 buffers after the
     * first draw that indexes past the stated window. */
    ULONG transformTypes[32];   /* SETTRANSFORM types received; 31 counts any other */
    /* Texture surfaces refused at CanCreateSurface or CreateSurface, with
     * the last one's caps, description flags, size, pixel format (flags << 8
     * | bits) and surface count; mipmapped and managed textures registered. */
    ULONG surfaceRefused, refusedCaps, refusedFlags, refusedWidth, refusedHeight, refusedFormat, refusedCount;
    ULONG regMipmaps, regManaged;
    ULONG lightsBeyond;   /* SETLIGHT records for an index past the 32 kept */
    /* Mipmapped textures registered with a full chain, and why a chain stopped
     * early: no attach list, no attached surface, not a mipmap, size, memory. */
    ULONG mipChains, mipStops[5];
    /* Display-driver time: whole DrawPrimitives2 calls, and within them the
     * once-a-scene texture reads and checksums and the texture uploads. */
    LONGLONG frequency, dp2Ticks, checkTicks, uploadTicks;
    LONGLONG copyTicks, drawTicks;   /* the command-buffer copy; draw requests to the miniport */
    ULONG drawRequests;              /* IOCTL_RB2K_D3D7_DRAWS requests carrying the draws */
    ULONG textureWrites, checksSkipped;   /* texture locks and blits seen; checks they saved */
    /* DirectDraw setup per desktop depth (8, 16, 32 bits): DrvGetDirectDrawInfo
     * calls, its failures, the last failure's reason (1 no lock, 2 no mapping,
     * 3 capability query failed), and DrvEnableDirectDraw failures. */
    ULONG ddModes[3][4];
    ULONG historyStop, historyNext;
    ULONG history[32][10];
    struct {
        ULONG colorStage, alphaStage, blend, alphaTest, depthState, cull, shade, fvf, texFormat, transform;
        ULONG calls, primitives;
    } states[64];
    /* The Direct3D 8 interface: GetDriverInfo2 requests by type (31 counts
     * any other), the DirectX version the runtime announced, context
     * creations by the runtime's input handle value (7 counts any other),
     * buffers registered, draws from vertex streams, and the first DirectX 8
     * commands: opcode, record count, DrawPrimitives2 flags and the first
     * four record words. */
    ULONG d8Requests[32], d8Version, d8ContextKinds[8], d8Buffers, d8StreamDraws;
    ULONG d8TraceCount, d8Trace[64][7];
    ULONG d8LogCount, d8Log[24][2];     /* the first GetDriverInfo2 requests: type, expected size */
    /* Pixel shaders given to the miniport, those refused by reason (the
     * translator's codes; 7 counts other failures), the token behind the
     * last refusal, and pixel shader draw records queued. */
    ULONG psCreated, psFailed[8], psLastDetail, psDraws;
    /* Vertex shaders created, and the last four (by creation count modulo 4): handle, declaration and
     * code bytes, then the first 45 words of declaration and code. */
    ULONG vsCreated, vsLog[4][48];
    /* Vertex programs made, shaders refused by reason, the token behind the
     * last refusal, triangles drawn with a vertex program, those of them with
     * the texture stages as their pixel program, and stage programs refused. */
    ULONG vsTranslated, vsFailed[8], vsLastDetail, vsDraws, vsStageDraws, stageFailed;
    /* The first six pixel shaders (handle, bytes, the first 42 words), and the
     * state combinations of draws with a vertex program: vertex and pixel
     * shader handles, blend, alpha test, fog, depth state, draw calls. */
    ULONG psLogCount, psLog[6][44], comboCount, combos[24][7];
    /* State block operations by D3DHAL_STATESET* code (6 counts others), and
     * blocks that could not grow. */
    ULONG stateSets[7], blockFailures;
    ULONG blockUploads;                 /* compressed textures sent as blocks for the host to decode */
    ULONG blockStatus;                  /* the last failure status of such an upload */
    ULONG cubeTextures;                 /* cube maps registered */
    ULONG volumeTextures;               /* volume textures registered */
    /* Full-screen chains: flips, those that showed a drawn image, chain
     * front images made, blits touching a chain surface; the last eight flips
     * (current and target page offsets, their image ids, result) and render
     * target changes (image id). */
    ULONG flips, flipPresents, chainFronts, chainBacks, chainBlts, flipLog[8][5], targets, targetLog[8];
    ULONG targetFailures;               /* render targets set by a handle no image has */
    /* The last eight DirectDraw locks: caps, size (width << 16 | height),
     * fpVidMem, the address returned (0 when Windows locks it), result. */
    ULONG locks, lockLog[8][5];
    ULONG targetTextures;               /* render target textures registered */
    /* Texture skips by reason: no such handle, not usable (format, owner,
     * kind), the target itself, not ready (read or upload failed). */
    ULONG texSkips[4];
    /* The last eight surface creations: caps, size (width << 16 | height),
     * count, slots in use << 8 | where it ended (0 done, 1 no slot, 2 host
     * image refused), result. */
    ULONG creates, createLog[8][5];
    ULONG codeAt;       /* where rb_d7_drawstats was loaded, to place samples */
} RB2K_D7_DRAWSTATS;
enum { RB2K_D7_FAIL_RANGE, RB2K_D7_FAIL_VBCAPS, RB2K_D7_FAIL_LINEAR, RB2K_D7_FAIL_ADDRESS,
       RB2K_D7_FAIL_PROBE, RB2K_D7_FAIL_IMMEDIATE, RB2K_D7_FAIL_LENGTH, RB2K_D7_FAIL_OTHER };
#define RB2K_D7_DRAWSTATS_VERSION 3
#define RB2K_D3D7_QUERY_LIMIT 24
typedef struct {
    RB2K_D3D7_STATS totals;
    ULONG surfaceCalls, contextCalls, clearCalls, commandCalls;
    ULONG lastCommand, lastError, errorOffset;
    ULONG halInfoCalls, enableCalls, nativeEnabled, callbackQueries, surfaceQueries;
    ULONG queryCount, queryIds[RB2K_D3D7_QUERY_LIMIT];
} RB2K_D3D7_DIAGNOSTICS;

#endif
