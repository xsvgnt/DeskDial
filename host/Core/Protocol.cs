// DeskDial serial protocol, PC side. Mirrors firmware/include/protocol.h;
// described in docs/protocol.md.
// SPDX-License-Identifier: MIT

namespace DeskDial;

static class Proto
{
    public const byte Sync0 = 0xA5, Sync1 = 0x5A;
    public const byte Version = 1;
    public const int Baud = 921600; // ignored by native USB CDC

    // Message types
    public const byte Hello = 0x01, State = 0x02, Art = 0x03, ArtNone = 0x04, Hw = 0x05, HwTemps = 0x06,
                      TextImg = 0x07, Cmd = 0x10, Page = 0x11;

    // HELLO: 'D' 'D' version features flags
    public const byte FeatureMedia = 0x01, FeatureHardware = 0x02;
    public const byte HelloFlagReply = 0x01;

    public const byte PageMedia = 0, PageHw = 1;
    public const byte CmdPlayPause = 1, CmdNext = 2, CmdPrev = 3;
    public const byte FlagSession = 0x01, FlagPlaying = 0x02;
    public const int ArtW = 150, ArtH = 150, ArtBytes = ArtW * ArtH * 2;
    public const int MaxPayload = 4 + ArtBytes;

    /// The modules compiled into this program.
    public const byte Features = 0
#if MEDIA
        | FeatureMedia
#endif
#if HARDWARE
        | FeatureHardware
#endif
        ;

    public static byte[] HelloPayload(bool reply) =>
        new byte[] { (byte)'D', (byte)'D', Version, Features, reply ? HelloFlagReply : (byte)0 };

    /// Reads a HELLO from the board; false if it isn't a DeskDial board.
    public static bool ParseHello(byte[] p, out byte features, out bool reply)
    {
        features = 0;
        reply = false;
        if (p.Length < 4 || p[0] != 'D' || p[1] != 'D') return false;
        features = p[3];
        reply = p.Length >= 5 && (p[4] & HelloFlagReply) != 0;
        return true;
    }

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

/// Byte stream -> frames.
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
