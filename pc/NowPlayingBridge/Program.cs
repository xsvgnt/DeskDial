// NowPlayingBridge — feeds the ESP32-S3 "now playing" display over USB serial.
//
// Reads Spotify's state from Windows' media controls (the same data shown in
// the volume flyout), pushes it to the board on every change plus once a
// second, and executes play/pause/next/previous commands coming back.
// Wire format: see firmware/NowPlaying/protocol.h.
//
// It also sends hardware readings (Hardware.cs) while the board shows its
// hardware page. Run elevated for temperatures and power.
//
// Usage: NowPlayingBridge.exe [--port COM5] [--spotify-only] [--debug]
//        NowPlayingBridge.exe --list-sensors
//        NowPlayingBridge.exe --trace-time   (debug log + position trace CSV)

using System.IO; // not implicit in WPF projects
using System.IO.Ports;
using System.Runtime.InteropServices;
using System.Text;
using Windows.Graphics.Imaging;
using Windows.Media.Control;
using Windows.Storage.Streams;

using Session = Windows.Media.Control.GlobalSystemMediaTransportControlsSession;
using SessionManager = Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager;
using PlaybackStatus = Windows.Media.Control.GlobalSystemMediaTransportControlsSessionPlaybackStatus;

namespace NowPlayingBridge;

static class Program
{
    [DllImport("kernel32.dll")]
    static extern bool AllocConsole();

    [DllImport("kernel32.dll")]
    static extern bool AttachConsole(int processId);

    /// WinExe has no console: reuse the terminal it was started from, or open one.
    /// Returns true when a new window was opened.
    static bool OpenConsole()
    {
        if (AttachConsole(-1)) return false; // ATTACH_PARENT_PROCESS
        AllocConsole();
        return true;
    }

    static async Task<int> Main(string[] args)
    {
        string? port = null;
        bool spotifyOnly = false, listSensors = false;
        for (int i = 0; i < args.Length; i++)
        {
            switch (args[i].ToLowerInvariant())
            {
                case "--debug": OpenConsole(); Log.Enabled = true; break;
                case "--port" when i + 1 < args.Length: port = args[++i]; break;
                case "--spotify-only": spotifyOnly = true; break;
                case "--list-sensors": listSensors = true; break;
                case "--trace-time":
                    OpenConsole();
                    Log.Enabled = true;
                    Log.Write($"Tracing the playback position to {TimeTrace.Start()}");
                    break;
            }
        }

        var overrides = HardwareMonitor.LoadOverrides(System.IO.Path.Combine(AppContext.BaseDirectory, "sensors.ini"));
        if (listSensors)
        {
            bool newWindow = OpenConsole();
            Log.Enabled = true;
            Console.WriteLine();
            if (!HardwareMonitor.IsElevated())
                Console.WriteLine("Note: not running as administrator, most sensors will be missing.\n");
            using (var hw = new HardwareMonitor(overrides))
                hw.PrintAll(Console.Out);
            if (newWindow) { Console.WriteLine("\nPress Enter to close."); Console.ReadLine(); }
            return 0;
        }

        using var mutex = new Mutex(true, @"Local\NowPlayingBridge", out bool firstInstance);
        if (!firstInstance)
        {
            Log.Write("Another instance is already running.");
            return 1;
        }

        await new Bridge(port, spotifyOnly, new HardwareMonitor(overrides)).RunAsync();
        return 0;
    }
}

static class Log
{
    public static bool Enabled;
    public static void Write(string msg)
    {
        if (Enabled) Console.WriteLine($"{DateTime.Now:HH:mm:ss.fff}  {msg}");
    }
}

// ----------------------------------------------------------------- protocol --
static class Proto
{
    public const byte Sync0 = 0xA5, Sync1 = 0x5A;
    public const byte Hello = 0x01, State = 0x02, Art = 0x03, ArtNone = 0x04, Hw = 0x05, HwTemps = 0x06,
                      TextImg = 0x07, Cmd = 0x10, Page = 0x11;
    public const byte PageMedia = 0, PageHw = 1;
    public const byte CmdPlayPause = 1, CmdNext = 2, CmdPrev = 3;
    public const byte FlagSession = 0x01, FlagPlaying = 0x02;
    public const int ArtW = 150, ArtH = 150, ArtBytes = ArtW * ArtH * 2;
    public const int MaxPayload = 4 + ArtBytes;
    public const int Baud = 921600; // ignored by native USB CDC
    public static readonly byte[] HelloPayload = "NPR1"u8.ToArray();

