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
static char* g_args[8]; static int g_argc; static BootstrapConfig g_cfg;
static HANDLE g_thread; static int g_started;

static void say(const char* fmt, ...)
{
    char buf[1024]; FILE* f; va_list ap;
    va_start(ap, fmt); _vsnprintf(buf, sizeof(buf) - 1, fmt, ap); buf[sizeof(buf) - 1] = 0; va_end(ap);
    f = fopen(g_logPath, "a");
    if (f) { fprintf(f, "%s\n", buf); fclose(f); }
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

/* GeckoIE engine windows (title contains "GeckoIE") are docked into Internet Explorer; keep them from ever being
   shown as top-level windows. Once docked they have a parent, so this no longer applies to them. */
static int isEngineTop(HWND h)
{
    char cls[64], title[200];
    if (GetParent(h)) return 0;
    if (!GetClassNameA(h, cls, sizeof(cls)) || strcmp(cls, "MozillaWindowClass")) return 0;
    title[0] = 0; GetWindowTextA(h, title, sizeof(title));
    return strstr(title, "GeckoIE") != NULL;
}
static LRESULT CALLBACK wpHook(int code, WPARAM w, LPARAM l)
{
    if (code >= 0) {
        const CWPSTRUCT* m = (const CWPSTRUCT*)l;
        if (m->message == WM_WINDOWPOSCHANGING) {
            WINDOWPOS* wp = (WINDOWPOS*)m->lParam;
            if (wp && (wp->flags & SWP_SHOWWINDOW) && isEngineTop(m->hwnd)) wp->flags &= ~SWP_SHOWWINDOW;
        } else if (m->message == WM_WINDOWPOSCHANGED) {
            const WINDOWPOS* wp = (const WINDOWPOS*)m->lParam;
            if (wp && (wp->flags & SWP_SHOWWINDOW) && isEngineTop(m->hwnd))
                SetWindowPos(m->hwnd, NULL, 0, 0, 0, 0, SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
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

static DWORD WINAPI engineThread(LPVOID u)
{
    int rc;
    HHOOK hk = NULL;
    if (!getenv("GECKOIE_NOHIDE")) hk = SetWindowsHookExA(WH_CALLWNDPROC, wpHook, g_self, GetCurrentThreadId());
    say("engine thread: window hook %s", hk ? "installed" : "off");
    say("engine thread: XRE_main");
    rc = g_legacyMain ? g_legacyMain(g_argc, g_args, g_appData, 0) : g_main(g_boot, g_argc, g_args, &g_cfg);
    say("engine thread: XRE_main returned %d", rc);
    if (g_marker[0]) DeleteFileA(g_marker);
    if (hk) UnhookWindowsHookEx(hk);
    return 0;
}

__declspec(dllexport) int __cdecl GH_IsRunning(void)
{
    return g_thread && WaitForSingleObject(g_thread, 0) == WAIT_TIMEOUT;
}

__declspec(dllexport) int __cdecl GH_Start(const char* engine, const char* appDir, const char* profile, int port, const char* token)
{
    char path[MAX_PATH * 2], build[64], num[16]; size_t n; HMODULE glue, xul; GetBootstrapFn gb; void** vt;
    if (g_started) return 1;
    strncpy(g_engine, engine, MAX_PATH - 1);
    n = strlen(g_engine); while (n > 3 && (g_engine[n - 1] == '\\' || g_engine[n - 1] == '/')) g_engine[--n] = 0;
    strncpy(g_profile, profile, sizeof(g_profile) - 1);
    say("---- GeckoIE v0 build 261024.3 GH_Start engine=%s app=%s profile=%s port=%d", g_engine, appDir, profile, port);

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

    preloadAll();
    _snprintf(path, sizeof(path) - 1, "%s\\mozglue.dll", g_engine);
    glue = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!glue) { say("mozglue.dll failed (error %lu)", GetLastError()); return 4; }
    _snprintf(path, sizeof(path) - 1, "%s\\xul.dll", g_engine);
    xul = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
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
    g_args[g_argc] = NULL;
    g_cfg.appData = NULL; g_cfg.appDataPath = g_ini;

    g_thread = CreateThread(NULL, 0, engineThread, NULL, 0, NULL);
    if (!g_thread) return 9;
    g_started = 1;
    if (g_marker[0]) { HANDLE mt = CreateThread(NULL, 0, markerThread, NULL, 0, NULL); if (mt) CloseHandle(mt); }
    say("engine thread started");
    return 0;
}
