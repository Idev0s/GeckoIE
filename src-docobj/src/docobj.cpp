// GeckoIE DocObject "hello" experiment: minimal ActiveX Document server for IE6.
// Goal: learn exactly how IE6 hosts a DocObject and what it asks for (everything is logged).
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <ole2.h>
#include <olectl.h>
#include <docobj.h>
#include <servprov.h>
#include <urlmon.h>
#include <exdisp.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>
#include <ctype.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#define GIE_VERSION "v0 build 261024.3"
#define GIE_VERSION_W L"v0 build 261024.3"
#include <stdlib.h>

// No libstdc++/winpthread: they import GetThreadId (Vista+), which breaks loading on XP.
void* operator new(size_t n) { return HeapAlloc(GetProcessHeap(), 0, n); }
void operator delete(void* p) { if (p) HeapFree(GetProcessHeap(), 0, p); }
void operator delete(void* p, size_t) { if (p) HeapFree(GetProcessHeap(), 0, p); }
extern "C" void __cxa_pure_virtual() { ExitProcess(1); }

static const GUID CLSID_GeckoDoc = {0x6c1f4a7e,0x3b52,0x4d19,{0x9e,0x08,0x5a,0xb7,0x41,0xc3,0xd2,0x6f}};
#define CLSID_STR L"{6C1F4A7E-3B52-4D19-9E08-5AB741C3D26F}"
#define MIME_STR  L"application/x-geckoie"
#define EXT_STR   L".gieh"
#define PROGID    L"GeckoIE.DocObject.1"
static const GUID IID_IPM    = {0x79eac9c9,0xbaf9,0x11ce,{0x8c,0x82,0x00,0xaa,0x00,0x4b,0xa9,0x0b}};
static const GUID IID_IWB2   = {0xD30C1661,0xCDAF,0x11D0,{0x8A,0x3E,0x00,0xC0,0x4F,0xC9,0xE2,0x6E}};
static const GUID IID_IWBApp = {0x0002DF05,0x0000,0x0000,{0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};

static HINSTANCE g_hinst; static LONG g_objs = 0, g_locks = 0; static char g_log[MAX_PATH];

static void Log(const char* fmt, ...) {
  FILE* f = fopen(g_log, "a"); if (!f) return;
  static int cnt = 0;
  if ((++cnt & 127) == 0) { fseek(f, 0, SEEK_END); if (ftell(f) > 1000000) { fclose(f); char old[MAX_PATH + 8]; snprintf(old, sizeof old, "%s.old", g_log); DeleteFileA(old); MoveFileA(g_log, old); f = fopen(g_log, "a"); if (!f) return; } }
  fprintf(f, "%8lu t%lu ", GetTickCount(), GetCurrentThreadId());
  va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
  fputc('\n', f); fclose(f);
}
static void IidName(REFIID iid, char* out, size_t n) {
  wchar_t g[64], key[128], name[128]; HKEY k; DWORD cb = sizeof(name);
  StringFromGUID2(iid, g, 64); name[0] = 0;
  wsprintfW(key, L"Interface\\%s", g);
  if (RegOpenKeyExW(HKEY_CLASSES_ROOT, key, 0, KEY_READ, &k) == 0) {
    if (RegQueryValueExW(k, NULL, NULL, NULL, (BYTE*)name, &cb) != 0) name[0] = 0;
    name[127] = 0; RegCloseKey(k);
  }
  snprintf(out, n, "%ls %ls", g, name);
}


#define CMD_REFRESH 22
#define CMD_STOP 23
#define CMD_SETPROGRESSTEXT 27
#define CMD_SETTITLE 28
#define CMD_SETDOWNLOADSTATE 29

static char g_suffix[64] = "Microsoft Internet Explorer";
static char g_ctxLayout[2300] = "";
static wchar_t g_dllDir[MAX_PATH];
static int g_ieMajor = 6, g_osMajor = 5; static volatile int g_devtools = 0, g_dlMode = 0;   // downloads: 0 = Internet Explorer, 1 = engine
static void DetectEnv() {
  OSVERSIONINFOW vi; memset(&vi, 0, sizeof vi); vi.dwOSVersionInfoSize = sizeof vi; if (GetVersionExW(&vi)) g_osMajor = (int)vi.dwMajorVersion;
  HKEY k; char v[64]; DWORD cb = sizeof v, ty = 0; v[0] = 0;
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\Microsoft\\Internet Explorer", 0, KEY_READ, &k) == 0) {
    if (RegQueryValueExA(k, "svcVersion", NULL, &ty, (BYTE*)v, &cb) != 0) { cb = sizeof v; RegQueryValueExA(k, "Version", NULL, &ty, (BYTE*)v, &cb); }
    RegCloseKey(k);
  }
  v[sizeof v - 1] = 0; if (atoi(v) > 0) g_ieMajor = atoi(v);
}
static const char* DefaultSuffix() { return g_ieMajor >= 11 ? "Internet Explorer" : (g_ieMajor >= 7 ? "Windows Internet Explorer" : "Microsoft Internet Explorer"); }
// Vista and later: Protected Mode IE can only write to LocalLow, so user settings live there.
static bool LocalLowFile(char* out, size_t n, bool create) {   // per-user settings file (LocalLow on Vista+, %APPDATA% before)
  char d[MAX_PATH + 40];
  if (g_osMajor >= 6) {
    char up[MAX_PATH]; if (!GetEnvironmentVariableA("USERPROFILE", up, sizeof up)) return false;
    if (create) { char t[MAX_PATH + 40]; snprintf(t, sizeof t, "%s\\AppData\\LocalLow", up); CreateDirectoryA(t, NULL); }
    snprintf(d, sizeof d, "%s\\AppData\\LocalLow\\GeckoIE", up);
  } else {
    char ad[MAX_PATH]; if (!GetEnvironmentVariableA("APPDATA", ad, sizeof ad)) return false;
    snprintf(d, sizeof d, "%s\\GeckoIE", ad);
  }
  if (create) CreateDirectoryA(d, NULL);
  snprintf(out, n, "%s\\settings.ini", d); return true;
}
static volatile int g_home = 0;   // 0 = white blank page (default), 1 = "Loading" screen before the engine is up
static CRITICAL_SECTION g_cs; static int g_port = 0; static bool g_owned = false, g_everStarted = false;
static LONG g_sessions = 0; static HMODULE g_gh = 0;
typedef int (__cdecl *GHStartFn)(const char*, const char*, const char*, int, const char*);
typedef int (__cdecl *GHRunFn)(void);
static GHStartFn p_start = 0; static GHRunFn p_run = 0;

struct Cfg { char base[MAX_PATH]; char engineDir[MAX_PATH]; char profileDir[MAX_PATH]; char token[64]; int port; };
static char* Trim(char* s) { while (*s && isspace((unsigned char)*s)) s++; char* e = s + strlen(s); while (e > s && isspace((unsigned char)e[-1])) *--e = 0; return s; }
static bool LoadCfg(Cfg* c) {
  memset(c, 0, sizeof *c); g_home = 0; g_ctxLayout[0] = 0; g_devtools = 0; g_dlMode = 0; lstrcpyA(g_suffix, DefaultSuffix()); lstrcpyA(c->engineDir, "engine-mypal"); lstrcpyA(c->token, "dev"); c->port = 38650;
  bool found = false;
  static const char* keys[] = { "Software\\GeckoIE", "Software\\IEBridge" };
  for (int i = 0; i < 2 && !found; i++) {
    HKEY k; DWORD cb = MAX_PATH, ty = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, keys[i], 0, KEY_READ, &k) == 0) {
      if (RegQueryValueExA(k, "InstallDir", NULL, &ty, (BYTE*)c->base, &cb) == 0) found = true;
      RegCloseKey(k);
    }
  }
  if (!found) {   // fall back to where the BHO was registered: <install>\bho\IEBridgeBho.dll
    static const char* bk[] = { "CLSID\\{FC8F2A4B-3C6E-4B53-9D0F-6A1C2E7B5D31}\\InprocServer32",
                                "CLSID\\{FC8F2A4B-3C6E-4B53-9D0F-6A1C2E7B5D31}\\InprocServer32\\1.0.0.0" };
    for (int i = 0; i < 2 && !found; i++) {
      HKEY k; char cbp[1024]; DWORD cb = sizeof cbp, ty = 0;
      if (RegOpenKeyExA(HKEY_CLASSES_ROOT, bk[i], 0, KEY_READ, &k) == 0) {
        if (RegQueryValueExA(k, "CodeBase", NULL, &ty, (BYTE*)cbp, &cb) == 0) {
          char path[MAX_PATH]; DWORD pl = MAX_PATH;
          if (PathCreateFromUrlA(cbp, path, &pl, 0) == S_OK) { PathRemoveFileSpecA(path); PathRemoveFileSpecA(path); lstrcpynA(c->base, path, MAX_PATH); found = true; }
        }
        RegCloseKey(k);
      }
    }
  }
  if (!found) return false;
  c->base[MAX_PATH - 1] = 0; int n = lstrlenA(c->base); while (n > 3 && c->base[n - 1] == '\\') c->base[--n] = 0;
  char ini[MAX_PATH]; PathCombineA(ini, c->base, "ieb.ini"); FILE* f = fopen(ini, "r");
  if (f) {
    char line[2500];
    while (fgets(line, sizeof line, f)) {
      char* l = Trim(line); if (!*l || *l == ';' || *l == '#') continue;
      char* eq = strchr(l, '='); if (!eq) continue; *eq = 0; char* key = Trim(l); char* val = Trim(eq + 1);
      for (char* q = key; *q; ++q) *q = (char)tolower((unsigned char)*q);
      if (!strcmp(key, "enginedir") && *val) lstrcpynA(c->engineDir, val, MAX_PATH);
      else if (!strcmp(key, "profiledir")) lstrcpynA(c->profileDir, val, MAX_PATH);
      else if (!strcmp(key, "token") && *val) lstrcpynA(c->token, val, 64);
      else if (!strcmp(key, "titlesuffix")) lstrcpynA(g_suffix, val, 64);
      else if (!strcmp(key, "contextmenu")) lstrcpynA(g_ctxLayout, val, sizeof g_ctxLayout);
      else if (!strcmp(key, "homescreen")) g_home = !_stricmp(val, "loading") ? 1 : 0;
      else if (!strcmp(key, "devtools")) g_devtools = atoi(val) ? 1 : 0;
      else if (!strcmp(key, "downloads")) g_dlMode = !_stricmp(val, "engine") ? 1 : 0;
      else if (!strcmp(key, "port")) { int p = atoi(val); if (p > 0 && p < 65536) c->port = p; }
    }
    fclose(f);
  }
  {   // settings changed in the Settings window (per user, they win over ieb.ini)
    char ll[MAX_PATH + 40];
    if (LocalLowFile(ll, sizeof ll, false)) {
      FILE* g = fopen(ll, "r");
      if (g) {
        char l2[2500];
        while (fgets(l2, sizeof l2, g)) {
          char* q = Trim(l2); char* eq = strchr(q, '='); if (!eq) continue; *eq = 0; char* key = Trim(q); char* val = Trim(eq + 1);
          for (char* z = key; *z; ++z) *z = (char)tolower((unsigned char)*z);
          if (!strcmp(key, "contextmenu")) lstrcpynA(g_ctxLayout, val, sizeof g_ctxLayout);
          else if (!strcmp(key, "homescreen")) g_home = !_stricmp(val, "loading") ? 1 : 0;
          else if (!strcmp(key, "downloads")) g_dlMode = !_stricmp(val, "engine") ? 1 : 0;
        }
        fclose(g);
      }
    }
  }
  return true;
}
static int FindFreePort(int first) {
  for (int p = first; p < first + 50; p++) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); if (s == INVALID_SOCKET) continue;
    sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons((u_short)p); a.sin_addr.s_addr = inet_addr("127.0.0.1");
    int rc = bind(s, (sockaddr*)&a, sizeof a); closesocket(s); if (rc == 0) return p;
  }
  return first;
}
static void FindProfile(const Cfg* c, char* out) {
  char base[MAX_PATH], tmp[MAX_PATH];
  if (!c->profileDir[0]) PathCombineA(base, c->base, "profile");
  else { ExpandEnvironmentStringsA(c->profileDir, tmp, MAX_PATH); if (PathIsRelativeA(tmp)) PathCombineA(base, c->base, tmp); else lstrcpynA(base, tmp, MAX_PATH); }
  lstrcatA(base, "-inproc");
  for (int i = 1; i <= 8; i++) {
    snprintf(out, MAX_PATH, "%s%d", base, i); CreateDirectoryA(out, NULL);
    char lk[MAX_PATH]; PathCombineA(lk, out, "parent.lock");
    if (GetFileAttributesA(lk) != INVALID_FILE_ATTRIBUTES) {
      HANDLE h = CreateFileA(lk, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
      if (h == INVALID_HANDLE_VALUE) continue; CloseHandle(h);
    }
    return;
  }
  snprintf(out, MAX_PATH, "%s1", base);
}
// Returns the bridge port of the engine running in THIS process (starting it the first time), or 0 with err set.
static int EnsureEngine(const Cfg* c, char* err, size_t en) {
  int result = 0; err[0] = 0; EnterCriticalSection(&g_cs);
  WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
  if (!g_gh) {
    char dll[MAX_PATH]; PathCombineA(dll, c->base, "bho\\GeckoHost.dll"); g_gh = LoadLibraryA(dll);
    if (g_gh) { p_start = (GHStartFn)GetProcAddress(g_gh, "GH_Start"); p_run = (GHRunFn)GetProcAddress(g_gh, "GH_IsRunning"); }
    if (!g_gh || !p_start || !p_run) { snprintf(err, en, "cannot load %s", dll); g_gh = 0; goto done; }
  }
  if (g_port && g_owned && p_run()) { result = g_port; goto done; }
  if (p_run()) { Log("engine already running in this process (BHO) - attaching by PID scan"); result = -1; goto done; }
  if (g_everStarted) { snprintf(err, en, "engine has stopped in this IE process - restart IE"); goto done; }
  {
    char eng[MAX_PATH], app[MAX_PATH], prof[MAX_PATH]; PathCombineA(eng, c->base, c->engineDir); PathCombineA(app, c->base, "bare-app");
    FindProfile(c, prof); int port = FindFreePort(c->port);
    int rc = p_start(eng, app, prof, port, c->token); Log("GH_Start(%s, %s, %s, %d) -> %d", eng, app, prof, port, rc);
    if (rc != 0) { snprintf(err, en, "GH_Start failed (%d), see gh-log.txt", rc); goto done; }
    g_everStarted = true; g_owned = true; g_port = port; result = port;
  }
done:
  LeaveCriticalSection(&g_cs); return result;
}

struct Sess { HWND wnd; int attach; int port; char token[64]; volatile LONG stop; SOCKET s; LONG refs; };
static void SessRelease(Sess* s) { if (InterlockedDecrement(&s->refs) == 0) HeapFree(GetProcessHeap(), 0, s); }
static void HideEngineWnd(const char* line) {
  if (strncmp(line, "HWND ", 5)) return; const char* a = line + 5; long long v;
  if (a[0] == '0' && (a[1] == 'x' || a[1] == 'X')) v = strtoll(a + 2, NULL, 16); else v = strtoll(a, NULL, 10);
  HWND h = (HWND)(INT_PTR)v;
  if (h && IsWindow(h)) SetWindowPos(h, NULL, 0, 0, 0, 0, SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
}
static void PostLine(HWND w, const char* line) {
  HideEngineWnd(line);
  size_t n = strlen(line) + 1; char* p = (char*)HeapAlloc(GetProcessHeap(), 0, n); if (!p) return;
  memcpy(p, line, n); if (!PostMessageW(w, WM_APP + 1, 0, (LPARAM)p)) HeapFree(GetProcessHeap(), 0, p);
}
static DWORD WINAPI Worker(LPVOID p) {
  Sess* ss = (Sess*)p; WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
  SOCKET s = INVALID_SOCKET; DWORD t0 = GetTickCount();
  enum { LINEMAX = 262144 }; char buf[1024]; char* line = (char*)HeapAlloc(GetProcessHeap(), 0, LINEMAX); int ll = 0; const int HN = 16; static char held[16][300]; int nheld = 0;
  if (ss->attach) {   // an engine already runs in this IE process (started by the BHO): find it by its PID
    DWORD me = GetCurrentProcessId(); int sweeps = 0;
    while (s == INVALID_SOCKET && !ss->stop && GetTickCount() - t0 < 120000) {
      for (int pt = ss->port; pt < ss->port + 50 && !ss->stop && s == INVALID_SOCKET; pt++) {
        SOCKET c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons((u_short)pt); a.sin_addr.s_addr = inet_addr("127.0.0.1");
        if (connect(c, (sockaddr*)&a, sizeof a) != 0) { closesocket(c); break; }   // first free port = engine not up yet; stop the sweep (a refused connect costs ~1.5 s on XP)
        DWORD to = 700; setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof to);
        char au[120]; snprintf(au, sizeof au, "AUTH %s\r\n", ss->token); send(c, au, (int)strlen(au), 0);
        bool match = false; int nh = 0; ll = 0; DWORD t1 = GetTickCount();
        while (!match && GetTickCount() - t1 < 2500) {
          int n = recv(c, buf, sizeof buf, 0); if (n <= 0) break;
          for (int i = 0; i < n; i++) {
            char ch = buf[i];
            if (ch == '\n') {
              line[ll] = 0; if (ll > 0 && line[ll - 1] == '\r') line[--ll] = 0;
              if (ll) { if (nh < HN) lstrcpynA(held[nh++], line, 300); if (!strncmp(line, "PID ", 4) && (DWORD)atol(line + 4) == me) match = true; }
              ll = 0;
            } else if (ll < (LINEMAX - 1)) line[ll++] = ch;
          }
        }
        if (match) { s = c; nheld = nh; to = 0; setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof to); Log("attached to the BHO's engine on port %d after %lu ms, %d sweeps", pt, GetTickCount() - t0, sweeps); }
        else { send(c, "QUIT\r\n", 6, 0); closesocket(c); }
      }
      sweeps++; if (s == INVALID_SOCKET) Sleep(100);
    }
  } else {
    while (!ss->stop && GetTickCount() - t0 < 120000) {
      s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
      sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons((u_short)ss->port); a.sin_addr.s_addr = inet_addr("127.0.0.1");
      if (connect(s, (sockaddr*)&a, sizeof a) == 0) break;
      closesocket(s); s = INVALID_SOCKET; Sleep(100);
    }
    if (s != INVALID_SOCKET) Log("connected to our engine after %lu ms", GetTickCount() - t0);
  }
  if (s == INVALID_SOCKET) { if (!ss->stop) PostLine(ss->wnd, "X_CONNECTFAIL"); HeapFree(GetProcessHeap(), 0, line); SessRelease(ss); WSACleanup(); return 0; }
  ss->s = s;
  if (!ss->attach) { char auth[120]; snprintf(auth, sizeof auth, "AUTH %s\r\n", ss->token); send(s, auth, (int)strlen(auth), 0); }
  for (int i = 0; i < nheld; i++) PostLine(ss->wnd, held[i]);
  while (!ss->stop) {
    int n = recv(s, buf, sizeof buf, 0); if (n <= 0) break;
    for (int i = 0; i < n; i++) {
      char ch = buf[i];
      if (ch == '\n') { line[ll] = 0; if (ll > 0 && line[ll - 1] == '\r') line[--ll] = 0; if (ll) PostLine(ss->wnd, line); ll = 0; }
      else if (ll < (LINEMAX - 1)) line[ll++] = ch;
    }
  }
  if (!ss->stop) PostLine(ss->wnd, "X_DISCONNECTED");
  ss->s = INVALID_SOCKET; closesocket(s); HeapFree(GetProcessHeap(), 0, line); SessRelease(ss); WSACleanup(); return 0;
}
struct CF { HWND skip; DWORD pid; int n; };
static BOOL CALLBACK CountFrames(HWND h, LPARAM l) {
  CF* c = (CF*)l; DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
  if (pid == c->pid && h != c->skip && IsWindowVisible(h)) { wchar_t cn[32]; GetClassNameW(h, cn, 32); if (!lstrcmpW(cn, L"IEFrame")) c->n++; }
  return TRUE;
}
static HHOOK g_hook = 0;
static bool FwdKey(WPARAM vk) {
  bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, alt = (GetKeyState(VK_MENU) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  if (vk == VK_F6 || vk == VK_F11) return !alt && !ctrl;
  if (vk == VK_F5) return !alt;
  if (alt && !ctrl) return vk == 'D' || vk == VK_HOME;
  if (ctrl && !alt && !shift) { switch (vk) { case 'L': case 'O': case 'N': case 'H': case 'D': case 'W': return true; } }
  return false;
}
static HWND ViewOf(HWND x) {
  for (; x; x = GetParent(x)) {
    HWND pr = GetParent(x); if (!pr) break; wchar_t cn[32]; GetClassNameW(pr, cn, 32);
    if (!lstrcmpW(cn, L"GeckoIEDocView")) return pr;
  }
  return NULL;
}
static LRESULT CALLBACK GetMsgProc(int code, WPARAM w, LPARAM l) {
  if (code == HC_ACTION && w == PM_REMOVE) {
    MSG* m = (MSG*)l;
    if ((m->message == WM_KEYDOWN || m->message == WM_SYSKEYDOWN) && FwdKey(m->wParam)) {
      HWND view = ViewOf(m->hwnd);
      if (view) { PostMessageW(view, WM_APP + 3, m->wParam, m->lParam); m->message = WM_NULL; }
    } else if (m->message == WM_RBUTTONUP) {
      HWND view = ViewOf(m->hwnd); if (view) PostMessageW(view, WM_APP + 5, 0, 0);   // Gecko still gets the click
    }
  }
  return CallNextHookEx(NULL, code, w, l);
}
static void InstallKeyHook(HWND engineWnd) {
  if (g_hook) return; DWORD tid = GetWindowThreadProcessId(engineWnd, NULL); if (!tid) return;
  g_hook = SetWindowsHookExW(WH_GETMESSAGE, GetMsgProc, g_hinst, tid); Log("key hook on engine thread %lu -> %p", tid, g_hook);
}
static void RemoveKeyHook() { if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = 0; Log("key hook removed"); } }
static void WaitEngineGone() {
  DWORD end = GetTickCount() + 8000;
  while (GetTickCount() < end && p_run && p_run()) {
    MSG m; while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) { if (m.message == WM_QUIT) { PostQuitMessage((int)m.wParam); return; } TranslateMessage(&m); DispatchMessageW(&m); }
    Sleep(20);
  }
  Log("engine %s after EXIT", (p_run && p_run()) ? "did NOT stop in time" : "stopped");
}
static void PctDecode(const char* in, char* out, size_t n) {
  size_t o = 0;
  for (; *in && o + 1 < n; ++in) {
    if (*in == '%' && isxdigit((unsigned char)in[1]) && isxdigit((unsigned char)in[2])) { char h[3] = { in[1], in[2], 0 }; out[o++] = (char)strtol(h, NULL, 16); in += 2; }
    else out[o++] = *in;
  }
  out[o] = 0;
}
static void FileFromUrl(const wchar_t* in, wchar_t* out, int n) {
  const wchar_t* p = in; wchar_t tmp[2048];
  if (!_wcsnicmp(p, L"file:", 5)) { p += 5; while ((*p == L'/' || *p == L'\\') && !(p[1] && p[2] == L':') && p[1] == L'/') p++; while ((*p == L'/' || *p == L'\\') && p[1] && p[2] != L':' && !(p[1] && p[2] == L':')) { if (p[1] && p[2] == L':') break; p++; } if (*p == L'/' || *p == L'\\') { if (p[1] && p[2] == L':') p++; } }
  int o = 0;
  for (; *p && o < 2047; ++p) {
    if (*p == L'%' && iswxdigit(p[1]) && iswxdigit(p[2])) { wchar_t h[3] = { p[1], p[2], 0 }; tmp[o++] = (wchar_t)wcstol(h, NULL, 16); p += 2; }
    else tmp[o++] = (*p == L'/') ? L'\\' : *p;
  }
  tmp[o] = 0; lstrcpynW(out, tmp, n);
}

