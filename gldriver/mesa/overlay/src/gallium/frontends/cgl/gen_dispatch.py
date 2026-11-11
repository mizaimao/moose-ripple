#!/usr/bin/env python3
"""Generate the CGL dispatch thunks (moose-ripple, M4).

Apple's public GL dispatch table (`GLIFunctionDispatch`, SDK gliDispatch.h) has 764 function
pointers, each taking the GLIContext first. libGL calls through it for every GL function
(docs/re/gl-plugin-interface.md). For each entry this emits a thunk that makes the context
current for Mesa if needed and calls Mesa's dispatch (`CALL_<Name>` from main/dispatch.h),
plus `ripple_cgl_fill_dispatch(void **table)` that stores the thunks in table order.

Apple's entry names are snake_case GL names (`alpha_func`, `uniform1f_ARB`). They are matched
to Mesa's GL API XML: an entry point name, resolved through `alias=` to the function that owns
the dispatch slot. Entries with no match get a stub that reports the call once.

Only the Python standard library is used; Mesa's XML is read as data.

usage: gen_dispatch.py GLI_DISPATCH_H GL_API_XML OUT.c [MESA_DISPATCH_H] [--report]

With MESA_DISPATCH_H (main/dispatch.h, generated), only functions that have a dispatch slot
there (a CALL_ macro) are called; the API XML also lists a few that don't.
"""
import os
import re
import sys
import xml.etree.ElementTree as ET

# Apple names whose snake_case doesn't map mechanically to a GL entry point Mesa has.
SPECIAL = {
    # GL_APPLE_vertex_array_object: Mesa only has the ARB/core vertex array objects.
    'gen_vertex_arrays_EXT': 'GenVertexArrays',
    'bind_vertex_array_EXT': 'BindVertexArray',
    'delete_vertex_arrays_EXT': 'DeleteVertexArrays',
    'is_vertex_array_EXT': 'IsVertexArray',
    # GL_APPLE_flush_render
    'flush_render_APPLE': 'Flush',
    'finish_render_APPLE': 'Finish',
}

# Entries implemented by the CGL frontend itself (cgl_dispatch.h declares them with the
# same parameters, after `void *rend`).
CUSTOM = {
    'gen_fences_APPLE': 'ripple_cgl_gen_fences',
    'delete_fences_APPLE': 'ripple_cgl_delete_fences',
    'set_fence_APPLE': 'ripple_cgl_set_fence',
    'is_fence_APPLE': 'ripple_cgl_is_fence',
    'test_fence_APPLE': 'ripple_cgl_test_fence',
    'finish_fence_APPLE': 'ripple_cgl_finish_fence',
    'test_object_APPLE': 'ripple_cgl_test_object',
    'finish_object_APPLE': 'ripple_cgl_finish_object',
    'swap_APPLE': 'ripple_cgl_swap',
    # Apple-only hint values Mesa would refuse with GL_INVALID_ENUM: filtered, the rest
    # forwarded (cgl_apple.c). Not GL_UNPACK_CLIENT_STORAGE_APPLE: accepting that tells
    # WindowServer the GL reads its texture memory at draw time, and it then reuses buffers
    # (Mesa copies once: wrong window contents everywhere, 2026-11-10).
    'tex_parameteri': 'ripple_cgl_tex_parameteri',
    'tex_parameterf': 'ripple_cgl_tex_parameterf',
    'tex_parameteriv': 'ripple_cgl_tex_parameteriv',
    'tex_parameterfv': 'ripple_cgl_tex_parameterfv',
    'hint': 'ripple_cgl_hint',
    # Window offset for drawing into the screen through a rectangle (cgl_apple.c).
    'viewport': 'ripple_cgl_viewport',
    'scissor': 'ripple_cgl_scissor',
    'clear': 'ripple_cgl_clear',
    'get_integerv': 'ripple_cgl_get_integerv',
    'read_pixels': 'ripple_cgl_read_pixels',
    'copy_pixels': 'ripple_cgl_copy_pixels',
    'copy_tex_image1D': 'ripple_cgl_copy_tex_image1D',
    'copy_tex_image2D': 'ripple_cgl_copy_tex_image2D',
    'copy_tex_sub_image1D': 'ripple_cgl_copy_tex_sub_image1D',
    'copy_tex_sub_image2D': 'ripple_cgl_copy_tex_sub_image2D',
    'copy_tex_sub_image3D': 'ripple_cgl_copy_tex_sub_image3D',
}


