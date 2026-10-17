// Renders a title or artist line to a 4-bit alpha image for the board, for
// text its built-in fonts can't draw (Chinese, Japanese, Korean, Arabic,
// Hebrew, Thai, Cyrillic, emoji, ...). Uses WPF's text engine: Windows fonts,
// automatic font fallback, Arabic shaping and right-to-left layout.
// Format and sizes: MSG_TEXT_IMG / TEXT_IMG_* in firmware/include/protocol.h.
// SPDX-License-Identifier: MIT

using System.Collections.Concurrent;
using System.Globalization;
using System.Text;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace DeskDial;

static class TextImage
{
    public const int TitleEm = 22, TitleHeight = 30, TitleBaseline = 23, TitleMaxWidth = 1024;
    public const int ArtistEm = 16, ArtistHeight = 24, ArtistBaseline = 18, ArtistMaxWidth = 240;

    public readonly record struct Bitmap(int Width, int Height, byte[] Alpha4);

    // WPF objects need an STA thread: one long-lived worker runs every render.
    static readonly BlockingCollection<Action> Jobs = new();

    static TextImage()
    {
        var t = new Thread(() => { foreach (var job in Jobs.GetConsumingEnumerable()) job(); })
        {
            IsBackground = true,
            Name = "TextImage",
        };
        t.SetApartmentState(ApartmentState.STA);
        t.Start();
    }

    public static Task<Bitmap?> RenderAsync(string text, bool title)
    {
        var tcs = new TaskCompletionSource<Bitmap?>(TaskCreationOptions.RunContinuationsAsynchronously);
        Jobs.Add(() =>
        {
            try
            {
                tcs.SetResult(title
                    ? Render(text, TitleEm, TitleHeight, TitleBaseline, TitleMaxWidth)
                    : Render(text, ArtistEm, ArtistHeight, ArtistBaseline, ArtistMaxWidth));
            }
            catch (Exception e)
            {
                Log.Write($"text image: {e.Message}");
                tcs.SetResult(null);
            }
        });
        return tcs.Task;
    }

    static Bitmap? Render(string text, double em, int height, int baseline, int maxWidth)
    {
        text = Clean(text);
        if (text.Length == 0) return null;

        var (culture, fonts) = FontsFor(text);
        var typeface = new Typeface(new FontFamily(fonts), FontStyles.Normal, FontWeights.Medium, FontStretches.Normal);
        var ft = new FormattedText(text, culture, IsRightToLeft(text) ? FlowDirection.RightToLeft : FlowDirection.LeftToRight,
                                   typeface, em, Brushes.White, null, TextFormattingMode.Display, 1.0)
        {
            MaxTextWidth = maxWidth,
            MaxLineCount = 1,
            Trimming = TextTrimming.CharacterEllipsis,
        };

        // Put the baseline on the agreed row, then crop horizontally to what
        // was actually drawn (right-to-left text draws left of its origin).
        var origin = new Point(0, baseline - ft.Baseline);
        var probe = new DrawingVisual();
        using (var dc = probe.RenderOpen()) dc.DrawText(ft, origin);
        var bounds = probe.ContentBounds;
        if (bounds.IsEmpty || bounds.Width < 1) return null;
        int x0 = (int)Math.Floor(bounds.X);
        int width = Math.Clamp((int)Math.Ceiling(bounds.Right) - x0, 1, maxWidth);

        var visual = new DrawingVisual();
        using (var dc = visual.RenderOpen())
        {
            dc.PushTransform(new TranslateTransform(-x0, 0));
            dc.DrawText(ft, origin);
            dc.Pop();
        }
        var rtb = new RenderTargetBitmap(width, height, 96, 96, PixelFormats.Pbgra32);
        rtb.Render(visual);
        var px = new byte[width * height * 4];
        rtb.CopyPixels(px, width * 4, 0);

        // White text on transparent: alpha is the coverage. Pack to 4 bits,
        // rows of ceil(width/2) bytes, first pixel in the high nibble (LVGL).
        int stride = (width + 1) / 2;
        var a4 = new byte[stride * height];
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++)
            {
                int v = (px[(y * width + x) * 4 + 3] + 8) / 17; // 0..255 -> 0..15
                a4[y * stride + x / 2] |= (byte)((x & 1) == 0 ? v << 4 : v);
            }
        return new Bitmap(width, height, a4);
    }

    static string Clean(string s)
    {
        var sb = new StringBuilder(s.Length);
        foreach (var r in s.Normalize(NormalizationForm.FormC).EnumerateRunes())
            sb.Append(Rune.IsControl(r) ? " " : r.ToString());
        return sb.ToString().Trim();
    }

    static bool Any(string s, Func<int, bool> test) => s.EnumerateRunes().Any(r => test(r.Value));
    static bool IsKana(int c) => c is >= 0x3040 and <= 0x30FF or >= 0x31F0 and <= 0x31FF or >= 0xFF66 and <= 0xFF9F;
    static bool IsHangul(int c) => c is >= 0xAC00 and <= 0xD7AF or >= 0x1100 and <= 0x11FF or >= 0x3130 and <= 0x318F;
    static bool IsHan(int c) => c is >= 0x4E00 and <= 0x9FFF or >= 0x3400 and <= 0x4DBF or >= 0xF900 and <= 0xFAFF
                                    or >= 0x20000 and <= 0x2FFFF;
    static bool IsRtlLetter(int c) => c is >= 0x0590 and <= 0x08FF or >= 0xFB1D and <= 0xFDFF or >= 0xFE70 and <= 0xFEFF;

    /// Paragraph direction from the first letter, as in the Unicode bidi algorithm.
    static bool IsRightToLeft(string s)
    {
        foreach (var r in s.EnumerateRunes())
            if (Rune.IsLetter(r)) return IsRtlLetter(r.Value);
        return false;
    }

    /// Chinese and Japanese share characters drawn differently in each, so the
    /// first CJK font in the list follows the script: kana -> Japanese,
    /// Hangul -> Korean, other Han -> Chinese. Windows' fallback covers the rest.
    static (CultureInfo, string) FontsFor(string s)
    {
        const string Tail = "Segoe UI Symbol, Segoe UI Emoji";
        if (Any(s, IsKana))
            return (new CultureInfo("ja-JP"), $"Segoe UI, Yu Gothic UI, Meiryo UI, Microsoft YaHei UI, Malgun Gothic, {Tail}");
        if (Any(s, IsHangul))
            return (new CultureInfo("ko-KR"), $"Segoe UI, Malgun Gothic, Yu Gothic UI, Microsoft YaHei UI, {Tail}");
        if (Any(s, IsHan))
            return (new CultureInfo("zh-CN"), $"Segoe UI, Microsoft YaHei UI, Microsoft JhengHei UI, Yu Gothic UI, Malgun Gothic, {Tail}");
        return (CultureInfo.InvariantCulture,
                $"Segoe UI, Leelawadee UI, Nirmala UI, Ebrima, Gadugi, Segoe UI Historic, Microsoft YaHei UI, Yu Gothic UI, Malgun Gothic, {Tail}");
    }
}
