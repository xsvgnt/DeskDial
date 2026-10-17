// --trace-time: records the playback position as the player publishes it and
// as the companion sends it to the board, to analyse elapsed/total time issues.
//
// Each row is one of:
//   timeline-event / playback-event  the player told Windows something changed
//                                    (values read at that moment)
//   read                             the companion re-read the timeline
//   sent                             position/duration sent to the board
// Written to timeline-trace-<date>-<time>.csv on the Desktop; event and read
// rows are also printed to the console.
// SPDX-License-Identifier: MIT

using System.Globalization;
using System.IO;
using System.Text;
using Session = Windows.Media.Control.GlobalSystemMediaTransportControlsSession;

namespace DeskDial;

static class TimeTrace
{
    static StreamWriter? _file;
    static readonly object Lock = new();
    static readonly CultureInfo Inv = CultureInfo.InvariantCulture;

    public static bool Enabled => _file is not null;

    /// Opens the CSV file; returns its full path.
    public static string Start()
    {
        // Desktop: easy to find, and an admin terminal's current folder is System32
        var folder = Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
        if (string.IsNullOrEmpty(folder)) folder = Environment.CurrentDirectory;
        var path = Path.Combine(folder, $"timeline-trace-{DateTime.Now:yyyyMMdd-HHmmss}.csv");
        _file = new StreamWriter(path, false, new UTF8Encoding(false)) { AutoFlush = true };
        _file.WriteLine("time,kind,app,status,rate,position_s,start_s,end_s,min_seek_s,max_seek_s," +
                        "last_updated,last_updated_age_s,sent_position_s,sent_duration_s,sent_playing,sent_rate");
        return path;
    }

    /// What the player publishes right now, read straight from Windows.
    public static void Snapshot(string kind, Session s)
    {
        if (_file is null) return;
        string app = "", status = "", rate = "", pos = "", start = "", end = "", min = "", max = "", upd = "", age = "";
        try
        {
            app = s.SourceAppUserModelId;
            var pb = s.GetPlaybackInfo();
            status = pb?.PlaybackStatus.ToString() ?? "";
            rate = pb?.PlaybackRate?.ToString("0.###", Inv) ?? "";
            var tl = s.GetTimelineProperties();
            if (tl is not null)
            {
                pos = Sec(tl.Position);
                start = Sec(tl.StartTime);
                end = Sec(tl.EndTime);
                min = Sec(tl.MinSeekTime);
                max = Sec(tl.MaxSeekTime);
                upd = tl.LastUpdatedTime.ToLocalTime().ToString("HH:mm:ss.fff", Inv);
                age = (DateTimeOffset.Now - tl.LastUpdatedTime).TotalSeconds.ToString("0.000", Inv);
            }
        }
        catch (Exception e)
        {
            status = "error: " + e.Message;
        }
        Row(kind, app, status, rate, pos, start, end, min, max, upd, age, "", "", "", "");
        Log.Write($"trace {kind,-14} {status,-8} rate={rate,-4} pos={pos,8}s end={end,8}s " +
                  $"updated={upd} (age {age}s)");
    }

    /// What the companion sends to the board.
    public static void Sent(string app, uint posMs, uint durMs, bool playing, double rate)
    {
        if (_file is null) return;
        Row("sent", app, "", "", "", "", "", "", "", "", "",
            (posMs / 1000.0).ToString("0.000", Inv), (durMs / 1000.0).ToString("0.000", Inv), playing ? "1" : "0",
            rate.ToString("0.###", Inv));
    }

    static string Sec(TimeSpan t) => t.TotalSeconds.ToString("0.000", Inv);

    static void Row(params string[] fields)
    {
        var sb = new StringBuilder(DateTime.Now.ToString("HH:mm:ss.fff", Inv));
        foreach (var f in fields)
        {
            sb.Append(',');
            sb.Append(f.Contains(',') || f.Contains('"') ? "\"" + f.Replace("\"", "\"\"") + "\"" : f);
        }
        lock (Lock) _file?.WriteLine(sb.ToString());
    }
}