def parse_gli(path):
    """Return [(name, return_type, [(type, param_name), ...])] in table order."""
    text = open(path).read()
    body = text[text.index('typedef struct __GLIFunctionDispatchRec'):]
    body = body[body.index('{') + 1:body.index('}')]
    body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
    entries = []
    for m in re.finditer(r'([\w\s\*]+?)\(\s*\*\s*(\w+)\s*\)\s*\(([^)]*)\)\s*;', body):
        ret, name, params = ' '.join(m.group(1).split()), m.group(2), m.group(3)
        plist = [p.strip() for p in params.split(',')]
        if not plist or not plist[0].startswith('GLIContext'):
            raise SystemExit('unexpected first parameter in %s: %r' % (name, params))
        args = []
        for i, p in enumerate(plist[1:]):
            pm = re.match(r'(.*?)(\w+)\s*(\[\s*\d*\s*\])?$', p)
            ptype, pname = ' '.join(pm.group(1).split()), pm.group(2)
            if pm.group(3):
                ptype += ' *'
            if not ptype:          # unnamed parameter: the word was the type
                ptype, pname = pname, 'a%d' % i
            args.append((ptype, pname))
        entries.append((name, ret, args))
    return entries


def parse_api(path):
    """Return {entry_point_name: dispatch_function_name} from Mesa's GL API XML."""
    names = {}
    seen = set()

    def load(p):
        p = os.path.normpath(p)
        if p in seen:
            return
        seen.add(p)
        root = ET.parse(p).getroot()
        for el in root.iter():
            tag = el.tag.split('}')[-1]
            if tag == 'include' and el.get('href'):
                load(os.path.join(os.path.dirname(p), el.get('href')))
            elif tag == 'function':
                names[el.get('name')] = el.get('alias') or el.get('name')

    load(path)
    # Resolve chains of aliases.
    for k in list(names):
        v, hops = names[k], 0
        while names.get(v, v) != v and hops < 8:
            v, hops = names[v], hops + 1
        names[k] = v
    return names


FLOATS = ('GLfloat', 'GLclampf', 'GLdouble', 'GLclampd')


def trace_call(index, name, params):
    """A ripple_cgl_trace() call for the debug trace: integers in hex, floats, pointers."""
    fmts, args = [], []
    for ptype, pname in params:
        t = ptype.replace('const ', '').strip()
        if '*' in t or t in ('GLhandleARB', 'GLsync'):
            fmts.append('%p')
            args.append('(const void *)(uintptr_t)' + pname if t in ('GLhandleARB',) else '(const void *)' + pname)
        elif t in FLOATS:
            fmts.append('%g')
            args.append('(double)' + pname)
        else:
            fmts.append('%#llx')
            args.append('(unsigned long long)' + pname)
    fmt = '%s(%s)' % (name, ', '.join(fmts))
    return ('   if (unlikely(ripple_cgl_tracing))\n      ripple_cgl_trace(%d, "%s"%s);\n'
            % (index, fmt, ''.join(', ' + a for a in args)))


def gl_name(apple):
    if apple in SPECIAL:
        return SPECIAL[apple]
    return ''.join(p[:1].upper() + p[1:] for p in apple.split('_') if p)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    report = '--report' in sys.argv
    gli, xml, out = args[:3]
    entries = parse_gli(gli)
    api = parse_api(xml)
    if len(args) > 3:
        slots = set(re.findall(r'#define CALL_(\w+)\(', open(args[3]).read()))
        api = {k: v for k, v in api.items() if v in slots}

    lines = ['/* Generated by gen_dispatch.py (moose-ripple). Do not edit. */',
             '#include "cgl_dispatch.h"', '']
    fills, missing = [], []
    for index, (name, ret, params) in enumerate(entries):
        decl = ', '.join(['void *rend'] + ['%s %s' % p for p in params])
        call = ', '.join(p[1] for p in params)
        target = api.get(gl_name(name))
        fn = 'ripple_gl_' + name
        if name in CUSTOM:
            fn = CUSTOM[name]
            lines.append('%s GLAPIENTRY %s(%s);   /* in the frontend */\n' % (ret, fn, decl))
        elif target:
            lines.append('static %s GLAPIENTRY %s(%s)\n{\n%s   RIPPLE_CGL_ENTER(rend);\n'
                         '   %sCALL_%s(GET_DISPATCH(), (%s));\n}\n'
                         % (ret, fn, decl, trace_call(index, name, params),
                            '' if ret == 'void' else 'return ', target, call))
        else:
            missing.append(name)
            lines.append('static %s GLAPIENTRY %s(%s)\n{\n   ripple_cgl_unsupported("%s");\n%s}\n'
                         % (ret, fn, decl, name, '' if ret == 'void' else '   return (%s)0;\n' % ret))
        fills.append(fn)
    lines.append('const unsigned ripple_cgl_dispatch_count = %d;\n' % len(fills))
    lines.append('void\nripple_cgl_fill_dispatch(void **table)\n{')
    for i, fn in enumerate(fills):
        lines.append('   table[%d] = (void *)%s;' % (i, fn))
    lines.append('}\n')
    with open(out, 'w') as f:
        f.write('\n'.join(lines))
    if report or missing:
        sys.stderr.write('%d dispatch entries, %d mapped to Mesa, %d unsupported%s\n'
                         % (len(entries), len(entries) - len(missing), len(missing),
                            (': ' + ' '.join(missing)) if missing else ''))


if __name__ == '__main__':
    main()
