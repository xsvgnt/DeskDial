// Hardware readings for the display's hardware page.
//
// Temperatures, loads, clocks and power come from LibreHardwareMonitorLib,
// which needs administrator rights and the PawnIO driver (https://pawnio.eu)
// for CPU and motherboard sensors. Sensors are picked automatically by name;
// anything a machine doesn't expose is simply not sent and shows as "--".
// Picks can be overridden in sensors.ini next to the exe (see README), and
// `--list-sensors` prints every sensor with its identifier.
//
// Display layout v2 picks:
//   CPU temp   "CCD1 (Tdie)" (AMD), falling back to other package sensors
//   CPU clock  "Cores (Average)" (or the average of the "Core #n" clocks)
//   GPU        the one with the most dedicated memory (skips a Ryzen/Intel iGPU);
//              "GPU Core" load / clock / temp, "GPU Package" power,
//              "GPU Memory Used" / "GPU Memory Total"
//   Bottom row "RAM" = hottest "DIMM #n" reading, then the SSD

using System.IO; // not implicit in WPF projects
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Text.RegularExpressions;
using LibreHardwareMonitor.Hardware;

namespace NowPlayingBridge;

/// Metric ids, identical to HW_* in firmware/NowPlaying/protocol.h.
enum Metric : byte
{
    CpuTemp = 1, CpuLoad = 2, CpuClock = 3, CpuPower = 4, CpuFan = 5,
    RamUsed = 6, RamTotal = 7,
    GpuTemp = 8, GpuLoad = 9, GpuClock = 10, GpuPower = 11, GpuFan = 12,
    VramUsed = 13, VramTotal = 14,
    DiskUsed = 15, DiskTotal = 16,
}

/// What sensors.ini can override.
sealed class SensorOverrides
{
    public readonly Dictionary<Metric, string> Metrics = new();
    public string? Gpu; // name substring or identifier of the GPU to use
    public readonly string?[] TempIds = new string?[3];
    public readonly string?[] TempLabels = new string?[3];
    public bool HasTemps => TempIds.Any(t => t is not null);
}

sealed class HardwareMonitor : IDisposable
{
    public const int MaxTemps = 3; // HW_TEMPS_MAX in protocol.h
    const int MaxLabelBytes = 11;  // HW_TEMP_LABEL_MAX

    Computer? _pc;
    readonly Dictionary<Metric, ISensor> _map = new();
    readonly List<ISensor> _cpuCoreClocks = new();
    readonly List<(string Label, List<ISensor> Sensors)> _temps = new(); // value = max of sensors
    readonly SensorOverrides _overrides;
    bool _warned;

    public HardwareMonitor(SensorOverrides overrides) => _overrides = overrides;

    public static bool IsElevated()
    {
        using var id = WindowsIdentity.GetCurrent();
        return new WindowsPrincipal(id).IsInRole(WindowsBuiltInRole.Administrator);
    }

    void EnsureOpen()
    {
        if (_pc is not null) return;
        if (!IsElevated())
            Log.Write("Not running as administrator: temperatures and power will be missing.");
        _pc = new Computer
        {
            IsCpuEnabled = true,
            IsGpuEnabled = true,
            IsMemoryEnabled = true,  // RAM module temperatures (RAM usage comes from Windows)
            IsStorageEnabled = true, // SSD temperature
        };
        _pc.Open();
        UpdateAll();
        Resolve();
    }

    int _updates;

    /// slowToo = also refresh storage and RAM modules. Their temperatures move
    /// slowly and reading them (SMART, SMBus) is the costliest part, so the
    /// once-a-second sample only includes them every 10th time.
    void UpdateAll(bool slowToo = true)
    {
        foreach (var hw in _pc!.Hardware)
            if (slowToo || hw.HardwareType is not (HardwareType.Storage or HardwareType.Memory))
                Update(hw);
        static void Update(IHardware h)
        {
            h.Update();
            foreach (var sub in h.SubHardware) Update(sub);
        }
    }

    static IEnumerable<ISensor> Sensors(IHardware h) => h.Sensors.Concat(h.SubHardware.SelectMany(Sensors));

