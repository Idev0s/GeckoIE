#!/bin/sh
# Needs i686-w64-mingw32-gcc (apt: g++-mingw-w64-i686). Built via the gcc driver so libstdc++/winpthread are NOT linked.
cd "$(dirname "$0")"

i686-w64-mingw32-gcc -x c++ -O2 -Wall -Wno-unused -DWINVER=0x0501 -D_WIN32_WINNT=0x0501 -D_WIN32_IE=0x0600 \
  -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit -shared -static-libgcc \
  -o GeckoIEDoc.dll src/docobj.cpp -Wl,--kill-at -Wl,--image-base=0x5F800000 -Wl,-Map=GeckoIEDoc.map -luuid -lole32 -loleaut32 -luser32 -lgdi32 -ladvapi32 -lshlwapi -lws2_32 -lcomctl32 -lcomdlg32 -lshell32
rm -f gie.res.o
i686-w64-mingw32-strip --strip-unneeded GeckoIEDoc.dll
