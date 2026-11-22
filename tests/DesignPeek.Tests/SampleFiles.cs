using System.Buffers.Binary;
using System.Text;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace DesignPeek.Tests;

/// <summary>Builds small, valid PSD/PSB files in memory, following Adobe's file format specification.</summary>
internal static class SampleFiles
{
    public const int DocWidth = 320;
    public const int DocHeight = 240;
    public const int ThumbWidth = 160;
    public const int ThumbHeight = 120;

    public sealed class Spec
    {
        public bool Psb { get; init; }
        public ushort Mode { get; init; } = 3; // RGB
        public int Channels { get; init; } = 3;
        public int Depth { get; init; } = 8;
        public int Compression { get; init; } // 0 raw, 1 RLE

        /// <summary>Channel values (0-255) for pixel x,y. Default: left half red, right half blue.</summary>
        public Func<int, int, byte[]> Pixel { get; init; } = (x, _) => x < DocWidth / 2 ? [255, 0, 0] : [0, 0, 255];

        public List<(ushort Id, byte[] Data)> Resources { get; init; } = [];
        public byte[]? LayerInfo { get; init; }
    }

    /// <summary>JPEG whose left half is red and right half is blue (stored swapped when <paramref name="storeAsBgr"/>).</summary>
    public static byte[] CreateTwoColorJpeg(int width, int height, bool storeAsBgr)
    {
        byte[] pixels = new byte[width * height * 4];
        for (int y = 0; y < height; y++)
        {
            for (int x = 0; x < width; x++)
            {
                bool red = x < width / 2;
                if (storeAsBgr)
                {
                    red = !red;
                }

                int i = (y * width + x) * 4;
                pixels[i + 0] = red ? (byte)0 : (byte)255;
                pixels[i + 2] = red ? (byte)255 : (byte)0;
                pixels[i + 3] = 255;
            }
        }

        var source = BitmapSource.Create(width, height, 96, 96, PixelFormats.Bgr32, null, pixels, width * 4);
        var encoder = new JpegBitmapEncoder { QualityLevel = 92 };
        encoder.Frames.Add(BitmapFrame.Create(source));
        using var stream = new MemoryStream();
        encoder.Save(stream);
        return stream.ToArray();
    }

    public static byte[] ThumbnailResource(byte[] jpeg)
    {
        int widthBytes = (ThumbWidth * 24 + 31) / 32 * 4;
        var data = new MemoryStream();
        U32(data, 1);
        U32(data, ThumbWidth);
        U32(data, ThumbHeight);
        U32(data, (uint)widthBytes);
        U32(data, (uint)(widthBytes * ThumbHeight));
        U32(data, (uint)jpeg.Length);
        U16(data, 24);
        U16(data, 1);
        data.Write(jpeg);
        return data.ToArray();
    }

    public static byte[] ResolutionResource(double dpi)
    {
        var data = new MemoryStream();
        uint fixedDpi = (uint)(dpi * 65536);
        U32(data, fixedDpi);
        U16(data, 1);
        U16(data, 1);
        U32(data, fixedDpi);
        U16(data, 1);
        U16(data, 1);
        return data.ToArray();
    }

    /// <summary>Resource 1057: version 1, hasRealMergedData, then (empty) writer/reader strings.</summary>
    public static byte[] VersionInfoResource(bool hasRealMergedData)
    {
        var data = new MemoryStream();
        U32(data, 1);
        data.WriteByte(hasRealMergedData ? (byte)1 : (byte)0);
        U32(data, 0);
        U32(data, 0);
        U32(data, 1);
        return data.ToArray();
    }

    public static byte[] BuildPsd(Spec spec)
    {
        var file = new MemoryStream();
        file.Write("8BPS"u8);
        U16(file, spec.Psb ? (ushort)2 : (ushort)1);
        file.Write(new byte[6]);
        U16(file, (ushort)spec.Channels);
        U32(file, DocHeight);
        U32(file, DocWidth);
        U16(file, (ushort)spec.Depth);
        U16(file, spec.Mode);
        U32(file, 0); // color mode data

        var resources = new MemoryStream();
        foreach (var (id, data) in spec.Resources)
        {
            resources.Write("8BIM"u8);
            U16(resources, id);
            resources.Write([0, 0]);
            U32(resources, (uint)data.Length);
            resources.Write(data);
            if (data.Length % 2 == 1)
            {
                resources.WriteByte(0);
            }
        }

        U32(file, (uint)resources.Length);
        file.Write(resources.ToArray());

        // Layer and mask information section
        byte[] layerInfo = spec.LayerInfo ?? [];
        long sectionLength = layerInfo.Length == 0 ? 0 : (spec.Psb ? 8 : 4) + layerInfo.Length + 4;
        if (spec.Psb)
        {
            U64(file, (ulong)sectionLength);
        }
        else
        {
            U32(file, (uint)sectionLength);
        }

        if (layerInfo.Length > 0)
        {
            if (spec.Psb)
            {
                U64(file, (ulong)layerInfo.Length);
            }
            else
            {
                U32(file, (uint)layerInfo.Length);
            }

            file.Write(layerInfo);
            U32(file, 0); // global layer mask info
        }

        WriteComposite(file, spec);
        return file.ToArray();
    }

