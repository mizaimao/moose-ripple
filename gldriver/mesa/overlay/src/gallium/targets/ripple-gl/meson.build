# RippleGLDriver: the OpenGL driver bundle for Mac OS X 10.6 (moose-ripple, M4). Exports only
# the 80 gld* entry points Apple's OpenGL looks up; Mesa and the virgl driver are linked in.
# The binary goes into RippleGLDriver.bundle/Contents/MacOS (gldriver/mesa/bundle.sh).

ripple_gl = shared_module(
  'RippleGLDriver',
  ['ripple_gl_target.c', 'gld_config.c', 'gld_window.c'],
  name_prefix : '',
  name_suffix : 'dylib',   # renamed when the Makefile packs the bundle
  include_directories : [inc_include, inc_src, inc_mapi, inc_mesa, inc_gallium, inc_gallium_aux,
                         inc_gallium_winsys, include_directories('../../frontends')],
  link_whole : [libcgl_st, libglapi_static],
  link_with : [libmesa, libgallium, libgalliumvl_stub],   # no video: stubs for virgl_video.c
  link_args : ['-Wl,-exported_symbols_list,' + (meson.current_source_dir() / 'ripple-gl.exports'),
               '-Wl,-undefined,error', '-framework', 'OpenGL', '-framework', 'CoreFoundation'],
  link_depends : files('ripple-gl.exports'),
  dependencies : [driver_virgl, dep_libvirglripple, idep_mesautil, idep_nir, dep_thread],
)