    /// The GPU to show. LibreHardwareMonitor has no "integrated" flag (a Ryzen's
    /// built-in Radeon is a GpuAmd like any graphics card, and the AMD group is
    /// listed before NVIDIA), so take the one with the most dedicated memory:
    /// a card has GBs, an iGPU only its small carve-out of system RAM.
    IHardware? PickGpu()
    {
        var gpus = _pc!.Hardware
            .Where(h => h.HardwareType is HardwareType.GpuNvidia or HardwareType.GpuAmd or HardwareType.GpuIntel)
            .ToList();
        if (_overrides.Gpu is string want)
        {
            var forced = gpus.FirstOrDefault(g => g.Identifier.ToString().Equals(want, StringComparison.OrdinalIgnoreCase) ||
                                                  g.Name.Contains(want, StringComparison.OrdinalIgnoreCase));
            if (forced is not null) return forced;
            Log.Write($"sensors.ini: no GPU matching '{want}'");
        }
        foreach (var g in gpus)
            Log.Write($"  GPU candidate: {g.Name} [{g.Identifier}], {DedicatedMemoryMB(g):0} MB dedicated");
        return gpus.OrderByDescending(DedicatedMemoryMB)
                   .ThenBy(g => g.HardwareType == HardwareType.GpuIntel ? 1 : 0)
                   .FirstOrDefault();
    }

    static float DedicatedMemoryMB(IHardware gpu)
    {
        var s = Sensors(gpu).ToList();
        var total = Pick(s, SensorType.SmallData, false, "GPU Memory Total", "D3D Dedicated Memory Total");
        if (total?.Value is float mb) return mb;
        total = Pick(s, SensorType.Data, false, "GPU Memory Total");
        return total?.Value is float gb ? gb * 1024 : 0;
    }

    /// First sensor of the given type whose name matches one of the names, in priority order.
    static ISensor? Pick(IEnumerable<ISensor> sensors, SensorType type, bool fallbackToAny, params string[] names)
    {
        var ofType = sensors.Where(s => s.SensorType == type).ToList();
        foreach (var name in names)
        {
            var hit = ofType.FirstOrDefault(s => s.Name.Equals(name, StringComparison.OrdinalIgnoreCase));
            if (hit is not null) return hit;
        }
        return fallbackToAny ? ofType.FirstOrDefault() : null;
    }

    void Resolve()
    {
        _map.Clear();
        _cpuCoreClocks.Clear();
        _temps.Clear();

        var cpu = _pc!.Hardware.FirstOrDefault(h => h.HardwareType == HardwareType.Cpu);
        if (cpu is not null)
        {
            var s = Sensors(cpu).ToList();
            Set(Metric.CpuTemp, Pick(s, SensorType.Temperature, true,
                "CCD1 (Tdie)", "CPU Package", "Core (Tctl/Tdie)", "Tctl/Tdie", "Core (Tctl)", "Tdie",
                "Core Max", "Core Average", "CPU Cores"));
            Set(Metric.CpuLoad, Pick(s, SensorType.Load, false, "CPU Total"));
            Set(Metric.CpuPower, Pick(s, SensorType.Power, false, "CPU Package", "Package"));
            Set(Metric.CpuClock, Pick(s, SensorType.Clock, false, "Cores (Average)"));
            _cpuCoreClocks.AddRange(s.Where(x => x.SensorType == SensorType.Clock &&
                (x.Name.StartsWith("Core #", StringComparison.OrdinalIgnoreCase) ||
                 x.Name.StartsWith("CPU Core #", StringComparison.OrdinalIgnoreCase))));
        }

        var gpu = PickGpu();
        if (gpu is not null)
        {
            var s = Sensors(gpu).ToList();
            Set(Metric.GpuTemp, Pick(s, SensorType.Temperature, true, "GPU Core", "GPU Edge", "GPU Hot Spot"));
            Set(Metric.GpuLoad, Pick(s, SensorType.Load, false, "GPU Core", "D3D 3D"));
            Set(Metric.GpuClock, Pick(s, SensorType.Clock, false, "GPU Core"));
            Set(Metric.GpuPower, Pick(s, SensorType.Power, true, "GPU Package", "GPU Power", "GPU Core"));
            // SmallData (MB) in LHM 0.9.6; Data (GB) in newer builds. Both handled.
            Set(Metric.VramUsed, Pick(s, SensorType.SmallData, false, "GPU Memory Used", "D3D Dedicated Memory Used")
                                 ?? Pick(s, SensorType.Data, false, "GPU Memory Used"));
            Set(Metric.VramTotal, Pick(s, SensorType.SmallData, false, "GPU Memory Total", "D3D Dedicated Memory Total")
                                  ?? Pick(s, SensorType.Data, false, "GPU Memory Total"));
        }

        var all = _pc.Hardware.SelectMany(Sensors).ToList();

        // User overrides by sensor identifier, e.g. cpu_temp = /amdcpu/0/temperature/3
        foreach (var (metric, id) in _overrides.Metrics)
        {
            var hit = all.FirstOrDefault(x => x.Identifier.ToString().Equals(id, StringComparison.OrdinalIgnoreCase));
            if (hit is not null) { _map[metric] = hit; if (metric == Metric.CpuClock) _cpuCoreClocks.Clear(); }
            else Log.Write($"sensors.ini: no sensor '{id}' for {metric}");
        }

        // Bottom row: sensors.ini temp1..temp3, or automatic (RAM modules, then SSD)
        if (_overrides.HasTemps)
        {
            for (int i = 0; i < MaxTemps; i++)
            {
                var id = _overrides.TempIds[i];
                if (id is null) continue;
                var hit = all.FirstOrDefault(x => x.Identifier.ToString().Equals(id, StringComparison.OrdinalIgnoreCase));
                if (hit is null) { Log.Write($"sensors.ini: no sensor '{id}' for temp{i + 1}"); continue; }
                _temps.Add((_overrides.TempLabels[i] ?? hit.Name, new List<ISensor> { hit }));
            }
        }
        else
        {
            // Each module's reading is "DIMM #n". Its other Temperature sensors are
            // fixed SPD values (resolution, low/high/critical limits): skip them.
            var dimms = _pc.Hardware.Where(h => h.HardwareType == HardwareType.Memory)
                .SelectMany(h => new[] { h }.Concat(h.SubHardware))
                .SelectMany(h => h.Sensors.Where(x => x.SensorType == SensorType.Temperature &&
                                                      x.Name.StartsWith("DIMM #", StringComparison.OrdinalIgnoreCase)))
                .ToList();
            if (dimms.Count > 0)
                _temps.Add(("RAM", dimms)); // hottest module, whatever the slot layout

            var ssd = _pc.Hardware.Where(h => h.HardwareType == HardwareType.Storage)
                .Select(h => Pick(Sensors(h), SensorType.Temperature, true, "Composite Temperature", "Temperature"))
                .FirstOrDefault(x => x is not null);
            if (ssd is not null && _temps.Count < MaxTemps)
                _temps.Add(("SSD", new List<ISensor> { ssd }));
        }

        foreach (var (m, sensor) in _map)
            Log.Write($"  {m,-9} <- {sensor.Hardware.Name} / {sensor.Name}  [{sensor.Identifier}]");
        if (!_map.ContainsKey(Metric.CpuClock) && _cpuCoreClocks.Count > 0)
            Log.Write($"  CpuClock  <- average of {_cpuCoreClocks.Count} core clocks");
        foreach (var (label, sensors) in _temps)
            Log.Write($"  {label,-9} <- {(sensors.Count > 1 ? "max of " : "")}" +
                      string.Join(", ", sensors.Select(x => $"{x.Hardware.Name} / {x.Name} [{x.Identifier}]")));

        void Set(Metric m, ISensor? sensor) { if (sensor is not null) _map[m] = sensor; }
    }

