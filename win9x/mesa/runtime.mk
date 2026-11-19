# Built on the Linux host. Link these objects ahead of llvm-mingw's static libraries.
SHIM := $(dir $(lastword $(MAKEFILE_LIST)))../compat/k32shim.c
FORMAT_OBJECTS := $(addsuffix .o,$(FORMATTERS))
OBJECTS := k32shim.o $(FORMAT_OBJECTS)
CPU_FLAGS := -O2 -march=pentium2 -femulated-tls
# Match the 32-bit MSVCRT libmingwex configuration and its printf ABI.
FORMAT_FLAGS := -mno-sse -mno-sse2 -std=gnu99 -fno-builtin -D_CRTBLD \
                -D_WIN32_WINNT=0x0f00 -D__MSVCRT_VERSION__=0x600 -D__USE_MINGW_ANSI_STDIO=0

.DELETE_ON_ERROR:
runtime.stamp: $(OBJECTS)
	touch $@

$(OBJECTS): $(lastword $(MAKEFILE_LIST)) $(CC)
k32shim.o: $(SHIM)
	$(CC) $(CPU_FLAGS) -Wall -Werror -c $(SHIM) -o $@

vpath %.c $(CRT)/stdio:$(CRT)/gdtoa
$(FORMAT_OBJECTS): %.o: %.c
	$(CC) $(CPU_FLAGS) $(FORMAT_FLAGS) -MMD -MP -c $< -o $@

-include $(FORMAT_OBJECTS:.o=.d)
