/*
 * GeckoHost.dll: runs the Mypal engine inside the current process (for example iexplore.exe)
 * on its own thread, without firefox.exe. Written for Mypal 74.1.4 (checked at start).
 *
 *   int GH_Start(engineDir, appDir, profileDir, port, token)   0 = started, other = error code
 *   int GH_IsRunning(void)                                     1 while the engine thread runs
 */
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#define EXPECTED_BUILD "20251116213814"
#define SLOT_XRE_MAIN 6

typedef struct BootstrapConfig { const void* appData; const char* appDataPath; } BootstrapConfig;
typedef void (__cdecl *GetBootstrapFn)(void** holder);
typedef int (__attribute__((thiscall)) *XreMainFn)(void* self, int argc, char** argv, const BootstrapConfig* cfg);
typedef DWORD (WINAPI *GmfnW)(HMODULE, LPWSTR, DWORD);
typedef DWORD (WINAPI *GmfnA)(HMODULE, LPSTR, DWORD);
typedef BOOL (WINAPI *SetDllDirectoryAFn)(LPCSTR);

static HMODULE g_self;
static char g_logPath[MAX_PATH * 2];
static char g_engine[MAX_PATH], g_fakeA[MAX_PATH], g_ini[MAX_PATH * 2], g_profile[MAX_PATH * 2];
static wchar_t g_fakeW[MAX_PATH];
static GmfnW realW; static GmfnA realA;
typedef int (__cdecl *LegacyMainFn)(int, char**, const void*, unsigned int);
typedef long (__cdecl *GetFileFn)(const char*, void**);
typedef long (__cdecl *CreateAppDataFn)(void*, void**);
static LegacyMainFn g_legacyMain; static void* g_appData;
static void* g_boot; static XreMainFn g_main;
#define GIE_BUILD "v0.1 Alpha 3 (build 261101.2)"
static char* g_args[8]; static int g_argc; static BootstrapConfig g_cfg;
static HANDLE g_thread; static int g_started;

static void say(const char* fmt, ...)
{
    char buf[1024]; FILE* f; va_list ap;
    va_start(ap, fmt); _vsnprintf(buf, sizeof(buf) - 1, fmt, ap); buf[sizeof(buf) - 1] = 0; va_end(ap);
    f = fopen(g_logPath, "a");
    if (f) { SYSTEMTIME st; GetLocalTime(&st); fprintf(f, "%02d:%02d:%02d.%03d  %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf); fclose(f); }
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD why, LPVOID r)
{
    if (why == DLL_PROCESS_ATTACH) {
        char p[MAX_PATH], *s;
        g_self = h;
        DisableThreadLibraryCalls(h);
        GetModuleFileNameA(h, p, MAX_PATH);
        s = strrchr(p, '\\'); if (s) *s = 0;
        _snprintf(g_logPath, sizeof(g_logPath) - 1, "%s\\gh-log.txt", p);
    }
    return TRUE;
}

static int fileExists(const char* p) { DWORD a = GetFileAttributesA(p); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }

static void preload(const char* name)
{
    char full[MAX_PATH * 2];
    _snprintf(full, sizeof(full) - 1, "%s\\%s", g_engine, name); full[sizeof(full) - 1] = 0;
    if (fileExists(full) && !LoadLibraryExA(full, NULL, LOAD_WITH_ALTERED_SEARCH_PATH))
        say("preload failed: %s (error %lu)", name, GetLastError());
}

static void preloadAll(void)
{
    SetDllDirectoryAFn sdd = (SetDllDirectoryAFn)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetDllDirectoryA");
    WIN32_FIND_DATAA fd; char pat[MAX_PATH * 2]; HANDLE h;
    if (sdd) sdd(g_engine);
    preload("ucrtbase.dll"); preload("vcruntime140.dll"); preload("msvcp140.dll"); preload("concrt140.dll");
    _snprintf(pat, sizeof(pat) - 1, "%s\\api-ms-win-*.dll", g_engine); pat[sizeof(pat) - 1] = 0;
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) { do { preload(fd.cFileName); } while (FindNextFileA(h, &fd)); FindClose(h); }
}

static DWORD WINAPI hookW(HMODULE h, LPWSTR b, DWORD n)
{
    if (h == NULL) { DWORD l = (DWORD)lstrlenW(g_fakeW); if (!n) return 0; if (l >= n) l = n - 1; memcpy(b, g_fakeW, l * 2); b[l] = 0; return l; }
    return realW(h, b, n);
}
static DWORD WINAPI hookA(HMODULE h, LPSTR b, DWORD n)
{
    if (h == NULL) { DWORD l = (DWORD)lstrlenA(g_fakeA); if (!n) return 0; if (l >= n) l = n - 1; memcpy(b, g_fakeA, l); b[l] = 0; return l; }
    return realA(h, b, n);
}

static int patchModule(HMODULE mod, FARPROC from, FARPROC to)
{
    BYTE* base = (BYTE*)mod;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR* imp; int n = 0;
    if (!d.VirtualAddress) return 0;
    for (imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + d.VirtualAddress); imp->Name; imp++) {
        IMAGE_THUNK_DATA* t = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; t->u1.Function; t++)
            if ((FARPROC)t->u1.Function == from) {
                DWORD old; VirtualProtect(&t->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
                t->u1.Function = (DWORD)(DWORD_PTR)to;
                VirtualProtect(&t->u1.Function, sizeof(void*), old, &old); n++;
            }
    }
    return n;
}

