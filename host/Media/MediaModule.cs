// Media module: what Windows is playing (any app that uses the Windows media
// controls: Spotify, browsers, VLC...), sent to the board's media page, and
// play/pause/next/previous coming back.
//
// Windows is queried only for what an event says changed (plus a full re-read
// every 15 s as a safety net); the 1 s heartbeat re-sends from the cache.
// SPDX-License-Identifier: MIT

using System.Text;
using Windows.Graphics.Imaging;
using Windows.Media.Control;
using Windows.Storage.Streams;

using Session = Windows.Media.Control.GlobalSystemMediaTransportControlsSession;
using SessionManager = Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager;
using PlaybackStatus = Windows.Media.Control.GlobalSystemMediaTransportControlsSessionPlaybackStatus;

namespace DeskDial;

sealed class MediaModule : IModule
{
    readonly bool _spotifyOnly;
    Companion _companion = null!;

    SessionManager _mgr = null!;
    Session? _session;
    string _sessionId = "";
    volatile bool _sessionsChanged = true;

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
    double _rate = 1;                     // playback speed reported by the player
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

    public MediaModule(bool spotifyOnly) => _spotifyOnly = spotifyOnly;

    public byte Feature => Proto.FeatureMedia;

    void Wake() => _companion.Wake();

    public async Task StartAsync(Companion companion)
    {
        _companion = companion;
        _mgr = await SessionManager.RequestAsync();
        _mgr.SessionsChanged += (_, _) => { _sessionsChanged = true; _watchDirty = true; _selectDirty = true; Wake(); };
        _mgr.CurrentSessionChanged += (_, _) => { _selectDirty = true; Wake(); };
    }

    public async Task UpdateAsync(SerialLink link, bool full)
    {
        await RefreshAsync();
        await PushAsync(link, full);
    }

    public void OnFrame(byte type, byte[] payload)
    {
        if (type == Proto.Cmd && payload.Length == 1)
            _ = RunCommandAsync(payload[0]);
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

    // ------------------------------------------------------------ sessions --
    static bool IsSpotify(Session s) =>
        s.SourceAppUserModelId.Contains("Spotify", StringComparison.OrdinalIgnoreCase);

    static PlaybackStatus StatusOf(Session s)
    {
        try { return s.GetPlaybackInfo()?.PlaybackStatus ?? PlaybackStatus.Closed; }
        catch { return PlaybackStatus.Closed; }
    }

    /// Playback speed (1 = normal). Players report 0 while paused, and some
    /// report nothing: both count as 1, since the clock only runs while playing.
    static double RateOf(Session s)
    {
        try
        {
            var r = s.GetPlaybackInfo()?.PlaybackRate;
            return r is > 0.01 and < 16 ? r.Value : 1.0;
        }
        catch { return 1.0; }
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
                // Paused players report speed 0: keep the last real speed then
                var rate = status == PlaybackStatus.Playing ? RateOf(s) : _rate;
                // The position is projected from an anchor at the current speed.
                // When play/pause or the speed changes, re-anchor it at the
                // position reached so far, so each stretch is counted at its own
                // speed (a timeline published by the player still replaces it).
                if (status != _status || Math.Abs(rate - _rate) > 0.001)
                {
                    RebaseTimeline();
                    if (Math.Abs(rate - _rate) > 0.001) Log.Write($"playback speed: {rate:0.##}x");
                }
                _status = status;
                _rate = rate;
            }
            if (_timelineDirty)
            {
                _timelineDirty = false;
                // A new timeline can come with a new speed and no playback event
                if (_status == PlaybackStatus.Playing)
                {
                    var rate = RateOf(s);
                    if (Math.Abs(rate - _rate) > 0.001)
                    {
                        RebaseTimeline();
                        Log.Write($"playback speed: {rate:0.##}x");
                        _rate = rate;
                    }
                }
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
    async Task PushAsync(SerialLink link, bool full)
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
        TimeTrace.Sent(_sessionId, posMs, durMs, has && playing, _rate);
    }

    /// Position within the track from the cached timeline. Players report it
    /// only on play/pause/seek, so it is projected forward while playing, at
    /// the playback speed (1.5x: 1.5 s of media per second).
    TimeSpan ProjectedPosition()
    {
        var dur = _tlEnd - _tlStart;
        var pos = _tlPos - _tlStart;
        if (_status == PlaybackStatus.Playing && _tlUpdated.Year > 2000)
        {
            var age = DateTimeOffset.Now - _tlUpdated;
            if (age > TimeSpan.Zero && age < TimeSpan.FromHours(12))
                pos += TimeSpan.FromTicks((long)(age.Ticks * _rate));
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

    async Task SendTextImageAsync(SerialLink link, byte field, string text, bool full)
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
        var p = new byte[13 + 1 + t.Length + 1 + a.Length + 2];
        p[0] = (byte)((has ? Proto.FlagSession : 0) | (has && playing ? Proto.FlagPlaying : 0));
        BitConverter.TryWriteBytes(p.AsSpan(1), pos);
        BitConverter.TryWriteBytes(p.AsSpan(5), dur);
        BitConverter.TryWriteBytes(p.AsSpan(9), artId);
        int o = 13;
        p[o++] = (byte)t.Length; t.CopyTo(p, o); o += t.Length;
        p[o++] = (byte)a.Length; a.CopyTo(p, o); o += a.Length;
        // Playback speed x1000, so the board counts at the same speed between
        // updates (older firmware ignores these trailing bytes)
        BitConverter.TryWriteBytes(p.AsSpan(o, 2), (ushort)Math.Clamp(Math.Round(_rate * 1000), 1, 16000));
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
