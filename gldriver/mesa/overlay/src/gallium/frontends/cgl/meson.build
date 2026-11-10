# The CGL frontend (moose-ripple, M4): Mesa behind Apple's OpenGL on Mac OS X 10.6.
# Apple's OpenGL calls our gld* entry points to manage renderers, pixel formats, contexts and
# drawables; each context's public dispatch table is pointed at the thunks generated here.

ripple_sdk = meson.get_external_property('ripple_sdk')

cgl_dispatch_c = custom_target(
  'cgl_dispatch.c',
  input : ['gen_dispatch.py',
           ripple_sdk / 'System/Library/Frameworks/OpenGL.framework/Headers/gliDispatch.h',
           files('../../../mapi/glapi/gen/gl_and_es_API.xml'), main_dispatch_h],
  output : 'cgl_dispatch.c',
  command : [prog_python, '@INPUT0@', '@INPUT1@', '@INPUT2@', '@OUTPUT@', '@INPUT3@'],
)

libcgl_st = static_library(
  'cgl_st',
  ['cgl_device.c', 'cgl_context.c', 'cgl_apple.c', 'cgl_takeover.c', cgl_dispatch_c,
   main_dispatch_h],
  include_directories : [inc_include, inc_src, inc_mapi, inc_mesa, inc_gallium, inc_gallium_aux,
                         inc_gallium_winsys],
  dependencies : [idep_mesautil, dep_libvirglripple],
  gnu_symbol_visibility : 'hidden',
)
