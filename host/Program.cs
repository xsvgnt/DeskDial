// DeskDial companion: feeds the DeskDial round display over USB serial.
//
//   media      what Windows is playing, with artwork and play/pause/next/previous
//              from the board (Media/)
//   hardware   loads, temperatures, clocks, power and memory for the board's
//              hardware page (Hardware/; run elevated for temperatures)
//
// Which modules are compiled in: DeskDial.csproj (EnableMedia, EnableHardware).
// Wire format: docs/protocol.md.
//
// Usage: DeskDial.exe [--port COM5] [--debug] [--spotify-only] [--trace-time]
//        DeskDial.exe --list-sensors
// SPDX-License-Identifier: MIT

namespace DeskDial;

static class Program
{
    const string Usage =
        "DeskDial.exe [options]\n" +
        "  --port COMx      use this port instead of probing every COM port\n" +
        "  --debug          open a console with a log\n" +
#if MEDIA
        "  --spotify-only   show Spotify and no other app\n" +
        "  --trace-time     debug log plus a CSV trace of the playback position (Desktop)\n" +
#endif
#if HARDWARE
        "  --list-sensors   print every hardware sensor and the automatic picks\n" +
#endif
        "";

    static async Task<int> Main(string[] args)
    {
        string? port = null;
        bool spotifyOnly = false, listSensors = false, traceTime = false;
        for (int i = 0; i < args.Length; i++)
        {
            switch (args[i].ToLowerInvariant())
            {
                case "--debug": ConsoleWindow.Open(); Log.Enabled = true; break;
                case "--port" when i + 1 < args.Length: port = args[++i]; break;
                case "--spotify-only": spotifyOnly = true; break;
                case "--trace-time": traceTime = true; break;
                case "--list-sensors": listSensors = true; break;
                default:
                    ConsoleWindow.Open();
                    Console.WriteLine($"Unknown option {args[i]}\n\n{Usage}");
                    return 2;
            }
        }

        var modules = new List<IModule>();
#if MEDIA
        if (traceTime)
        {
            ConsoleWindow.Open();
            Log.Enabled = true;
            Log.Write($"Tracing the playback position to {TimeTrace.Start()}");
        }
        modules.Add(new MediaModule(spotifyOnly));
#else
        if (spotifyOnly || traceTime) return NotInThisBuild("--spotify-only and --trace-time need the media module");
#endif

#if HARDWARE
        var overrides = HardwareMonitor.LoadOverrides(System.IO.Path.Combine(AppContext.BaseDirectory, "sensors.ini"));
        if (listSensors)
        {
            bool newWindow = ConsoleWindow.Open();
            Log.Enabled = true;
            Console.WriteLine();
            if (!HardwareMonitor.IsElevated())
                Console.WriteLine("Note: not running as administrator, most sensors will be missing.\n");
            using (var hw = new HardwareMonitor(overrides))
                hw.PrintAll(Console.Out);
            if (newWindow) { Console.WriteLine("\nPress Enter to close."); Console.ReadLine(); }
            return 0;
        }
        modules.Add(new HardwareModule(new HardwareMonitor(overrides)));
#else
        if (listSensors) return NotInThisBuild("--list-sensors needs the hardware module");
#endif

        using var mutex = new Mutex(true, @"Local\DeskDial", out bool firstInstance);
        if (!firstInstance)
        {
            Log.Write("Another instance is already running.");
            return 1;
        }

        await new Companion(port, modules).RunAsync();
        return 0;
    }

    static int NotInThisBuild(string what)
    {
        ConsoleWindow.Open();
        Console.WriteLine($"This DeskDial build has no such module: {what}.");
        return 2;
    }
}
