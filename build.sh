#!/bin/sh
# Native math test, then cross-build the 32-bit DLL.
set -e
cd "$(dirname "$0")/src"
gcc -O1 -Wall -o /tmp/test_math test_math.c -lm && /tmp/test_math
i686-w64-mingw32-gcc -O2 -Wall -Wextra -Wno-array-bounds -mno-sse -mno-mmx -shared -o ../dinput8.dll dllmain.c \
    -static-libgcc -Wl,--kill-at -s -luser32 -lkernel32
echo "built ../dinput8.dll"
