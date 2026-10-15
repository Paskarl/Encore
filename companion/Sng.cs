// Reads Clone Hero's song checksum from a .sng file.
//
// Clone Hero identifies a song by the MD5 of its chart file (notes.mid or notes.chart). For a
// folder song that is the file itself; for a .sng it is the chart file packed inside. enchor.us's
// "md5" is a different value (it names the download), so the checksum for POST /queue has to be
// computed from the downloaded file.
//
// .sng layout (SNGPKG v1, all integers little-endian):
//   "SNGPKG", uint32 version, byte[16] xorMask,
//   uint64 metadataLength, uint64 count, count x (int32 len, key, int32 len, value),
//   uint64 fileMetaLength, uint64 count, count x (byte nameLen, name, uint64 size, uint64 offset),
//   uint64 fileDataLength, file data.
// File bytes are masked: byte i of a file is stored as b ^ xorMask[i % 16] ^ (i & 0xFF).
using System.Security.Cryptography;
using System.Text;

sealed class SngInfo
{
    public required string Checksum { get; init; } // upper-case hex, as Encore reports it
    public required Dictionary<string, string> Metadata { get; init; }
}

static class Sng
{
    public static SngInfo Read(string path)
    {
        using var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        using var r = new BinaryReader(fs, Encoding.UTF8);
        if (Encoding.ASCII.GetString(r.ReadBytes(6)) != "SNGPKG")
            throw new InvalidDataException("not a .sng file");
        uint version = r.ReadUInt32();
        if (version != 1)
            throw new InvalidDataException($"unsupported .sng version {version}");
        byte[] mask = r.ReadBytes(16);

        long metaEnd = fs.Position + 8 + checked((long)r.ReadUInt64());
        if (metaEnd > fs.Length)
            throw new InvalidDataException("metadata section lies outside the .sng");
        var metadata = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        ulong pairs = r.ReadUInt64();
        // Lengths are checked against the metadata section first: ReadBytes allocates the full
        // count up front, so a corrupt length could otherwise ask for gigabytes.
        string ReadString()
        {
            int len = r.ReadInt32();
            if (len < 0 || len > metaEnd - fs.Position)
                throw new InvalidDataException("bad metadata length");
            return Encoding.UTF8.GetString(r.ReadBytes(len));
        }
        for (ulong i = 0; i < pairs && fs.Position < metaEnd; ++i)
        {
            string key = ReadString();
            metadata[key] = ReadString();
        }
        fs.Position = metaEnd;

        r.ReadUInt64(); // fileMetaLength
        ulong files = r.ReadUInt64();
        if (files > 10000)
            throw new InvalidDataException("implausible file count");
        var entries = new Dictionary<string, (long size, long offset)>(StringComparer.OrdinalIgnoreCase);
        for (ulong i = 0; i < files; ++i)
        {
            string name = Encoding.UTF8.GetString(r.ReadBytes(r.ReadByte()));
            long size = checked((long)r.ReadUInt64());
            long offset = checked((long)r.ReadUInt64());
            entries[name] = (size, offset);
        }
        // Clone Hero prefers notes.mid when both exist.
        if (!entries.TryGetValue("notes.mid", out var chart) && !entries.TryGetValue("notes.chart", out chart))
            throw new InvalidDataException("no notes.mid or notes.chart in the .sng");
        if (chart.offset < 0 || chart.size < 0 || chart.offset + chart.size > fs.Length)
            throw new InvalidDataException("chart file lies outside the .sng");

        using var md5 = IncrementalHash.CreateHash(HashAlgorithmName.MD5);
        fs.Position = chart.offset;
        byte[] buf = new byte[81920];
        long done = 0;
        while (done < chart.size)
        {
            int n = fs.Read(buf, 0, (int)Math.Min(buf.Length, chart.size - done));
            if (n <= 0)
                throw new InvalidDataException("truncated .sng");
            for (int k = 0; k < n; ++k)
            {
                long j = done + k;
                buf[k] ^= (byte)(mask[j % 16] ^ (byte)(j & 0xFF));
            }
            md5.AppendData(buf, 0, n);
            done += n;
        }
        return new SngInfo { Checksum = Convert.ToHexString(md5.GetHashAndReset()), Metadata = metadata };
    }
}