static void NormUrl(const char* in, char* out, size_t n) {
  size_t o = 0; for (; *in && *in != '#' && o + 1 < n; ++in) out[o++] = (char)tolower((unsigned char)*in);
  while (o > 0 && out[o - 1] == '/') o--; out[o] = 0;
}
static bool UrlEq(const char* a, const char* b) { char x[2100], y[2100]; NormUrl(a, x, sizeof x); NormUrl(b, y, sizeof y); return !strcmp(x, y); }

static void SetIniKey(const char* base, const char* key, const char* val) {
  char path[MAX_PATH + 40], tmp[MAX_PATH + 48]; PathCombineA(path, base, "ieb.ini"); LocalLowFile(path, sizeof path, true); snprintf(tmp, sizeof tmp, "%s.tmp", path);
  FILE* in = fopen(path, "r"); FILE* out = fopen(tmp, "w"); if (!out) { if (in) fclose(in); return; }
  char line[2500]; size_t kl = strlen(key);
  if (in) {
    while (fgets(line, sizeof line, in)) { char* l = line; while (*l == ' ' || *l == '\t') l++; if (!_strnicmp(l, key, kl) && l[kl] == '=') continue; fputs(line, out); }
    fclose(in);
  }
  fprintf(out, "%s=%s\n", key, val); fclose(out); MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
}
static void SetClip(HWND h, const wchar_t* t) {
  if (!OpenClipboard(h)) return; EmptyClipboard(); size_t n = (lstrlenW(t) + 1) * 2; HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n);
  if (g) { void* p = GlobalLock(g); memcpy(p, t, n); GlobalUnlock(g); SetClipboardData(CF_UNICODETEXT, g); } CloseClipboard();
}
static void SanitizeName(wchar_t* s);
// ------------------------------------------------------------------ favorites
// IE saves the address it navigated to, which for our pages is gie:<real url>. A watcher rewrites such shortcuts in the
// Favorites folder (and recent ones on the desktop) to the real address, so favorites also work without GeckoIE.
static volatile LONG g_favStarted = 0;
static bool FixUrlFile(const wchar_t* path) {
  HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
  if (f == INVALID_HANDLE_VALUE) return false;
  DWORD sz = GetFileSize(f, NULL); if (sz == 0 || sz > 65536) { CloseHandle(f); return false; }
  char* b = (char*)HeapAlloc(GetProcessHeap(), 0, sz + 8); char* o = (char*)HeapAlloc(GetProcessHeap(), 0, sz + 64 + 16);
  DWORD rd = 0; bool ok = b && o && ReadFile(f, b, sz, &rd, NULL); CloseHandle(f); bool changed = false;
  if (ok) {
    b[rd] = 0; char* p = b; char* w = o;
    while (*p) {
      char* e = strchr(p, '\\n'); size_t ll = e ? (size_t)(e - p) + 1 : strlen(p);
      if (!_strnicmp(p, "URL=gie:", 8)) {
        const char* rest = p + 8; while (*rest == '/') rest++;
        memcpy(w, p, 4); w += 4;
        char line[2200]; size_t rl = ll - (size_t)(rest - p); if (rl >= sizeof line) rl = sizeof line - 1; memcpy(line, rest, rl); line[rl] = 0;
        if (!strstr(line, "://") && _strnicmp(line, "about:", 6)) { memcpy(w, "http://", 7); w += 7; }
        memcpy(w, rest, ll - (size_t)(rest - p)); w += ll - (size_t)(rest - p); changed = true;
      } else { memcpy(w, p, ll); w += ll; }
      p += ll;
    }
    if (changed) {
      HANDLE g = INVALID_HANDLE_VALUE; for (int t = 0; t < 10; t++) { g = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL); if (g != INVALID_HANDLE_VALUE) break; Sleep(200); }
      if (g == INVALID_HANDLE_VALUE) changed = false; else { DWORD wr; WriteFile(g, o, (DWORD)(w - o), &wr, NULL); CloseHandle(g); }
    }
  }
  if (b) HeapFree(GetProcessHeap(), 0, b); if (o) HeapFree(GetProcessHeap(), 0, o);
  return changed;
}
static void FixFavDir(const wchar_t* dir, bool recentOnly, int depth, int maxDepth) {
  wchar_t pat[MAX_PATH + 4]; if (lstrlenW(dir) > MAX_PATH - 8) return; lstrcpyW(pat, dir); lstrcatW(pat, L"\\*");
  WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd); if (h == INVALID_HANDLE_VALUE) return;
  FILETIME nowf; GetSystemTimeAsFileTime(&nowf); ULONGLONG now = ((ULONGLONG)nowf.dwHighDateTime << 32) | nowf.dwLowDateTime;
  do {
    if (fd.cFileName[0] == L'.' && (!fd.cFileName[1] || (fd.cFileName[1] == L'.' && !fd.cFileName[2]))) continue;
    if (lstrlenW(dir) + 1 + lstrlenW(fd.cFileName) >= MAX_PATH) continue;
    wchar_t full[MAX_PATH]; lstrcpyW(full, dir); lstrcatW(full, L"\\"); lstrcatW(full, fd.cFileName);
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (depth < maxDepth) FixFavDir(full, recentOnly, depth + 1, maxDepth); }
    else {
      int l = lstrlenW(fd.cFileName); if (l <= 4 || lstrcmpiW(fd.cFileName + l - 4, L".url")) continue;
      if (recentOnly) { ULONGLONG wt = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime; if (now > wt && now - wt > 300ULL * 10000000ULL) continue; }
      if (FixUrlFile(full)) Log("favorite fixed (gie: removed): %ls", full);
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
}
static DWORD WINAPI FavThread(LPVOID) {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  wchar_t fav[MAX_PATH], desk[MAX_PATH]; fav[0] = desk[0] = 0;
  SHGetSpecialFolderPathW(NULL, fav, CSIDL_FAVORITES, FALSE); SHGetSpecialFolderPathW(NULL, desk, CSIDL_DESKTOPDIRECTORY, FALSE);
  if (fav[0]) FixFavDir(fav, false, 0, 8);   // one pass over what is already there
  HANDLE hs[2]; int n = 0; int which[2];
  if (fav[0]) { HANDLE h = FindFirstChangeNotificationW(fav, TRUE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE); if (h != INVALID_HANDLE_VALUE) { hs[n] = h; which[n++] = 0; } }
  if (desk[0]) { HANDLE h = FindFirstChangeNotificationW(desk, FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE); if (h != INVALID_HANDLE_VALUE) { hs[n] = h; which[n++] = 1; } }
  if (!n) return 0;
  for (;;) {
    DWORD r = WaitForMultipleObjects(n, hs, FALSE, INFINITE); if (r < WAIT_OBJECT_0 || r >= WAIT_OBJECT_0 + n) break;
    int i = r - WAIT_OBJECT_0; Sleep(500);
    if (which[i] == 0) FixFavDir(fav, true, 0, 8); else FixFavDir(desk, true, 0, 0);
    if (!FindNextChangeNotification(hs[i])) break;
  }
  return 0;
}
static void StartFavWatcher() { if (InterlockedExchange(&g_favStarted, 1)) return; HANDLE t = CreateThread(NULL, 0, FavThread, NULL, 0, NULL); if (t) CloseHandle(t); }
// Fallback when IE's own Add Favorite helper is unavailable: write the shortcut straight into the Favorites folder.
static void SaveFavoriteDirect(const wchar_t* url, const wchar_t* title) {
  wchar_t fav[MAX_PATH]; if (!SHGetSpecialFolderPathW(NULL, fav, CSIDL_FAVORITES, FALSE)) return;
  wchar_t name[200], path[MAX_PATH + 220]; lstrcpynW(name, title && title[0] ? title : url, 120); SanitizeName(name);
  lstrcpyW(path, fav); lstrcatW(path, L"\\"); lstrcatW(path, name); lstrcatW(path, L".url");
  char u8[2300], body[2500]; WideCharToMultiByte(CP_ACP, 0, url, -1, u8, sizeof u8, NULL, NULL);
  int n = snprintf(body, sizeof body, "[InternetShortcut]\r\nURL=%s\r\n", u8);
  HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (f != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(f, body, n, &w, NULL); CloseHandle(f); Log("favorite saved directly: %ls", path); }
}
static void AddFavoriteDisp(const wchar_t* url, const wchar_t* title) {
  CLSID cl; IDispatch* d = 0; bool done = false;
  if (SUCCEEDED(CLSIDFromProgID(L"Shell.UIHelper", &cl)) && SUCCEEDED(CoCreateInstance(cl, NULL, CLSCTX_INPROC_SERVER, IID_IDispatch, (void**)&d)) && d) {
    DISPID id; OLECHAR* nm = (OLECHAR*)L"AddFavorite";
    if (SUCCEEDED(d->GetIDsOfNames(IID_NULL, &nm, 1, LOCALE_USER_DEFAULT, &id))) {
      VARIANTARG a[2]; VariantInit(&a[0]); VariantInit(&a[1]); a[1].vt = VT_BSTR; a[1].bstrVal = SysAllocString(url); a[0].vt = VT_BSTR; a[0].bstrVal = SysAllocString(title);
      DISPPARAMS dp = { a, NULL, 2, 0 }; HRESULT hr = d->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD, &dp, NULL, NULL, NULL);
      VariantClear(&a[0]); VariantClear(&a[1]); done = SUCCEEDED(hr); if (!done) Log("AddFavorite failed 0x%08lx", hr);
    }
    d->Release();
  }
  if (!done) SaveFavoriteDirect(url, title);
}

