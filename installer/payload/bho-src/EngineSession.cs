using System;
using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Forms;

namespace IEBridgeBho
{
    // One IE window's connection to the shared engine. The engine process serves every IE window: each
    // window gets its own engine browser window, which is docked over IE's content area.
    // Everything here runs on IE's UI thread.
    internal sealed class EngineSession : IDisposable
    {
        private delegate void StringHandler(string s);

        private readonly Bho owner;
        private readonly Settings settings;
        private readonly Control ui;
        private Mutex launchMutex;      // held only while this window is starting the engine
        private Process process;        // set only when this window started the engine
        private bool startedEngine;
        private bool inproc;            // the engine runs inside this IE process
        private int enginePort;
        private EngineClient client;

        private IntPtr engineHwnd = IntPtr.Zero;
        private IntPtr parentHwnd = IntPtr.Zero;   // IE's top-level frame
        private IntPtr viewHwnd = IntPtr.Zero;     // IE's content area, only used for position and size
        private bool loggedGone, dumpedWindows;
        private bool connected, docked, failed, disposed;
        private bool wantVisible = true;
        private uint enginePid;
        private bool hiderStarted;
        private volatile bool stopHider;
        private string pendingUrl;
        private string engineHwndText;
        private int lastX = -1, lastY = -1, lastW = -1, lastH = -1;
        private int startTicks, hwndTicks, styleFixes;

        private System.Windows.Forms.Timer tick;
        private System.Windows.Forms.Timer fast;
        private IntPtr rebarHwnd = IntPtr.Zero, statusHwnd = IntPtr.Zero;
        private DateTime nextChromeSearch = DateTime.MinValue, nextViewSearch = DateTime.MinValue;
        private bool lastMouseDown;

        public bool Failed { get { return failed; } }
        public bool Ready { get { return connected && docked; } }

        private EngineSession(Bho owner, Settings s, Control ui)
        {
            this.owner = owner;
            this.settings = s;
            this.ui = ui;
        }

        public static EngineSession TryCreate(Bho owner, Settings s, Control ui, bool startHidden)
        {
            EngineSession es = new EngineSession(owner, s, ui);
            es.wantVisible = !startHidden;
            if (!es.Start()) { es.Dispose(); return null; }
            return es;
        }

        private bool Start()
        {
            // If the engine is already running (started by another IE window), just connect to it.
            // Otherwise exactly one window starts it; the others wait for it to come up.
            enginePort = settings.Port;
            if (settings.InProc && InProcEngine.Ensure(settings))
            {
                inproc = true;
                enginePort = InProcEngine.Port;
                InProcEngine.AddSession();
                enginePid = (uint)Process.GetCurrentProcess().Id;
                StartHider();
                Bho.Log("using the engine inside this IE process (port " + enginePort + ")");
            }
            else if (!EngineClient.Probe(enginePort))
            {
                bool created;
                launchMutex = new Mutex(true, "Local\\IEBridgeEngineLaunch", out created);
                if (created)
                {
                    if (!EngineClient.Probe(enginePort))
                    {
                        try
                        {
                            process = EngineLauncher.Start(settings);
                            startedEngine = true;
                            enginePid = (uint)process.Id;
                            StartHider();
                            Bho.Log("engine started, pid " + process.Id);
                        }
                        catch (Exception ex)
                        {
                            Bho.Log("engine start failed: " + ex.Message);
                            ReleaseLaunch();
                            return false;
                        }
                    }
                    else ReleaseLaunch();
                }
                else
                {
                    launchMutex.Close();
                    launchMutex = null;
                    Bho.Log("another IE window is starting the engine, waiting for it");
                }
            }
            else
            {
                Bho.Log("engine already running, connecting");
            }

            client = new EngineClient(enginePort, settings.Token);
            client.LineReceived += new LineReceivedHandler(OnLine);
            client.ConnectFailed += new EventHandler(OnConnectFailed);
            client.Disconnected += new EventHandler(OnDisconnected);
            client.Start();

            tick = new System.Windows.Forms.Timer();
            tick.Interval = 250;
            tick.Tick += new EventHandler(OnTick);
            tick.Start();

            // Fast timer: keeps the engine pinned over IE's content area and helps it get keyboard focus.
            fast = new System.Windows.Forms.Timer();
            fast.Interval = 50;
            fast.Tick += new EventHandler(OnFast);
            fast.Start();
            return true;
        }