    public static byte[] Frame(byte type, ReadOnlySpan<byte> payload)
    {
        var f = new byte[7 + payload.Length + 1];
        f[0] = Sync0; f[1] = Sync1; f[2] = type;
        BitConverter.TryWriteBytes(f.AsSpan(3, 4), (uint)payload.Length); // little-endian on x86/x64
        payload.CopyTo(f.AsSpan(7));
        byte sum = 0;
        for (int i = 2; i < f.Length - 1; i++) sum += f[i];
        f[^1] = sum;
        return f;
    }
}

sealed class FrameParser
{
    int _state, _hn;
    readonly byte[] _hdr = new byte[5];
    byte[] _payload = Array.Empty<byte>();
    int _plen, _pn;
    byte _sum;

    public void Feed(ReadOnlySpan<byte> data, Action<byte, byte[]> onFrame)
    {
        foreach (byte b in data)
        {
            switch (_state)
            {
                case 0: if (b == Proto.Sync0) _state = 1; break;
                case 1: _state = b == Proto.Sync1 ? 2 : (b == Proto.Sync0 ? 1 : 0); _hn = 0; break;
                case 2:
                    _hdr[_hn++] = b;
                    if (_hn == 5)
                    {
                        _plen = (int)BitConverter.ToUInt32(_hdr, 1);
                        _sum = (byte)(_hdr[0] + _hdr[1] + _hdr[2] + _hdr[3] + _hdr[4]);
                        if (_plen < 0 || _plen > Proto.MaxPayload) { _state = 0; break; }
                        _payload = new byte[_plen];
                        _pn = 0;
                        _state = _plen > 0 ? 3 : 4;
                    }
                    break;
                case 3:
                    _payload[_pn++] = b;
                    _sum += b;
                    if (_pn == _plen) _state = 4;
                    break;
                case 4:
                    if (b == _sum) onFrame(_hdr[0], _payload);
                    _state = 0;
                    break;
            }
        }
    }
}

// --------------------------------------------------------------- serial link --
sealed class Link : IDisposable
{
    readonly SerialPort _sp;
    readonly object _writeLock = new();
    public volatile bool Dead;
    public string Name => _sp.PortName;
    public event Action<byte, byte[]>? FrameReceived;

    Link(SerialPort sp) => _sp = sp;

    /// Opens the port and waits for the board's HELLO; null if it isn't ours.
    public static async Task<Link?> TryOpenAsync(string name)
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

        var link = new Link(sp);
        var hello = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        void OnProbe(byte type, byte[] _) { if (type == Proto.Hello) hello.TrySetResult(); }
        link.FrameReceived += OnProbe;
        link.StartReader();
        link.Send(Proto.Hello, Proto.HelloPayload);

        var winner = await Task.WhenAny(hello.Task, Task.Delay(1500));
        link.FrameReceived -= OnProbe;
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

// -------------------------------------------------------------------- bridge --
sealed class Bridge
{
    readonly string? _fixedPort;
    readonly bool _spotifyOnly;
    readonly HardwareMonitor _hw;
    volatile bool _hwWanted; // board is showing the hardware page
    readonly SemaphoreSlim _wake = new(0, 1);

    SessionManager _mgr = null!;
    Session? _session;
    string _sessionId = "";
    volatile bool _sessionsChanged = true;

    Link? _link;
    volatile bool _needFull = true;

    // Artwork. Browsers publish a new song's title first and its artwork later
    // (after downloading it), so for a moment Windows reports the new title
    // with the previous cover. Every thumbnail update is therefore decoded
    // again, compared by fingerprint, and re-checked shortly after each track
    // change in case the real artwork arrives without a separate event.
    string _trackKey = "";               // title/artist/album currently shown
    uint _artId;
    byte[]? _artPixels;                  // null = placeholder
    ulong _artFingerprint;               // of _artPixels, 0 = placeholder
    bool _artDirty;                      // thumbnail may have changed: decode again
    bool _artForTrack;                   // an image was loaded for the current track
    readonly List<DateTime> _artRechecks = new();
    static readonly TimeSpan[] ArtRecheckDelays = { TimeSpan.FromSeconds(1), TimeSpan.FromSeconds(3) };
    uint _sentArtId = uint.MaxValue;
    readonly string?[] _sentTextImg = new string?[2]; // per field: text sent as image, "" = label
    readonly string?[] _textSeen = new string?[2];    // per field: last text checked for images

