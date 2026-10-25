using System;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace IEBridgeBho
{
    // Starts and stops the engine inside this IE process through GeckoHost.dll. One engine per process.
    internal static class InProcEngine
    {
        [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true)]
        private static extern IntPtr LoadLibraryA(string path);

        [DllImport("GeckoHost.dll", CallingConvention = CallingConvention.Cdecl)]
        private static extern int GH_Start(string engineDir, string appDir, string profileDir, int port, string token);

        [DllImport("GeckoHost.dll", CallingConvention = CallingConvention.Cdecl)]
        private static extern int GH_IsRunning();

        private static bool started;
        private static int port;
        private static int sessions;

        public static int Port { get { return port; } }

        public static void AddSession() { sessions++; }

        // True when the engine is running in this process (starting it the first time).
        public static bool Ensure(Settings s)
        {
            try
            {
                if (started) return GH_IsRunning() != 0;   // after it has stopped it cannot be restarted

                string dll = Path.Combine(Bho.DllDir, "GeckoHost.dll");
                if (!File.Exists(dll)) { Bho.Log("in-process engine: GeckoHost.dll not found, using a separate engine process"); return false; }
                if (LoadLibraryA(dll) == IntPtr.Zero) { Bho.Log("in-process engine: could not load GeckoHost.dll"); return false; }

                port = FindFreePort(s.Port);
                string profile = FindProfile(s);
                string app = Path.Combine(s.BaseDir, "bare-app");
                int rc = GH_Start(s.EngineDirFull, app, profile, port, s.Token);
                Bho.Log("GH_Start returned " + rc + " (port " + port + ", profile " + profile + ")");
                if (rc != 0) return false;
                started = true;
                return true;
            }
            catch (Exception ex)
            {
                Bho.Log("in-process engine failed: " + ex.Message);
                return false;
            }
        }

        // The last window of this IE process asks the engine to exit and waits (while still handling
        // messages, because the engine talks to IE's windows) before IE goes away.
        public static void Release(EngineClient client)
        {
            sessions--;
            if (sessions > 0 || !started) return;
            try { if (client != null) client.Send("EXIT"); } catch (Exception) { }
            DateTime end = DateTime.Now.AddSeconds(8);
            while (DateTime.Now < end)
            {
                try { if (GH_IsRunning() == 0) break; } catch (Exception) { break; }
                Application.DoEvents();
                System.Threading.Thread.Sleep(20);
            }
            bool stopped;
            try { stopped = GH_IsRunning() == 0; } catch (Exception) { stopped = true; }
            Bho.Log("in-process engine " + (stopped ? "stopped" : "did not stop in time"));
        }

        public static bool Alive()
        {
            try { return started && GH_IsRunning() != 0; } catch (Exception) { return false; }
        }

        // docobj mode: no EngineSession exists, so ask the engine to exit through a short-lived bridge connection.
        public static void ReleaseDocObj(Settings s)
        {
            sessions--;
            if (sessions > 0 || !started) return;
            try
            {
                TcpClient c = new TcpClient();
                c.Connect(IPAddress.Loopback, port);
                byte[] b = Encoding.ASCII.GetBytes("AUTH " + s.Token + "\r\nEXIT\r\n");
                NetworkStream ns = c.GetStream();
                ns.Write(b, 0, b.Length);
                ns.Flush();
                System.Threading.Thread.Sleep(200);
                c.Close();
            }
            catch (Exception) { }
            DateTime end = DateTime.Now.AddSeconds(8);
            while (DateTime.Now < end)
            {
                try { if (GH_IsRunning() == 0) break; } catch (Exception) { break; }
                Application.DoEvents();
                System.Threading.Thread.Sleep(20);
            }
            bool stopped;
            try { stopped = GH_IsRunning() == 0; } catch (Exception) { stopped = true; }
            Bho.Log("in-process engine " + (stopped ? "stopped" : "did not stop in time"));
        }

        private static int FindFreePort(int first)
        {
            for (int p = first; p < first + 50; p++)
            {
                try
                {
                    TcpListener l = new TcpListener(IPAddress.Loopback, p);
                    l.Start();
                    l.Stop();
                    return p;
                }
                catch (SocketException) { }
            }
            return first;
        }

        // A profile folder no other running engine is using (each IE process gets its own).
        private static string FindProfile(Settings s)
        {
            string baseDir = s.ProfileDirFull + "-inproc";
            for (int i = 1; i <= 8; i++)
            {
                string dir = baseDir + i;
                Directory.CreateDirectory(dir);
                string lockFile = Path.Combine(dir, "parent.lock");
                if (File.Exists(lockFile))
                {
                    try { new FileStream(lockFile, FileMode.Open, FileAccess.ReadWrite, FileShare.None).Close(); }
                    catch (IOException) { continue; }
                }
                return dir;
            }
            return baseDir + "1";
        }
    }
}
