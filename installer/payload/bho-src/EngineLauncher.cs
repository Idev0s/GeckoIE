using System;
using System.Diagnostics;
using System.IO;

namespace IEBridgeBho
{
    internal static class EngineLauncher
    {
        // Copies the bridge overlay into the engine folder (when allowed) and starts the engine
        // with no visible window.
        public static Process Start(Settings s)
        {
            if (!File.Exists(s.EngineExeFull))
                throw new FileNotFoundException("Engine not found: " + s.EngineExeFull);

            // The installer already put these files in place, so a failure here (for example no
            // write access to Program Files) is not fatal.
            try
            {
                string overlay = Path.Combine(s.BaseDir, "engine-overlay");
                if (Directory.Exists(overlay)) CopyDir(overlay, s.EngineDirFull);
            }
            catch (Exception) { }

            try
            {
                string log = Path.Combine(s.EngineDirFull, "iebridge-log.txt");
                if (File.Exists(log)) File.Delete(log);
            }
            catch (Exception) { }

            string profile = s.ProfileDirFull;
            Directory.CreateDirectory(profile);

            // The engine reads these from its environment. They are unique names, so setting them
            // in this process is harmless, and it lets the engine start through ShellExecute,
            // which is what honours "start hidden".
            Environment.SetEnvironmentVariable("IEBRIDGE_PORT", s.Port.ToString());
            Environment.SetEnvironmentVariable("IEBRIDGE_TOKEN", s.Token);
            Environment.SetEnvironmentVariable("IEBRIDGE_KIOSK", s.Kiosk ? "1" : "0");

            ProcessStartInfo psi = new ProcessStartInfo(s.EngineExeFull,
                "-no-remote -profile \"" + profile + "\" about:blank");
            psi.UseShellExecute = true;
            psi.WorkingDirectory = s.EngineDirFull;
            psi.WindowStyle = ProcessWindowStyle.Hidden;

            Process p = Process.Start(psi);
            if (p == null) throw new InvalidOperationException("The engine did not start");
            ApplyLimits(p, s);
            return p;
        }

        // Keep the rest of Windows responsive while the engine is busy.
        private static void ApplyLimits(Process p, Settings s)
        {
            try
            {
                switch (s.Priority)
                {
                    case "idle": p.PriorityClass = ProcessPriorityClass.Idle; break;
                    case "belownormal": p.PriorityClass = ProcessPriorityClass.BelowNormal; break;
                }
            }
            catch (InvalidOperationException) { }
            catch (System.ComponentModel.Win32Exception) { }

            try
            {
                if (s.MaxWorkingSetMB > 0)
                    p.MaxWorkingSet = new IntPtr(s.MaxWorkingSetMB * 1024 * 1024);
            }
            catch (InvalidOperationException) { }
            catch (System.ComponentModel.Win32Exception) { }
        }

        private static void CopyDir(string from, string to)
        {
            Directory.CreateDirectory(to);
            foreach (string file in Directory.GetFiles(from))
                File.Copy(file, Path.Combine(to, Path.GetFileName(file)), true);
            foreach (string dir in Directory.GetDirectories(from))
                CopyDir(dir, Path.Combine(to, Path.GetFileName(dir)));
        }
    }
}