        private void ReleaseLaunch()
        {
            if (launchMutex == null) return;
            try { launchMutex.ReleaseMutex(); } catch (ApplicationException) { }
            launchMutex.Close();
            launchMutex = null;
        }

        // Keeps the engine's own top-level windows invisible from the moment they appear until they have
        // been docked into IE, in case the engine ignores the "start hidden" request.
        private void StartHider()
        {
            if (hiderStarted || enginePid == 0) return;
            hiderStarted = true;
            Thread t = new Thread(new ThreadStart(HideLoop));
            t.IsBackground = true;
            t.Start();
        }

        private void HideLoop()
        {
            DateTime end = DateTime.Now.AddSeconds(150);
            while (!stopHider && !disposed && !docked && DateTime.Now < end)
            {
                try
                {
                    IntPtr h = Native.FindEngineWindow(enginePid, true);
                    if (h != IntPtr.Zero) Native.ShowWindow(h, Native.SW_HIDE);
                }
                catch (Exception) { }
                Thread.Sleep(15);
            }
        }

        // ------------------------------------------------------------ commands

        // Returns false when the engine can't take the page and IE should render it itself.
        public bool Navigate(string url)
        {
            if (failed || disposed) return false;
            wantVisible = true; // a page is about to be shown, so the engine view must become visible
            if (connected)
            {
                pendingUrl = null;
                client.Send("GO " + url);
                Show(true);
            }
            else
            {
                pendingUrl = url;
                owner.EngineStatus("Starting modern engine...");
            }
            return true;
        }

        public void Reload()
        {
            if (connected && client != null) client.Send("RELOAD");
        }

        public void Show(bool visible)
        {
            wantVisible = visible;
            if (!docked || !Native.IsWindow(engineHwnd)) return;
            if (visible)
                Native.SetWindowPos(engineHwnd, Native.HWND_TOP, 0, 0, 0, 0,
                    Native.SWP_NOACTIVATE | Native.SWP_SHOWWINDOW | 0x0001 | 0x0002); // NOSIZE | NOMOVE
            else
                Native.ShowWindow(engineHwnd, Native.SW_HIDE);
        }

        // ------------------------------------------------------- engine events

        private void OnLine(string line)
        {
            if (disposed || ui.IsDisposed || !ui.IsHandleCreated) return;
            try { ui.BeginInvoke(new StringHandler(Handle), new object[] { line }); }
            catch (InvalidOperationException) { }
        }

        private void OnConnectFailed(object sender, EventArgs e)
        {
            if (disposed || ui.IsDisposed || !ui.IsHandleCreated) return;
            try { ui.BeginInvoke(new StringHandler(Fail), new object[] { "could not connect to engine" }); }
            catch (InvalidOperationException) { }
        }

        private void OnDisconnected(object sender, EventArgs e)
        {
            if (disposed || ui.IsDisposed || !ui.IsHandleCreated) return;
            try { ui.BeginInvoke(new StringHandler(Fail), new object[] { "engine connection closed; " + ProcessInfo() }); }
            catch (InvalidOperationException) { }
        }

        private static string Decode(string s)
        {
            try { return Uri.UnescapeDataString(s); }
            catch (UriFormatException) { return s; }
        }

