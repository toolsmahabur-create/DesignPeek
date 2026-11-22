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
                pixels[i + 0] = red ? (byte)0 : (byte)255; // B
                pixels[i + 1] = 0;                          // G
                pixels[i + 2] = red ? (byte)255 : (byte)0;  // R
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

    public static byte[] ThumbnailResource(byte[] jpeg, int width, int height)
    {
        int widthBytes = (width * 24 + 31) / 32 * 4;
        var data = new MemoryStream();
        WriteUInt32(data, 1); // kJpegRGB
        WriteUInt32(data, (uint)width);
        WriteUInt32(data, (uint)height);
        WriteUInt32(data, (uint)widthBytes);
        WriteUInt32(data, (uint)(widthBytes * height));
        WriteUInt32(data, (uint)jpeg.Length);
        WriteUInt16(data, 24);
        WriteUInt16(data, 1);
        data.Write(jpeg);
        return data.ToArray();
    }

    public static byte[] ResolutionResource(double dpi)
    {
        var data = new MemoryStream();
        uint fixedDpi = (uint)(dpi * 65536);
        WriteUInt32(data, fixedDpi);
        WriteUInt16(data, 1); // pixels per inch
        WriteUInt16(data, 1);
        WriteUInt32(data, fixedDpi);
        WriteUInt16(data, 1);
        WriteUInt16(data, 1);
        return data.ToArray();
    }

    public static byte[] BuildPsd(bool psb, params (ushort Id, byte[] Data)[] resources)
    {
        const int channels = 3;
        var file = new MemoryStream();

        // Header
        file.Write(Encoding.ASCII.GetBytes("8BPS"));
        WriteUInt16(file, psb ? (ushort)2 : (ushort)1);
        file.Write(new byte[6]);
        WriteUInt16(file, channels);
        WriteUInt32(file, DocHeight);
        WriteUInt32(file, DocWidth);
        WriteUInt16(file, 8);
        WriteUInt16(file, 3); // RGB

        // Color mode data
        WriteUInt32(file, 0);

        // Image resources
        var section = new MemoryStream();
        foreach (var (id, data) in resources)
        {
            section.Write(Encoding.ASCII.GetBytes("8BIM"));
            WriteUInt16(section, id);
            section.Write([0, 0]); // empty Pascal name, padded to even
            WriteUInt32(section, (uint)data.Length);
            section.Write(data);
            if (data.Length % 2 == 1)
            {
                section.WriteByte(0);
            }
        }

        WriteUInt32(file, (uint)section.Length);
        section.Position = 0;
        section.CopyTo(file);

        // Layer and mask information: empty (8-byte length in PSB)
        if (psb)
        {
            WriteUInt32(file, 0);
            WriteUInt32(file, 0);
        }
        else
        {
            WriteUInt32(file, 0);
        }

        // Image data: raw, mid-grey
        WriteUInt16(file, 0);
        byte[] plane = new byte[DocWidth * DocHeight];
        Array.Fill(plane, (byte)128);
        for (int c = 0; c < channels; c++)
        {
            file.Write(plane);
        }

        return file.ToArray();
    }

    private static void WriteUInt16(Stream s, ushort value)
    {
        Span<byte> b = stackalloc byte[2];
        BinaryPrimitives.WriteUInt16BigEndian(b, value);
        s.Write(b);
    }

    private static void WriteUInt32(Stream s, uint value)
    {
        Span<byte> b = stackalloc byte[4];
        BinaryPrimitives.WriteUInt32BigEndian(b, value);
        s.Write(b);
    }
}