/* Only the engine's own modules are patched, never the host program's. */
static void installHook(void)
{
    HMODULE k32 = GetModuleHandleA("kernel32.dll"); HANDLE snap; MODULEENTRY32 me; size_t len = strlen(g_engine); int total = 0;
    _snprintf(g_fakeA, sizeof(g_fakeA) - 1, "%s\\firefox.exe", g_engine);
    if (!fileExists(g_fakeA)) _snprintf(g_fakeA, sizeof(g_fakeA) - 1, "%s\\mypal.exe", g_engine);
    if (!fileExists(g_fakeA)) _snprintf(g_fakeA, sizeof(g_fakeA) - 1, "%s\\ieengine.exe", g_engine);
    MultiByteToWideChar(CP_ACP, 0, g_fakeA, -1, g_fakeW, MAX_PATH);
    realW = (GmfnW)GetProcAddress(k32, "GetModuleFileNameW"); realA = (GmfnA)GetProcAddress(k32, "GetModuleFileNameA");
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;
    me.dwSize = sizeof(me);
    if (Module32First(snap, &me)) do {
        if (_strnicmp(me.szExePath, g_engine, len) == 0)
            total += patchModule(me.hModule, (FARPROC)realW, (FARPROC)hookW) + patchModule(me.hModule, (FARPROC)realA, (FARPROC)hookA);
    } while (Module32Next(snap, &me));
    CloseHandle(snap);
    say("hook: %d call(s) patched", total);
}

/* GeckoIE engine windows (their title contains "GeckoIE") are docked into Internet Explorer. While GeckoIE hosts them
   (HostMode=docobj) every new top-level engine window is created fully transparent, without a taskbar button and
   without ever being activated, and is never shown: nothing can flash and Internet Explorer keeps the focus. The
   host removes these marks when it docks the window. A window that turns out not to be an engine window (the
   developer tools, for example) becomes a normal window again the moment it is about to be shown. */
