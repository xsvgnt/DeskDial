// Finds the board, keeps the link up and runs the feature modules over it.
//
// Handshake (docs/protocol.md): both sides send HELLO with the features they
// were built with. Each module only talks to a board that has its feature.
// SPDX-License-Identifier: MIT

using System.IO.Ports;

namespace DeskDial;

/// A feature of the companion (media, hardware).
interface IModule
{
    byte Feature { get; } // Proto.Feature*

    /// Once, before the first connection.
    Task StartAsync(Companion companion);

    /// After every wake-up (an event or the 1 s heartbeat) while linked to a
    /// board that has this feature. `full`: the board just said HELLO, resend
    /// everything.
    Task UpdateAsync(SerialLink link, bool full);

    /// Board -> PC frames other than HELLO and PAGE.
    void OnFrame(byte type, byte[] payload);
}

sealed class Companion
{
    readonly string? _fixedPort;
    readonly IReadOnlyList<IModule> _modules;
    readonly SemaphoreSlim _wake = new(0, 1);

    volatile SerialLink? _link;
    volatile bool _needFull = true;
    volatile byte _boardFeatures; // what the board announced
    volatile byte _boardPage = Proto.PageMedia;

    public Companion(string? port, IReadOnlyList<IModule> modules)
    {
        _fixedPort = port;
        _modules = modules;
    }

    /// The current link, null or dead while the board is away.
    public SerialLink? Link => _link;

    /// Page the board shows (Proto.Page*).
    public byte BoardPage => _boardPage;

    public bool BoardHas(byte feature) => (_boardFeatures & feature) != 0;

    /// Runs the main loop soon (something changed).
    public void Wake()
    {
        try { _wake.Release(); } catch (SemaphoreFullException) { }
    }

    public async Task RunAsync()
    {
        foreach (var m in _modules) await m.StartAsync(this);
        if (Log.Enabled) _ = Task.Run(MemoryStatsLoopAsync);

        while (true)
        {
            try
            {
                var link = _link;
                if (link is null || link.Dead)
                {
                    link?.Dispose();
                    _link = null;
                    _boardPage = Proto.PageMedia;
                    _boardFeatures = 0;
                    link = await ConnectAsync();
                    if (link is null)
                    {
                        await Task.Delay(3000);
                        continue;
                    }
                    _link = link;
                }

                bool full = _needFull;
                _needFull = false;
                foreach (var m in _modules)
                    if (BoardHas(m.Feature))
                        await m.UpdateAsync(link, full);
            }
            catch (Exception e)
            {
                Log.Write($"loop: {e}");
            }
            await _wake.WaitAsync(1000); // event-driven, plus a 1 s heartbeat
            await Task.Delay(100);       // let a burst of events collapse into one update
        }
    }

    async Task<SerialLink?> ConnectAsync()
    {
        var names = _fixedPort is not null ? new[] { _fixedPort } : SerialPort.GetPortNames().Distinct().ToArray();
        foreach (var name in names)
        {
            var link = await SerialLink.TryOpenAsync(name, OnFrame);
            if (link is null) continue;
            Log.Write($"Connected to the display on {name}");
            return link;
        }
        return null;
    }

    void OnFrame(SerialLink link, byte type, byte[] payload)
    {
        switch (type)
        {
            case Proto.Hello: // board (re)booted, reconnected or answered: resend everything
                if (!Proto.ParseHello(payload, out byte features, out bool reply))
                {
                    Log.Write($"{link.Name}: not a DeskDial board (or firmware older than DeskDial 1.0)");
                    return;
                }
                if (features != _boardFeatures)
                    Log.Write($"board features: {Describe(features)} (this program: {Describe(Proto.Features)})");
                _boardFeatures = features;
                if (!reply) link.Send(Proto.Hello, Proto.HelloPayload(reply: true));
                _needFull = true;
                Wake();
                break;
            case Proto.Page when payload.Length == 1:
                _boardPage = payload[0];
                Log.Write($"board page: {(payload[0] == Proto.PageHw ? "hardware" : "media")}");
                break;
            default:
                foreach (var m in _modules) m.OnFrame(type, payload);
                break;
        }
    }

    static string Describe(byte f) =>
        f switch
        {
            Proto.FeatureMedia | Proto.FeatureHardware => "media + hardware",
            Proto.FeatureMedia => "media",
            Proto.FeatureHardware => "hardware",
            _ => "none",
        };

    /// --debug only: allocation rate and heap size every 30 s, to check that
    /// memory stays flat (a sawtooth from 30 MB to 230 MB means garbage).
    static async Task MemoryStatsLoopAsync()
    {
        long last = GC.GetTotalAllocatedBytes();
        while (true)
        {
            await Task.Delay(30_000);
            long now = GC.GetTotalAllocatedBytes();
            Log.Write($"memory: {(now - last) / 30.0 / 1024:0.0} KB/s allocated, " +
                      $"GC heap {GC.GetTotalMemory(false) / 1048576.0:0.0} MB, " +
                      $"working set {Environment.WorkingSet / 1048576.0:0} MB, " +
                      $"collections {GC.CollectionCount(0)}/{GC.CollectionCount(1)}/{GC.CollectionCount(2)}");
            last = now;
        }
    }
}