    /// Reads every available metric plus the labelled temperatures. One thread only.
    public (List<(Metric, float)> Metrics, List<(string Label, float Value)> Temps) Sample()
    {
        var result = new List<(Metric, float)>();
        var temps = new List<(string, float)>();
        try
        {
            EnsureOpen();
            UpdateAll(slowToo: _updates++ % 10 == 0);
            foreach (var (m, sensor) in _map)
            {
                if (sensor.Value is not float v || float.IsNaN(v)) continue;
                if (sensor.SensorType == SensorType.SmallData) v /= 1024f; // MB -> GB
                result.Add((m, v));
            }
            if (!_map.ContainsKey(Metric.CpuClock) && _cpuCoreClocks.Count > 0)
            {
                var clocks = _cpuCoreClocks.Select(c => c.Value).Where(v => v is > 0).Select(v => v!.Value).ToList();
                if (clocks.Count > 0) result.Add((Metric.CpuClock, clocks.Average()));
            }
            foreach (var (label, sensors) in _temps)
            {
                var values = sensors.Select(x => x.Value).Where(v => v is float f && !float.IsNaN(f) && f > 0)
                                    .Select(v => v!.Value).ToList();
                if (values.Count > 0) temps.Add((Label(label), values.Max()));
            }
            if (!_warned && !_map.ContainsKey(Metric.CpuTemp))
            {
                _warned = true;
                Log.Write(IsElevated()
                    ? "No CPU temperature: install the PawnIO driver (https://pawnio.eu) and restart."
                    : "No CPU temperature: run as administrator.");
            }
        }
        catch (Exception e)
        {
            Log.Write($"hardware: {e.Message}");
        }

        // RAM straight from Windows
        var mem = new MemoryStatusEx { dwLength = (uint)Marshal.SizeOf<MemoryStatusEx>() };
        if (GlobalMemoryStatusEx(ref mem))
        {
            const float GB = 1024f * 1024f * 1024f;
            result.Add((Metric.RamUsed, (mem.ullTotalPhys - mem.ullAvailPhys) / GB));
            result.Add((Metric.RamTotal, mem.ullTotalPhys / GB));
        }

        // System drive (not on screen yet, available for future layouts)
        try
        {
            var root = System.IO.Path.GetPathRoot(Environment.SystemDirectory) ?? "C:\\";
            var d = new DriveInfo(root);
            const float GB = 1024f * 1024f * 1024f;
            result.Add((Metric.DiskUsed, (d.TotalSize - d.AvailableFreeSpace) / GB));
            result.Add((Metric.DiskTotal, d.TotalSize / GB));
        }
        catch { }

        return (result, temps);
    }

