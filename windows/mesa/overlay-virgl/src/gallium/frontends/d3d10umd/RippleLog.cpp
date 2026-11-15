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
