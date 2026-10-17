// One open COM port to the board: reader task, locked writes, probing.
// SPDX-License-Identifier: MIT

using System.IO; // not implicit in WPF projects
using System.IO.Ports;

namespace DeskDial;

sealed class SerialLink : IDisposable
{
    readonly SerialPort _sp;
    readonly object _writeLock = new();
    public volatile bool Dead;
    public string Name => _sp.PortName;
    public event Action<byte, byte[]>? FrameReceived;

    SerialLink(SerialPort sp) => _sp = sp;

    /// Opens the port, sends a HELLO and waits for the board's HELLO; null if
    /// nothing DeskDial answers. `onFrame` sees every frame from the start, the
    /// probe's answer included.
    public static async Task<SerialLink?> TryOpenAsync(string name, Action<SerialLink, byte, byte[]> onFrame)
    {
        var sp = new SerialPort(name, Proto.Baud)
        {
            // DTR on / RTS off: needed for native USB CDC to talk, and never
            // pulls EN low on boards with a USB-UART auto-reset circuit.
            DtrEnable = true,
            RtsEnable = false,
            Handshake = Handshake.None,
            WriteTimeout = 2000,
        };
        try { sp.Open(); }
        catch (Exception e)
        {
            Log.Write($"{name}: {e.Message}");
            sp.Dispose();
            return null;
        }

        var link = new SerialLink(sp);
        var hello = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        link.FrameReceived += (type, payload) =>
        {
            if (type == Proto.Hello && Proto.ParseHello(payload, out _, out _)) hello.TrySetResult();
            onFrame(link, type, payload);
        };
        link.StartReader();
        link.Send(Proto.Hello, Proto.HelloPayload(reply: false));

        var winner = await Task.WhenAny(hello.Task, Task.Delay(1500));
        if (winner == hello.Task && !link.Dead)
            return link;

        Log.Write($"{name}: no answer");
        link.Dispose();
        return null;
    }

    void StartReader() => Task.Run(async () =>
    {
        var buf = new byte[4096];
        var parser = new FrameParser();
        try
        {
            while (!Dead)
            {
                int n = await _sp.BaseStream.ReadAsync(buf);
                if (n <= 0) throw new IOException("port closed");
                parser.Feed(buf.AsSpan(0, n), (t, p) => FrameReceived?.Invoke(t, p));
            }
        }
        catch (Exception e)
        {
            if (!Dead) Log.Write($"{Name}: read failed: {e.Message}");
            Dead = true;
        }
    });

    public void Send(byte type, ReadOnlySpan<byte> payload)
    {
        if (Dead) return;
        var frame = Proto.Frame(type, payload);
        lock (_writeLock)
        {
            try { _sp.BaseStream.Write(frame, 0, frame.Length); }
            catch (Exception e)
            {
                Log.Write($"{Name}: write failed: {e.Message}");
                Dead = true;
            }
        }
    }

    public void Dispose()
    {
        Dead = true;
        try { _sp.Close(); } catch { }
        _sp.Dispose();
    }
}
