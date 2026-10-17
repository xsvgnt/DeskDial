// What the board's own fonts can draw (firmware/src/fonts: Latin-1 plus a few
// typographic marks).
// SPDX-License-Identifier: MIT

using System.Text;

namespace DeskDial;

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

    /// Maps text onto those characters: accented letters outside the set lose
    /// their accent (ą -> a), anything else becomes '?'.
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
