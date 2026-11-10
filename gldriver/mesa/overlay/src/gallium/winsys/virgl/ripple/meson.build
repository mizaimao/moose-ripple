# The ripple virgl winsys (moose-ripple): virgl over the moose-ripple GPU user client.
# ripple_gpu_uapi.h is copied in from the moose-ripple repo's include/ by its build script.
libvirglripple = static_library(
  'virglripple',
  'ripple_virgl_winsys.c',
  include_directories : [inc_include, inc_src, inc_mapi, inc_mesa, inc_gallium, inc_gallium_aux, inc_gallium_drivers, inc_virtio],
  dependencies : [dep_libvirglcommon, idep_mesautil, idep_nir_headers],
  gnu_symbol_visibility : 'hidden',
)

dep_libvirglripple = declare_dependency(
  link_with : libvirglripple,
  include_directories : include_directories('.'),
  link_args : ['-framework', 'IOKit', '-framework', 'CoreFoundation'],
)
