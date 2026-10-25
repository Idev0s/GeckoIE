using System;
using System.Runtime.InteropServices;
using System.Text;

namespace IEBridgeBho
{
    internal delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [StructLayout(LayoutKind.Sequential)]
    internal struct POINT
    {
        public int X, Y;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct RECT
    {
        public int Left, Top, Right, Bottom;
    }

    internal static class Native
    {
        public const int GWL_STYLE = -16;
        public const int GWL_EXSTYLE = -20;

        public const int WS_POPUP = unchecked((int)0x80000000);
        public const int WS_CHILD = 0x40000000;
        public const int WS_VISIBLE = 0x10000000;
        public const int WS_CLIPSIBLINGS = 0x04000000;
        public const int WS_CLIPCHILDREN = 0x02000000;
        public const int WS_BORDER = 0x00800000;
        public const int WS_DLGFRAME = 0x00400000;
        public const int WS_CAPTION = 0x00C00000;
        public const int WS_THICKFRAME = 0x00040000;
        public const int WS_SYSMENU = 0x00080000;
        public const int WS_MINIMIZEBOX = 0x00020000;
        public const int WS_MAXIMIZEBOX = 0x00010000;

        public const int WS_EX_DLGMODALFRAME = 0x00000001;
        public const int WS_EX_WINDOWEDGE = 0x00000100;
        public const int WS_EX_CLIENTEDGE = 0x00000200;
        public const int WS_EX_APPWINDOW = 0x00040000;
        public const int WS_EX_STATICEDGE = 0x00020000;
        public const int WS_EX_TOOLWINDOW = 0x00000080;

        public const uint SWP_NOACTIVATE = 0x0010;
        public const uint SWP_FRAMECHANGED = 0x0020;
        public const uint SWP_SHOWWINDOW = 0x0040;

        public const int SW_HIDE = 0;
        public const int SW_SHOWNA = 8;
        public const int WM_SETTEXT = 0x000C;

        public static readonly IntPtr HWND_TOP = IntPtr.Zero;

        [DllImport("user32.dll", SetLastError = true)]
        public static extern IntPtr SetParent(IntPtr hWndChild, IntPtr hWndNewParent);

        [DllImport("user32.dll")]
        public static extern IntPtr GetParent(IntPtr hWnd);

        [DllImport("user32.dll")]
        public static extern bool GetCursorPos(out POINT p);

        [DllImport("user32.dll")]
        public static extern bool ClientToScreen(IntPtr hWnd, ref POINT p);

        [DllImport("user32.dll")]
        public static extern int SetWindowRgn(IntPtr hWnd, IntPtr region, bool redraw);

        [DllImport("gdi32.dll")]
        public static extern IntPtr CreateRectRgn(int left, int top, int right, int bottom);

        [DllImport("user32.dll")]
        public static extern short GetAsyncKeyState(int vKey);

        [DllImport("user32.dll")]
        public static extern IntPtr GetFocus();

        [DllImport("user32.dll")]
        public static extern IntPtr SetFocus(IntPtr hWnd);

        [DllImport("user32.dll")]
        public static extern IntPtr GetForegroundWindow();

        [DllImport("user32.dll")]
        public static extern int MapWindowPoints(IntPtr from, IntPtr to, ref RECT rect, int points);

        [DllImport("user32.dll")]
        public static extern bool IsWindow(IntPtr hWnd);

        [DllImport("user32.dll")]
        public static extern bool IsWindowVisible(IntPtr hWnd);

        [DllImport("user32.dll")]
        public static extern bool ShowWindow(IntPtr hWnd, int cmd);

        [DllImport("user32.dll")]
        public static extern bool MoveWindow(IntPtr hWnd, int x, int y, int w, int h, bool repaint);

        [DllImport("user32.dll")]
        public static extern bool SetWindowPos(IntPtr hWnd, IntPtr after, int x, int y, int cx, int cy, uint flags);

        [DllImport("user32.dll")]
        public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);

        [DllImport("user32.dll")]
        public static extern bool GetClientRect(IntPtr hWnd, out RECT rect);

        [DllImport("user32.dll")]
        public static extern bool EnumWindows(EnumWindowsProc proc, IntPtr lParam);

        [DllImport("user32.dll")]
        public static extern bool EnumChildWindows(IntPtr parent, EnumWindowsProc proc, IntPtr lParam);

        [DllImport("user32.dll")]
        public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);

        [DllImport("user32.dll", CharSet = CharSet.Auto)]
        public static extern int GetClassName(IntPtr hWnd, StringBuilder name, int maxCount);

        [DllImport("user32.dll", CharSet = CharSet.Auto)]
        public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string className, string title);

        [DllImport("user32.dll", CharSet = CharSet.Auto)]
        public static extern IntPtr SendMessage(IntPtr hWnd, int msg, IntPtr wParam, string lParam);

        [DllImport("user32.dll", EntryPoint = "GetWindowLong")]
        private static extern int GetWindowLong32(IntPtr hWnd, int index);

        [DllImport("user32.dll", EntryPoint = "SetWindowLong")]
        private static extern int SetWindowLong32(IntPtr hWnd, int index, int value);

        [DllImport("user32.dll", EntryPoint = "GetWindowLongPtr")]
        private static extern IntPtr GetWindowLongPtr64(IntPtr hWnd, int index);

        [DllImport("user32.dll", EntryPoint = "SetWindowLongPtr")]
        private static extern IntPtr SetWindowLongPtr64(IntPtr hWnd, int index, IntPtr value);

        public static int GetStyle(IntPtr hWnd, int index)
        {
            if (IntPtr.Size == 4) return GetWindowLong32(hWnd, index);
            return (int)GetWindowLongPtr64(hWnd, index).ToInt64();
        }

        public static void SetStyle(IntPtr hWnd, int index, int value)
        {
            if (IntPtr.Size == 4) SetWindowLong32(hWnd, index, value);
            else SetWindowLongPtr64(hWnd, index, new IntPtr(value));
        }

        public static string ClassOf(IntPtr h)
        {
            StringBuilder sb = new StringBuilder(128);
            GetClassName(h, sb, sb.Capacity);
            return sb.ToString();
        }

        // First descendant window (any depth) with the given class name, or Zero.
        public static IntPtr FindDescendant(IntPtr root, string className)
        {
            IntPtr found = IntPtr.Zero;
            EnumChildWindows(root, delegate(IntPtr h, IntPtr l)
            {
                if (ClassOf(h) == className) { found = h; return false; }
                return true;
            }, IntPtr.Zero);
            return found;
        }

        // Largest top-level Gecko window owned by the given process, or Zero.
        public static IntPtr FindEngineWindow(uint pid, bool requireVisible)
        {
            IntPtr found = IntPtr.Zero;
            int bestArea = 0;
            EnumWindows(delegate(IntPtr h, IntPtr l)
            {
                uint p;
                GetWindowThreadProcessId(h, out p);
                if (p != pid || (requireVisible && !IsWindowVisible(h))) return true;
                if (ClassOf(h) != "MozillaWindowClass") return true;
                RECT r;
                if (!GetWindowRect(h, out r)) return true;
                int area = (r.Right - r.Left) * (r.Bottom - r.Top);
                if (area > bestArea) { bestArea = area; found = h; }
                return true;
            }, IntPtr.Zero);
            return found;
        }
    }
}