        private void Handle(string line)
        {
            if (disposed) return;
            int sp = line.IndexOf(' ');
            string cmd = sp < 0 ? line : line.Substring(0, sp);
            string arg = sp < 0 ? "" : line.Substring(sp + 1);

            switch (cmd)
            {
                case "HELLO":
                    connected = true;
                    Bho.Log("engine connected");
                    ReleaseLaunch();
                    if (pendingUrl != null)
                    {
                        string first = pendingUrl;
                        pendingUrl = null;
                        client.Send("GO " + first);
                    }
                    CheckReady();
                    break;
                case "PID":
                    uint pid;
                    if (uint.TryParse(arg.Trim(), out pid) && pid != 0)
                    {
                        enginePid = pid;
                        StartHider();
                    }
                    break;
                case "HWND":
                    engineHwndText = arg.Trim();
                    TryDock();
                    break;
                case "LOC":
                    owner.EngineLocation(arg.Trim());
                    break;
                case "TITLE":
                    owner.EngineTitle(Decode(arg));
                    break;
                case "STATE":
                    if (arg.Trim() == "loading") owner.EngineStatus("Opening page...");
                    else { owner.EngineStatus("Done"); owner.EngineLoaded(); }
                    break;
                case "STATUS":
                    string st = Decode(arg);
                    if (st.Length > 0) owner.EngineStatus(st);
                    break;
                case "KEY":
                    if (arg.Trim() == "BACK") owner.GoBack();
                    else if (arg.Trim() == "FWD") owner.GoForward();
                    break;
                case "CLASSIC":
                    owner.OpenClassic(Decode(arg));
                    break;
                case "ERR":
                    Bho.Log("engine error: " + Decode(arg));
                    break;
            }
        }

        // -------------------------------------------------------------- docking

        private static string Hex(IntPtr h)
        {
            return "0x" + h.ToInt64().ToString("X");
        }

        private string ProcessInfo()
        {
            try
            {
                if (inproc) return "engine runs inside this process";
                if (process == null) return "engine started by another window";
                if (process.HasExited) return "engine process exited, code " + process.ExitCode;
                return "engine process still running";
            }
            catch (Exception ex) { return "process state unknown (" + ex.Message + ")"; }
        }

        private IntPtr FindFrame()
        {
            int frame = owner.BrowserHwnd();
            return frame == 0 ? IntPtr.Zero : new IntPtr(frame);
        }

        private static readonly string[] ViewClasses = new string[]
        {
            "Shell DocObject View", "Shell Embedding", "Internet Explorer_Server"
        };

        private static IntPtr FindView(IntPtr frame)
        {
            if (frame == IntPtr.Zero) return IntPtr.Zero;
            for (int i = 0; i < ViewClasses.Length; i++)
            {
                IntPtr h = Native.FindDescendant(frame, ViewClasses[i]);
                if (h != IntPtr.Zero) return h;
            }
            return IntPtr.Zero;
        }

        private static RECT InFrame(IntPtr frame, IntPtr child)
        {
            RECT r;
            Native.GetWindowRect(child, out r);
            Native.MapWindowPoints(IntPtr.Zero, frame, ref r, 2);
            return r;
        }

        // Remembers IE's toolbar and status bar windows (searched at most once a second).
        private void RefreshChromeHandles()
        {
            if (Native.IsWindow(rebarHwnd) && Native.IsWindow(statusHwnd)) return;
            if (DateTime.Now < nextChromeSearch) return;
            nextChromeSearch = DateTime.Now.AddSeconds(1);
            if (!Native.IsWindow(rebarHwnd)) rebarHwnd = Native.FindDescendant(parentHwnd, "ReBarWindow32");
            if (!Native.IsWindow(statusHwnd)) statusHwnd = Native.FindDescendant(parentHwnd, "msctls_statusbar32");
        }

