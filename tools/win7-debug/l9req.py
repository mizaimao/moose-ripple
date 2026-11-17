# l9req.py d3d10level9.dll: what Windows 7's Direct3D 10 Level 9 layer requires of a Direct3D 9
# driver for feature levels 9.1, 9.2 and 9.3, read from the RequiredCaps tables in the DLL
# (MinCapsLevel1..3: lists of flag masks, minimum values, minimum floats and formats with
# the operations each must support, checked by ExceedsCapsBits).
import struct, sys
d = open(sys.argv[1], "rb").read()
e = struct.unpack_from("<I", d, 0x3C)[0]
nsec, = struct.unpack_from("<H", d, e + 6)
opt = e + 24
base, = struct.unpack_from("<Q", d, opt + 24)
secs = []
o = opt + struct.unpack_from("<H", d, e + 20)[0]
for i in range(nsec):
    vsz, va, rsz, raw = struct.unpack_from("<IIII", d, o + 40 * i + 8)
    secs.append((va, max(vsz, rsz), raw))
def off(va):
    r = va - base
    for a, s, raw in secs:
        if a <= r < a + s: return raw + r - a
    raise ValueError(hex(va))
F = {0:"DeviceType",4:"AdapterOrdinal",8:"Caps",12:"Caps2",16:"Caps3",20:"PresentationIntervals",24:"CursorCaps",28:"DevCaps",32:"PrimitiveMiscCaps",36:"RasterCaps",40:"ZCmpCaps",44:"SrcBlendCaps",48:"DestBlendCaps",52:"AlphaCmpCaps",56:"ShadeCaps",60:"TextureCaps",64:"TextureFilterCaps",68:"CubeTextureFilterCaps",72:"VolumeTextureFilterCaps",76:"TextureAddressCaps",80:"VolumeTextureAddressCaps",84:"LineCaps",88:"MaxTextureWidth",92:"MaxTextureHeight",96:"MaxVolumeExtent",100:"MaxTextureRepeat",104:"MaxTextureAspectRatio",108:"MaxAnisotropy",112:"MaxVertexW",116:"GuardBandLeft",120:"GuardBandTop",124:"GuardBandRight",128:"GuardBandBottom",132:"ExtentsAdjust",136:"StencilCaps",140:"FVFCaps",144:"TextureOpCaps",148:"MaxTextureBlendStages",152:"MaxSimultaneousTextures",156:"VertexProcessingCaps",160:"MaxActiveLights",164:"MaxUserClipPlanes",168:"MaxVertexBlendMatrices",172:"MaxVertexBlendMatrixIndex",176:"MaxPointSize",180:"MaxPrimitiveCount",184:"MaxVertexIndex",188:"MaxStreams",192:"MaxStreamStride",196:"VertexShaderVersion",200:"MaxVertexShaderConst",204:"PixelShaderVersion",208:"PixelShader1xMaxValue",212:"DevCaps2",216:"MaxNpatchTessellationLevel",236:"DeclTypes",240:"NumSimultaneousRTs",244:"StretchRectFilterCaps",248:"VS20Caps.Caps",252:"VS20Caps.DynamicFlowControlDepth",256:"VS20Caps.NumTemps",260:"VS20Caps.StaticFlowControlDepth",264:"PS20Caps.Caps",268:"PS20Caps.DynamicFlowControlDepth",272:"PS20Caps.NumTemps",276:"PS20Caps.StaticFlowControlDepth",280:"PS20Caps.NumInstructionSlots",284:"VertexTextureFilterCaps",288:"MaxVShaderInstructionsExecuted",292:"MaxPShaderInstructionsExecuted",296:"MaxVertexShader30InstructionSlots",300:"MaxPixelShader30InstructionSlots"}
def name(o): return F.get(o, "caps+0x%x" % o)
for lvl, va in (("9.1", int(sys.argv[2], 16)), ("9.2", int(sys.argv[3], 16)), ("9.3", int(sys.argv[4], 16))):
    p = off(va)
    a, b, c, f, extra, _, fn = struct.unpack_from("<QQQQIIQ", d, p)
    print("== feature level %s: extra flags 0x%x, hard-coded check at %x" % (lvl, extra, fn))
    if a:
        q = off(a)
        while True:
            o, m = struct.unpack_from("<QI", d, q)
            if not m: break
            print("  flags  %-28s 0x%08x" % (name(o), m)); q += 16
    if b:
        q = off(b)
        while True:
            o, m = struct.unpack_from("<QI", d, q)
            if not m: break
            print("  least  %-28s %d (0x%x)" % (name(o), m, m)); q += 16
    if c:
        q = off(c)
        while True:
            o, = struct.unpack_from("<Q", d, q); m, = struct.unpack_from("<f", d, q + 8)
            if m == 0.0: break
            print("  least  %-28s %g" % (name(o), m)); q += 16
    if f:
        q = off(f)
        while True:
            fmt, ops, x, flag, opt_ = struct.unpack_from("<iIIB3xB3x", d, q)
            if not fmt: break
            print("  format DXGI %-3d ops 0x%08x%s%s" % (fmt, ops, " +msaa?" if flag else "", " (optional)" if opt_ else "")); q += 20