    static string Label(string s)
    {
        s = TextFilter.ForDisplay(s);
        while (System.Text.Encoding.UTF8.GetByteCount(s) > MaxLabelBytes) s = s[..^1];
        return s;
    }

    /// Prints every sensor and the automatic picks (--list-sensors).
    public void PrintAll(TextWriter w)
    {
        EnsureOpen();
        UpdateAll();
        foreach (var hw in _pc!.Hardware) Print(hw, 0);
        w.WriteLine();
        w.WriteLine("Automatic picks:");
        foreach (var (m, s) in _map) w.WriteLine($"  {ToKey(m),-10} = {s.Identifier}   ({s.Hardware.Name} / {s.Name})");
        if (!_map.ContainsKey(Metric.CpuClock) && _cpuCoreClocks.Count > 0)
            w.WriteLine($"  {"cpu_clock",-10} = average of {_cpuCoreClocks.Count} core clocks");
        foreach (var (label, sensors) in _temps)
            w.WriteLine($"  bottom \"{label}\" = {(sensors.Count > 1 ? "max of " : "")}" +
                        string.Join(", ", sensors.Select(x => $"{x.Identifier} ({x.Hardware.Name} / {x.Name})")));

        void Print(IHardware h, int depth)
        {
            var pad = new string(' ', depth * 2);
            w.WriteLine($"{pad}[{h.HardwareType}] {h.Name}");
            foreach (var s in h.Sensors)
                w.WriteLine($"{pad}  {s.SensorType,-12} {s.Name,-32} {(s.Value?.ToString("0.##") ?? "-"),10}   {s.Identifier}");
            foreach (var sub in h.SubHardware) Print(sub, depth + 1);
        }
    }

    // ---- sensors.ini -----------------------------------------------------
    static readonly Dictionary<string, Metric> Keys = new(StringComparer.OrdinalIgnoreCase)
    {
        ["cpu_temp"] = Metric.CpuTemp, ["cpu_load"] = Metric.CpuLoad, ["cpu_clock"] = Metric.CpuClock,
        ["cpu_power"] = Metric.CpuPower,
        ["gpu_temp"] = Metric.GpuTemp, ["gpu_load"] = Metric.GpuLoad, ["gpu_clock"] = Metric.GpuClock,
        ["gpu_power"] = Metric.GpuPower,
        ["vram_used"] = Metric.VramUsed, ["vram_total"] = Metric.VramTotal,
    };

    static string ToKey(Metric m) => Keys.FirstOrDefault(kv => kv.Value == m).Key ?? m.ToString();

    /// Lines of "key = value"; '#' starts a comment. Keys: the metric names above,
    /// temp1..temp3 (sensor identifier) and temp1_label..temp3_label.
    public static SensorOverrides LoadOverrides(string path)
    {
        var o = new SensorOverrides();
        if (!File.Exists(path)) return o;
        foreach (var raw in File.ReadAllLines(path))
        {
            var line = raw.Split('#')[0].Trim();
            int eq = line.IndexOf('=');
            if (eq <= 0) continue;
            var key = line[..eq].Trim();
            var val = line[(eq + 1)..].Trim();
            var tm = Regex.Match(key, @"^temp([1-3])(_label)?$", RegexOptions.IgnoreCase);
            if (key.Equals("gpu", StringComparison.OrdinalIgnoreCase) && val.Length > 0)
                o.Gpu = val;
            else if (tm.Success && val.Length > 0)
            {
                int i = int.Parse(tm.Groups[1].Value) - 1;
                if (tm.Groups[2].Success) o.TempLabels[i] = val; else o.TempIds[i] = val;
            }
            else if (Keys.TryGetValue(key, out var m) && val.Length > 0) o.Metrics[m] = val;
            else Log.Write($"sensors.ini: ignored '{raw}'");
        }
        return o;
    }

    public void Dispose()
    {
        try { _pc?.Close(); } catch { }
        _pc = null;
    }

    // ---- Win32 ----------------------------------------------------------
    [StructLayout(LayoutKind.Sequential)]
    struct MemoryStatusEx
    {
        public uint dwLength, dwMemoryLoad;
        public ulong ullTotalPhys, ullAvailPhys, ullTotalPageFile, ullAvailPageFile,
                     ullTotalVirtual, ullAvailVirtual, ullAvailExtendedVirtual;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    static extern bool GlobalMemoryStatusEx(ref MemoryStatusEx buffer);
}