        // Where IE shows page content, in the frame's client coordinates. Uses IE's content window when
        // it exists, otherwise the space between the toolbars and the status bar.
        private bool ContentRect(out int x, out int y, out int w, out int h)
        {
            x = y = w = h = 0;
            if (!Native.IsWindow(parentHwnd)) return false;

            if ((viewHwnd == IntPtr.Zero || !Native.IsWindow(viewHwnd)) && DateTime.Now >= nextViewSearch)
            {
                viewHwnd = FindView(parentHwnd);
                nextViewSearch = DateTime.Now.AddSeconds(1);
            }
            if (viewHwnd != IntPtr.Zero && Native.IsWindow(viewHwnd))
            {
                RECT v = InFrame(parentHwnd, viewHwnd);
                if (v.Right - v.Left > 0 && v.Bottom - v.Top > 0)
                {
                    x = v.Left; y = v.Top; w = v.Right - v.Left; h = v.Bottom - v.Top;
                    return true;
                }
            }

            RECT c;
            if (!Native.GetClientRect(parentHwnd, out c)) return false;
            int top = 0, bottom = c.Bottom;
            RefreshChromeHandles();
            if (Native.IsWindow(rebarHwnd)) top = Math.Max(top, InFrame(parentHwnd, rebarHwnd).Bottom);
            if (Native.IsWindow(statusHwnd) && Native.IsWindowVisible(statusHwnd))
                bottom = Math.Min(bottom, InFrame(parentHwnd, statusHwnd).Top);
            x = 0; y = top; w = c.Right - c.Left; h = bottom - top;
            return w > 0 && h > 0;
        }

        // One-off diagnostic: lists IE's child windows so we can see how this IE is put together.
        private void DumpWindows(IntPtr frame)
        {
            if (dumpedWindows || frame == IntPtr.Zero) return;
            dumpedWindows = true;
            int count = 0;
            Bho.Log("window dump of IE frame " + Hex(frame) + " (" + Native.ClassOf(frame) + "):");
            Native.EnumChildWindows(frame, delegate(IntPtr h, IntPtr l)
            {
                if (count++ >= 80) return false;
                RECT r;
                Native.GetWindowRect(h, out r);
                Bho.Log("  " + Hex(h) + " class=" + Native.ClassOf(h) + " parent=" + Hex(Native.GetParent(h)) +
                        " visible=" + Native.IsWindowVisible(h) +
                        " rect=" + r.Left + "," + r.Top + "," + r.Right + "," + r.Bottom);
                return true;
            }, IntPtr.Zero);
        }

        private void TryDock()
        {
            if (docked || engineHwndText == null) return;

            IntPtr h = IntPtr.Zero;
            string text = engineHwndText;
            if (text.StartsWith("0x", StringComparison.OrdinalIgnoreCase)) text = text.Substring(2);
            long v;
            if (long.TryParse(text, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out v) &&
                Native.IsWindow(new IntPtr(v))) h = new IntPtr(v);
            else if (long.TryParse(text, NumberStyles.Integer, CultureInfo.InvariantCulture, out v) &&
                     Native.IsWindow(new IntPtr(v))) h = new IntPtr(v);
            if (h == IntPtr.Zero) return; // timer falls back to searching by process id

            Dock(h);
        }

        private void Dock(IntPtr h)
        {
            IntPtr frame = FindFrame();
            if (frame == IntPtr.Zero)
            {
                Bho.Log("IE frame window not available, will retry");
                return;
            }
            parentHwnd = frame;
            viewHwnd = IntPtr.Zero;

            int cx, cy, cw, ch;
            if (!ContentRect(out cx, out cy, out cw, out ch))
            {
                Bho.Log("could not work out IE's content area (frame " + Hex(frame) + "), will retry");
                DumpWindows(frame);
                return;
            }

            StripFrame(h, true);

            // Parent the engine to IE's top-level frame, which lives as long as the IE window does.
            IntPtr old = Native.SetParent(h, frame);
            int err = Marshal.GetLastWin32Error();
            IntPtr now = Native.GetParent(h);
            Bho.Log("dock: frame=" + Hex(frame) + " (" + Native.ClassOf(frame) + ") view=" + Hex(viewHwnd) +
                    " engine=" + Hex(h) + " SetParent returned " + Hex(old) + " err=" + err +
                    " GetParent=" + Hex(now));

            engineHwnd = h;
            docked = true;
            lastX = lastY = lastW = lastH = -1;
            LayoutEngine(true);
            Bho.Log("engine window docked into IE");
            CheckReady();
        }

