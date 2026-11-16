/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright 2026 the moose-ripple authors
 */

#include "RippleLog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

extern "C" {

void
ripple_vlogf(const char *prefix, const char *format, va_list args)
{
   char line[1024];
   int n = snprintf(line, sizeof(line), "%s", prefix);

   vsnprintf(line + n, sizeof(line) - n - 2, format, args);
   n = (int)strlen(line);
   if (n == 0 || line[n - 1] != '\n')
      strcat(line, "\n");
   OutputDebugStringA(line);

   static const char *path;
   static bool asked;
   if (!asked) {
      path = getenv("RIPPLE_UMD_LOG");
      asked = true;
   }
   if (path) {
      FILE *f = fopen(path, "a");
      if (f) {
         fputs(line, f);
         fclose(f);
      }
   }
}

void
ripple_logf(const char *format, ...)
{
   va_list args;

   va_start(args, format);
   ripple_vlogf("", format, args);
   va_end(args);
}

}

/*
 * With RIPPLE_UMD_LOG set, the driver writes where the program's main modules sit in memory
 * when it is loaded: with samples of the guest's instruction pointer taken from the host,
 * that tells which module (and with its symbol table, which function) the time goes to.
 */
namespace {
struct RippleModuleBases {
   RippleModuleBases()
   {
      if (!getenv("RIPPLE_UMD_LOG"))
         return;
      static const char *const names[] = {
         NULL, "viogpu_d3d10.dll", "d3d10.dll", "d3d10core.dll", "d3d10_1.dll", "d3d10_1core.dll",
         "d3d11.dll", "dxgi.dll", "d3d9.dll", "ntdll.dll", "kernel32.dll", "KernelBase.dll",
         "msvcrt.dll", "gdi32.dll", "user32.dll",
      };
      char exe[MAX_PATH] = "";
      GetModuleFileNameA(NULL, exe, sizeof(exe));
      ripple_logf("modules of process %lu (%s):", GetCurrentProcessId(), exe);
      for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
         HMODULE m = GetModuleHandleA(names[i]);
         if (m)
            ripple_logf("  module %p %s", (void *)m, names[i] ? names[i] : "(program)");
      }
   }
} ripple_module_bases;
}