// ------------------------------------------------------------------ context menu layout
#define C_PAGE 1
#define C_LINK 2
#define C_IMG 4
#define C_SEL 8
#define C_EDIT 16
#define C_ALWAYS 32
#define C_SEP 64
struct CtxDef { const char* id; const wchar_t* label; unsigned ctx; int on; };
static const CtxDef g_ctxTbl[] = {
  { "link_open", L"Open", C_LINK, 1 },
  { "link_newwin", L"Open in New Window", C_LINK, 1 },
  { "link_classic", L"Open Link in Classic IE", C_LINK, 0 },
  { "link_save", L"Save Target As...", C_LINK, 1 },
  { "link_copy", L"Copy Shortcut", C_LINK, 1 },
  { "link_fav", L"Add Link to Favorites...", C_LINK, 1 },
  { "sep1", L"", C_SEP, 1 },
  { "img_newwin", L"Open Picture in New Window", C_IMG, 1 },
  { "img_save", L"Save Picture As...", C_IMG, 1 },
  { "img_copy", L"Copy Picture", C_IMG, 1 },
  { "img_copyurl", L"Copy Picture Location", C_IMG, 1 },
  { "sep2", L"", C_SEP, 1 },
  { "undo", L"Undo", C_EDIT, 1 },
  { "cut", L"Cut", C_EDIT, 1 },
  { "copy", L"Copy", C_SEL | C_EDIT, 1 },
  { "paste", L"Paste", C_EDIT, 1 },
  { "delete", L"Delete", C_EDIT, 0 },
  { "selectall", L"Select All", C_PAGE | C_EDIT | C_SEL, 1 },
  { "search", L"Search the Web for Selection", C_SEL, 1 },
  { "sep3", L"", C_SEP, 1 },
  { "back", L"Back", C_PAGE, 1 },
  { "forward", L"Forward", C_PAGE, 1 },
  { "reload", L"Refresh", C_PAGE, 1 },
  { "zoom", L"Zoom", C_PAGE, 1 },
  { "sep4", L"", C_SEP, 1 },
  { "page_save", L"Save Page As...", C_PAGE, 1 },
  { "page_copyurl", L"Copy Page Address", C_PAGE, 1 },
  { "page_fav", L"Add to Favorites...", C_PAGE, 1 },
  { "page_shortcut", L"Create Shortcut on Desktop", C_PAGE, 0 },
  { "page_mail", L"Send Page by E-mail...", C_PAGE, 0 },
  { "page_source", L"View Source", C_PAGE, 1 },
  { "inspect", L"Inspect Element (Developer Tools)", C_ALWAYS, 1 },
  { "sep5", L"", C_SEP, 1 },
  { "page_classic", L"Open This Page in Classic IE", C_ALWAYS, 1 },
  { "gieset", L"GeckoIE Settings...", C_ALWAYS, 1 },
};
#define NCTX ((int)(sizeof g_ctxTbl / sizeof g_ctxTbl[0]))
struct LayoutEnt { int idx; int on; };
// The saved layout is an ordered, comma separated list of item ids; a leading '-' hides the item. Items missing from it
// (for example added by a newer version) are appended with their default visibility.
static int LayoutFrom(const char* spec, LayoutEnt* out) {
  int n = 0; bool used[NCTX]; memset(used, 0, sizeof used); char buf[2300]; lstrcpynA(buf, spec, sizeof buf);
  for (char* tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
    char* t = Trim(tok); int on = 1; if (*t == '-') { on = 0; t++; }
    for (int i = 0; i < NCTX; i++) if (!used[i] && !strcmp(t, g_ctxTbl[i].id)) { used[i] = true; out[n].idx = i; out[n].on = on; n++; break; }
  }
  for (int i = 0; i < NCTX; i++) {   // items missing from the saved list: put each after the nearest default item before it
    if (used[i]) continue; int pos = 0;
    for (int j = i - 1; j >= 0; j--) if (used[j]) { for (int k = 0; k < n; k++) if (out[k].idx == j) { pos = k + 1; break; } break; }
    for (int k = n; k > pos; k--) out[k] = out[k - 1];
    out[pos].idx = i; out[pos].on = g_ctxTbl[i].on; n++; used[i] = true;
  }
  return n;
}
static void UrlEncUtf8(const char* in, char* out, size_t n) {
  static const char* hx = "0123456789ABCDEF"; size_t o = 0;
  for (; *in && o + 4 < n; ++in) {
    unsigned char c = (unsigned char)*in;
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out[o++] = (char)c; else { out[o++] = '%'; out[o++] = hx[c >> 4]; out[o++] = hx[c & 15]; }
  }
  out[o] = 0;
}
static void SanitizeName(wchar_t* s) { for (; *s; ++s) if (*s < 32 || wcschr(L"\\/:*?\"<>|", *s)) *s = L'_'; }