        // Removes everything that makes a window look or act like a window: title bar, borders, resize
        // frame, system menu and all the extended edge styles. Returns true if anything had to change.
        private bool StripFrame(IntPtr h, bool force)
        {
            int style = Native.GetStyle(h, Native.GWL_STYLE);
            int ex = Native.GetStyle(h, Native.GWL_EXSTYLE);

            int badStyle = Native.WS_POPUP | Native.WS_CAPTION | Native.WS_BORDER | Native.WS_DLGFRAME |
                           Native.WS_THICKFRAME | Native.WS_SYSMENU | Native.WS_MINIMIZEBOX | Native.WS_MAXIMIZEBOX;
            int badEx = Native.WS_EX_APPWINDOW | Native.WS_EX_WINDOWEDGE | Native.WS_EX_CLIENTEDGE |
                        Native.WS_EX_DLGMODALFRAME | Native.WS_EX_STATICEDGE | Native.WS_EX_TOOLWINDOW;

            bool changed = (style & badStyle) != 0 || (ex & badEx) != 0 || (style & Native.WS_CHILD) == 0;
            if (!changed && !force) return false;

            style &= ~badStyle;
            style |= Native.WS_CHILD | Native.WS_CLIPSIBLINGS | Native.WS_CLIPCHILDREN;
            if (wantVisible) style |= Native.WS_VISIBLE; else style &= ~Native.WS_VISIBLE;
            ex &= ~badEx;
            Native.SetStyle(h, Native.GWL_STYLE, style);
            Native.SetStyle(h, Native.GWL_EXSTYLE, ex);
            return changed;
        }

        // Even with every frame style gone, make sure nothing outside the client area can be seen or
        // clicked: the window is cut down to exactly its client rectangle.
        private void ClipToClientArea()
        {
            try
            {
                RECT wr, cr;
                POINT p = new POINT();
                if (!Native.GetWindowRect(engineHwnd, out wr) || !Native.GetClientRect(engineHwnd, out cr)) return;
                int cwid = cr.Right - cr.Left, chei = cr.Bottom - cr.Top;
                if (cwid <= 0 || chei <= 0) return;
                Native.ClientToScreen(engineHwnd, ref p);
                int l = p.X - wr.Left, t = p.Y - wr.Top;
                IntPtr rgn = Native.CreateRectRgn(l, t, l + cwid, t + chei);
                if (rgn != IntPtr.Zero) Native.SetWindowRgn(engineHwnd, rgn, true); // the system owns rgn now
            }
            catch (Exception) { }
        }

        // Places the engine window exactly over IE's content area, in the frame's coordinates.
        private void LayoutEngine(bool force)
        {
            if (!docked || !Native.IsWindow(engineHwnd) || !Native.IsWindow(parentHwnd)) return;
            int x, y, w, h;
            if (!ContentRect(out x, out y, out w, out h)) return;
            if (!force && x == lastX && y == lastY && w == lastW && h == lastH) return;
            lastX = x; lastY = y; lastW = w; lastH = h;

            uint flags = Native.SWP_NOACTIVATE | Native.SWP_FRAMECHANGED;
            if (wantVisible) flags |= Native.SWP_SHOWWINDOW;
            Native.SetWindowPos(engineHwnd, Native.HWND_TOP, x, y, w, h, flags);
            if (!wantVisible) Native.ShowWindow(engineHwnd, Native.SW_HIDE);
            ClipToClientArea();
            if (force) Bho.Log("engine placed at " + x + "," + y + " size " + w + "x" + h);
        }

        private void CheckReady()
        {
            if (!Ready) return;
            Bho.Log("engine ready");
            Show(wantVisible);
        }

        // ---------------------------------------------------- pinning and focus

        private void OnFast(object sender, EventArgs e)
        {
            if (disposed || !docked || !Native.IsWindow(engineHwnd) || !Native.IsWindow(parentHwnd)) return;
            EnforceLook();
            FocusAssist();
        }

