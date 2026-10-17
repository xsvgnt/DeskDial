// Debug log (--debug) and the console window behind it.
// SPDX-License-Identifier: MIT

using System.Runtime.InteropServices;

namespace DeskDial;

static class Log
{
    public static bool Enabled;

    public static void Write(string msg)
    {
        if (Enabled) Console.WriteLine($"{DateTime.Now:HH:mm:ss.fff}  {msg}");
    }
}

static class ConsoleWindow
{
    [DllImport("kernel32.dll")]
    static extern bool AllocConsole();

    [DllImport("kernel32.dll")]
    static extern bool AttachConsole(int processId);

    /// The program is a WinExe without a console: reuse the terminal it was
    /// started from, or open one. Returns true when a new window was opened.
    public static bool Open()
    {
        if (AttachConsole(-1)) return false; // ATTACH_PARENT_PROCESS
        AllocConsole();
        return true;
    }
}