// ---- GeckoIE Settings window: homescreen + right-click menu layout (checkbox = show, Up/Down = order)
static HWND g_cfgLV, g_cfgHome, g_cfgDl, g_cfgWnd; static bool g_cfgDone; static char g_cfgBase[MAX_PATH]; static bool g_cfgHave;
// The list view is a Unicode window, so the explicit ...W messages are needed (the plain macros send the ANSI ones here).
static int LvInsert(HWND lv, LVITEMW* it) { return (int)SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)it); }
static BOOL LvGet(HWND lv, LVITEMW* it) { return (BOOL)SendMessageW(lv, LVM_GETITEMW, 0, (LPARAM)it); }
static BOOL LvSet(HWND lv, LVITEMW* it) { return (BOOL)SendMessageW(lv, LVM_SETITEMW, 0, (LPARAM)it); }
static void CfgFill(HWND lv, const LayoutEnt* e, int n) {
  ListView_DeleteAllItems(lv); int row = 0;
  for (int i = 0; i < n; i++) {
    const CtxDef& d = g_ctxTbl[e[i].idx];
    if (!g_devtools && !strcmp(d.id, "inspect")) continue;   // only offered when the developer tools are installed
    LVITEMW it; memset(&it, 0, sizeof it); it.mask = LVIF_TEXT | LVIF_PARAM; it.iItem = row; it.lParam = e[i].idx;
    it.pszText = (LPWSTR)((d.ctx & C_SEP) ? L"-------------  separator  -------------" : d.label);
    LvInsert(lv, &it); ListView_SetCheckState(lv, row, e[i].on ? TRUE : FALSE); row++;
  }
  if (row) ListView_SetItemState(lv, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
}
static void CfgMove(HWND lv, int dir) {
  int i = ListView_GetNextItem(lv, -1, LVNI_SELECTED); if (i < 0) return;
  int j = i + dir; if (j < 0 || j >= ListView_GetItemCount(lv)) return;
  wchar_t ta[160], tb[160]; LVITEMW a, b; memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
  a.mask = b.mask = LVIF_TEXT | LVIF_PARAM; a.iItem = i; b.iItem = j; a.pszText = ta; b.pszText = tb; a.cchTextMax = b.cchTextMax = 160;
  LvGet(lv, &a); LvGet(lv, &b);
  BOOL ca = ListView_GetCheckState(lv, i), cb = ListView_GetCheckState(lv, j);
  a.iItem = j; b.iItem = i; LvSet(lv, &a); LvSet(lv, &b);
  ListView_SetCheckState(lv, j, ca); ListView_SetCheckState(lv, i, cb);
  ListView_SetItemState(lv, j, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED); ListView_EnsureVisible(lv, j, FALSE);
}
static HWND MkCtl(HWND p, const wchar_t* cls, const wchar_t* t, int id, int x, int y, int w, int h, DWORD st) {
  HWND c = CreateWindowExW(0, cls, t, WS_CHILD | WS_VISIBLE | st, x, y, w, h, p, (HMENU)(INT_PTR)id, g_hinst, NULL);
  SendMessageW(c, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE); return c;
}
static LRESULT CALLBACK CfgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_CREATE: {
      wchar_t ico[MAX_PATH]; lstrcpyW(ico, g_dllDir); lstrcatW(ico, L"\\GeckoIE.ico");
      HANDLE i1 = LoadImageW(NULL, ico, IMAGE_ICON, 16, 16, LR_LOADFROMFILE), i2 = LoadImageW(NULL, ico, IMAGE_ICON, 32, 32, LR_LOADFROMFILE);
      if (i1) SendMessageW(h, WM_SETICON, ICON_SMALL, (LPARAM)i1); if (i2) SendMessageW(h, WM_SETICON, ICON_BIG, (LPARAM)i2);
      MkCtl(h, L"STATIC", L"Homescreen \x2014 what the page area shows while the engine is starting:", 0, 12, 12, 440, 18, 0);
      g_cfgHome = MkCtl(h, L"COMBOBOX", L"", 20, 12, 34, 330, 120, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP);
      SendMessageW(g_cfgHome, CB_ADDSTRING, 0, (LPARAM)L"White blank page (default)");
      SendMessageW(g_cfgHome, CB_ADDSTRING, 0, (LPARAM)L"\"Loading\" screen");
      SendMessageW(g_cfgHome, CB_SETCURSEL, g_home == 1 ? 1 : 0, 0);
      MkCtl(h, L"STATIC", L"Downloads \x2014 who handles the files you download:", 0, 12, 72, 440, 18, 0);
      g_cfgDl = MkCtl(h, L"COMBOBOX", L"", 21, 12, 94, 330, 120, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP);
      SendMessageW(g_cfgDl, CB_ADDSTRING, 0, (LPARAM)L"Internet Explorer (File Download window, default)");
      SendMessageW(g_cfgDl, CB_ADDSTRING, 0, (LPARAM)L"GeckoIE (Save As window)");
      SendMessageW(g_cfgDl, CB_SETCURSEL, g_dlMode == 1 ? 1 : 0, 0);
      MkCtl(h, L"STATIC", L"Right-click menu \x2014 tick the items you want and arrange them with Move Up / Move Down. Items that do not apply to what you right-click are skipped automatically.", 0, 12, 134, 440, 44, 0);
      g_cfgLV = CreateWindowExW(WS_EX_CLIENTEDGE, L"SysListView32", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOCOLUMNHEADER, 12, 184, 330, 300, h, (HMENU)10, g_hinst, NULL);
      SendMessageW(g_cfgLV, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
      ListView_SetExtendedListViewStyle(g_cfgLV, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
      LVCOLUMNW col; memset(&col, 0, sizeof col); col.mask = LVCF_WIDTH; col.cx = 304; SendMessageW(g_cfgLV, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
      MkCtl(h, L"BUTTON", L"Move &Up", 11, 352, 184, 100, 26, BS_PUSHBUTTON | WS_TABSTOP);
      MkCtl(h, L"BUTTON", L"Move &Down", 12, 352, 216, 100, 26, BS_PUSHBUTTON | WS_TABSTOP);
      MkCtl(h, L"BUTTON", L"&Reset", 13, 352, 258, 100, 26, BS_PUSHBUTTON | WS_TABSTOP);
      MkCtl(h, L"STATIC", L"GeckoIE " GIE_VERSION_W, 0, 12, 520, 250, 18, 0);
      MkCtl(h, L"BUTTON", L"OK", IDOK, 264, 514, 90, 28, BS_DEFPUSHBUTTON | WS_TABSTOP);
      MkCtl(h, L"BUTTON", L"Cancel", IDCANCEL, 362, 514, 90, 28, BS_PUSHBUTTON | WS_TABSTOP);
      LayoutEnt e[NCTX + 2]; int n = LayoutFrom(g_ctxLayout, e); CfgFill(g_cfgLV, e, n);
      return 0; }
    case WM_COMMAND:
      switch (LOWORD(w)) {
        case 11: CfgMove(g_cfgLV, -1); break;
        case 12: CfgMove(g_cfgLV, 1); break;
        case 13: { LayoutEnt e[NCTX + 2]; int n = LayoutFrom("", e); CfgFill(g_cfgLV, e, n); } break;
        case IDOK: {
          char spec[2300]; spec[0] = 0; int cnt = ListView_GetItemCount(g_cfgLV);
          for (int i = 0; i < cnt; i++) {
            LVITEMW it; memset(&it, 0, sizeof it); it.mask = LVIF_PARAM; it.iItem = i; LvGet(g_cfgLV, &it);
            if (i) lstrcatA(spec, ","); if (!ListView_GetCheckState(g_cfgLV, i)) lstrcatA(spec, "-"); lstrcatA(spec, g_ctxTbl[it.lParam].id);
          }
          lstrcpynA(g_ctxLayout, spec, sizeof g_ctxLayout);
          g_home = (SendMessageW(g_cfgHome, CB_GETCURSEL, 0, 0) == 1) ? 1 : 0;
          g_dlMode = (SendMessageW(g_cfgDl, CB_GETCURSEL, 0, 0) == 1) ? 1 : 0;
          if (g_cfgHave) { SetIniKey(g_cfgBase, "ContextMenu", spec); SetIniKey(g_cfgBase, "HomeScreen", g_home ? "loading" : "blank"); SetIniKey(g_cfgBase, "Downloads", g_dlMode ? "engine" : "ie"); }
          Log("settings saved: homescreen=%s, context menu layout saved", g_home ? "loading" : "blank");
          DestroyWindow(h); } break;
        case IDCANCEL: DestroyWindow(h); break;
      }
      return 0;
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: g_cfgDone = true; return 0;
  }
  return DefWindowProcW(h, m, w, l);
}
static void ShowSettingsWindow(HWND owner) {
  if (g_cfgWnd && IsWindow(g_cfgWnd)) { SetForegroundWindow(g_cfgWnd); return; }
  static bool reg = false;
  if (!reg) {
    INITCOMMONCONTROLSEX ic; ic.dwSize = sizeof ic; ic.dwICC = ICC_LISTVIEW_CLASSES; InitCommonControlsEx(&ic);
    WNDCLASSEXW wc; memset(&wc, 0, sizeof wc); wc.cbSize = sizeof wc; wc.lpfnWndProc = CfgProc; wc.hInstance = g_hinst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.lpszClassName = L"GeckoIECtxCfg"; RegisterClassExW(&wc); reg = true;
  }
  Cfg c; g_cfgHave = LoadCfg(&c); lstrcpynA(g_cfgBase, c.base, MAX_PATH);
  DWORD st = WS_POPUP | WS_CAPTION | WS_SYSMENU, ex = WS_EX_DLGMODALFRAME; RECT r = { 0, 0, 466, 554 }; AdjustWindowRectEx(&r, st, FALSE, ex);
  int W = r.right - r.left, H = r.bottom - r.top, x = (GetSystemMetrics(SM_CXSCREEN) - W) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - H) / 2;
  HWND d = CreateWindowExW(ex, L"GeckoIECtxCfg", L"GeckoIE Settings", st, x, y, W, H, owner, NULL, g_hinst, NULL);
  if (!d) return; g_cfgWnd = d; g_cfgDone = false; if (owner) EnableWindow(owner, FALSE); ShowWindow(d, SW_SHOW); SetForegroundWindow(d);
  MSG msg; int gm = 1; while (!g_cfgDone && (gm = GetMessageW(&msg, NULL, 0, 0)) > 0) { if (!IsDialogMessageW(d, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); } }
  if (gm == 0) PostQuitMessage((int)msg.wParam);
  if (IsWindow(d)) DestroyWindow(d);
  g_cfgWnd = 0; if (owner) { EnableWindow(owner, TRUE); SetForegroundWindow(owner); }
}

class Doc;
// A page the engine navigated to by itself (link click, redirect): the old view asks IE for a real navigation to
// gie:<url> and hands its live engine view over to the new view, so nothing reloads and IE's history stays in sync.
struct Adopt { int used; int claimed; char url[2100]; Doc* owner; Sess* sess; HWND engine; DWORD tick; };
static Adopt g_ad[8]; static HWND g_holder = 0;
static void SweepAdopt();
static LRESULT CALLBACK HolderProc(HWND h, UINT m, WPARAM w, LPARAM l) { if (m == WM_TIMER) { SweepAdopt(); return 0; } return DefWindowProcW(h, m, w, l); }
static HWND EnsureHolder() {
  EnterCriticalSection(&g_cs);
  if (!g_holder || !IsWindow(g_holder)) {
    WNDCLASSEXW wc; memset(&wc, 0, sizeof wc); wc.cbSize = sizeof wc; wc.lpfnWndProc = HolderProc; wc.hInstance = g_hinst; wc.lpszClassName = L"GeckoIEHolder";
    RegisterClassExW(&wc);
    g_holder = CreateWindowExW(WS_EX_TOOLWINDOW, L"GeckoIEHolder", L"", WS_POPUP, 0, 0, 10, 10, NULL, NULL, g_hinst, NULL);
    if (g_holder) SetTimer(g_holder, 1, 1000, NULL);
  }
  HWND h = g_holder; LeaveCriticalSection(&g_cs); return h;
}
static void SendRawLine(Sess* ss, const char* line) {
  SOCKET sk = ss->s; if (sk == INVALID_SOCKET) return; char b[64]; int n = snprintf(b, sizeof b, "%s\r\n", line); ::send(sk, b, n, 0);
}
static void EndSessRaw(Sess* ss) {
  SendRawLine(ss, "QUIT"); ss->stop = 1; SOCKET sk = ss->s; if (sk != INVALID_SOCKET) shutdown(sk, SD_BOTH);
  SessRelease(ss); InterlockedDecrement(&g_sessions); if (g_sessions == 0) RemoveKeyHook(); Log("parked session ended");
}
struct FE { HWND found; };
static BOOL CALLBACK FindAddr(HWND h, LPARAM l) {
  wchar_t c[40]; GetClassNameW(h, c, 40); if (lstrcmpiW(c, L"Edit")) return TRUE;
  HWND p = GetParent(h); if (!p) return TRUE; GetClassNameW(p, c, 40); if (lstrcmpiW(c, L"ComboBox")) return TRUE;
  HWND g = GetParent(p); if (!g) return TRUE; GetClassNameW(g, c, 40); if (lstrcmpiW(c, L"ComboBoxEx32")) return TRUE;
  ((FE*)l)->found = h; return FALSE;
}
static LRESULT CALLBACK ViewProc(HWND, UINT, WPARAM, LPARAM);

class Doc : public IOleObject, public IOleDocument, public IOleDocumentView,
            public IPersistMoniker, public IPersistFile, public IOleInPlaceObject,
            public IOleInPlaceActiveObject, public IOleCommandTarget {
  LONG ref_; IOleClientSite* site_; IOleInPlaceSite* ips_; IOleInPlaceFrame* frame_;
  IOleInPlaceUIWindow* uiw_; IMoniker* mk_; HWND parent_, hwnd_; RECT rc_;
  bool haveRc_, inplace_, uiact_, docked_, goSent_, lastDown_;
  Sess* sess_; HWND engine_; char nav_[2100], curNav_[2100], lastLoc_[2100]; IWebBrowser2* wbp_; IOleCommandTarget* sct_;
  bool xferPending_, sawLoading_, fail_, inMenu_, ctxPending_; int phase_, fixTicks_; DWORD claimTick_; wchar_t title_[600];
  wchar_t url_[2048], chain_[512], wb_[1100], state_[200];
public:
  Doc() : ref_(1), site_(0), ips_(0), frame_(0), uiw_(0), mk_(0), parent_(0), hwnd_(0),
          haveRc_(false), inplace_(false), uiact_(false), docked_(false), goSent_(false), lastDown_(false),
          sess_(0), engine_(0), wbp_(0), sct_(0), xferPending_(false), sawLoading_(false), fail_(false), inMenu_(false), ctxPending_(false), phase_(0), fixTicks_(0), claimTick_(0) {
    nav_[0] = curNav_[0] = lastLoc_[0] = 0; title_[0] = 0;
    memset(&rc_, 0, sizeof rc_); url_[0] = chain_[0] = wb_[0] = 0;
    lstrcpyW(state_, L"created"); InterlockedIncrement(&g_objs); Log("Doc created");
  }
  ~Doc() { if (mk_) { mk_->Release(); mk_ = 0; } if (ips_) { ips_->Release(); ips_ = 0; } if (site_) { site_->Release(); site_ = 0; }
    EnterCriticalSection(&g_cs); for (int i = 0; i < 8; i++) if (g_ad[i].owner == this) g_ad[i].owner = 0; LeaveCriticalSection(&g_cs); Log("Doc destroyed"); InterlockedDecrement(&g_objs); }

  // ---- IUnknown (one implementation overrides all bases)
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** pp) {
    void* p = 0; char nm[220];
    if (IsEqualIID(r, IID_IUnknown) || IsEqualIID(r, IID_IOleObject)) p = (IOleObject*)this;
    else if (IsEqualIID(r, IID_IOleDocument)) p = (IOleDocument*)this;
    else if (IsEqualIID(r, IID_IOleDocumentView)) p = (IOleDocumentView*)this;
    else if (IsEqualIID(r, IID_IPM)) p = (IPersistMoniker*)this;
    else if (IsEqualIID(r, IID_IPersistFile) || IsEqualIID(r, IID_IPersist)) p = (IPersistFile*)this;
    else if (IsEqualIID(r, IID_IOleWindow) || IsEqualIID(r, IID_IOleInPlaceObject)) p = (IOleInPlaceObject*)this;
    else if (IsEqualIID(r, IID_IOleInPlaceActiveObject)) p = (IOleInPlaceActiveObject*)this;
    else if (IsEqualIID(r, IID_IOleCommandTarget)) p = (IOleCommandTarget*)this;
    IidName(r, nm, sizeof nm); Log("QI %s -> %s", nm, p ? "OK" : "no");
    if (pp) *pp = p; if (!p) return E_NOINTERFACE; AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() { return InterlockedIncrement(&ref_); }
  ULONG STDMETHODCALLTYPE Release() { LONG n = InterlockedDecrement(&ref_); if (!n) delete this; return n; }

  // ---- IOleObject
  HRESULT STDMETHODCALLTYPE SetClientSite(IOleClientSite* s) {
    Log("SetClientSite %p", s); if (s) s->AddRef(); if (site_) site_->Release(); site_ = s; return S_OK; }
  HRESULT STDMETHODCALLTYPE GetClientSite(IOleClientSite** pp) { *pp = site_; if (site_) site_->AddRef(); return S_OK; }
  HRESULT STDMETHODCALLTYPE SetHostNames(LPCOLESTR a, LPCOLESTR b) { Log("SetHostNames %ls / %ls", a ? a : L"", b ? b : L""); return S_OK; }
  HRESULT STDMETHODCALLTYPE Close(DWORD o) { Log("IOleObject::Close %lu", o); CloseViewImpl(); return S_OK; }
  HRESULT STDMETHODCALLTYPE SetMoniker(DWORD, IMoniker*) { Log("SetMoniker"); return S_OK; }
  HRESULT STDMETHODCALLTYPE GetMoniker(DWORD, DWORD, IMoniker** pp) { if (mk_) { *pp = mk_; mk_->AddRef(); return S_OK; } *pp = 0; return E_FAIL; }
  HRESULT STDMETHODCALLTYPE InitFromData(IDataObject*, BOOL, DWORD) { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetClipboardData(DWORD, IDataObject**) { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE DoVerb(LONG v, LPMSG, IOleClientSite*, LONG, HWND, LPCRECT) {
    Log("DoVerb %ld", v);
    switch (v) {
      case OLEIVERB_PRIMARY: case OLEIVERB_SHOW: case OLEIVERB_INPLACEACTIVATE: case OLEIVERB_UIACTIVATE: return Activate();
      case OLEIVERB_HIDE: if (hwnd_) ShowWindow(hwnd_, SW_HIDE); return S_OK;
    }
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE EnumVerbs(IEnumOLEVERB**) { return OLE_S_USEREG; }
  HRESULT STDMETHODCALLTYPE Update() { return S_OK; }
  HRESULT STDMETHODCALLTYPE IsUpToDate() { return S_OK; }
  HRESULT STDMETHODCALLTYPE GetUserClassID(CLSID* c) { *c = CLSID_GeckoDoc; return S_OK; }
  HRESULT STDMETHODCALLTYPE GetUserType(DWORD, LPOLESTR*) { return OLE_S_USEREG; }
  HRESULT STDMETHODCALLTYPE SetExtent(DWORD, SIZEL*) { return S_OK; }
  HRESULT STDMETHODCALLTYPE GetExtent(DWORD, SIZEL*) { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Advise(IAdviseSink*, DWORD*) { return OLE_E_ADVISENOTSUPPORTED; }
  HRESULT STDMETHODCALLTYPE Unadvise(DWORD) { return OLE_E_ADVISENOTSUPPORTED; }
  HRESULT STDMETHODCALLTYPE EnumAdvise(IEnumSTATDATA**) { return OLE_E_ADVISENOTSUPPORTED; }
  HRESULT STDMETHODCALLTYPE GetMiscStatus(DWORD, DWORD*) { return OLE_S_USEREG; }
  HRESULT STDMETHODCALLTYPE SetColorScheme(LOGPALETTE*) { return E_NOTIMPL; }

  // ---- IOleDocument
  HRESULT STDMETHODCALLTYPE CreateView(IOleInPlaceSite* s, IStream*, DWORD, IOleDocumentView** pp) {
    Log("CreateView ips=%p", s); if (s) SetInPlaceSite(s); *pp = (IOleDocumentView*)this; AddRef(); return S_OK; }
  HRESULT STDMETHODCALLTYPE GetDocMiscStatus(DWORD* d) { *d = DOCMISC_CANTOPENEDIT; return S_OK; }
  HRESULT STDMETHODCALLTYPE EnumViews(IEnumOleDocumentViews** e, IOleDocumentView** v) {
    Log("EnumViews"); *e = 0; *v = (IOleDocumentView*)this; AddRef(); return S_OK; }

  // ---- IOleDocumentView
  HRESULT STDMETHODCALLTYPE SetInPlaceSite(IOleInPlaceSite* s) {
    Log("SetInPlaceSite %p", s);
    if (ips_ && ips_ != s) { CloseViewImpl(); ips_->Release(); ips_ = 0; }
    if (s && s != ips_) { s->AddRef(); ips_ = s; } return S_OK; }
  HRESULT STDMETHODCALLTYPE GetInPlaceSite(IOleInPlaceSite** pp) { *pp = ips_; if (ips_) ips_->AddRef(); return S_OK; }
  HRESULT STDMETHODCALLTYPE GetDocument(IUnknown** pp) { *pp = (IOleObject*)this; AddRef(); return S_OK; }
  HRESULT STDMETHODCALLTYPE SetRect(LPRECT r) {
    Log("SetRect %ld,%ld,%ld,%ld", r->left, r->top, r->right, r->bottom); rc_ = *r; haveRc_ = true; Move(); return S_OK; }
  HRESULT STDMETHODCALLTYPE GetRect(LPRECT r) { *r = rc_; return S_OK; }
  HRESULT STDMETHODCALLTYPE SetRectComplex(LPRECT, LPRECT, LPRECT, LPRECT) { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Show(BOOL f) {
    Log("Show %d", f);
    if (f) { HRESULT hr = CreateWin(); if (FAILED(hr)) return hr; ShowWindow(hwnd_, SW_SHOW); }
    else if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
    return S_OK; }
  HRESULT STDMETHODCALLTYPE UIActivate(BOOL f) {
    Log("UIActivate %d", f);
    if (f) {
      HRESULT hr = CreateWin(); if (FAILED(hr)) return hr; ShowWindow(hwnd_, SW_SHOW);
      if (!uiact_) {
        uiact_ = true; ips_->OnUIActivate();
        if (frame_) frame_->SetActiveObject((IOleInPlaceActiveObject*)this, L"GeckoIE");
        if (uiw_) uiw_->SetActiveObject((IOleInPlaceActiveObject*)this, L"GeckoIE");
      }
      SetFocus(hwnd_); lstrcpyW(state_, L"UI-active");
    } else if (uiact_) {
      uiact_ = false; if (ips_) ips_->OnUIDeactivate(FALSE);
      if (frame_) frame_->SetActiveObject(NULL, NULL);
      if (uiw_) uiw_->SetActiveObject(NULL, NULL);
    }
    return S_OK; }
  HRESULT STDMETHODCALLTYPE Open() { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE CloseView(DWORD) { Log("CloseView"); CloseViewImpl(); return S_OK; }
  HRESULT STDMETHODCALLTYPE SaveViewState(LPSTREAM) { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE ApplyViewState(LPSTREAM) { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Clone(IOleInPlaceSite*, IOleDocumentView**) { return E_NOTIMPL; }

  // ---- IPersist / IPersistMoniker / IPersistFile
  HRESULT STDMETHODCALLTYPE GetClassID(CLSID* c) { *c = CLSID_GeckoDoc; return S_OK; }
  HRESULT STDMETHODCALLTYPE IsDirty() { return S_FALSE; }
  HRESULT STDMETHODCALLTYPE Load(BOOL full, IMoniker* mk, LPBC bc, DWORD mode) {
    Log("IPersistMoniker::Load full=%d mode=0x%lx", full, mode);
    if (mk_) mk_->Release(); mk_ = mk; if (mk) mk->AddRef();
    if (mk) { LPOLESTR n = 0; if (SUCCEEDED(mk->GetDisplayName(bc, NULL, &n)) && n) { lstrcpynW(url_, n, 2048); CoTaskMemFree(n); } }
    Log("  moniker=%ls", url_); ParseTarget(); if (hwnd_) InvalidateRect(hwnd_, NULL, TRUE); return S_OK; }
  HRESULT STDMETHODCALLTYPE Save(IMoniker*, LPBC, BOOL) { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SaveCompleted(IMoniker*, LPBC) { return S_OK; }
  HRESULT STDMETHODCALLTYPE GetCurMoniker(IMoniker** pp) { if (mk_) { *pp = mk_; mk_->AddRef(); return S_OK; } *pp = 0; return E_FAIL; }
  HRESULT STDMETHODCALLTYPE Load(LPCOLESTR f, DWORD mode) {
    Log("IPersistFile::Load %ls mode=0x%lx", f, mode); lstrcpynW(url_, f, 2048); ParseTarget(); if (hwnd_) InvalidateRect(hwnd_, NULL, TRUE); return S_OK; }
  HRESULT STDMETHODCALLTYPE Save(LPCOLESTR, BOOL) { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SaveCompleted(LPCOLESTR) { return S_OK; }
  HRESULT STDMETHODCALLTYPE GetCurFile(LPOLESTR*) { return E_NOTIMPL; }

  // ---- IOleWindow / IOleInPlaceObject / IOleInPlaceActiveObject
  HRESULT STDMETHODCALLTYPE GetWindow(HWND* h) { *h = hwnd_; return hwnd_ ? S_OK : E_FAIL; }
  HRESULT STDMETHODCALLTYPE ContextSensitiveHelp(BOOL) { return S_OK; }
  HRESULT STDMETHODCALLTYPE InPlaceDeactivate() { Log("InPlaceDeactivate"); CloseViewImpl(); return S_OK; }
  HRESULT STDMETHODCALLTYPE UIDeactivate() { Log("UIDeactivate"); UIActivate(FALSE); return S_OK; }
  HRESULT STDMETHODCALLTYPE SetObjectRects(LPCRECT p, LPCRECT) {
    Log("SetObjectRects %ld,%ld,%ld,%ld", p->left, p->top, p->right, p->bottom); rc_ = *p; haveRc_ = true; Move(); return S_OK; }
  HRESULT STDMETHODCALLTYPE ReactivateAndUndo() { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE TranslateAccelerator(LPMSG m) { Log("TranslateAccelerator msg=0x%x vk=0x%x", (unsigned)m->message, (unsigned)m->wParam); return S_FALSE; }
  HRESULT STDMETHODCALLTYPE OnFrameWindowActivate(BOOL f) { Log("OnFrameWindowActivate %d", f); return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDocWindowActivate(BOOL f) { Log("OnDocWindowActivate %d", f); return S_OK; }
  HRESULT STDMETHODCALLTYPE ResizeBorder(LPCRECT, IOleInPlaceUIWindow*, BOOL) { return S_OK; }
  HRESULT STDMETHODCALLTYPE EnableModeless(BOOL) { return S_OK; }

  // ---- IOleCommandTarget (log what IE sends)
  HRESULT STDMETHODCALLTYPE QueryStatus(const GUID* g, ULONG n, OLECMD* c, OLECMDTEXT*) {
    if (g) return OLECMDERR_E_UNKNOWNGROUP;
    bool any = false;
    for (ULONG i = 0; i < n; i++) {
      if (c[i].cmdID == CMD_REFRESH || c[i].cmdID == CMD_STOP) { c[i].cmdf = OLECMDF_SUPPORTED | OLECMDF_ENABLED; any = true; }
      else c[i].cmdf = 0;
    }
    return any ? S_OK : OLECMDERR_E_NOTSUPPORTED; }
  HRESULT STDMETHODCALLTYPE Exec(const GUID* g, DWORD id, DWORD opt, VARIANTARG*, VARIANTARG*) {
    if (!g && id == CMD_STOP) {
      if (xferPending_ || (claimTick_ && GetTickCount() - claimTick_ < 1500)) { Log("IE Stop ignored (page hand-over)"); return S_OK; }
      Log("IE Stop -> engine STOP"); SendLine("STOP"); return S_OK; }
    if (!g && id == CMD_REFRESH) { Log("IE Refresh -> engine RELOAD"); SendLine("RELOAD"); return S_OK; }
    wchar_t s[64] = L"(null)"; if (g) StringFromGUID2(*g, s, 64);
    Log("Exec group=%ls id=%lu opt=%lu", s, id, opt); return OLECMDERR_E_NOTSUPPORTED; }

  // ---- helpers
  HRESULT Activate() {
    if (ips_) { Show(TRUE); return UIActivate(TRUE); }
    if (!site_) return E_UNEXPECTED;
    IOleDocumentSite* ds = 0;
    if (SUCCEEDED(site_->QueryInterface(IID_IOleDocumentSite, (void**)&ds))) {
      HRESULT hr = ds->ActivateMe(NULL); Log("ActivateMe -> 0x%08lx", hr); ds->Release(); return hr; }
    IOleInPlaceSite* s = 0;
    if (SUCCEEDED(site_->QueryInterface(IID_IOleInPlaceSite, (void**)&s))) {
      SetInPlaceSite(s); s->Release(); Show(TRUE); return UIActivate(TRUE); }
    Log("Activate: no IOleDocumentSite / IOleInPlaceSite"); return E_FAIL;
  }
  void Move() { if (hwnd_ && haveRc_) MoveWindow(hwnd_, rc_.left, rc_.top, rc_.right - rc_.left, rc_.bottom - rc_.top, TRUE); }
  static void RegCls() {
    static bool done = false; if (done) return; done = true;
    WNDCLASSEXW wc; memset(&wc, 0, sizeof wc); wc.cbSize = sizeof wc; wc.lpfnWndProc = ViewProc;
    wc.hInstance = g_hinst; wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.lpszClassName = L"GeckoIEDocView";
    RegisterClassExW(&wc);
  }
  HRESULT CreateWin() {
    if (hwnd_) return S_OK; if (!ips_) return E_UNEXPECTED;
    { Cfg hc; LoadCfg(&hc); }
    StartFavWatcher();
    ips_->GetWindow(&parent_);
    HRESULT hr = ips_->CanInPlaceActivate(); Log("CanInPlaceActivate -> 0x%08lx parent=%p", hr, parent_);
    if (hr != S_OK) return FAILED(hr) ? hr : E_FAIL;
    ips_->OnInPlaceActivate();
    RECT pos, clip; OLEINPLACEFRAMEINFO fi; memset(&pos, 0, sizeof pos); memset(&clip, 0, sizeof clip);
    memset(&fi, 0, sizeof fi); fi.cb = sizeof fi;
    hr = ips_->GetWindowContext(&frame_, &uiw_, &pos, &clip, &fi);
    Log("GetWindowContext -> 0x%08lx pos=%ld,%ld,%ld,%ld frame=%p uiwin=%p", hr, pos.left, pos.top, pos.right, pos.bottom, frame_, uiw_);
    RECT r = haveRc_ ? rc_ : pos; if (!haveRc_) { rc_ = pos; haveRc_ = true; }
    RegCls();
    hwnd_ = CreateWindowExW(0, L"GeckoIEDocView", L"", WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_VISIBLE,
      r.left, r.top, r.right - r.left, r.bottom - r.top, parent_, NULL, g_hinst, this);
    inplace_ = true; Log("view window %p", hwnd_); Gather(); BeginSession();
    return hwnd_ ? S_OK : E_FAIL;
  }
  void Gather() {
    chain_[0] = 0; HWND w = parent_;
    for (int i = 0; i < 6 && w; ++i, w = GetParent(w)) {
      wchar_t c[64] = L"?"; GetClassNameW(w, c, 64);
      if (lstrlenW(chain_) + lstrlenW(c) + 4 < 500) { if (i) lstrcatW(chain_, L" < "); lstrcatW(chain_, c); } }
    Log("parent chain: %ls", chain_);
    lstrcpyW(wb_, L"not reachable");
    IUnknown* cands[2] = { ips_, site_ };
    for (int i = 0; i < 2; ++i) {
      IServiceProvider* sp = 0; if (!cands[i] || FAILED(cands[i]->QueryInterface(IID_IServiceProvider, (void**)&sp))) continue;
      IWebBrowser2* wb = 0; HRESULT hr = sp->QueryService(IID_IWBApp, IID_IWB2, (void**)&wb); sp->Release();
      Log("QueryService(WebBrowserApp, IWebBrowser2) via %s -> 0x%08lx", i ? "clientsite" : "ipsite", hr);
      if (SUCCEEDED(hr) && wb) {
        BSTR u = 0, t = 0; wb->get_LocationURL(&u); wb->get_LocationName(&t);
        wsprintfW(wb_, L"yes - url=%s title=%s", u ? u : L"", t ? t : L""); Log("IWebBrowser2: %ls", wb_);
        SysFreeString(u); SysFreeString(t); wbp_ = wb; break; }
    }
    IOleCommandTarget* ct = 0;
    if (site_ && SUCCEEDED(site_->QueryInterface(IID_IOleCommandTarget, (void**)&ct))) { Log("site has IOleCommandTarget"); sct_ = ct; }
    if (hwnd_) InvalidateRect(hwnd_, NULL, TRUE);
  }
  void CloseViewImpl() {
    if (uiact_) UIActivate(FALSE);
    HWND root = hwnd_ ? GetAncestor(hwnd_, GA_ROOT) : NULL; bool last = false;
    if (sess_) {   // a navigation we asked IE for may not have arrived yet: park the session for the next view
      bool parked = false; EnterCriticalSection(&g_cs);
      for (int i = 0; i < 8; i++) if (g_ad[i].used && g_ad[i].owner == this && !g_ad[i].claimed) { g_ad[i].owner = 0; g_ad[i].tick = GetTickCount(); parked = true; }
      LeaveCriticalSection(&g_cs);
      if (parked) {
        HWND hold = EnsureHolder(); if (engine_ && IsWindow(engine_)) { ShowWindow(engine_, SW_HIDE); SetParent(engine_, hold); }
        sess_ = 0; engine_ = 0; docked_ = false; xferPending_ = false; Log("session parked for the next view");
      }
    }
    if (sess_ && g_owned) {
      CF cf = { root, GetCurrentProcessId(), 0 }; EnumWindows(CountFrames, (LPARAM)&cf);
      bool rootGone = !root || !IsWindow(root) || !IsWindowVisible(root);
      last = (g_sessions == 1 && cf.n == 0 && rootGone);
      Log("close: root=%p rootGone=%d otherIEFrames=%d sessions=%ld last=%d", root, rootGone, cf.n, g_sessions, last);
    }
    if (hwnd_) KillTimer(hwnd_, 1);
    if (engine_ && IsWindow(engine_)) { ShowWindow(engine_, SW_HIDE); SetParent(engine_, NULL); }
    engine_ = 0; docked_ = false;
    if (sess_) {
      SendLine(last ? "EXIT" : "QUIT"); sess_->stop = 1; SOCKET sk = sess_->s; if (sk != INVALID_SOCKET) shutdown(sk, SD_BOTH);
      SessRelease(sess_); sess_ = 0; InterlockedDecrement(&g_sessions); if (g_sessions == 0) RemoveKeyHook(); if (last) WaitEngineGone();
    }
    if (wbp_) { wbp_->Release(); wbp_ = 0; } if (sct_) { sct_->Release(); sct_ = 0; }
    if (hwnd_) { HWND h = hwnd_; hwnd_ = 0; DestroyWindow(h); }
    if (inplace_) { inplace_ = false; if (ips_) ips_->OnInPlaceDeactivate(); }
    if (frame_) { frame_->Release(); frame_ = 0; } if (uiw_) { uiw_->Release(); uiw_ = 0; }
    lstrcpyW(state_, L"closed");
  }
  void SetState(const wchar_t* t) { lstrcpynW(state_, t, 200); if (hwnd_) InvalidateRect(hwnd_, NULL, TRUE); }
  // ---- engine session
  void ParseTarget() {
    nav_[0] = 0; wchar_t path[2048]; path[0] = 0;
    if (!_wcsnicmp(url_, L"gie:", 4)) {
      const wchar_t* w = url_ + 4; while (*w == L'/') w++; char t[2000];
      WideCharToMultiByte(CP_UTF8, 0, w, -1, t, sizeof t, NULL, NULL);
      if (!strstr(t, "://") && _strnicmp(t, "about:", 6)) snprintf(nav_, sizeof nav_, "http://%s", t); else lstrcpynA(nav_, t, sizeof nav_);
      Log("target (gie:) %s", nav_); return;
    }
    int n = lstrlenW(url_); if (n < 6 || _wcsicmp(url_ + n - 5, L".gieh")) return;
    FileFromUrl(url_, path, 2048);
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { Log("cannot read %ls (err %lu)", path, GetLastError()); return; }
    char b[2048]; DWORD got = 0; ReadFile(f, b, sizeof b - 1, &got, NULL); CloseHandle(f); b[got] = 0;
    char* p = b; if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
    char* e = strpbrk(p, "\r\n"); if (e) *e = 0; p = Trim(p);
    if (!_strnicmp(p, "http://", 7) || !_strnicmp(p, "https://", 8) || !_strnicmp(p, "about:", 6)) { lstrcpynA(nav_, p, sizeof nav_); Log("target (file) %s", nav_); }
    else Log("%ls has no URL on its first line - stage 0 page only", path);
  }
  void SendLine(const char* line) {
    if (!sess_ || sess_->s == INVALID_SOCKET) return; char buf[2300]; int n = snprintf(buf, sizeof buf, "%s\r\n", line);
    ::send(sess_->s, buf, n, 0); Log("engine> %s", line);
  }
  void BeginSession() {
    if (sess_ || !nav_[0] || !hwnd_) return;
    if (TryAdopt()) return;
    lstrcpynA(curNav_, nav_, sizeof curNav_); lastLoc_[0] = 0; phase_ = 0; sawLoading_ = false;
    Cfg c; char err[300];
    if (!LoadCfg(&c)) { Fail(L"GeckoIE install not found (HKLM\\\\Software\\\\IEBridge)"); Log("LoadCfg failed"); return; }
    Log("install dir: %s (ini port %d)", c.base, c.port);
    int port = EnsureEngine(&c, err, sizeof err);
    if (port == 0) { wchar_t w[300]; MultiByteToWideChar(CP_ACP, 0, err, -1, w, 300); Fail(w); Log("engine unavailable: %s", err); return; }
    Sess* ss = (Sess*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Sess));
    ss->wnd = hwnd_; ss->attach = (port < 0); ss->port = (port > 0) ? port : c.port; lstrcpynA(ss->token, c.token, 64); ss->s = INVALID_SOCKET; ss->refs = 2;
    sess_ = ss; InterlockedIncrement(&g_sessions); goSent_ = false;
    HANDLE th = CreateThread(NULL, 0, Worker, ss, 0, NULL); if (th) CloseHandle(th);
    SetTimer(hwnd_, 1, 50, NULL); SetState(L"starting engine...");
    Log("session started, %s port %d", ss->attach ? "attach, scanning from" : "engine", ss->port);
  }
  void CmdStr(DWORD id, const wchar_t* t) {
    if (!sct_) return; VARIANT v; VariantInit(&v); v.vt = VT_BSTR; v.bstrVal = SysAllocString(t);
    HRESULT hr = sct_->Exec(NULL, id, OLECMDEXECOPT_DONTPROMPTUSER, &v, NULL); if (FAILED(hr)) Log("site Exec %lu -> 0x%08lx", id, hr); VariantClear(&v); }
  void CmdInt(DWORD id, long n) {
    if (!sct_) return; VARIANT v; VariantInit(&v); v.vt = VT_I4; v.lVal = n;
    HRESULT hr = sct_->Exec(NULL, id, OLECMDEXECOPT_DONTPROMPTUSER, &v, NULL); if (FAILED(hr)) Log("site Exec %lu -> 0x%08lx", id, hr); }
  bool StripFrame(bool force) {
    LONG st = GetWindowLongW(engine_, GWL_STYLE), ex = GetWindowLongW(engine_, GWL_EXSTYLE);
    LONG badS = WS_POPUP | WS_CAPTION | WS_BORDER | WS_DLGFRAME | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    LONG badE = WS_EX_APPWINDOW | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME | WS_EX_STATICEDGE | WS_EX_TOOLWINDOW;
    bool changed = (st & badS) || (ex & badE) || !(st & WS_CHILD);
    if (!changed && !force) return false;
    st &= ~badS; st |= WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_VISIBLE; ex &= ~badE;
    SetWindowLongW(engine_, GWL_STYLE, st); SetWindowLongW(engine_, GWL_EXSTYLE, ex); return changed;
  }
  void Fill() {
    RECT r; GetClientRect(hwnd_, &r);
    SetWindowPos(engine_, HWND_TOP, 0, 0, r.right, r.bottom, SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    RECT wr, cr; POINT p = { 0, 0 };
    if (GetWindowRect(engine_, &wr) && GetClientRect(engine_, &cr) && cr.right > 0 && cr.bottom > 0) {
      ClientToScreen(engine_, &p); int l = p.x - wr.left, t = p.y - wr.top;
      HRGN rg = CreateRectRgn(l, t, l + cr.right, t + cr.bottom); if (rg) SetWindowRgn(engine_, rg, TRUE);
    }
  }
  void Dock(HWND h) {
    if (!hwnd_ || !IsWindow(h)) return;
    engine_ = h; fail_ = false; HWND old = SetParent(h, hwnd_); StripFrame(true); InstallKeyHook(h); Fill(); docked_ = true;
    Log("docked engine window %p into view %p (old parent %p, GetParent=%p)", h, hwnd_, old, GetParent(h));
    SetState(L"engine docked");
  }
  void IntPtr_dummy() {}
  void Tick() {
    if (fixTicks_ > 0) { fixTicks_--; if (fixTicks_ == 17 || fixTicks_ == 10 || fixTicks_ == 0) { FixAddress(); ApplyFrameTitle(); } }
    if (!docked_) return;
    if (!IsWindow(engine_)) { docked_ = false; engine_ = 0; Fail(L"engine window gone"); return; }
    RECT er, vr; GetClientRect(engine_, &er); GetClientRect(hwnd_, &vr);
    if (StripFrame(false) || er.right != vr.right || er.bottom != vr.bottom) Fill();
    bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (down && !lastDown_) {
      POINT p; RECT wr; GetCursorPos(&p); GetWindowRect(engine_, &wr);
      if (PtInRect(&wr, p) && GetFocus() != engine_ && GetForegroundWindow() == GetAncestor(hwnd_, GA_ROOT)) SetFocus(engine_);
    }
    lastDown_ = down;
  }
  void ClearXfer() { xferPending_ = false; }
  void DetachForAdopt() { sess_ = 0; engine_ = 0; docked_ = false; xferPending_ = false; if (hwnd_) KillTimer(hwnd_, 1); SetState(L"page handed over"); }
  bool TryAdopt() {
    int f = -1; Sess* sg = 0; HWND eng = 0; Doc* o = 0; char url[2100]; DWORD now = GetTickCount(); EnterCriticalSection(&g_cs);
    for (int i = 0; i < 8; i++) { Adopt& a = g_ad[i]; if (a.used && !a.claimed && now - a.tick < 10000 && UrlEq(a.url, nav_)) { f = i; break; } }
    if (f >= 0) { Adopt& a = g_ad[f]; a.claimed = 1; a.used = 0; sg = a.sess; eng = a.engine; o = a.owner; lstrcpynA(url, a.url, sizeof url); }
    LeaveCriticalSection(&g_cs);
    if (f < 0) return false;
    if (o) o->DetachForAdopt();
    sess_ = sg; sess_->wnd = hwnd_; goSent_ = true; phase_ = 0; sawLoading_ = true; claimTick_ = GetTickCount();
    lstrcpynA(curNav_, url, sizeof curNav_); lstrcpynA(lastLoc_, url, sizeof lastLoc_); fixTicks_ = 20;
    SetTimer(hwnd_, 1, 50, NULL); Log("adopted the live engine view for %s", url);
    if (eng && IsWindow(eng)) Dock(eng); else Fail(L"engine window lost");
    return true;
  }
  void RequestAdopt(const char* url, bool replace) {
    EnsureHolder(); int slot = -1; EnterCriticalSection(&g_cs);
    for (int i = 0; i < 8; i++) if (!g_ad[i].used) { slot = i; break; }
    if (slot >= 0) { Adopt& a = g_ad[slot]; a.used = 1; a.claimed = 0; lstrcpynA(a.url, url, sizeof a.url); a.owner = this; a.sess = sess_; a.engine = engine_; a.tick = GetTickCount(); }
    LeaveCriticalSection(&g_cs);
    if (slot < 0) return;
    xferPending_ = true; Log("engine navigated by itself (%s): %s", replace ? "replace entry" : "new entry", url);
    wchar_t w[2200]; lstrcpyW(w, L"gie:"); MultiByteToWideChar(CP_UTF8, 0, url, -1, w + 4, 2190);
    VARIANT vu, vf, ve; VariantInit(&vu); VariantInit(&vf); VariantInit(&ve);
    vu.vt = VT_BSTR; vu.bstrVal = SysAllocString(w); vf.vt = VT_I4; vf.lVal = replace ? 2 : 0;   // 2 = navNoHistory (replace the entry)
    HRESULT hr = wbp_->Navigate2(&vu, &vf, &ve, &ve, &ve); VariantClear(&vu); Log("Navigate2 -> 0x%08lx", hr);
    if (FAILED(hr)) { EnterCriticalSection(&g_cs); g_ad[slot].used = 0; LeaveCriticalSection(&g_cs); xferPending_ = false; }
  }
  void OnLoc(const char* u) {
    if (_strnicmp(u, "http://", 7) && _strnicmp(u, "https://", 8)) return;
    if (UrlEq(u, lastLoc_)) { lstrcpynA(lastLoc_, u, sizeof lastLoc_); return; }
    bool first = (lastLoc_[0] == 0); lstrcpynA(lastLoc_, u, sizeof lastLoc_);
    if (first && UrlEq(u, curNav_)) return;               // the page IE is already showing
    if (xferPending_ || !wbp_ || !sess_) return;
    if (phase_ != 0 && claimTick_ && GetTickCount() - claimTick_ < 1500) return;
    RequestAdopt(u, phase_ == 0);                          // still loading what IE asked for = redirect: replace the entry
  }
  void FixAddress() {
    if (!hwnd_ || !curNav_[0]) return; HWND root = GetAncestor(hwnd_, GA_ROOT); if (!root) return;
    FE fe = { 0 }; EnumChildWindows(root, FindAddr, (LPARAM)&fe); if (!fe.found || GetFocus() == fe.found) return;
    wchar_t w[2100]; MultiByteToWideChar(CP_UTF8, 0, curNav_, -1, w, 2100); SendMessageW(fe.found, WM_SETTEXT, 0, (LPARAM)w);
  }
  void ApplyFrameTitle() {
    if (g_ieMajor >= 7) return;
    if (!hwnd_ || !title_[0]) return; HWND root = GetAncestor(hwnd_, GA_ROOT); if (!root) return;
    wchar_t full[800], sx[80], cur[800]; lstrcpynW(full, title_, 600);
    if (g_suffix[0]) { MultiByteToWideChar(CP_ACP, 0, g_suffix, -1, sx, 80); lstrcatW(full, L" - "); lstrcatW(full, sx); }
    GetWindowTextW(root, cur, 800); if (lstrcmpW(cur, full)) SetWindowTextW(root, full);
  }
  void Fail(const wchar_t* t) { fail_ = true; SetState(t); }
  void NavTo(const char* url, bool newWin) {
    if (!wbp_ || !url[0]) return; wchar_t w[2200]; lstrcpyW(w, L"gie:"); MultiByteToWideChar(CP_UTF8, 0, url, -1, w + 4, 2190);
    VARIANT vu, vf, ve; VariantInit(&vu); VariantInit(&vf); VariantInit(&ve);
    vu.vt = VT_BSTR; vu.bstrVal = SysAllocString(w); vf.vt = VT_I4; vf.lVal = newWin ? 1 : 0;
    wbp_->Navigate2(&vu, &vf, &ve, &ve, &ve); VariantClear(&vu);
  }
  // Right click in the page: IE's own page menu belongs to mshtml, so this is the same menu rebuilt as a normal Windows menu.
  void OnRClick() {   // seen by the hook; the engine's CTX message normally follows within a few ms
    if (inMenu_ || !hwnd_) return; ctxPending_ = true; SetTimer(hwnd_, 3, 400, NULL); Log("right click in the engine window");
  }
  void OnCtxTimeout() {
    if (hwnd_) KillTimer(hwnd_, 3);
    if (ctxPending_) { Log("no CTX from the engine, showing the basic page menu"); OnCtx("fallback=1"); }
  }
  void OnCtx(const char* arg) {
    if (hwnd_) KillTimer(hwnd_, 3);
    ctxPending_ = false;
    if (inMenu_ || !hwnd_) return;
    if (strstr(arg, "prevented=1")) { Log("page handles its own context menu"); return; }
    Log("context menu: %.160s", arg);
    char link[2100], img[2100], st[700], lt[400]; link[0] = img[0] = st[0] = lt[0] = 0; bool sel = false, edit = false; const char* p = arg;
    while (*p) {
      while (*p == ' ') p++; if (!*p) break;
      const char* e = strchr(p, ' '); size_t n = e ? (size_t)(e - p) : strlen(p); char tok[2300]; if (n >= sizeof tok) n = sizeof tok - 1;
      memcpy(tok, p, n); tok[n] = 0;
      if (!strncmp(tok, "link=", 5)) PctDecode(tok + 5, link, sizeof link); else if (!strncmp(tok, "img=", 4)) PctDecode(tok + 4, img, sizeof img);
      else if (!strncmp(tok, "st=", 3)) PctDecode(tok + 3, st, sizeof st);
      else if (!strncmp(tok, "lt=", 3)) PctDecode(tok + 3, lt, sizeof lt);
      else if (!strcmp(tok, "sel=1")) sel = true; else if (!strcmp(tok, "edit=1")) edit = true;
      p += n;
    }
    if (st[0]) sel = true;
    unsigned mask = (link[0] ? C_LINK : 0) | (img[0] ? C_IMG : 0) | (edit ? C_EDIT : 0) | (sel ? C_SEL : 0); if (!mask) mask = C_PAGE;
    LayoutEnt le[NCTX + 2]; int n = LayoutFrom(g_ctxLayout, le);
    HMENU m = CreatePopupMenu(); int lastKind = 0, count = 0;   // 0 nothing yet, 1 item, 2 separator
    for (int i = 0; i < n; i++) {
      if (!le[i].on) continue; const CtxDef& d = g_ctxTbl[le[i].idx];
      if (!g_devtools && !strcmp(d.id, "inspect")) continue;
      if (d.ctx & C_SEP) { if (lastKind == 1) { AppendMenuW(m, MF_SEPARATOR, 0, NULL); lastKind = 2; } continue; }
      if (!((d.ctx & C_ALWAYS) || (d.ctx & mask))) continue;
      if (!strcmp(d.id, "zoom")) {
        HMENU zm = CreatePopupMenu(); AppendMenuW(zm, MF_STRING, 2001, L"Zoom &In"); AppendMenuW(zm, MF_STRING, 2002, L"Zoom &Out"); AppendMenuW(zm, MF_STRING, 2003, L"&Reset (100%)");
        AppendMenuW(m, MF_POPUP, (UINT_PTR)zm, d.label);
      } else if (!strcmp(d.id, "search")) {
        if (!st[0]) continue;
        wchar_t sw[120], lab[200]; MultiByteToWideChar(CP_UTF8, 0, st, -1, sw, 120); if (lstrlenW(sw) > 26) { sw[26] = 0; lstrcatW(sw, L"..."); }
        lstrcpyW(lab, L"Search the Web for \""); lstrcatW(lab, sw); lstrcatW(lab, L"\""); AppendMenuW(m, MF_STRING, 1000 + le[i].idx, lab);
      } else AppendMenuW(m, MF_STRING, 1000 + le[i].idx, d.label);
      lastKind = 1; count++;
    }
    if (lastKind == 2) DeleteMenu(m, GetMenuItemCount(m) - 1, MF_BYPOSITION);
    if (count == 0) AppendMenuW(m, MF_STRING, 1000 + NCTX - 1, L"GeckoIE Settings...");   // never leave the user without a way back
    POINT pt; GetCursorPos(&pt); inMenu_ = true; SetForegroundWindow(GetAncestor(hwnd_, GA_ROOT));
    int id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTBUTTON | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, NULL);
    DestroyMenu(m); inMenu_ = false; if (!hwnd_) return; PostMessageW(hwnd_, WM_NULL, 0, 0);
    if (id >= 1000 && id < 1000 + NCTX) RunCtx(g_ctxTbl[id - 1000].id, link, img, st, lt);
    else if (id == 2001) SendLine("ZOOM in"); else if (id == 2002) SendLine("ZOOM out"); else if (id == 2003) SendLine("ZOOM reset");
  }
  void RunCtx(const char* it, const char* link, const char* img, const char* st, const char* lt) {
    const char* pu = lastLoc_[0] ? lastLoc_ : curNav_; wchar_t wl[2200]; bool edt = false;
    if (!strcmp(it, "link_open")) NavTo(link, false);
    else if (!strcmp(it, "link_newwin")) NavTo(link, true);
    else if (!strcmp(it, "link_classic")) OpenClassic(link);
    else if (!strcmp(it, "link_save")) SaveAs(link, "");
    else if (!strcmp(it, "link_copy")) { MultiByteToWideChar(CP_UTF8, 0, link, -1, wl, 2200); SetClip(hwnd_, wl); }
    else if (!strcmp(it, "link_fav")) { wchar_t lw[420]; lw[0] = 0; if (lt[0]) MultiByteToWideChar(CP_UTF8, 0, lt, -1, lw, 420); MultiByteToWideChar(CP_UTF8, 0, link, -1, wl, 2200); AddFavoriteDisp(wl, lw[0] ? lw : wl); }
    else if (!strcmp(it, "img_newwin")) NavTo(img, true);
    else if (!strcmp(it, "img_save")) SaveAs(img, "image");
    else if (!strcmp(it, "img_copy")) SendLine("COPYIMG");
    else if (!strcmp(it, "img_copyurl")) { MultiByteToWideChar(CP_UTF8, 0, img, -1, wl, 2200); SetClip(hwnd_, wl); }
    else if (!strcmp(it, "undo")) { SendLine("EDIT undo"); edt = true; }
    else if (!strcmp(it, "cut")) { SendLine("EDIT cut"); edt = true; }
    else if (!strcmp(it, "copy")) { SendLine("EDIT copy"); edt = true; }
    else if (!strcmp(it, "paste")) { SendLine("EDIT paste"); edt = true; }
    else if (!strcmp(it, "delete")) { SendLine("EDIT delete"); edt = true; }
    else if (!strcmp(it, "selectall")) { SendLine("EDIT selectall"); edt = true; }
    else if (!strcmp(it, "search")) { char q[1500], u[1600]; UrlEncUtf8(st, q, sizeof q); snprintf(u, sizeof u, "https://www.google.com/search?q=%s", q); NavTo(u, false); }
    else if (!strcmp(it, "back")) { if (wbp_) wbp_->GoBack(); }
    else if (!strcmp(it, "forward")) { if (wbp_) wbp_->GoForward(); }
    else if (!strcmp(it, "reload")) SendLine("RELOAD");
    else if (!strcmp(it, "page_save")) SaveAs(pu, "page");
    else if (!strcmp(it, "page_copyurl")) { MultiByteToWideChar(CP_UTF8, 0, pu, -1, wl, 2200); SetClip(hwnd_, wl); }
    else if (!strcmp(it, "page_fav")) { MultiByteToWideChar(CP_UTF8, 0, pu, -1, wl, 2200); AddFavoriteDisp(wl, title_[0] ? title_ : wl); }
    else if (!strcmp(it, "page_shortcut")) CreateShortcut(pu);
    else if (!strcmp(it, "page_mail")) MailPage(pu);
    else if (!strcmp(it, "page_source")) { char vs[2200]; snprintf(vs, sizeof vs, "view-source:%s", pu); NavTo(vs, true); }
    else if (!strcmp(it, "page_classic")) OpenClassic(pu);
    else if (!strcmp(it, "inspect")) SendLine("DEVTOOLS");
    else if (!strcmp(it, "gieset")) ShowSettingsWindow(GetAncestor(hwnd_, GA_ROOT));
    if (edt && docked_ && engine_ && IsWindow(engine_)) SetFocus(engine_);
  }
  void SaveAs(const char* url, const char* kind, const char* pref = 0) {
    if (!url[0]) return;
    char seg[300]; seg[0] = 0; const char* e = url + strlen(url); const char* cut = strpbrk(url, "?#"); if (cut) e = cut;
    const char* sp = e; while (sp > url && sp[-1] != '/') --sp; size_t n = (size_t)(e - sp); if (n >= sizeof seg) n = sizeof seg - 1; memcpy(seg, sp, n); seg[n] = 0;
    char dec[300]; PctDecode(seg, dec, sizeof dec); wchar_t path[MAX_PATH]; path[0] = 0; MultiByteToWideChar(CP_UTF8, 0, dec, -1, path, 200);
    if (pref && pref[0]) MultiByteToWideChar(CP_UTF8, 0, pref, -1, path, 200);
    if (!path[0]) { if (!strcmp(kind, "page") && title_[0]) { lstrcpynW(path, title_, 150); lstrcatW(path, L".html"); } else lstrcpyW(path, !strcmp(kind, "image") ? L"image" : L"page.html"); }
    else if (!strcmp(kind, "page") && !wcschr(path, L'.')) lstrcatW(path, L".html");
    SanitizeName(path);
    OPENFILENAMEW ofn; memset(&ofn, 0, sizeof ofn); ofn.lStructSize = sizeof ofn; ofn.hwndOwner = GetAncestor(hwnd_, GA_ROOT);
    ofn.lpstrFilter = L"All Files (*.*)\0*.*\0"; ofn.lpstrFile = path; ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    if (!GetSaveFileNameW(&ofn)) return;
    char p8[900], eu[1100], ep[1300], cmd[2300]; WideCharToMultiByte(CP_UTF8, 0, path, -1, p8, sizeof p8, NULL, NULL);
    UrlEncUtf8(url, eu, sizeof eu); UrlEncUtf8(p8, ep, sizeof ep); snprintf(cmd, sizeof cmd, "SAVE %s %s", eu, ep); SendLine(cmd);
  }
  void CreateShortcut(const char* url) {
    wchar_t desk[MAX_PATH]; if (!SHGetSpecialFolderPathW(NULL, desk, CSIDL_DESKTOPDIRECTORY, FALSE)) return;
    wchar_t name[200], path[MAX_PATH + 220]; lstrcpynW(name, title_[0] ? title_ : L"GeckoIE page", 120); SanitizeName(name);
    lstrcpyW(path, desk); lstrcatW(path, L"\\"); lstrcatW(path, name); lstrcatW(path, L".url");
    char body[2400]; int n = snprintf(body, sizeof body, "[InternetShortcut]\r\nURL=%s\r\n", url);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(f, body, n, &w, NULL); CloseHandle(f); MessageBoxW(GetAncestor(hwnd_, GA_ROOT), L"A shortcut to this page was created on the desktop.", L"GeckoIE", MB_ICONINFORMATION); }
  }
  void MailPage(const char* url) {
    char t8[700], et[2200], eu[2300], cmd[5000]; t8[0] = 0; if (title_[0]) WideCharToMultiByte(CP_UTF8, 0, title_, -1, t8, sizeof t8, NULL, NULL);
    UrlEncUtf8(t8, et, sizeof et); UrlEncUtf8(url, eu, sizeof eu); snprintf(cmd, sizeof cmd, "mailto:?subject=%s&body=%s", et, eu);
    ShellExecuteA(NULL, "open", cmd, NULL, NULL, SW_SHOWNORMAL);
  }
  void OpenClassic(const char* url) {   // a new IE process that starts with IEBRIDGE_CLASSIC=1, so GeckoIE stays out of it
    wchar_t pf[MAX_PATH], w[2200], cmdl[MAX_PATH + 2300]; if (!SHGetSpecialFolderPathW(NULL, pf, CSIDL_PROGRAM_FILES, FALSE)) return;
    MultiByteToWideChar(CP_UTF8, 0, url, -1, w, 2200);
    lstrcpyW(cmdl, L"\""); lstrcatW(cmdl, pf); lstrcatW(cmdl, L"\\Internet Explorer\\iexplore.exe\" \""); lstrcatW(cmdl, w); lstrcatW(cmdl, L"\"");
    SetEnvironmentVariableW(L"IEBRIDGE_CLASSIC", L"1");
    STARTUPINFOW si; PROCESS_INFORMATION pi; memset(&si, 0, sizeof si); si.cb = sizeof si;
    if (CreateProcessW(NULL, cmdl, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); Log("opened a classic IE window"); }
    else Log("classic IE failed (%lu)", GetLastError());
    SetEnvironmentVariableW(L"IEBRIDGE_CLASSIC", NULL);
  }
  // A page <select>: shown as a native menu below the control.
  void OnSelect(const char* arg) {
    if (inMenu_ || !hwnd_) { SendLine("SELECTED -1"); return; }
    int x = 0, y = 0, sel = -1, k = 0; HMENU m = CreatePopupMenu(); const char* p = arg;
    while (*p) {
      while (*p == ' ') p++; if (!*p) break;
      const char* e = strchr(p, ' '); size_t n = e ? (size_t)(e - p) : strlen(p); char tok[1000]; if (n >= sizeof tok) n = sizeof tok - 1; memcpy(tok, p, n); tok[n] = 0; p += n;
      if (!strncmp(tok, "x=", 2)) x = atoi(tok + 2); else if (!strncmp(tok, "y=", 2)) y = atoi(tok + 2); else if (!strncmp(tok, "sel=", 4)) sel = atoi(tok + 4);
      else if (tok[0] == 'o' || tok[0] == 'd' || tok[0] == 'g') {
        char d[800]; PctDecode(tok + 1, d, sizeof d); wchar_t w[420], w2[860]; MultiByteToWideChar(CP_UTF8, 0, d, -1, w, 420); int j = 0;
        for (int i = 0; w[i] && j < 850; i++) { w2[j++] = w[i]; if (w[i] == L'&') w2[j++] = L'&'; } w2[j] = 0;
        if (tok[0] == 'g') AppendMenuW(m, MF_STRING | MF_GRAYED | MF_DISABLED, 0, w2[0] ? w2 : L" ");
        else { AppendMenuW(m, MF_STRING | (tok[0] == 'd' ? MF_GRAYED : 0) | (k == sel ? MF_CHECKED : 0), 3000 + k, w2[0] ? w2 : L" "); k++; }
      }
    }
    if (!k) { DestroyMenu(m); SendLine("SELECTED -1"); return; }
    for (int i = 0; i < 60 && (GetAsyncKeyState(VK_LBUTTON) & 0x8000); i++) Sleep(10);   // let the click that opened it finish
    inMenu_ = true; SetForegroundWindow(GetAncestor(hwnd_, GA_ROOT));
    int id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTBUTTON, x, y, 0, hwnd_, NULL);
    DestroyMenu(m); inMenu_ = false; if (!hwnd_) return; PostMessageW(hwnd_, WM_NULL, 0, 0);
    char r[40]; snprintf(r, sizeof r, "SELECTED %d", id >= 3000 ? id - 3000 : -1); SendLine(r);
    if (docked_ && engine_ && IsWindow(engine_)) SetFocus(engine_);
  }
  // A download the engine did not start itself: hand it to Internet Explorer (or save it ourselves, see Settings).
  void OnDownload(const char* arg) {
    char url[2100], name[400]; url[0] = name[0] = 0; const char* p = arg;
    while (*p) {
      while (*p == ' ') p++; if (!*p) break;
      const char* e = strchr(p, ' '); size_t n = e ? (size_t)(e - p) : strlen(p); char tok[7200]; if (n >= sizeof tok) n = sizeof tok - 1; memcpy(tok, p, n); tok[n] = 0; p += n;
      if (!strncmp(tok, "url=", 4)) PctDecode(tok + 4, url, sizeof url); else if (!strncmp(tok, "name=", 5)) PctDecode(tok + 5, name, sizeof name);
    }
    if (!url[0]) return;
    static char last[2100]; static DWORD lastTick = 0;
    if (!strcmp(last, url) && GetTickCount() - lastTick < 20000) { Log("download hand-off repeated, ignored"); return; }
    lstrcpynA(last, url, sizeof last); lastTick = GetTickCount();
    Log("download: %s (%s)", url, g_dlMode ? "saved by the engine" : "Internet Explorer");
    if (g_dlMode) { SaveAs(url, "", name); return; }
    // The engine fetches the file (modern TLS, its cookies) and Internet Explorer then opens it with its File Download window.
    char eu[2300], en[500], cmd[2900]; UrlEncUtf8(url, eu, sizeof eu); UrlEncUtf8(name[0] ? name : "download", en, sizeof en);
    snprintf(cmd, sizeof cmd, "FETCH %s %s", eu, en); SendLine(cmd);
    CmdInt(CMD_SETDOWNLOADSTATE, 1); CmdStr(CMD_SETPROGRESSTEXT, L"Downloading...");
  }
  void OnDlProg(const char* arg) {
    int pct = atoi(arg); const char* sp = strchr(arg, ' '); const char* nm = sp ? strchr(sp + 1, ' ') : 0; char d[700]; d[0] = 0;
    if (nm) PctDecode(nm + 1, d, sizeof d);
    wchar_t w[500], t[700]; MultiByteToWideChar(CP_UTF8, 0, d, -1, w, 500);
    if (pct >= 0) wsprintfW(t, L"Downloading %s... %d%%", w, pct); else wsprintfW(t, L"Downloading %s...", w);
    CmdInt(CMD_SETDOWNLOADSTATE, 1); CmdStr(CMD_SETPROGRESSTEXT, t);
  }
  void OnDlDone(const char* arg) {
    char p8[900]; PctDecode(arg, p8, sizeof p8); wchar_t wp[MAX_PATH], url[2200]; MultiByteToWideChar(CP_UTF8, 0, p8, -1, wp, MAX_PATH);
    CmdInt(CMD_SETDOWNLOADSTATE, 0); CmdStr(CMD_SETPROGRESSTEXT, L"Download complete");
    DWORD n = 2200; if (FAILED(UrlCreateFromPathW(wp, url, &n, 0)) || !wbp_) return;
    Log("download finished, opening in Internet Explorer: %ls", url);
    VARIANT vu, vf, ve; VariantInit(&vu); VariantInit(&vf); VariantInit(&ve);
    vu.vt = VT_BSTR; vu.bstrVal = SysAllocString(url); vf.vt = VT_I4; vf.lVal = 0;
    HRESULT hr = wbp_->Navigate2(&vu, &vf, &ve, &ve, &ve); VariantClear(&vu); Log("Navigate2 (finished download) -> 0x%08lx", hr);
  }
  void OnFwdKey(WPARAM vk, LPARAM lp) {
    if (!frame_) return; MSG m; memset(&m, 0, sizeof m); m.hwnd = hwnd_;
    m.message = (GetKeyState(VK_MENU) & 0x8000) ? WM_SYSKEYDOWN : WM_KEYDOWN; m.wParam = vk; m.lParam = lp; m.time = GetTickCount(); GetCursorPos(&m.pt);
    HRESULT hr = frame_->TranslateAccelerator(&m, 0); Log("forwarded key vk=0x%x to IE frame -> 0x%08lx", (unsigned)vk, hr);
  }
  void OnLine(const char* line) {
    const char* sp = strchr(line, ' '); char cmd[32]; size_t n = sp ? (size_t)(sp - line) : strlen(line); if (n > 31) n = 31;
    memcpy(cmd, line, n); cmd[n] = 0; const char* arg = sp ? sp + 1 : "";
    if (strcmp(cmd, "PONG") && strcmp(cmd, "STATUS") && strcmp(cmd, "PROG") && strcmp(cmd, "NAVSTATE") && strcmp(cmd, "TITLE") && strcmp(cmd, "STATE")) Log("engine< %.200s", line);
    if (!strcmp(cmd, "HELLO")) {
      SetState(L"engine connected, loading page...");
      if (!goSent_ && nav_[0]) { goSent_ = true; char b[2200]; snprintf(b, sizeof b, "GO %s", nav_); SendLine(b); }
    } else if (!strcmp(cmd, "HWND")) {
      long long v = 0; const char* a = arg; while (*a == ' ') a++;
      if (a[0] == '0' && (a[1] == 'x' || a[1] == 'X')) v = strtoll(a + 2, NULL, 16); else v = strtoll(a, NULL, 10);
      Dock((HWND)(INT_PTR)v);
    } else if (!strcmp(cmd, "TITLE")) {
      char d[1200]; wchar_t w[600]; PctDecode(arg, d, sizeof d); MultiByteToWideChar(CP_UTF8, 0, d, -1, w, 600); CmdStr(CMD_SETTITLE, w);
      lstrcpynW(title_, w, 600); fixTicks_ = 20; ApplyFrameTitle();
    } else if (!strcmp(cmd, "LOC")) {
      OnLoc(arg);
    } else if (!strcmp(cmd, "CTX")) {
      OnCtx(arg);
    } else if (!strcmp(cmd, "SELECT")) {
      OnSelect(arg);
    } else if (!strcmp(cmd, "DOWNLOAD")) {
      OnDownload(arg);
    } else if (!strcmp(cmd, "NEWWIN")) {
      char nu[2100]; nu[0] = 0; if (!strncmp(arg, "url=", 4)) PctDecode(arg + 4, nu, sizeof nu); if (nu[0]) NavTo(nu, true);
    } else if (!strcmp(cmd, "DEVFAIL")) {
      Log("developer tools failed: %.200s", arg);
      MessageBoxW(GetAncestor(hwnd_, GA_ROOT), L"The developer tools could not be opened in this engine.", L"GeckoIE", MB_ICONINFORMATION);
    } else if (!strcmp(cmd, "DLPROG")) {
      OnDlProg(arg);
    } else if (!strcmp(cmd, "DLDONE")) {
      OnDlDone(arg);
    } else if (!strcmp(cmd, "DLFAIL")) {
      CmdInt(CMD_SETDOWNLOADSTATE, 0); CmdStr(CMD_SETPROGRESSTEXT, L"Download failed"); Log("download failed: %.200s", arg);
      MessageBoxW(GetAncestor(hwnd_, GA_ROOT), L"The download failed.", L"GeckoIE", MB_ICONWARNING);
    } else if (!strcmp(cmd, "SAVED")) {
      Log("save: %.200s", arg);
      if (!strncmp(arg, "err", 3)) MessageBoxW(GetAncestor(hwnd_, GA_ROOT), L"The file could not be saved.", L"GeckoIE", MB_ICONWARNING);
    } else if (!strcmp(cmd, "STATE")) {
      if (!strncmp(arg, "loading", 7)) { sawLoading_ = true; CmdInt(CMD_SETDOWNLOADSTATE, 1); CmdStr(CMD_SETPROGRESSTEXT, L"Opening page..."); }
      else { if (sawLoading_) phase_ = 1; fixTicks_ = 20; CmdInt(CMD_SETDOWNLOADSTATE, 0); CmdStr(CMD_SETPROGRESSTEXT, L"Done"); }
    } else if (!strcmp(cmd, "STATUS")) {
      char d[1200]; wchar_t w[600]; PctDecode(arg, d, sizeof d); MultiByteToWideChar(CP_UTF8, 0, d, -1, w, 600); if (w[0]) CmdStr(CMD_SETPROGRESSTEXT, w);
    } else if (!strcmp(cmd, "KEY")) {
      if (wbp_) { if (!strncmp(arg, "BACK", 4)) wbp_->GoBack(); else if (!strncmp(arg, "FWD", 3)) wbp_->GoForward(); }
    } else if (!strcmp(cmd, "X_CONNECTFAIL")) Fail(L"could not connect to the engine bridge");
    else if (!strcmp(cmd, "X_DISCONNECTED")) Fail(L"engine connection closed");
  }
  static void Row(HDC dc, int y, const wchar_t* k, const wchar_t* v) {
    SetTextColor(dc, RGB(130, 134, 140)); TextOutW(dc, 52, y, k, lstrlenW(k));
    SetTextColor(dc, RGB(235, 235, 235)); TextOutW(dc, 230, y, v, lstrlenW(v));
  }
  static HFONT Fnt(int h, int w, const wchar_t* face) {
    return CreateFontW(h, 0, 0, 0, w, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 5, VARIABLE_PITCH | FF_SWISS, face); }
  void Paint(HWND h) {
    PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT rc; GetClientRect(h, &rc);
    HBRUSH b = CreateSolidBrush(RGB(255, 255, 255)); FillRect(dc, &rc, b); DeleteObject(b); SetBkMode(dc, TRANSPARENT);
    if (fail_) {
      HFONT f = Fnt(-15, FW_NORMAL, L"Tahoma"); HGDIOBJ old = SelectObject(dc, f); SetTextColor(dc, RGB(90, 90, 90));
      RECT r = { 24, 24, rc.right - 24, rc.bottom }; DrawTextW(dc, state_, -1, &r, DT_WORDBREAK | DT_NOPREFIX);
      SelectObject(dc, old); DeleteObject(f);
    } else if (g_home == 1 && !docked_) {
      HFONT f = Fnt(-28, FW_LIGHT, L"Segoe UI Light"); HGDIOBJ old = SelectObject(dc, f); SetTextColor(dc, RGB(120, 120, 120));
      DrawTextW(dc, L"Loading", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      SelectObject(dc, old); DeleteObject(f);
    }
    EndPaint(h, &ps);
  }
  friend LRESULT CALLBACK ViewProc(HWND, UINT, WPARAM, LPARAM);
};

static void SweepAdopt() {
  DWORD now = GetTickCount(); Sess* kill[8]; int nk = 0; EnterCriticalSection(&g_cs);
  for (int i = 0; i < 8; i++) {
    Adopt& a = g_ad[i]; if (!a.used || now - a.tick < 10000) continue;
    if (a.owner) a.owner->ClearXfer(); else kill[nk++] = a.sess;   // owner keeps its session if the navigation never arrived
    a.used = 0;
  }
  LeaveCriticalSection(&g_cs);
  for (int k = 0; k < nk; k++) EndSessRaw(kill[k]);
}
static LRESULT CALLBACK ViewProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  Doc* d = (Doc*)GetWindowLongPtrW(h, GWLP_USERDATA);
  switch (m) {
    case WM_NCCREATE: SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)l)->lpCreateParams); break;
    case WM_PAINT: if (d) { d->Paint(h); return 0; } break;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: InvalidateRect(h, NULL, TRUE); if (d && d->docked_ && IsWindow(d->engine_)) d->Fill(); break;
    case WM_LBUTTONDOWN: SetFocus(h); return 0;
    case WM_SETFOCUS: Log("view WM_SETFOCUS"); if (d && d->docked_ && IsWindow(d->engine_)) SetFocus(d->engine_); break;
    case WM_TIMER: if (d) { if (w == 1) d->Tick(); else if (w == 3) d->OnCtxTimeout(); } return 0;
    case WM_APP + 5: if (d) d->OnRClick(); return 0;
    case WM_APP + 3: if (d) d->OnFwdKey(w, l); return 0;
    case WM_APP + 1: { char* ln = (char*)l; if (d) d->OnLine(ln); HeapFree(GetProcessHeap(), 0, ln); return 0; }
    case WM_NCDESTROY: SetWindowLongPtrW(h, GWLP_USERDATA, 0); break;
  }
  return DefWindowProcW(h, m, w, l);
}

static const GUID CLSID_GieProto = {0x9a3d1c52,0x6e07,0x4b88,{0xa1,0x4c,0x2f,0xd8,0x73,0x0b,0x5e,0x19}};
#define PROTO_CLSID_STR L"{9A3D1C52-6E07-4B88-A14C-2FD8730B5E19}"
static const GUID IID_IIPRoot = {0x79eac9e3,0xbaf9,0x11ce,{0x8c,0x82,0x00,0xaa,0x00,0x4b,0xa9,0x0b}};
static const GUID IID_IIP     = {0x79eac9e4,0xbaf9,0x11ce,{0x8c,0x82,0x00,0xaa,0x00,0x4b,0xa9,0x0b}};
// Minimal pluggable protocol: every gie:<url> "downloads" one byte of application/x-geckoie, so IE hands the
// navigation to our DocObject (which reads the real URL from the moniker).
class Proto : public IInternetProtocol {
  LONG ref_; bool sent_;
public:
  Proto() : ref_(1), sent_(false) { InterlockedIncrement(&g_objs); }
  ~Proto() { InterlockedDecrement(&g_objs); }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** pp) {
    if (IsEqualIID(r, IID_IUnknown) || IsEqualIID(r, IID_IIPRoot) || IsEqualIID(r, IID_IIP)) { *pp = (IInternetProtocol*)this; AddRef(); return S_OK; }
    *pp = 0; return E_NOINTERFACE; }
  ULONG STDMETHODCALLTYPE AddRef() { return InterlockedIncrement(&ref_); }
  ULONG STDMETHODCALLTYPE Release() { LONG n = InterlockedDecrement(&ref_); if (!n) delete this; return n; }
  HRESULT STDMETHODCALLTYPE Start(LPCWSTR url, IInternetProtocolSink* sink, IInternetBindInfo*, DWORD, HANDLE_PTR) {
    Log("gie: protocol Start %ls", url);
    sink->ReportProgress(BINDSTATUS_VERIFIEDMIMETYPEAVAILABLE, MIME_STR);
    sink->ReportData(BSCF_FIRSTDATANOTIFICATION | BSCF_LASTDATANOTIFICATION | BSCF_DATAFULLYAVAILABLE, 1, 1);
    sink->ReportResult(S_OK, 0, NULL); return S_OK; }
  HRESULT STDMETHODCALLTYPE Continue(PROTOCOLDATA*) { return S_OK; }
  HRESULT STDMETHODCALLTYPE Abort(HRESULT, DWORD) { return S_OK; }
  HRESULT STDMETHODCALLTYPE Terminate(DWORD) { return S_OK; }
  HRESULT STDMETHODCALLTYPE Suspend() { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Resume() { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Read(void* pv, ULONG cb, ULONG* pcb) {
    if (sent_ || cb < 1) { *pcb = 0; return sent_ ? S_FALSE : S_OK; } *(char*)pv = ' '; *pcb = 1; sent_ = true; return S_OK; }
  HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER, DWORD, ULARGE_INTEGER*) { return E_FAIL; }
  HRESULT STDMETHODCALLTYPE LockRequest(DWORD) { return S_OK; }
  HRESULT STDMETHODCALLTYPE UnlockRequest() { return S_OK; }
};

static const GUID CLSID_GieBtn = {0x5b7e2c91,0x0a3d,0x4f66,{0x8d,0x1b,0x7c,0x44,0xe9,0x20,0x3a,0xf5}};
#define BTN_CLSID_STR L"{5B7E2C91-0A3D-4F66-8D1B-7C44E9203AF5}"
#define BTN_EXT_STR   L"{C3A1F0D2-6B4E-4C7A-9E58-2D1B0F7A6E33}"
class Btn : public IOleCommandTarget {
  LONG ref_;
public:
  Btn() : ref_(1) { InterlockedIncrement(&g_objs); }
  ~Btn() { InterlockedDecrement(&g_objs); }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** pp) {
    if (IsEqualIID(r, IID_IUnknown) || IsEqualIID(r, IID_IOleCommandTarget)) { *pp = (IOleCommandTarget*)this; AddRef(); return S_OK; }
    *pp = 0; return E_NOINTERFACE; }
  ULONG STDMETHODCALLTYPE AddRef() { return InterlockedIncrement(&ref_); }
  ULONG STDMETHODCALLTYPE Release() { LONG n = InterlockedDecrement(&ref_); if (!n) delete this; return n; }
  HRESULT STDMETHODCALLTYPE QueryStatus(const GUID*, ULONG n, OLECMD* c, OLECMDTEXT*) { for (ULONG i = 0; i < n; i++) c[i].cmdf = OLECMDF_SUPPORTED | OLECMDF_ENABLED; return S_OK; }
  HRESULT STDMETHODCALLTYPE Exec(const GUID*, DWORD, DWORD, VARIANTARG*, VARIANTARG*) { ShowSettingsWindow(GetForegroundWindow()); return S_OK; }
};

class Factory : public IClassFactory {
  LONG r_; int kind_;
public:
  Factory(int k = 0) : r_(1), kind_(k) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID i, void** pp) {
    if (IsEqualIID(i, IID_IUnknown) || IsEqualIID(i, IID_IClassFactory)) { *pp = this; AddRef(); return S_OK; }
    *pp = 0; return E_NOINTERFACE; }
  ULONG STDMETHODCALLTYPE AddRef() { return InterlockedIncrement(&r_); }
  ULONG STDMETHODCALLTYPE Release() { LONG n = InterlockedDecrement(&r_); if (!n) delete this; return n; }
  HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* o, REFIID i, void** pp) {
    char nm[220]; IidName(i, nm, sizeof nm); Log("CreateInstance %s", nm);
    if (o) return CLASS_E_NOAGGREGATION;
    if (kind_ == 2) { Btn* bt = new Btn; HRESULT h3 = bt->QueryInterface(i, pp); bt->Release(); return h3; }
    if (kind_) { Proto* pr = new Proto; HRESULT h2 = pr->QueryInterface(i, pp); pr->Release(); return h2; }
    Doc* d = new Doc; HRESULT hr = d->QueryInterface(i, pp); d->Release(); return hr; }
  HRESULT STDMETHODCALLTYPE LockServer(BOOL f) { if (f) InterlockedIncrement(&g_locks); else InterlockedDecrement(&g_locks); return S_OK; }
};

extern "C" {
BOOL WINAPI DllMain(HINSTANCE h, DWORD why, LPVOID) {
  if (why == DLL_PROCESS_ATTACH) {
    g_hinst = h; DisableThreadLibraryCalls(h); InitializeCriticalSection(&g_cs);
    GetModuleFileNameW(h, g_dllDir, MAX_PATH); PathRemoveFileSpecW(g_dllDir); DetectEnv();
    char tmp[MAX_PATH], exe[MAX_PATH]; GetTempPathA(MAX_PATH, tmp); GetModuleFileNameA(NULL, exe, MAX_PATH);
    snprintf(g_log, MAX_PATH, "%sgeckoie-docobj.log", tmp); Log("---- GeckoIE %s: DLL loaded in %s", GIE_VERSION, exe);
  }
  return TRUE;
}
__declspec(dllexport) HRESULT STDAPICALLTYPE DllGetClassObject(REFCLSID c, REFIID i, LPVOID* pp) {
  int kind = IsEqualCLSID(c, CLSID_GieProto) ? 1 : (IsEqualCLSID(c, CLSID_GieBtn) ? 2 : 0);
  if (!kind && !IsEqualCLSID(c, CLSID_GeckoDoc)) return CLASS_E_CLASSNOTAVAILABLE;
  Factory* f = new Factory(kind); HRESULT hr = f->QueryInterface(i, pp); f->Release(); return hr; }
__declspec(dllexport) HRESULT STDAPICALLTYPE DllCanUnloadNow(void) { return (g_objs || g_locks || g_favStarted) ? S_FALSE : S_OK; }

static bool RS(const wchar_t* sub, const wchar_t* name, const wchar_t* val) {
  HKEY k; if (RegCreateKeyExW(HKEY_CLASSES_ROOT, sub, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != 0) return false;
  LONG r = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)val, (DWORD)((lstrlenW(val) + 1) * 2)); RegCloseKey(k); return r == 0; }
static bool RD(const wchar_t* sub, const wchar_t* name, DWORD v) {
  HKEY k; if (RegCreateKeyExW(HKEY_CLASSES_ROOT, sub, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != 0) return false;
  LONG r = RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, 4); RegCloseKey(k); return r == 0; }

static bool RSH(HKEY root, const wchar_t* sub, const wchar_t* name, const wchar_t* val) {
  HKEY k; if (RegCreateKeyExW(root, sub, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != 0) return false;
  LONG r = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)val, (DWORD)((lstrlenW(val) + 1) * 2)); RegCloseKey(k); return r == 0; }
__declspec(dllexport) HRESULT STDAPICALLTYPE DllRegisterServer(void) {
  wchar_t dll[MAX_PATH], k[300]; GetModuleFileNameW(g_hinst, dll, MAX_PATH); bool ok = true;
  ok &= RS(L"CLSID\\" CLSID_STR, NULL, L"GeckoIE DocObject");
  ok &= RS(L"CLSID\\" CLSID_STR L"\\InprocServer32", NULL, dll);
  ok &= RS(L"CLSID\\" CLSID_STR L"\\InprocServer32", L"ThreadingModel", L"Apartment");
  ok &= RS(L"CLSID\\" CLSID_STR L"\\ProgID", NULL, PROGID);
  ok &= RS(L"CLSID\\" CLSID_STR L"\\DocObject", NULL, L"0");
  ok &= RS(PROGID, NULL, L"GeckoIE DocObject");
  ok &= RS(PROGID L"\\CLSID", NULL, CLSID_STR);
  ok &= RS(PROGID L"\\DocObject", NULL, L"0");
  ok &= RS(PROGID L"\\BrowseInPlace", NULL, L"");
  ok &= RD(PROGID, L"EditFlags", 0x10000);
  ok &= RS(EXT_STR, NULL, PROGID);
  ok &= RS(EXT_STR, L"Content Type", MIME_STR);
  wsprintfW(k, L"MIME\\Database\\Content Type\\%s", MIME_STR);
  ok &= RS(k, L"CLSID", CLSID_STR); ok &= RS(k, L"Extension", EXT_STR);
  ok &= RS(L"CLSID\\" PROTO_CLSID_STR, NULL, L"GeckoIE gie: protocol");
  ok &= RS(L"CLSID\\" PROTO_CLSID_STR L"\\InprocServer32", NULL, dll);
  ok &= RS(L"CLSID\\" PROTO_CLSID_STR L"\\InprocServer32", L"ThreadingModel", L"Both");
  ok &= RS(L"PROTOCOLS\\Handler\\gie", NULL, L"GeckoIE gie: protocol");
  ok &= RS(L"PROTOCOLS\\Handler\\gie", L"CLSID", PROTO_CLSID_STR);
  ok &= RS(L"gie", NULL, L"URL: GeckoIE Protocol");
  ok &= RS(L"gie", L"URL Protocol", L"");
  ok &= RS(L"CLSID\\" BTN_CLSID_STR, NULL, L"GeckoIE Settings toolbar button");
  ok &= RS(L"CLSID\\" BTN_CLSID_STR L"\\InprocServer32", NULL, dll);
  ok &= RS(L"CLSID\\" BTN_CLSID_STR L"\\InprocServer32", L"ThreadingModel", L"Apartment");
  {   // optional toolbar button, hidden until the user adds it (View > Toolbars > Customize)
    const wchar_t* ek = L"Software\\Microsoft\\Internet Explorer\\Extensions\\" BTN_EXT_STR;
    RSH(HKEY_LOCAL_MACHINE, ek, L"CLSID", L"{1FBA04EE-3024-11d2-8F1F-0000F87ABD16}");
    RSH(HKEY_LOCAL_MACHINE, ek, L"ClsidExtension", BTN_CLSID_STR);
    RSH(HKEY_LOCAL_MACHINE, ek, L"ButtonText", L"GeckoIE Settings");
    RSH(HKEY_LOCAL_MACHINE, ek, L"Default Visible", L"No");
    wchar_t ico[MAX_PATH]; lstrcpyW(ico, dll); PathRemoveFileSpecW(ico); PathAppendW(ico, L"GeckoIE.ico");
    if (GetFileAttributesW(ico) != INVALID_FILE_ATTRIBUTES) { RSH(HKEY_LOCAL_MACHINE, ek, L"Icon", ico); RSH(HKEY_LOCAL_MACHINE, ek, L"HotIcon", ico); }
  }
  return ok ? S_OK : SELFREG_E_CLASS; }
__declspec(dllexport) HRESULT STDAPICALLTYPE DllUnregisterServer(void) {
  wchar_t k[300];
  SHDeleteKeyW(HKEY_CLASSES_ROOT, L"CLSID\\" CLSID_STR); SHDeleteKeyW(HKEY_CLASSES_ROOT, PROGID);
  SHDeleteKeyW(HKEY_CLASSES_ROOT, EXT_STR);
  wsprintfW(k, L"MIME\\Database\\Content Type\\%s", MIME_STR); SHDeleteKeyW(HKEY_CLASSES_ROOT, k);
  SHDeleteKeyW(HKEY_CLASSES_ROOT, L"CLSID\\" PROTO_CLSID_STR); SHDeleteKeyW(HKEY_CLASSES_ROOT, L"PROTOCOLS\\Handler\\gie"); SHDeleteKeyW(HKEY_CLASSES_ROOT, L"gie");
  SHDeleteKeyW(HKEY_CLASSES_ROOT, L"CLSID\\" BTN_CLSID_STR);
  SHDeleteKeyW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Internet Explorer\\Extensions\\" BTN_EXT_STR);
  return S_OK; }
}