static int g_q = 0;
static const char* PROP_Q = "gie_q";
static int isMozTop(HWND h)
{
    char cls[64];
    if (GetParent(h)) return 0;
    return GetClassNameA(h, cls, sizeof(cls)) && !strcmp(cls, "MozillaWindowClass");
}
static int isEngineTitle(HWND h)
{
    char t[200]; t[0] = 0; GetWindowTextA(h, t, sizeof(t));
    return strstr(t, "GeckoIE") != NULL;
}
static void unquarantine(HWND h)
{
    LONG ex = GetWindowLongA(h, GWL_EXSTYLE);
    RemovePropA(h, PROP_Q);
    SetWindowLongA(h, GWL_EXSTYLE, ex & ~(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE));
}
static LRESULT CALLBACK wpHook(int code, WPARAM w, LPARAM l)
{
    if (code >= 0) {
        const CWPSTRUCT* m = (const CWPSTRUCT*)l;
        if (m->message == WM_CREATE) {
            const CREATESTRUCTA* cs = (const CREATESTRUCTA*)m->lParam;
            if (g_q && cs && !cs->hwndParent && (cs->style & WS_CAPTION) == WS_CAPTION && isMozTop(m->hwnd)) {
                LONG ex = GetWindowLongA(m->hwnd, GWL_EXSTYLE);
                SetPropA(m->hwnd, PROP_Q, (HANDLE)1);
                SetWindowLongA(m->hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
                SetLayeredWindowAttributes(m->hwnd, 0, 0, LWA_ALPHA);
            }
        } else if (m->message == WM_WINDOWPOSCHANGING) {
            WINDOWPOS* wp = (WINDOWPOS*)m->lParam;
            if (wp && (wp->flags & SWP_SHOWWINDOW)) {
                if (GetPropA(m->hwnd, PROP_Q)) {
                    if (isEngineTitle(m->hwnd)) wp->flags = (wp->flags & ~SWP_SHOWWINDOW) | SWP_NOACTIVATE;
                    else unquarantine(m->hwnd);
                } else if (!g_q && isMozTop(m->hwnd) && isEngineTitle(m->hwnd)) wp->flags &= ~SWP_SHOWWINDOW;
            }
        } else if (m->message == WM_WINDOWPOSCHANGED) {
            const WINDOWPOS* wp = (const WINDOWPOS*)m->lParam;
            if (wp && (wp->flags & SWP_SHOWWINDOW) && isMozTop(m->hwnd) && isEngineTitle(m->hwnd) && (g_q ? GetPropA(m->hwnd, PROP_Q) != NULL : 1))
                SetWindowPos(m->hwnd, NULL, 0, 0, 0, 0, SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    return CallNextHookEx(NULL, code, w, l);
}
static LRESULT CALLBACK cbtHook(int code, WPARAM w, LPARAM l)
{
    if (code == HCBT_ACTIVATE && GetPropA((HWND)w, PROP_Q)) return 1;   /* a hidden engine window must never take the focus */
    return CallNextHookEx(NULL, code, w, l);
}

/* Settings the installer writes to <install>\ieb.ini (the folder above the engine folder). */
static int iniInt(const char* key, int def)
{
    char p[MAX_PATH * 2], line[300]; FILE* f; size_t n, kl = strlen(key); int v = def;
    strncpy(p, g_engine, sizeof(p) - 20); p[sizeof(p) - 20] = 0; n = strlen(p);
    while (n > 0 && p[n - 1] != '\\' && p[n - 1] != '/') n--;
    if (n == 0) return def;
    p[n - 1] = 0; strcat(p, "\\ieb.ini");
    f = fopen(p, "r"); if (!f) return def;
    while (fgets(line, sizeof(line), f)) {
        char* l = line; while (*l == ' ' || *l == '\t') l++;
        if (!_strnicmp(l, key, kl) && l[kl] == '=') v = atoi(l + kl + 1);
    }
    fclose(f); return v;
}

static char g_marker[MAX_PATH * 2];
static DWORD WINAPI markerThread(LPVOID u) { Sleep(25000); DeleteFileA(g_marker); return 0; }   /* the engine survived its first 25 s */
static int engMajor(void)
{
    char p[MAX_PATH * 2], v[32]; v[0] = 0;
    _snprintf(p, sizeof(p) - 1, "%s\\application.ini", g_engine); p[sizeof(p) - 1] = 0;
    GetPrivateProfileStringA("App", "Version", "", v, sizeof(v), p);
    return atoi(v);
}

static int userSetting(const char* key, int def)   /* the per-user settings.ini written by the Settings window */
{
    char p[MAX_PATH * 2], up[MAX_PATH], line[300]; FILE* f = NULL; size_t kl = strlen(key); int v = def;
    if (GetEnvironmentVariableA("USERPROFILE", up, sizeof up)) { _snprintf(p, sizeof p - 1, "%s\\AppData\\LocalLow\\GeckoIE\\settings.ini", up); p[sizeof p - 1] = 0; f = fopen(p, "r"); }
    if (!f && GetEnvironmentVariableA("APPDATA", up, sizeof up)) { _snprintf(p, sizeof p - 1, "%s\\GeckoIE\\settings.ini", up); p[sizeof p - 1] = 0; f = fopen(p, "r"); }
    if (!f) return def;
    while (fgets(line, sizeof(line), f)) { char* l = line; while (*l == ' ' || *l == '\t') l++; if (!_strnicmp(l, key, kl) && l[kl] == '=') v = atoi(l + kl + 1); }
    fclose(f); return v;
}
static int iniIs(const char* key, const char* want)
{
    char p[MAX_PATH * 2], line[300]; FILE* f; size_t n, kl = strlen(key); int yes = 0;
    strncpy(p, g_engine, sizeof(p) - 20); p[sizeof(p) - 20] = 0; n = strlen(p);
    while (n > 0 && p[n - 1] != '\\' && p[n - 1] != '/') n--;
    if (n == 0) return 0;
    p[n - 1] = 0; strcat(p, "\\ieb.ini");
    f = fopen(p, "r"); if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
        char* l = line; char* e; while (*l == ' ' || *l == '\t') l++;
        if (!_strnicmp(l, key, kl) && l[kl] == '=') { l += kl + 1; e = l + strlen(l); while (e > l && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0; yes = !_stricmp(l, want); }
    }
    fclose(f); return yes;
}

/* Reading the engine's files front to back once is much faster on a slow disk than letting Windows fetch them page by page
   while xul.dll loads (firefox.exe does the same; this host replaces firefox.exe). A file that comes out of the cache at
   once (4 MB in under 60 ms) is not read further. */
static void readAheadFile(const char* path, char* buf, DWORD cap)
{
    DWORD got, t0, chunks = 0; HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    t0 = GetTickCount();
    while (ReadFile(f, buf, cap, &got, NULL) && got) { if (++chunks == 4 && GetTickCount() - t0 < 60) break; }
    CloseHandle(f);
}
static void readAhead(void)
{
    DWORD t0 = GetTickCount(); char* buf = (char*)VirtualAlloc(NULL, 1 << 20, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    WIN32_FIND_DATAA fd; char pat[MAX_PATH * 2], full[MAX_PATH * 2]; HANDLE h;
    if (!buf) return;
    _snprintf(full, sizeof(full) - 1, "%s\\xul.dll", g_engine); full[sizeof(full) - 1] = 0; readAheadFile(full, buf, 1 << 20);
    _snprintf(full, sizeof(full) - 1, "%s\\omni.ja", g_engine); full[sizeof(full) - 1] = 0; readAheadFile(full, buf, 1 << 20);
    _snprintf(pat, sizeof(pat) - 1, "%s\\*.dll", g_engine); pat[sizeof(pat) - 1] = 0;
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!_strnicmp(fd.cFileName, "api-ms-win", 10) || !_stricmp(fd.cFileName, "xul.dll")) continue;
            _snprintf(full, sizeof(full) - 1, "%s\\%s", g_engine, fd.cFileName); full[sizeof(full) - 1] = 0; readAheadFile(full, buf, 1 << 20);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    _snprintf(pat, sizeof(pat) - 1, "%s\\startupCache\\*", g_profile); pat[sizeof(pat) - 1] = 0;
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            _snprintf(full, sizeof(full) - 1, "%s\\startupCache\\%s", g_profile, fd.cFileName); full[sizeof(full) - 1] = 0; readAheadFile(full, buf, 1 << 20);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    VirtualFree(buf, 0, MEM_RELEASE);
    say("read-ahead of the engine files took %lu ms", GetTickCount() - t0);
}

/* Everything that used to run inside GH_Start on Internet Explorer's own thread (and so held IE's window back while the engine
   loaded) now runs on the engine thread. A failure is logged here; the add-on notices it because GH_IsRunning turns false. */
static unsigned long fpFile(unsigned long h, const char* path)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (GetFileAttributesExA(path, GetFileExInfoStandard, &d)) { h = h * 33 + d.nFileSizeLow; h = h * 33 + d.ftLastWriteTime.dwLowDateTime; }
    return h;
}
static unsigned long fpDir(unsigned long h, const char* dir)
{
    WIN32_FIND_DATAA fd; char pat[MAX_PATH * 2]; HANDLE f;
    _snprintf(pat, sizeof(pat) - 1, "%s\\*", dir); pat[sizeof(pat) - 1] = 0;
    f = FindFirstFileA(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return h;
    do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { h = h * 33 + fd.nFileSizeLow; h = h * 33 + fd.ftLastWriteTime.dwLowDateTime; } } while (FindNextFileA(f, &fd));
    FindClose(f); return h;
}
static void makeFingerprint(char* out, size_t n)   /* changes when the engine's scripts, preferences or omni.ja change */
{
    char app[MAX_PATH * 2], p[MAX_PATH * 2], *sl; unsigned long h = 5381;
    strncpy(app, g_ini, sizeof(app) - 1); app[sizeof(app) - 1] = 0; sl = strrchr(app, '\\'); if (sl) *sl = 0;
    _snprintf(p, sizeof(p) - 1, "%s\\content", app); p[sizeof(p) - 1] = 0; h = fpDir(h, p);
    h = fpDir(h, app);
    _snprintf(p, sizeof(p) - 1, "%s\\defaults\\preferences", app); p[sizeof(p) - 1] = 0; h = fpDir(h, p);
    _snprintf(p, sizeof(p) - 1, "%s\\omni.ja", g_engine); p[sizeof(p) - 1] = 0; h = fpFile(h, p);
    _snprintf(p, sizeof(p) - 1, "%s\\browser\\omni.ja", g_engine); p[sizeof(p) - 1] = 0; h = fpFile(h, p);
    _snprintf(out, n, "fp-%08lx", h);
}
/* xul.dll (the engine, tens of megabytes) is linked for one fixed address. If that address is already in use when it is loaded
   it has to be relocated: Windows then reads the WHOLE file from disk (6-15 s on a cold slow disk) and turns every page
   with an address in it into private memory (tens of megabytes of RAM). Keeping the address range free until xul.dll is
   loaded lets Windows map it where it wants to be, and read only the pages that are really used. */
static void* g_xulWant = NULL; static void* g_xulRsv = NULL;
static void* reserveImageBase(const char* dll)
{
    HANDLE f; unsigned char hdr[4096]; DWORD got = 0; IMAGE_DOS_HEADER* dos; IMAGE_NT_HEADERS* nt; void* p; void* base; DWORD size;
    f = CreateFileA(dll, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    ReadFile(f, hdr, sizeof(hdr), &got, NULL); CloseHandle(f);
    dos = (IMAGE_DOS_HEADER*)hdr;
    if (got < 512 || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 3000) return NULL;
    nt = (IMAGE_NT_HEADERS*)(hdr + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return NULL;
    base = (void*)(ULONG_PTR)nt->OptionalHeader.ImageBase; size = nt->OptionalHeader.SizeOfImage; g_xulWant = base;
    p = VirtualAlloc(base, size, MEM_RESERVE, PAGE_NOACCESS);
    if (p) say("xul.dll is linked for address %p (%lu KB): kept free for it", base, size / 1024);
    else {
        MEMORY_BASIC_INFORMATION mbi; char who[MAX_PATH]; who[0] = 0;
        if (VirtualQuery(base, &mbi, sizeof(mbi)) && mbi.AllocationBase) { if (!GetModuleFileNameA((HMODULE)mbi.AllocationBase, who, sizeof(who))) lstrcpyA(who, "(not a module)"); }
        say("xul.dll is linked for address %p (%lu KB) but that range is taken by %s: it will be relocated", base, size / 1024, who);
    }
    return p;
}
static int loadEngine(void)
{
    char path[MAX_PATH * 2]; HMODULE glue, xul; GetBootstrapFn gb; void** vt;
    { DWORD t0 = GetTickCount(); if (iniInt("ReadAhead", 0)) readAhead(); say("load: read-ahead step done (+%lu ms)", GetTickCount() - t0); }
    { char xp[MAX_PATH * 2]; _snprintf(xp, sizeof(xp) - 1, "%s\\xul.dll", g_engine); xp[sizeof(xp) - 1] = 0; g_xulRsv = reserveImageBase(xp); }
    { DWORD t1 = GetTickCount(); preloadAll(); say("load: runtime DLLs preloaded (+%lu ms)", GetTickCount() - t1); }
    _snprintf(path, sizeof(path) - 1, "%s\\mozglue.dll", g_engine);
    glue = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!glue) { say("mozglue.dll failed (error %lu)", GetLastError()); return 4; }
    _snprintf(path, sizeof(path) - 1, "%s\\xul.dll", g_engine);
    if (g_xulRsv) { VirtualFree(g_xulRsv, 0, MEM_RELEASE); g_xulRsv = NULL; }   /* the address range is free again, right before xul.dll asks for it */
    { DWORD t2 = GetTickCount(); xul = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH); say("load: xul.dll loaded (+%lu ms) at %p%s", GetTickCount() - t2, (void*)xul, (g_xulWant && (void*)xul == g_xulWant) ? " (its own address: no relocation)" : " (relocated)"); }
    if (!xul) { say("xul.dll failed (error %lu)", GetLastError()); return 5; }
    gb = (GetBootstrapFn)GetProcAddress(xul, "XRE_GetBootstrap");
    if (gb) {
        int slot = iniInt("BootstrapSlot", 0), maj = engMajor();
        if (!slot) slot = (maj >= 66 || maj == 0) ? SLOT_XRE_MAIN : 4;   /* NS_LogInit/NS_LogTerm joined the interface in Firefox 66 */
        gb(&g_boot);
        if (!g_boot) return 7;
        vt = *(void***)g_boot;
        g_main = (XreMainFn)vt[slot];
        say("engine API: bootstrap, XRE_main in slot %d (engine version %d); vtable %p %p %p %p %p %p %p %p", slot, maj, vt[0], vt[1], vt[2], vt[3], vt[4], vt[5], vt[6], vt[7]);
    } else {
        void* iniFile = NULL;   /* engines before Firefox 62 export XRE_main directly */
        GetFileFn gf = (GetFileFn)GetProcAddress(xul, "XRE_GetFileFromPath");
        CreateAppDataFn ca = (CreateAppDataFn)GetProcAddress(xul, "XRE_CreateAppData");
        g_legacyMain = (LegacyMainFn)GetProcAddress(xul, "XRE_main");
        if (!g_legacyMain || !gf || !ca) { say("engine has neither XRE_GetBootstrap nor the legacy XRE_main entry points"); return 6; }
        if (gf(g_ini, &iniFile) < 0 || !iniFile || ca(iniFile, &g_appData) < 0 || !g_appData) { say("could not read application.ini through the engine"); return 10; }
        say("engine API: legacy XRE_main");
    }

    installHook();
    g_argc = 0;
    g_args[g_argc++] = g_fakeA;
    g_args[g_argc++] = "-no-remote";
    g_args[g_argc++] = "-profile";
    g_args[g_argc++] = g_profile;
    {   /* after an update the engine must not reuse scripts it compiled for the previous build */
        char mk[MAX_PATH * 2 + 32], cur[96], fp[32]; DWORD n = 0; HANDLE f; makeFingerprint(fp, sizeof fp);
        lstrcpyA(mk, g_profile); lstrcatA(mk, "\\geckoie-build.txt"); cur[0] = 0;
        f = CreateFileA(mk, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) { ReadFile(f, cur, sizeof cur - 1, &n, NULL); cur[n] = 0; CloseHandle(f); }
        if (strcmp(cur, fp)) {
            g_args[g_argc++] = "-purgecaches"; say("engine files changed (was '%s', now '%s'): the engine caches are purged once", cur, fp);
            f = CreateFileA(mk, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
            if (f != INVALID_HANDLE_VALUE) { WriteFile(f, fp, (DWORD)strlen(fp), &n, NULL); CloseHandle(f); }
        }
    }
    g_args[g_argc] = NULL;
    g_cfg.appData = NULL; g_cfg.appDataPath = g_ini;
    return 0;
}

static DWORD WINAPI engineThread(LPVOID u)
{
    int rc;
    HHOOK hk = NULL, hk2 = NULL;
    rc = loadEngine();
    if (rc) { say("engine could not be loaded (code %d); the add-on goes back to Internet Explorer's own rendering", rc); return 0; }
    g_q = iniIs("HostMode", "docobj");
    if (!getenv("GECKOIE_NOHIDE")) {
        hk = SetWindowsHookExA(WH_CALLWNDPROC, wpHook, g_self, GetCurrentThreadId());
        hk2 = SetWindowsHookExA(WH_CBT, cbtHook, g_self, GetCurrentThreadId());
    }
    say("engine thread: window hooks %s (invisible windows: %s)", (hk && hk2) ? "installed" : "NOT all installed", g_q ? "yes" : "no");
    say("engine thread: XRE_main starts");
    rc = g_legacyMain ? g_legacyMain(g_argc, g_args, g_appData, 0) : g_main(g_boot, g_argc, g_args, &g_cfg);
    say("engine thread: XRE_main returned %d", rc);
    if (g_marker[0]) DeleteFileA(g_marker);
    if (hk) UnhookWindowsHookEx(hk);
    if (hk2) UnhookWindowsHookEx(hk2);
    return 0;
}

__declspec(dllexport) int __cdecl GH_IsRunning(void)
{
    return g_thread && WaitForSingleObject(g_thread, 0) == WAIT_TIMEOUT;
}

__declspec(dllexport) int __cdecl GH_Start(const char* engine, const char* appDir, const char* profile, int port, const char* token)
{
    char path[MAX_PATH * 2], build[64], num[16]; size_t n;
    if (g_started) return 1;
    strncpy(g_engine, engine, MAX_PATH - 1);
    n = strlen(g_engine); while (n > 3 && (g_engine[n - 1] == '\\' || g_engine[n - 1] == '/')) g_engine[--n] = 0;
    strncpy(g_profile, profile, sizeof(g_profile) - 1);
    say("---- GeckoIE " GIE_BUILD " GH_Start engine=%s app=%s profile=%s port=%d", g_engine, appDir, profile, port);

    _snprintf(path, sizeof(path) - 1, "%s\\xul.dll", g_engine);
    if (!fileExists(path)) return 2;
    _snprintf(path, sizeof(path) - 1, "%s\\platform.ini", g_engine);
    build[0] = 0; GetPrivateProfileStringA("Build", "BuildID", "", build, sizeof(build), path);
    g_marker[0] = 0;
    if (strcmp(build, EXPECTED_BUILD) != 0) {
        /* another engine than the tested Mypal 74.1.4: try it, but if a start crashes the process, stay off next time */
        char* sl;
        say("untested engine build '%s' (version %d): experimental start", build, engMajor());
        strncpy(g_marker, g_engine, sizeof(g_marker) - 20); g_marker[sizeof(g_marker) - 20] = 0;
        sl = strrchr(g_marker, '\\'); if (sl) *sl = 0;
        strcat(g_marker, "\\gh-start.flag");
        if (fileExists(g_marker)) { say("the previous experimental start did not finish; the in-process engine stays off. Delete %s to try again.", g_marker); return 11; }
        { HANDLE mf = CreateFileA(g_marker, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL); if (mf != INVALID_HANDLE_VALUE) CloseHandle(mf); }
    }
    _snprintf(g_ini, sizeof(g_ini) - 1, "%s\\application.ini", appDir);
    if (!fileExists(g_ini)) { say("missing %s", g_ini); return 8; }

    _snprintf(num, sizeof(num) - 1, "%d", port);
    SetEnvironmentVariableA("IEBRIDGE_PORT", num);
    SetEnvironmentVariableA("IEBRIDGE_TOKEN", token);
    SetEnvironmentVariableA("IEBRIDGE_KIOSK", "1");
    SetEnvironmentVariableA("IEBRIDGE_NOIDLE", "1");
    SetEnvironmentVariableA("GECKOIE_BRIDGE", "1");
    SetEnvironmentVariableA("IEBRIDGE_BLOCKADS", userSetting("BlockAds", 1) ? "1" : "0");


    g_thread = CreateThread(NULL, 0, engineThread, NULL, 0, NULL);
    if (!g_thread) return 9;
    g_started = 1;
    if (g_marker[0]) { HANDLE mt = CreateThread(NULL, 0, markerThread, NULL, 0, NULL); if (mt) CloseHandle(mt); }
    say("engine thread started");
    return 0;
}
