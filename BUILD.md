GeckoIE source - how the setup is built (Linux + MinGW-w64 i686 + NSIS 3 with MUI2; nothing here runs on Linux)

Layout
  installer/GeckoIE.nsi          NSIS script (cd installer; makensis GeckoIE.nsi) -> GeckoIE-v0.1-Alpha3-Setup.exe
  installer/payload/             files the setup installs (bare-app = engine side JS/XHTML, bare-app-extra/perf.js = engine
                                 preferences, scripts, icon). GeckoIEDoc.dll, GeckoHost.dll, omnislim.exe are copied in from the builds below.
  native/docobj/src/docobj.cpp   GeckoIEDoc.dll: IE document view (docobj), gie: protocol, native Browser Helper Object (BHO),
                                 context menus, zoom slider, favicons, Settings window, favorites watcher, toolbar button
  native/GeckoHost.c             GeckoHost.dll: loads and runs the Gecko engine inside iexplore.exe (own thread)
  native/omnislim.c              omnislim.exe: removes entries from omni.ja without recompressing
  installer/payload/bho-src/     the OLD .NET BHO (no longer built or installed; kept for reference only)

Build (run from the folder named)
  native/docobj:  sh build.sh     -> GeckoIEDoc.dll  (gcc -x c++, image base 0x5F800000, link map GeckoIEDoc.map, no libstdc++/winpthread)
  native:         i686-w64-mingw32-gcc -O2 -shared -static-libgcc -Wl,--image-base=0x5FA00000 -o GeckoHost.dll GeckoHost.c -Wl,--kill-at
                  i686-w64-mingw32-strip --strip-unneeded GeckoHost.dll
  native:         i686-w64-mingw32-gcc -O2 -static -s -o omnislim.exe omnislim.c
  Copy GeckoIEDoc.dll, GeckoHost.dll, omnislim.exe into installer/payload/ before makensis.
  (The two image bases are deliberately NOT 0x10000000, so xul.dll can have that address: see CHANGES-alpha3.md.)

Checks
  JavaScript has no build step: cp file /tmp/x.js; node --check /tmp/x.js   (bare-app/content/*.js, *.jsm, bare-app-extra/perf.js)
  Every DLL must import nothing newer than Windows XP:
    i686-w64-mingw32-objdump -p GeckoIEDoc.dll | grep -E "GetThreadId|GetTickCount64|InitializeCriticalSectionEx|winpthread|libstdc"   (prints nothing)
  Keep the DLL and its .map from every delivered build: the crash watch in the log prints offsets that are decoded with that map.

Build stamp "GeckoIE v0.1 Alpha N (build YYMMDD.N)": replace the old stamp in docobj.cpp (GIE_VERSION), GeckoHost.c (GIE_BUILD), GeckoIE.nsi,
installer/payload/build.txt. YYMMDD = date of the day; .N counts that day's builds from 0.