        // The engine window must stay a plain, fixed child: no caption, no borders, no resize frame, and
        // always exactly over IE's content area. If anything changes that, put it back.
        private void EnforceLook()
        {
            bool restyled = StripFrame(engineHwnd, false);
            if (restyled && styleFixes++ < 10)
                Bho.Log("the engine put its window frame back (style 0x" +
                        Native.GetStyle(engineHwnd, Native.GWL_STYLE).ToString("X") + "), removed it again");

            int x, y, w, h;
            if (!ContentRect(out x, out y, out w, out h)) return;
            RECT r = InFrame(parentHwnd, engineHwnd);
            bool moved = r.Left != x || r.Top != y || (r.Right - r.Left) != w || (r.Bottom - r.Top) != h;
            if (restyled || (moved && wantVisible))
            {
                lastX = x; lastY = y; lastW = w; lastH = h;
                uint flags = Native.SWP_NOACTIVATE | Native.SWP_FRAMECHANGED;
                if (wantVisible) flags |= Native.SWP_SHOWWINDOW;
                Native.SetWindowPos(engineHwnd, Native.HWND_TOP, x, y, w, h, flags);
                if (!wantVisible) Native.ShowWindow(engineHwnd, Native.SW_HIDE);
                ClipToClientArea();
            }
        }

        // A click inside the engine area gives the engine keyboard focus. The engine lives in another
        // process, and without this the keyboard can stay with IE until you right-click.
        private void FocusAssist()
        {
            bool down = (Native.GetAsyncKeyState(0x01) & 0x8000) != 0; // left mouse button
            bool pressed = down && !lastMouseDown;
            lastMouseDown = down;
            if (!pressed || !wantVisible) return;
            if (Native.GetForegroundWindow() != parentHwnd) return;

            POINT p;
            RECT r;
            if (!Native.GetCursorPos(out p) || !Native.GetWindowRect(engineHwnd, out r)) return;
            if (p.X < r.Left || p.X >= r.Right || p.Y < r.Top || p.Y >= r.Bottom) return;
            if (Native.GetFocus() != engineHwnd) Native.SetFocus(engineHwnd);
        }

        // ---------------------------------------------------------------- timer

        private void OnTick(object sender, EventArgs e)
        {
            if (disposed) return;
            if (docked && !Native.IsWindow(engineHwnd))
            {
                if (!loggedGone)
                {
                    loggedGone = true;
                    Fail("engine window was destroyed; " + ProcessInfo());
                }
                return;
            }
            LayoutEngine(false);
            if (Ready) return;

            startTicks++;

            // Fallbacks while waiting for the dock: find the window by process id, or retry the parent lookup.
            if (connected && !docked)
            {
                hwndTicks++;
                if (hwndTicks >= 12 && enginePid != 0)
                {
                    IntPtr h = Native.FindEngineWindow(enginePid, false);
                    if (h != IntPtr.Zero) Dock(h);
                }
                else if (engineHwndText != null)
                {
                    TryDock();
                }
            }

            if (startTicks > 4 * 90) Fail("engine did not become ready in 90 seconds");
        }

        private void Fail(string reason)
        {
            if (failed || disposed) return;
            failed = true;
            Bho.Log("engine failed: " + reason + " [" + ProcessInfo() + "]");
            string url = pendingUrl;
            pendingUrl = null;
            // Only take the engine down if this window started it and it never came up.
            StopEngine(startedEngine && !connected);
            owner.EngineFailed(url);
        }

        // -------------------------------------------------------------- cleanup

        private void StopEngine(bool killProcess)
        {
            stopHider = true;
            if (tick != null) { tick.Stop(); }
            if (fast != null) { fast.Stop(); }
            if (client != null)
            {
                // Closing the connection is enough: the engine closes this window's view, and shuts
                // itself down when no IE window is using it any more.
                client.Close();
                client = null;
            }
            if (killProcess && process != null)
            {
                try
                {
                    if (!process.HasExited) process.Kill();
                }
                catch (InvalidOperationException) { }
                catch (System.ComponentModel.Win32Exception) { }
            }
            process = null;
            docked = false;
            ReleaseLaunch();
        }

        public void Dispose()
        {
            if (disposed) return;
            disposed = true;
            if (inproc) InProcEngine.Release(client); // the last window asks the engine to exit and waits for it
            StopEngine(false);
            if (tick != null) { tick.Dispose(); tick = null; }
            if (fast != null) { fast.Dispose(); fast = null; }
        }
    }
}
