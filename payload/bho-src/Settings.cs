using System;
using System.Collections.Generic;
using System.IO;

namespace IEBridgeBho
{
    // Reads ieb.ini next to the exe (Key=Value lines, ';' or '#' for comments).
    internal sealed class Settings
    {
        public string BaseDir;
        public string EngineDir = "engine-mypal";
        public string EngineExe = "firefox.exe";
        public int Port = 38650;
        public string Token = "dev";
        public string Home = "about:blank";
        public bool Kiosk = true;
        public bool BhoEnabled = true;
        public bool Prewarm = true;
        public bool DocObj = false;   // HostMode=docobj: http(s) navigations are redirected to gie: (native DocObject view)
        public bool InProc = false;   // EngineMode=inproc: run the engine inside the IE process
        public string ProfileDir = "";
        public string ShellUrl = "auto";
        public string Priority = "belownormal";
        public int MaxWorkingSetMB = 0;
        public string TitleSuffix = "Microsoft Internet Explorer";

        public string EngineDirFull { get { return Path.Combine(BaseDir, EngineDir); } }
        // Where the engine keeps its profile. Empty = "profile" next to the install; environment
        // variables such as %APPDATA% are expanded.
        public string ProfileDirFull
        {
            get
            {
                if (ProfileDir.Length == 0) return Path.Combine(BaseDir, "profile");
                string p = Environment.ExpandEnvironmentVariables(ProfileDir);
                return Path.IsPathRooted(p) ? p : Path.Combine(BaseDir, p);
            }
        }

        public string EngineExeFull { get { return Path.Combine(EngineDirFull, EngineExe); } }

        public static Settings Load(string baseDir)
        {
            Settings s = new Settings();
            s.BaseDir = baseDir;
            string path = Path.Combine(s.BaseDir, "ieb.ini");
            if (!File.Exists(path)) return s;

            foreach (string raw in File.ReadAllLines(path))
            {
                string line = raw.Trim();
                if (line.Length == 0 || line[0] == ';' || line[0] == '#') continue;
                int eq = line.IndexOf('=');
                if (eq <= 0) continue;
                string key = line.Substring(0, eq).Trim().ToLowerInvariant();
                string val = line.Substring(eq + 1).Trim();
                switch (key)
                {
                    case "enginedir": s.EngineDir = val; break;
                    case "engineexe": s.EngineExe = val; break;
                    case "port":
                        int p;
                        if (int.TryParse(val, out p) && p > 0 && p < 65536) s.Port = p;
                        break;
                    case "token": if (val.Length > 0) s.Token = val; break;
                    case "home": if (val.Length > 0) s.Home = val; break;
                    case "kiosk": s.Kiosk = (val == "1"); break;
                    case "titlesuffix": s.TitleSuffix = val; break;
                    case "enabled": s.BhoEnabled = (val != "0"); break;
                    case "prewarm": s.Prewarm = (val != "0"); break;
                    case "enginemode": s.InProc = (val.ToLowerInvariant() == "inproc"); break;
                    case "hostmode": s.DocObj = (val.ToLowerInvariant() == "docobj"); break;
                    case "profiledir": s.ProfileDir = val; break;
                    case "shellurl": if (val.Length > 0) s.ShellUrl = val; break;
                    case "priority": if (val.Length > 0) s.Priority = val.ToLowerInvariant(); break;
                    case "maxworkingsetmb":
                        int mb;
                        if (int.TryParse(val, out mb) && mb >= 0) s.MaxWorkingSetMB = mb;
                        break;
                }
            }
            return s;
        }
    }
}