    private static void WriteComposite(Stream file, Spec spec)
    {
        int bytesPerSample = spec.Depth / 8;
        int rowBytes = DocWidth * bytesPerSample;
        var rows = new List<byte[]>();
        for (int c = 0; c < spec.Channels; c++)
        {
            for (int y = 0; y < DocHeight; y++)
            {
                byte[] row = new byte[rowBytes];
                for (int x = 0; x < DocWidth; x++)
                {
                    byte value = spec.Pixel(x, y)[c];
                    row[x * bytesPerSample] = value; // 16-bit: high byte first
                }

                rows.Add(row);
            }
        }

        U16(file, (ushort)spec.Compression);
        if (spec.Compression == 0)
        {
            foreach (byte[] row in rows)
            {
                file.Write(row);
            }

            return;
        }

        List<byte[]> packed = rows.Select(PackBits).ToList();
        foreach (byte[] row in packed)
        {
            if (spec.Psb)
            {
                U32(file, (uint)row.Length);
            }
            else
            {
                U16(file, (ushort)row.Length);
            }
        }

        foreach (byte[] row in packed)
        {
            file.Write(row);
        }
    }

    public static byte[] PackBits(byte[] input)
    {
        var output = new MemoryStream();
        int i = 0;
        while (i < input.Length)
        {
            int run = 1;
            while (i + run < input.Length && run < 128 && input[i + run] == input[i])
            {
                run++;
            }

            if (run >= 3)
            {
                output.WriteByte((byte)(sbyte)(1 - run));
                output.WriteByte(input[i]);
                i += run;
                continue;
            }

            int start = i;
            int count = 0;
            while (i < input.Length && count < 128)
            {
                if (i + 2 < input.Length && input[i] == input[i + 1] && input[i] == input[i + 2])
                {
                    break;
                }

                i++;
                count++;
            }

            output.WriteByte((byte)(count - 1));
            output.Write(input, start, count);
        }

        return output.ToArray();
    }

    // ---------------- layers ----------------

    public sealed record Layer(string Name, string? UnicodeName = null, int SectionType = 0, string[]? Fonts = null);

    /// <summary>Layer info (count + records), layers listed bottom-to-top like Photoshop stores them.</summary>
    public static byte[] LayerInfo(params Layer[] layers)
    {
        var info = new MemoryStream();
        U16(info, (ushort)layers.Length);
        foreach (Layer layer in layers)
        {
            info.Write(new byte[16]); // bounds
            U16(info, 0);             // no channels
            info.Write("8BIMnorm"u8);
            info.Write([255, 0, 0, 0]);

            var extra = new MemoryStream();
            U32(extra, 0); // mask
            U32(extra, 0); // blending ranges
            byte[] ansi = Encoding.ASCII.GetBytes(layer.Name);
            extra.WriteByte((byte)ansi.Length);
            extra.Write(ansi);
            int nameTotal = 1 + ansi.Length;
            extra.Write(new byte[(4 - nameTotal % 4) % 4]);

            if (layer.UnicodeName is { } unicode)
            {
                var data = new MemoryStream();
                U32(data, (uint)unicode.Length);
                data.Write(Encoding.BigEndianUnicode.GetBytes(unicode));
                Block(extra, "luni", data.ToArray());
            }

            if (layer.SectionType != 0)
            {
                var data = new MemoryStream();
                U32(data, (uint)layer.SectionType);
                Block(extra, "lsct", data.ToArray());
            }

            if (layer.Fonts is { } fonts)
            {
                Block(extra, "TySh", TextEngineData(fonts));
            }

            U32(info, (uint)extra.Length);
            info.Write(extra.ToArray());
        }

        if (info.Length % 2 == 1)
        {
            info.WriteByte(0);
        }

        return info.ToArray();
    }

    private static byte[] TextEngineData(string[] fonts)
    {
        var text = new MemoryStream();
        text.Write(new byte[50]); // stand-in for version + transform
        text.Write("<< /ResourceDict << /FontSet [ "u8);
        foreach (string font in fonts)
        {
            text.Write(" << /Name ("u8);
            text.Write([0xFE, 0xFF]);
            text.Write(Encoding.BigEndianUnicode.GetBytes(font));
            text.Write(") /Type 1 >>"u8);
        }

        text.Write(" ] /StyleSheetSet [ << /Name ("u8);
        text.Write([0xFE, 0xFF]);
        text.Write(Encoding.BigEndianUnicode.GetBytes("Normal RGB"));
        text.Write(") >> ] >> >>"u8);
        return text.ToArray();
    }

    private static void Block(Stream stream, string key, byte[] data)
    {
        stream.Write("8BIM"u8);
        stream.Write(Encoding.ASCII.GetBytes(key));
        int padded = (data.Length + 1) & ~1;
        U32(stream, (uint)padded);
        stream.Write(data);
        stream.Write(new byte[padded - data.Length]);
    }

    private static void U16(Stream s, ushort value)
    {
        Span<byte> b = stackalloc byte[2];
        BinaryPrimitives.WriteUInt16BigEndian(b, value);
        s.Write(b);
    }

    private static void U32(Stream s, uint value)
    {
        Span<byte> b = stackalloc byte[4];
        BinaryPrimitives.WriteUInt32BigEndian(b, value);
        s.Write(b);
    }

    private static void U64(Stream s, ulong value)
    {
        Span<byte> b = stackalloc byte[8];
        BinaryPrimitives.WriteUInt64BigEndian(b, value);
        s.Write(b);
    }
}
