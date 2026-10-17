// Hardware module: sensor readings for the board's hardware page, once a
// second while the board shows that page. Run elevated for temperatures and
// power (HardwareMonitor.cs).
// SPDX-License-Identifier: MIT

using System.Text;

namespace DeskDial;

sealed class HardwareModule : IModule
{
    readonly HardwareMonitor _hw;

    public HardwareModule(HardwareMonitor hw) => _hw = hw;

    public byte Feature => Proto.FeatureHardware;

    public Task StartAsync(Companion companion)
    {
        // Sensor reads are slow (SMART, SMBus): they get their own task so
        // they never delay the media updates. Serial writes are locked.
        _ = Task.Run(() => LoopAsync(companion));
        return Task.CompletedTask;
    }

    public Task UpdateAsync(SerialLink link, bool full) => Task.CompletedTask;

    public void OnFrame(byte type, byte[] payload) { }

    async Task LoopAsync(Companion companion)
    {
        while (true)
        {
            var link = companion.Link;
            if (link is not null && !link.Dead && companion.BoardHas(Feature) && companion.BoardPage == Proto.PageHw)
            {
                try { Send(link); }
                catch (Exception e) { Log.Write($"hardware: {e.Message}"); }
            }
            await Task.Delay(1000);
        }
    }

    void Send(SerialLink link)
    {
        var (data, temps) = _hw.Sample();
        var payload = new byte[1 + data.Count * 5];
        payload[0] = (byte)data.Count;
        for (int i = 0; i < data.Count; i++)
        {
            payload[1 + i * 5] = (byte)data[i].Item1;
            BitConverter.TryWriteBytes(payload.AsSpan(2 + i * 5, 4), data[i].Item2);
        }
        link.Send(Proto.Hw, payload);

        // Labelled temperatures for the bottom row (always sent, even empty)
        var t = new List<byte> { (byte)Math.Min(temps.Count, HardwareMonitor.MaxTemps) };
        foreach (var (label, value) in temps.Take(HardwareMonitor.MaxTemps))
        {
            var lb = Encoding.UTF8.GetBytes(label);
            t.Add((byte)lb.Length);
            t.AddRange(lb);
            t.AddRange(BitConverter.GetBytes(value));
        }
        link.Send(Proto.HwTemps, t.ToArray());
    }
}