    // Cached media state. Windows is only queried for the part an event says
    // changed (plus a full re-read every 15 s as a safety net); the 1 s
    // heartbeat re-sends from this cache without touching WinRT, so it no
    // longer creates a stream of short-lived WinRT objects.
    volatile bool _watchDirty = true, _selectDirty = true;
    volatile bool _mediaDirty = true, _playbackDirty = true, _timelineDirty = true;
    readonly List<Session> _watched = new();
    DateTime _nextSafetyRefresh;
    string _title = "", _artist = "", _album = "", _mediaKey = ""; // _mediaKey: title/artist/album
    IRandomAccessStreamReference? _thumb;
    PlaybackStatus _status = PlaybackStatus.Closed;
    TimeSpan _tlStart, _tlEnd, _tlPos;
    DateTimeOffset _tlUpdated;
    // Firefox sometimes publishes an empty timeline (position 0, duration 0)
    // in the middle of a video that keeps playing, and only publishes a real
    // one again on the next pause/seek. While the same track is loaded, the
    // last valid timeline is kept and projected forward instead.
    bool _tlHasGood;                     // a valid timeline was seen for _tlGoodTrack
    string _tlGoodTrack = "";
    bool _tlHeld;                        // currently projecting a kept timeline
    string? _stTitle, _stArtist;          // BuildState cache
    byte[] _stTitleBytes = Array.Empty<byte>(), _stArtistBytes = Array.Empty<byte>();
    long _hwAllocated;                    // bytes allocated by sensor reads (debug stats)

    public Bridge(string? port, bool spotifyOnly, HardwareMonitor hw)
    {
        _fixedPort = port;
        _spotifyOnly = spotifyOnly;
        _hw = hw;
    }

    void Wake()
    {
        try { _wake.Release(); } catch (SemaphoreFullException) { }
    }

    public async Task RunAsync()
    {
        _mgr = await SessionManager.RequestAsync();
        _mgr.SessionsChanged += (_, _) => { _sessionsChanged = true; _watchDirty = true; _selectDirty = true; Wake(); };
        _mgr.CurrentSessionChanged += (_, _) => { _selectDirty = true; Wake(); };
        _ = Task.Run(HardwareLoopAsync);
        if (Log.Enabled) _ = Task.Run(MemoryStatsLoopAsync);

        while (true)
        {
            try
            {
                if (_link is null || _link.Dead)
                {
                    _link?.Dispose();
                    _hwWanted = false; // the board re-announces its page after HELLO
                    _link = await ConnectAsync();
                    if (_link is null)
                    {
                        await Task.Delay(3000);
                        continue;
                    }
                    _needFull = true;
                }
                await RefreshAsync();
                await PushAsync(_link);
            }
            catch (Exception e)
            {
                Log.Write($"loop: {e}");
            }
            await _wake.WaitAsync(1000); // event-driven, plus a 1 s heartbeat
            await Task.Delay(100);       // let a burst of events collapse into one refresh
        }
    }

    async Task<Link?> ConnectAsync()
    {
        var names = _fixedPort is not null ? new[] { _fixedPort } : SerialPort.GetPortNames().Distinct().ToArray();
        foreach (var name in names)
        {
            var link = await Link.TryOpenAsync(name);
            if (link is null) continue;
            Log.Write($"Connected to the display on {name}");
            link.FrameReceived += OnFrame;
            link.Send(Proto.Hello, Proto.HelloPayload); // board answers with HELLO + its current page
            return link;
        }
        return null;
    }

    void OnFrame(byte type, byte[] payload)
    {
        switch (type)
        {
            case Proto.Hello: // board (re)booted or lost sync: resend everything
                _needFull = true;
                Wake();
                break;
            case Proto.Page when payload.Length == 1:
                _hwWanted = payload[0] == Proto.PageHw;
                Log.Write($"board page: {(_hwWanted ? "hardware" : "media")}");
                break;
            case Proto.Cmd when payload.Length == 1:
                _ = RunCommandAsync(payload[0]);
                break;
        }
    }

