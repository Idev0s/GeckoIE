using System;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
using System.Windows.Forms;
using Microsoft.Win32;

[assembly: AssemblyTitle("IEBridge BHO")]
[assembly: AssemblyVersion("1.0.0.0")]

namespace IEBridgeBho
{
    [ComImport]
    [Guid("FC4801A3-2BA9-11CF-A229-00AA003D7352")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IObjectWithSite
    {
        [PreserveSig]
        int SetSite([In, MarshalAs(UnmanagedType.IUnknown)] object site);

        [PreserveSig]
        int GetSite(ref Guid guid, out IntPtr ppvSite);
    }

    // The DWebBrowserEvents2 events IE fires on its browser object.
    [ComVisible(true)]
    [Guid("34A715A0-6587-11D0-924A-0020AFC7AC4D")]
    [InterfaceType(ComInterfaceType.InterfaceIsIDispatch)]
    public interface DWebBrowserEvents2
    {
        [DispId(250)]
        void BeforeNavigate2(
            [In, MarshalAs(UnmanagedType.IDispatch)] object pDisp,
            [In] ref object URL,
            [In] ref object Flags,
            [In] ref object TargetFrameName,
            [In] ref object PostData,
            [In] ref object Headers,
            [In, Out] ref bool Cancel);

        [DispId(105)]
        void CommandStateChange([In] int Command, [In] bool Enable);

        [DispId(252)]
        void NavigateComplete2(
            [In, MarshalAs(UnmanagedType.IDispatch)] object pDisp,
            [In] ref object URL);

        [DispId(259)]
        void DocumentComplete(
            [In, MarshalAs(UnmanagedType.IDispatch)] object pDisp,
            [In] ref object URL);
    }

    [ComVisible(true)]
    [ClassInterface(ClassInterfaceType.None)]
    public sealed class EventSink : DWebBrowserEvents2
    {
        private readonly Bho owner;

        public EventSink(Bho owner) { this.owner = owner; }

        public void BeforeNavigate2(object pDisp, ref object URL, ref object Flags,
            ref object TargetFrameName, ref object PostData, ref object Headers, ref bool Cancel)
        {
            try
            {
                bool cancel = false;
                owner.OnBeforeNavigate(pDisp, URL as string, ref cancel);
                if (cancel) Cancel = true;
            }
            catch (Exception ex) { Bho.Log("BeforeNavigate2 error: " + ex.Message); }
        }

        public void CommandStateChange(int Command, bool Enable)
        {
            // 1 = Forward, 2 = Back
            if (Command == 1 || Command == 2)
                Bho.Log("CommandStateChange " + (Command == 2 ? "Back" : "Forward") + " enabled=" + Enable);
        }

        public void NavigateComplete2(object pDisp, ref object URL)
        {
            try { owner.OnNavigateComplete(pDisp, URL as string); }
            catch (Exception ex) { Bho.Log("NavigateComplete2 error: " + ex.Message); }
        }

        public void DocumentComplete(object pDisp, ref object URL)
        {
            try { owner.OnDocumentComplete(pDisp, URL as string); }
            catch (Exception ex) { Bho.Log("DocumentComplete error: " + ex.Message); }
        }
    }

    [ComVisible(true)]
    [Guid("FC8F2A4B-3C6E-4B53-9D0F-6A1C2E7B5D31")]
    [ClassInterface(ClassInterfaceType.None)]
    [ProgId("IEBridge.Bho")]
    public sealed class Bho : IObjectWithSite
    {
        private static readonly object logLock = new object();
        private static string dllDir;

        private object site;
        private IConnectionPoint connectionPoint;
        private int cookie;

        private Settings settings;
        private Control ui;
        private EngineSession session;

        // IE's history is made of small placeholder pages, one per engine page, with the real
        // address after the "#". That gives IE a normal Back/Forward list to work with.
        private string shellBase = "about:blank";
        private string shellFile = "";
        private bool shellIsFile;
        private int navCounter;
        private int lastSyntheticN = -1;  // entry number of the placeholder we just created
        private int currentN = -1;        // entry number IE is on
        private int handledN = -1;        // entry BeforeNavigate2 already dealt with
        private string lastSynthetic;     // same idea when the placeholder has no entry number
        private string handledUrl;
        private bool classicMode;         // this IE process was started as a plain, engine-free IE
        private bool historyNavInFlight;  // the engine is loading a page IE's Back/Forward asked for
        private string bypassUrl;         // URL IE must render itself (engine unavailable)
        private string currentEngineUrl = "";
        private string pendingTitle;
        private const string Version = "v0 build 261024.3";
        private bool docObjActive;        // HostMode=docobj: http(s) is redirected to gie: and the DocObject view hosts the engine

        // ---------------------------------------------------------- logging

        internal static string DllDir
        {
            get
            {
                if (dllDir == null)
                    dllDir = Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location);
                return dllDir;
            }
        }

        public static void Log(string message)
        {
            try
            {
                lock (logLock)
                {
                    File.AppendAllText(Path.Combine(DllDir, "bho-log.txt"),
                        DateTime.Now.ToString("HH:mm:ss") + " [pid " +
                        System.Diagnostics.Process.GetCurrentProcess().Id + "] " + message + "\r\n");
                }
            }
            catch (Exception) { }
        }

        // ------------------------------------------------------ IObjectWithSite

        public int SetSite(object newSite)
        {
            try
            {
                Disconnect();
                site = newSite;
                if (site != null)
                {
                    settings = Settings.Load(Path.GetDirectoryName(DllDir));
                    ResolveShell();
                    classicMode = Environment.GetEnvironmentVariable("IEBRIDGE_CLASSIC") == "1";
                    if (classicMode) Log("classic mode: this IE process uses Internet Explorer's own rendering");
                    ui = new Control();
                    IntPtr forceHandle = ui.Handle; // create it now, on IE's UI thread
                    Connect();
                    Log("SetSite: attached (engine " + (EngineAllowed() ? "enabled" : "disabled") + ")");

                    // Start the engine now, hidden, so the first page doesn't wait for it.
                    if (EngineAllowed() && settings.DocObj && settings.InProc && DocObjRegistered())
                    {
                        // Native view: start the engine now and let IE navigate to gie: addresses.
                        if (InProcEngine.Ensure(settings))
                        {
                            InProcEngine.AddSession();
                            docObjActive = true;
                            Log("GeckoIE " + Version + " - docobj mode: engine running, http(s) goes to gie:");
                        }
                        else Log("docobj mode: engine unavailable, IE renders pages itself");
                    }
                    else if (EngineAllowed() && settings.Prewarm)
                        session = EngineSession.TryCreate(this, settings, ui, true);
                }
                else
                {
                    Log("SetSite: released");
                }
            }
            catch (Exception ex)
            {
                Log("SetSite error: " + ex.Message);
            }
            return 0; // S_OK, never fail inside IE
        }

        public int GetSite(ref Guid guid, out IntPtr ppvSite)
        {
            ppvSite = IntPtr.Zero;
            if (site == null) return unchecked((int)0x80004005); // E_FAIL
            IntPtr unk = Marshal.GetIUnknownForObject(site);
            try { return Marshal.QueryInterface(unk, ref guid, out ppvSite); }
            finally { Marshal.Release(unk); }
        }

        private void Connect()
        {
            IConnectionPointContainer container = (IConnectionPointContainer)site;
            Guid iid = typeof(DWebBrowserEvents2).GUID;
            container.FindConnectionPoint(ref iid, out connectionPoint);
            connectionPoint.Advise(new EventSink(this), out cookie);
        }

        private void Disconnect()
        {
            if (session != null) { session.Dispose(); session = null; }
            if (docObjActive)
            {
                docObjActive = false;
                try { InProcEngine.ReleaseDocObj(settings); } catch (Exception ex) { Log("release failed: " + ex.Message); }
            }
            if (connectionPoint != null)
            {
                try { connectionPoint.Unadvise(cookie); } catch (Exception) { }
                connectionPoint = null;
                cookie = 0;
            }
            if (ui != null) { ui.Dispose(); ui = null; }
            site = null;
        }

        // ------------------------------------------------ late-bound IE calls

        private object GetProp(object target, string name)
        {
            return target.GetType().InvokeMember(name, BindingFlags.GetProperty, null, target, null);
        }

        private void SetProp(object target, string name, object value)
        {
            target.GetType().InvokeMember(name, BindingFlags.SetProperty, null, target, new object[] { value });
        }

        public int BrowserHwnd()
        {
            try { return Convert.ToInt32(GetProp(site, "HWND")); }
            catch (Exception) { return 0; }
        }

        private string BrowserUrl()
        {
            try { return GetProp(site, "LocationURL") as string; }
            catch (Exception) { return null; }
        }

        private void IeNavigate(string url)
        {
            site.GetType().InvokeMember("Navigate", BindingFlags.InvokeMethod, null, site, new object[] { url });
        }

        // True when pDisp is the top-level browser rather than a frame inside the page.
        private bool IsTopLevel(object pDisp)
        {
            if (site == null || pDisp == null) return false;
            IntPtr a = Marshal.GetIUnknownForObject(pDisp);
            IntPtr b = Marshal.GetIUnknownForObject(site);
            try { return a == b; }
            finally { Marshal.Release(a); Marshal.Release(b); }
        }

        private static bool IsHttp(string url)
        {
            return url != null &&
                   (url.StartsWith("http://", StringComparison.OrdinalIgnoreCase) ||
                    url.StartsWith("https://", StringComparison.OrdinalIgnoreCase));
        }

        private static bool Same(string a, string b)
        {
            return string.Equals(a, b, StringComparison.OrdinalIgnoreCase);
        }

        private void ResolveShell()
        {
            string s = settings.ShellUrl;
            if (string.Equals(s, "auto", StringComparison.OrdinalIgnoreCase))
            {
                string f = Path.Combine(settings.BaseDir, "shell.html");
                s = File.Exists(f) ? new Uri(f).AbsoluteUri : "about:blank";
            }
            shellBase = s;
            shellIsFile = s.StartsWith("file:", StringComparison.OrdinalIgnoreCase);
            shellFile = shellIsFile ? s.Substring(s.LastIndexOf('/') + 1) : "";
            Log("history placeholder page: " + shellBase);
        }

        // True for the placeholder page URLs we create (and IE reports back to us).
        private bool IsShellUrl(string url)
        {
            if (url == null) return false;
            if (shellIsFile)
                return url.StartsWith("file:", StringComparison.OrdinalIgnoreCase) &&
                       url.IndexOf("/" + shellFile + "?", StringComparison.OrdinalIgnoreCase) >= 0 &&
                       url.IndexOf('#') > 0;
            return url.StartsWith(shellBase + "#", StringComparison.OrdinalIgnoreCase);
        }

        // Splits a placeholder URL into its entry number (-1 if it has none) and the real address.
        private void ParseShell(string url, out int n, out string target)
        {
            int hash = url.IndexOf('#');
            target = hash >= 0 ? url.Substring(hash + 1) : "";
            n = -1;
            if (!shellIsFile || hash < 0) return;
            string key = "/" + shellFile + "?";
            int q = url.IndexOf(key, StringComparison.OrdinalIgnoreCase);
            if (q < 0) return;
            int start = q + key.Length;
            if (hash <= start) return;
            int v;
            if (int.TryParse(url.Substring(start, hash - start), out v)) n = v;
        }

        private string MakeShellUrl(string target, out int n)
        {
            n = -1;
            if (shellIsFile)
            {
                n = ++navCounter;
                return shellBase + "?" + n + "#" + target;
            }
            return shellBase + "#" + target;
        }

        // IE moved to one of our placeholder pages: Back, Forward, Refresh or the History list.
        private void HandleShellNavigation(string url)
        {
            int n;
            string target;
            ParseShell(url, out n, out target);

            // The page we created ourselves for something the engine already shows.
            bool ours = shellIsFile ? (n >= 0 && n == lastSyntheticN)
                                    : (lastSynthetic != null && Same(url, lastSynthetic));
            if (ours)
            {
                lastSyntheticN = -1;
                lastSynthetic = null;
                currentN = n;
                return;
            }

            if (session == null || session.Failed) return;

            bool sameEntry = !shellIsFile || n == currentN;
            if (Same(target, currentEngineUrl))
            {
                if (sameEntry)
                {
                    Log("refresh: reloading " + target);
                    session.Reload();
                }
            }
            else
            {
                Log("history navigation to " + target);
                historyNavInFlight = true;
                currentEngineUrl = target;
                session.Navigate(target);
            }
            currentN = n;
            session.Show(true);
        }

        // ------------------------------------------------------- IE events

        public void OnBeforeNavigate(object pDisp, string url, ref bool cancel)
        {
            if (!IsTopLevel(pDisp) || url == null) return;
            Log("BeforeNavigate2 " + url);
            if (!EngineAllowed()) return;

            if (docObjActive)
            {
                // Every http(s) page becomes a normal IE navigation to gie:<url>; the DocObject hosts the engine.
                string bypass = Environment.GetEnvironmentVariable("GECKOIE_BYPASS");
                if (!String.IsNullOrEmpty(bypass) && SameUrl(bypass, url))
                {
                    // the DocObject hands a download to Internet Explorer: let IE open it itself
                    Environment.SetEnvironmentVariable("GECKOIE_BYPASS", null);
                    Log("download hand-off, not redirected: " + url);
                    return;
                }
                if (IsHttp(url) && InProcEngine.Alive())
                {
                    cancel = true;
                    string target = "gie:" + url;
                    Log("redirect to " + target);
                    ui.BeginInvoke(new MethodInvoker(delegate() { try { IeNavigate(target); } catch (Exception ex) { Log("redirect failed: " + ex.Message); } }));
                }
                return;
            }

            // IE has to render this one itself because the engine is unavailable.
            if (bypassUrl != null && Same(url, bypassUrl))
            {
                bypassUrl = null;
                if (session != null) session.Show(false);
                return;
            }

            // One of our placeholder pages: IE is going through its history.
            if (IsShellUrl(url))
            {
                HandleShellNavigation(url);
                int hn;
                string ht;
                ParseShell(url, out hn, out ht);
                handledN = hn;
                handledUrl = url;
                return;
            }

            if (IsHttp(url))
            {
                if (session == null)
                    session = EngineSession.TryCreate(this, settings, ui, false);
                if (session != null && session.Navigate(url))
                {
                    historyNavInFlight = false;
                    cancel = true; // the engine takes over this navigation
                    Log("handed to engine: " + url);
                }
                return;
            }

            // Anything else (about:blank, file:, ...) is IE's own content.
            if (session != null) session.Show(false);
        }

        public void OnNavigateComplete(object pDisp, string url)
        {
            if (docObjActive) return;
            if (!IsTopLevel(pDisp) || settings == null || url == null) return;
            Log("NavigateComplete2 " + url);
            if (!IsShellUrl(url)) return;

            int n;
            string target;
            ParseShell(url, out n, out target);
            bool already = shellIsFile ? (n >= 0 && n == handledN) : Same(url, handledUrl);
            if (already)
            {
                handledN = -1;
                handledUrl = null;
            }
            else
            {
                // BeforeNavigate2 was not reported for this one, so handle it now.
                Log("history navigation noticed on NavigateComplete2");
                HandleShellNavigation(url);
            }
            ui.BeginInvoke(new MethodInvoker(ApplyChrome));
        }

        // Back/Forward pressed inside the engine (Alt+Left / Alt+Right): use IE's own history.
        public void GoBack()
        {
            try { site.GetType().InvokeMember("GoBack", BindingFlags.InvokeMethod, null, site, null); }
            catch (Exception ex) { Log("GoBack failed: " + ex.Message); }
        }

        public void GoForward()
        {
            try { site.GetType().InvokeMember("GoForward", BindingFlags.InvokeMethod, null, site, null); }
            catch (Exception ex) { Log("GoForward failed: " + ex.Message); }
        }

        public void OnDocumentComplete(object pDisp, string url)
        {
            if (docObjActive) return;
            if (!IsTopLevel(pDisp) || settings == null) return;
            Log("DocumentComplete " + url);
            if (IsShellUrl(url))
                ui.BeginInvoke(new MethodInvoker(ApplyChrome));
        }

        // Shows the engine's real address and title in IE's own address bar and caption.
        private void ApplyChrome()
        {
            if (docObjActive) return;
            if (site == null) return;
            try
            {
                if (currentEngineUrl.Length > 0)
                {
                    IntPtr frame = new IntPtr(BrowserHwnd());
                    IntPtr combo = Native.FindDescendant(frame, "ComboBoxEx32");
                    IntPtr inner = Native.FindWindowEx(combo, IntPtr.Zero, "ComboBox", null);
                    IntPtr edit = Native.FindWindowEx(inner, IntPtr.Zero, "Edit", null);
                    if (edit != IntPtr.Zero)
                        Native.SendMessage(edit, Native.WM_SETTEXT, IntPtr.Zero, currentEngineUrl);
                }
            }
            catch (Exception ex) { Log("address bar update failed: " + ex.Message); }

            ApplyTitle();
        }

        private void ApplyTitle()
        {
            if (site == null || pendingTitle == null) return;
            string here = BrowserUrl();
            if (!IsShellUrl(here)) return;
            try
            {
                object doc = GetProp(site, "Document");
                if (doc != null)
                {
                    SetProp(doc, "title", pendingTitle);
                }
            }
            catch (Exception ex) { Log("title update failed: " + ex.Message); }
        }

        // ---------------------------------------------- calls from the engine

        public void EngineLocation(string url)
        {
            if (!IsHttp(url) || Same(url, currentEngineUrl)) return;

            // A redirect while the engine is loading a page IE's Back/Forward asked for:
            // keep IE on the same history entry.
            if (historyNavInFlight)
            {
                currentEngineUrl = url;
                return;
            }

            // A page or link the engine navigated to: record it as a new IE history entry.
            currentEngineUrl = url;
            int n;
            string shell = MakeShellUrl(url, out n);
            lastSyntheticN = n;
            lastSynthetic = shellIsFile ? null : shell;
            try { IeNavigate(shell); }
            catch (Exception ex) { Log("history entry failed: " + ex.Message); }
        }

        public void EngineLoaded()
        {
            historyNavInFlight = false;
        }

        public void EngineTitle(string title)
        {
            pendingTitle = title;
            ApplyTitle();
        }

        public void EngineStatus(string text)
        {
            if (site == null) return;
            try { SetProp(site, "StatusText", text); }
            catch (Exception) { }
        }

        // The engine could not start or died: let IE render the page itself.
        public void EngineFailed(string pendingUrl)
        {
            Log("falling back to IE rendering");
            if (pendingUrl == null) return;
            bypassUrl = pendingUrl;
            try { IeNavigate(pendingUrl); }
            catch (Exception ex) { Log("fallback navigation failed: " + ex.Message); }
        }

        private static bool SameUrl(string a, string b)
        {
            return String.Compare(a.TrimEnd('/'), b.TrimEnd('/'), StringComparison.OrdinalIgnoreCase) == 0;
        }

        private static bool DocObjRegistered()
        {
            try { using (RegistryKey k = Registry.ClassesRoot.OpenSubKey(@"PROTOCOLS\Handler\gie")) { return k != null; } }
            catch (Exception) { return false; }
        }

        private bool EngineAllowed()
        {
            return settings != null && settings.BhoEnabled && !classicMode;
        }

        // Opens a new IE window that renders everything with Internet Explorer's own engine.
        public void OpenClassic(string url)
        {
            try
            {
                string ie = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),
                    "Internet Explorer\\iexplore.exe");
                Environment.SetEnvironmentVariable("IEBRIDGE_CLASSIC", "1"); // inherited by the new process
                try
                {
                    ProcessStartInfo psi = new ProcessStartInfo(ie, "\"" + url + "\"");
                    psi.UseShellExecute = true;
                    Process.Start(psi);
                }
                finally
                {
                    Environment.SetEnvironmentVariable("IEBRIDGE_CLASSIC", null);
                }
                Log("opened a classic IE window for " + url);
            }
            catch (Exception ex) { Log("classic window failed: " + ex.Message); }
        }

        // ------------------------------------------------------ registration

        private const string BhoKey =
            @"Software\Microsoft\Windows\CurrentVersion\Explorer\Browser Helper Objects\";

        [ComRegisterFunction]
        public static void Register(Type t)
        {
            using (RegistryKey k = Registry.LocalMachine.CreateSubKey(BhoKey + t.GUID.ToString("B")))
            {
                k.SetValue(null, "IEBridge BHO");
                k.SetValue("NoExplorer", 1, RegistryValueKind.DWord); // load in IE only, not Explorer
            }
        }

        [ComUnregisterFunction]
        public static void Unregister(Type t)
        {
            try { Registry.LocalMachine.DeleteSubKey(BhoKey + t.GUID.ToString("B"), false); }
            catch (Exception) { }
        }
    }
}
