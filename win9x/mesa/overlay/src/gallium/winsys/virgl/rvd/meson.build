# The virgl winsys for Windows 9x (moose-ripple): virgl through the mini-VDD ripplevd.vxd.
# rvd.h is copied in from the moose-ripple repository (win9x/minivdd) by its build script.

libvirglrvd = static_library(
  'virglrvd',
  'rvd_virgl_winsys.c',
  include_directories : [inc_include, inc_src, inc_mapi, inc_mesa, inc_gallium, inc_gallium_aux, inc_gallium_drivers, inc_virtio],
  dependencies : [dep_libvirglcommon, idep_mesautil, idep_nir_headers],
  gnu_symbol_visibility : 'hidden',
)