    async Task RunCommandAsync(byte cmd)
    {
        var s = _session;
        if (s is null) return;
        try
        {
            bool ok = cmd switch
            {
                Proto.CmdPlayPause => await s.TryTogglePlayPauseAsync(),
                Proto.CmdNext => await s.TrySkipNextAsync(),
                Proto.CmdPrev => await s.TrySkipPreviousAsync(),
                _ => false,
            };
            Log.Write($"command {cmd}: {(ok ? "ok" : "refused")}");
        }
        catch (Exception e)
        {
            Log.Write($"command {cmd} failed: {e.Message}");
        }
        // Push the real state soon after, so the board's optimistic UI settles.
        _playbackDirty = _timelineDirty = _mediaDirty = true;
        await Task.Delay(150); Wake();
        _playbackDirty = _timelineDirty = true;
        await Task.Delay(500); Wake();
    }

    // ------------------------------------------------------------ hardware --
    /// Once a second, while the board shows the hardware page. Sensor reads run
    /// on this task only; the serial write is shared with the media loop (locked).
    async Task HardwareLoopAsync()
    {
        while (true)
        {
            var link = _link;
            if (_hwWanted && link is not null && !link.Dead)
            {
                long before = GC.GetAllocatedBytesForCurrentThread();
                var (data, temps) = _hw.Sample();
                Interlocked.Add(ref _hwAllocated, GC.GetAllocatedBytesForCurrentThread() - before);
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
            await Task.Delay(1000);
        }
    }

    /// --debug only: allocation rate and heap size every 30 s, to check that
    /// memory stays flat (a sawtooth from 30 MB to 230 MB means garbage).
    async Task MemoryStatsLoopAsync()
    {
        long last = GC.GetTotalAllocatedBytes(), lastHw = 0;
        while (true)
        {
            await Task.Delay(30_000);
            long now = GC.GetTotalAllocatedBytes(), hw = Interlocked.Read(ref _hwAllocated);
            Log.Write($"memory: {(now - last) / 30.0 / 1024:0.0} KB/s allocated " +
                      $"({(hw - lastHw) / 30.0 / 1024:0.0} KB/s by sensor reads), " +
                      $"GC heap {GC.GetTotalMemory(false) / 1048576.0:0.0} MB, " +
                      $"working set {Environment.WorkingSet / 1048576.0:0} MB, " +
                      $"collections {GC.CollectionCount(0)}/{GC.CollectionCount(1)}/{GC.CollectionCount(2)}");
            last = now;
            lastHw = hw;
        }
    }

    // ------------------------------------------------------------ sessions --
    static bool IsSpotify(Session s) =>
        s.SourceAppUserModelId.Contains("Spotify", StringComparison.OrdinalIgnoreCase);

    static PlaybackStatus StatusOf(Session s)
    {
        try { return s.GetPlaybackInfo()?.PlaybackStatus ?? PlaybackStatus.Closed; }
        catch { return PlaybackStatus.Closed; }
    }

    /// Has a track loaded (title) and is playing or paused on it.
    static async Task<bool> HasMediaAsync(Session s)
    {
        if (StatusOf(s) is not (PlaybackStatus.Playing or PlaybackStatus.Paused or PlaybackStatus.Changing))
            return false;
        try
        {
            var props = await s.TryGetMediaPropertiesAsync();
            return !string.IsNullOrEmpty(props?.Title);
        }
        catch { return false; }
    }

    /// Watches every app's play/pause and track changes, so the choice of app
    /// is re-evaluated only when something actually changed.
    void WatchAllSessions()
    {
        foreach (var w in _watched)
        {
            try
            {
                w.PlaybackInfoChanged -= OnAnyPlaybackChanged;
                w.MediaPropertiesChanged -= OnAnyMediaChanged;
            }
            catch { /* already gone */ }
        }
        _watched.Clear();
        try
        {
            foreach (var w in _mgr.GetSessions())
            {
                w.PlaybackInfoChanged += OnAnyPlaybackChanged;
                w.MediaPropertiesChanged += OnAnyMediaChanged;
                _watched.Add(w);
            }
        }
        catch (Exception e)
        {
            Log.Write($"watch sessions: {e.Message}");
        }
    }

    void OnAnyPlaybackChanged(Session s, PlaybackInfoChangedEventArgs a) { _selectDirty = true; Wake(); }
    void OnAnyMediaChanged(Session s, MediaPropertiesChangedEventArgs a) { _selectDirty = true; Wake(); }

    /// Re-reads from Windows only what an event marked as changed.
    async Task RefreshAsync()
    {
        if (DateTime.UtcNow >= _nextSafetyRefresh) // in case an event was missed
        {
            _nextSafetyRefresh = DateTime.UtcNow.AddSeconds(15);
            _watchDirty = _selectDirty = _mediaDirty = _playbackDirty = _timelineDirty = true;
        }
        if (_artRechecks.Count > 0 && DateTime.UtcNow >= _artRechecks[0])
        {
            _artRechecks.RemoveAt(0);
            _mediaDirty = true; // look for artwork that arrived without an event
        }
        if (_watchDirty) { _watchDirty = false; WatchAllSessions(); }
        if (_selectDirty) { _selectDirty = false; await SelectSessionAsync(); }

        var s = _session;
        if (s is null)
        {
            _title = _artist = _album = _mediaKey = "";
            _artDirty = true;
            _thumb = null;
            _status = PlaybackStatus.Closed;
            _tlStart = _tlEnd = _tlPos = TimeSpan.Zero;
            _tlHasGood = _tlHeld = false;
            return;
        }
        try
        {
            // Flags are cleared before reading, so an event arriving meanwhile
            // marks the data dirty again instead of being lost.
            if (_mediaDirty)
            {
                _mediaDirty = false;
                var props = await s.TryGetMediaPropertiesAsync();
                _title = props?.Title ?? "";
                _artist = props?.Artist ?? "";
                if (_artist.Length == 0) _artist = props?.AlbumArtist ?? "";
                _album = props?.AlbumTitle ?? "";
                _thumb = props?.Thumbnail;
                _mediaKey = $"{_title}\u0001{_artist}\u0001{_album}";
                if (_mediaKey != _tlGoodTrack)
                {
                    // Another track: a kept timeline belongs to the previous one
                    _tlHasGood = false;
                    if (_tlHeld)
                    {
                        _tlHeld = false;
                        _tlStart = _tlEnd = _tlPos = TimeSpan.Zero;
                    }
                }
                _artDirty = true; // the thumbnail may have changed even if the title didn't
            }
            if (_playbackDirty)
            {
                _playbackDirty = false;
                var status = StatusOf(s);
                // A kept timeline is projected from its own anchor: re-anchor it
                // when play/pause changes so the count stops or resumes in place.
                if (_tlHeld && status != _status) RebaseTimeline();
                _status = status;
            }
            if (_timelineDirty)
            {
                _timelineDirty = false;
                var tl = s.GetTimelineProperties();
                var start = tl?.StartTime ?? TimeSpan.Zero;
                var end = tl?.EndTime ?? TimeSpan.Zero;
                bool valid = end - start > TimeSpan.Zero;
                if (!valid && _tlHasGood && _tlGoodTrack == _mediaKey)
                {
                    if (!_tlHeld)
                        Log.Write("timeline: the player cleared it mid-track, counting on from the last valid one");
                    RebaseTimeline();
                    _tlHeld = true;
                }
                else
                {
                    _tlStart = start;
                    _tlEnd = end;
                    _tlPos = tl?.Position ?? TimeSpan.Zero;
                    _tlUpdated = tl?.LastUpdatedTime ?? default;
                    _tlHeld = false;
                    if (valid)
                    {
                        _tlHasGood = true;
                        _tlGoodTrack = _mediaKey;
                    }
                }
                TimeTrace.Snapshot("read", s);
            }
        }
        catch (Exception e)
        {
            Log.Write($"read session: {e.Message}");
            _status = PlaybackStatus.Closed;
        }
    }

    /// Which app to show, in order:
    ///  1. something actually playing (Windows' current session, i.e. what its
    ///     own media flyout shows, usually the last one started; then the one
    ///     already shown; then Spotify; then any other);
    ///  2. otherwise something paused with a track loaded (the one already shown,
    ///     then Windows' current session, then Spotify, then any other), so a paused
    ///     video stays on screen instead of an idle Spotify taking over;
    ///  3. otherwise nothing ("Nothing is playing").
    /// --spotify-only restricts all of this to Spotify.
    async Task SelectSessionAsync()
    {
        Session? pick = null;
        try
        {
            var sessions = _mgr.GetSessions().Where(s => !_spotifyOnly || IsSpotify(s)).ToList();
            string currentId = "";
            try { currentId = _mgr.GetCurrentSession()?.SourceAppUserModelId ?? ""; } catch { }

            var withMedia = new List<Session>();
            foreach (var s in sessions)
                if (await HasMediaAsync(s)) withMedia.Add(s);
            var playing = withMedia.Where(s => StatusOf(s) == PlaybackStatus.Playing).ToList();

            pick = playing.FirstOrDefault(s => s.SourceAppUserModelId == currentId)
                   ?? playing.FirstOrDefault(s => s.SourceAppUserModelId == _sessionId)
                   ?? playing.FirstOrDefault(IsSpotify)
                   ?? playing.FirstOrDefault()
                   ?? withMedia.FirstOrDefault(s => s.SourceAppUserModelId == _sessionId)
                   ?? withMedia.FirstOrDefault(s => s.SourceAppUserModelId == currentId)
                   ?? withMedia.FirstOrDefault(IsSpotify)
                   ?? withMedia.FirstOrDefault();
        }
        catch (Exception e)
        {
            Log.Write($"GetSessions: {e.Message}");
        }

        string id = pick?.SourceAppUserModelId ?? "";
        if (id == _sessionId && !_sessionsChanged) return;
        _sessionsChanged = false;

        if (_session is not null)
        {
            try
            {
                _session.MediaPropertiesChanged -= OnMediaChanged;
                _session.PlaybackInfoChanged -= OnPlaybackChanged;
                _session.TimelinePropertiesChanged -= OnTimelineChanged;
            }
            catch { /* session already gone */ }
        }

        _session = pick;
        _sessionId = id;
        _mediaDirty = _playbackDirty = _timelineDirty = true;
        if (pick is not null)
        {
            pick.MediaPropertiesChanged += OnMediaChanged;
            pick.PlaybackInfoChanged += OnPlaybackChanged;
            pick.TimelinePropertiesChanged += OnTimelineChanged;
        }
        Log.Write($"session: {(id == "" ? "(none)" : id)}");
    }

    // The shown app's own events: mark exactly what to re-read.
    void OnMediaChanged(Session s, MediaPropertiesChangedEventArgs a) { _mediaDirty = true; Wake(); }
    void OnPlaybackChanged(Session s, PlaybackInfoChangedEventArgs a)
    {
        TimeTrace.Snapshot("playback-event", s);
        _playbackDirty = _timelineDirty = true;
        Wake();
    }

    void OnTimelineChanged(Session s, TimelinePropertiesChangedEventArgs a)
    {
        TimeTrace.Snapshot("timeline-event", s);
        _timelineDirty = true;
        Wake();
    }

    // ---------------------------------------------------------------- push --
    /// Sends the cached state. No WinRT calls here except loading new artwork.
    async Task PushAsync(Link link)
    {
        string title = _title, artist = _artist;
        var thumb = _thumb;
        bool playing = _status == PlaybackStatus.Playing;
        bool has = title.Length > 0 && _status is PlaybackStatus.Playing or PlaybackStatus.Paused or PlaybackStatus.Changing;
        uint posMs = 0, durMs = 0;

        // An empty timeline (no duration) means "position unknown": browsers
        // publish one when a video loads or changes (Chromium's ClearPosition)
        // and only send a real one on the next play/pause/seek. Send 0/0 and let
        // the board hide the time, rather than extrapolating a fake counter.
        var dur = _tlEnd - _tlStart;
        if (has && dur > TimeSpan.Zero)
        {
            var pos = ProjectedPosition();
            durMs = (uint)Math.Min(dur.TotalMilliseconds, uint.MaxValue);
            posMs = (uint)Math.Min(pos.TotalMilliseconds, uint.MaxValue);
        }

        await UpdateArtAsync(has, title, artist, thumb);

        bool full = _needFull;
        _needFull = false;
        if (full || _sentArtId != _artId)
        {
            var idBytes = BitConverter.GetBytes(_artId);
            if (_artPixels is not null)
            {
                var payload = new byte[4 + Proto.ArtBytes];
                idBytes.CopyTo(payload, 0);
                _artPixels.CopyTo(payload, 4);
                link.Send(Proto.Art, payload); // art first, so text and cover change together
            }
            else
            {
                link.Send(Proto.ArtNone, idBytes);
            }
            _sentArtId = _artId;
        }

        // Text the board's fonts can't draw goes as an image, before the state
        // frame so the new line appears together with the new track.
        await SendTextImageAsync(link, 0, has ? title : "", full);
        await SendTextImageAsync(link, 1, has ? artist : "", full);

        link.Send(Proto.State, BuildState(has, playing, posMs, durMs, _artId, title, artist));
        TimeTrace.Sent(_sessionId, posMs, durMs, has && playing);
    }

    /// Position within the track from the cached timeline. Players report it
    /// only on play/pause/seek, so it is projected forward while playing.
    TimeSpan ProjectedPosition()
    {
        var dur = _tlEnd - _tlStart;
        var pos = _tlPos - _tlStart;
        if (_status == PlaybackStatus.Playing && _tlUpdated.Year > 2000)
        {
            var age = DateTimeOffset.Now - _tlUpdated;
            if (age > TimeSpan.Zero && age < TimeSpan.FromHours(12)) pos += age;
        }
        if (pos < TimeSpan.Zero) pos = TimeSpan.Zero;
        if (dur > TimeSpan.Zero && pos > dur) pos = dur;
        return pos;
    }

    /// Re-anchors the cached timeline at the current projected position.
    void RebaseTimeline()
    {
        _tlPos = _tlStart + ProjectedPosition();
        _tlUpdated = DateTimeOffset.Now;
    }

    async Task UpdateArtAsync(bool has, string title, string artist, IRandomAccessStreamReference? thumb)
    {
        string trackKey = has ? _mediaKey : "";
        if (trackKey != _trackKey)
        {
            _trackKey = trackKey;
            _artDirty = true;
            _artForTrack = false;
            _artRechecks.Clear();
            if (has)
                foreach (var d in ArtRecheckDelays) _artRechecks.Add(DateTime.UtcNow + d);
            Log.Write(has ? $"track: {artist} - {title}" : "track: (none)");
        }
        if (!_artDirty) return;
        _artDirty = false;

        var px = has && thumb is not null ? await LoadArtAsync(thumb) : null;
        if (px is not null)
        {
            ulong fp = Fingerprint(px);
            bool same = fp == _artFingerprint;
            if (!same || !_artForTrack) // skip the 15 s safety re-reads that change nothing
                Log.Write($"artwork: {fp >> 48:X4} for \"{title}\"{(same ? " (same image as before)" : "")}");
            if (!same)
            {
                _artPixels = px;
                _artFingerprint = fp;
                _artId++; // new id: PushAsync sends it
            }
            _artForTrack = true;
        }
        else if (!has || thumb is null || !_artForTrack)
        {
            // No artwork, or it failed to load for a new track: show the
            // placeholder rather than the previous song's cover. A recheck
            // will pick it up if it arrives later.
            if (_artPixels is not null)
            {
                _artPixels = null;
                _artFingerprint = 0;
                _artId++;
            }
        }
    }

    /// FNV-1a over the converted pixels: identical images give identical values.
    static ulong Fingerprint(byte[] data)
    {
        ulong h = 14695981039346656037;
        foreach (byte b in data) { h ^= b; h *= 1099511628211; }
        return h == 0 ? 1 : h; // 0 is reserved for "placeholder"
    }

    async Task SendTextImageAsync(Link link, byte field, string text, bool full)
    {
        if (!full && _textSeen[field] == text) return; // unchanged: nothing to check
        _textSeen[field] = text;
        string key = TextFilter.AllSupported(text) ? "" : text;
        if (!full && _sentTextImg[field] == key) return;
        _sentTextImg[field] = key;

        var img = key.Length > 0 ? await TextImage.RenderAsync(key, title: field == 0) : null;
        if (img is not { } bmp)
        {
            link.Send(Proto.TextImg, new byte[] { field, 0, 0, 0, 0 }); // use the label
            return;
        }
        var payload = new byte[5 + bmp.Alpha4.Length];
        payload[0] = field;
        BitConverter.TryWriteBytes(payload.AsSpan(1, 2), (ushort)bmp.Width);
        BitConverter.TryWriteBytes(payload.AsSpan(3, 2), (ushort)bmp.Height);
        bmp.Alpha4.CopyTo(payload, 5);
        link.Send(Proto.TextImg, payload);
        Log.Write($"text image: field {field}, {bmp.Width}x{bmp.Height}, \"{key}\"");
    }

    byte[] BuildState(bool has, bool playing, uint pos, uint dur, uint artId, string title, string artist)
    {
        // Converted once per track, not once per heartbeat
        if (title != _stTitle) { _stTitle = title; _stTitleBytes = Utf8Clip(TextFilter.ForDisplay(title), 200); }
        if (artist != _stArtist) { _stArtist = artist; _stArtistBytes = Utf8Clip(TextFilter.ForDisplay(artist), 200); }
        var t = _stTitleBytes;
        var a = _stArtistBytes;
        var p = new byte[13 + 1 + t.Length + 1 + a.Length];
        p[0] = (byte)((has ? Proto.FlagSession : 0) | (has && playing ? Proto.FlagPlaying : 0));
        BitConverter.TryWriteBytes(p.AsSpan(1), pos);
        BitConverter.TryWriteBytes(p.AsSpan(5), dur);
        BitConverter.TryWriteBytes(p.AsSpan(9), artId);
        int o = 13;
        p[o++] = (byte)t.Length; t.CopyTo(p, o); o += t.Length;
        p[o++] = (byte)a.Length; a.CopyTo(p, o);
        return p;
    }

    static byte[] Utf8Clip(string s, int max)
    {
        var b = Encoding.UTF8.GetBytes(s);
        if (b.Length <= max) return b;
        int cut = max;
        while (cut > 0 && (b[cut] & 0xC0) == 0x80) cut--; // stay on a character boundary
        return b[..cut];
    }

    /// Thumbnail -> 150x150 centre crop -> RGB565, high byte first (LV_COLOR_16_SWAP=1).
    static async Task<byte[]?> LoadArtAsync(IRandomAccessStreamReference thumb)
    {
        try
        {
            using var stream = await thumb.OpenReadAsync();
            if (stream.Size == 0) return null;
            var dec = await BitmapDecoder.CreateAsync(stream);
            uint w = dec.PixelWidth, h = dec.PixelHeight;
            if (w == 0 || h == 0) return null;

            double scale = (double)Proto.ArtW / Math.Min(w, h);
            uint sw = Math.Max((uint)Proto.ArtW, (uint)Math.Round(w * scale));
            uint sh = Math.Max((uint)Proto.ArtH, (uint)Math.Round(h * scale));
            var transform = new BitmapTransform
            {
                ScaledWidth = sw,
                ScaledHeight = sh,
                InterpolationMode = BitmapInterpolationMode.Fant,
                Bounds = new BitmapBounds
                {
                    X = (sw - Proto.ArtW) / 2,
                    Y = (sh - Proto.ArtH) / 2,
                    Width = Proto.ArtW,
                    Height = Proto.ArtH,
                },
            };
            var data = await dec.GetPixelDataAsync(BitmapPixelFormat.Bgra8, BitmapAlphaMode.Ignore,
                transform, ExifOrientationMode.IgnoreExifOrientation, ColorManagementMode.DoNotColorManage);
            var px = data.DetachPixelData();
            if (px.Length < Proto.ArtW * Proto.ArtH * 4) return null;

            var rgb565 = new byte[Proto.ArtBytes];
            for (int i = 0, o = 0; i < Proto.ArtW * Proto.ArtH; i++, o += 2)
            {
                int b = px[4 * i], g = px[4 * i + 1], r = px[4 * i + 2];
                int v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
                rgb565[o] = (byte)(v >> 8);
                rgb565[o + 1] = (byte)v;
            }
            return rgb565;
        }
        catch (Exception e)
        {
            Log.Write($"artwork: {e.Message}");
            return null;
        }
    }
}

// ----------------------------------------------------------------- text --
/// Maps text onto the characters the board's fonts contain (Latin-1 plus a few
/// typographic marks). Accented letters outside that set lose their accent
/// (ą -> a); anything else becomes '?'.
static class TextFilter
{
    /// True when every character exists in the board's fonts (no image needed).
    public static bool AllSupported(string s) =>
        s.Normalize(NormalizationForm.FormC).EnumerateRunes()
         .All(r => Supported(r.Value) || Rune.IsWhiteSpace(r) || Rune.IsControl(r));

    static bool Supported(int c) =>
        (c >= 0x20 && c <= 0x7E) || (c >= 0xA0 && c <= 0xFF) ||
        c is 0x152 or 0x153 or 0x178 or 0x2013 or 0x2014 or 0x2018 or 0x2019 or 0x201A
          or 0x201C or 0x201D or 0x201E or 0x2022 or 0x2026 or 0x20AC;

    public static string ForDisplay(string s)
    {
        var sb = new StringBuilder(s.Length);
        foreach (var rune in s.Normalize(NormalizationForm.FormC).EnumerateRunes())
        {
            int c = rune.Value;
            if (Supported(c)) { sb.Append(rune.ToString()); continue; }
            if (Rune.IsWhiteSpace(rune) || Rune.IsControl(rune)) { sb.Append(' '); continue; }
            var decomposed = rune.ToString().Normalize(NormalizationForm.FormD);
            if (decomposed.Length > 1 && Supported(decomposed[0])) sb.Append(decomposed[0]);
            else sb.Append('?');
        }
        return sb.ToString().Trim();
    }
}
