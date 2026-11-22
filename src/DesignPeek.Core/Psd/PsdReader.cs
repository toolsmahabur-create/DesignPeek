using DesignPeek.Core.IO;

namespace DesignPeek.Core.Psd;

/// <summary>
/// Minimal, defensive PSD/PSB reader. It reads only the header and the image-resources section,
/// which sit at the very start of the file, so even a 2 GB PSB costs a few kilobytes of I/O.
/// Spec: "Adobe Photoshop File Formats Specification".
/// </summary>
public static class PsdReader
{
    private const ushort ResolutionInfoId = 1005;
    private const ushort ThumbnailPs4Id = 1033;   // BGR JPEG, Photoshop 4.0
    private const ushort ThumbnailId = 1036;      // RGB JPEG, Photoshop 5.0+
    private const int ThumbnailHeaderSize = 28;
    private const int MaxThumbnailBytes = 8 * 1024 * 1024;
    private const int MaxDimensionPsd = 30_000;
    private const int MaxDimensionPsb = 300_000;

    public static bool IsPsd(ReadOnlySpan<byte> header) =>
        header.Length >= 4 && header[0] == (byte)'8' && header[1] == (byte)'B' && header[2] == (byte)'P' && header[3] == (byte)'S';

    public static PsdDocument Read(Stream stream)
    {
        ArgumentNullException.ThrowIfNull(stream);
        if (!stream.CanSeek)
        {
            throw new ArgumentException("Stream must be seekable.", nameof(stream));
        }

        stream.Position = 0;
        Span<byte> signature = stackalloc byte[4];
        stream.ReadExactly(signature);
        if (!IsPsd(signature))
        {
            throw new InvalidDataException("Not a PSD/PSB file.");
        }

        ushort version = stream.ReadUInt16BE();
        if (version is not (1 or 2))
        {
            throw new InvalidDataException($"Unsupported PSD version {version}.");
        }

        bool isPsb = version == 2;
        stream.Skip(6); // reserved
        int channels = stream.ReadUInt16BE();
        uint height = stream.ReadUInt32BE();
        uint width = stream.ReadUInt32BE();
        int depth = stream.ReadUInt16BE();
        var mode = (PsdColorMode)stream.ReadUInt16BE();

        int maxDimension = isPsb ? MaxDimensionPsb : MaxDimensionPsd;
        if (channels is < 1 or > 56 || width is < 1 || height is < 1 || width > maxDimension || height > maxDimension ||
            depth is not (1 or 8 or 16 or 32))
        {
            throw new InvalidDataException("PSD header contains impossible values.");
        }

        var document = new PsdDocument
        {
            IsPsb = isPsb,
            Channels = channels,
            Width = (int)width,
            Height = (int)height,
            BitDepth = depth,
            ColorMode = mode,
        };

        uint colorModeDataLength = stream.ReadUInt32BE();
        stream.Skip(colorModeDataLength);

        uint resourcesLength = stream.ReadUInt32BE();
        long resourcesStart = stream.Position;
        long resourcesEnd = resourcesStart + resourcesLength;
        if (resourcesEnd > stream.Length)
        {
            throw new InvalidDataException("Image resources section is truncated.");
        }

        ReadImageResources(stream, resourcesStart, resourcesEnd, document);
        document.LayerSectionOffset = resourcesEnd;
        return document;
    }

    private static void ReadImageResources(Stream stream, long start, long end, PsdDocument document)
    {
        long position = start;
        Span<byte> signature = stackalloc byte[4];

        while (position + 12 <= end)
        {
            stream.Position = position;
            stream.ReadExactly(signature);
            if (!IsResourceSignature(signature))
            {
                break; // Unknown block: stop rather than guess.
            }

            ushort id = stream.ReadUInt16BE();

            // Pascal string name, padded so that length byte + text is even.
            int nameLength = stream.ReadByte();
            if (nameLength < 0)
            {
                throw new EndOfStreamException();
            }

            int namePadded = (1 + nameLength + 1) & ~1;
            stream.Skip(namePadded - 1);

            uint size = stream.ReadUInt32BE();
            long dataStart = stream.Position;
            long dataEnd = dataStart + size;
            if (dataEnd > end)
            {
                break; // Corrupt block; keep what we already found.
            }

            switch (id)
            {
                case ThumbnailId:
                    document.Thumbnail = ReadThumbnail(stream, size, isBgr: false) ?? document.Thumbnail;
                    break;
                case ThumbnailPs4Id when document.Thumbnail is null:
                    document.Thumbnail = ReadThumbnail(stream, size, isBgr: true);
                    break;
                case ResolutionInfoId when size >= 16:
                    ReadResolution(stream, document);
                    break;
            }

            position = dataEnd + (size & 1);
        }
    }

    private static PsdEmbeddedThumbnail? ReadThumbnail(Stream stream, uint size, bool isBgr)
    {
        if (size <= ThumbnailHeaderSize)
        {
            return null;
        }

        uint format = stream.ReadUInt32BE();
        int width = stream.ReadInt32BE();
        int height = stream.ReadInt32BE();
        stream.Skip(8); // widthbytes, total size
        uint compressedSize = stream.ReadUInt32BE();
        stream.Skip(4); // bits per pixel, planes

        if (format != 1 || width <= 0 || height <= 0)
        {
            return null; // format 0 (raw RGB) practically never occurs.
        }

        long jpegLength = Math.Min(compressedSize, size - ThumbnailHeaderSize);
        if (jpegLength <= 0 || jpegLength > MaxThumbnailBytes)
        {
            return null;
        }

        byte[] jpeg = new byte[jpegLength];
        stream.ReadExactly(jpeg);
        return new PsdEmbeddedThumbnail(width, height, isBgr, jpeg);
    }

    private static void ReadResolution(Stream stream, PsdDocument document)
    {
        // Fixed 16.16 horizontal res, unit, width unit, Fixed 16.16 vertical res, unit, height unit.
        double h = stream.ReadUInt32BE() / 65536.0;
        int hUnit = stream.ReadUInt16BE();
        stream.Skip(2);
        double v = stream.ReadUInt32BE() / 65536.0;
        int vUnit = stream.ReadUInt16BE();

        document.HorizontalDpi = ToDpi(h, hUnit);
        document.VerticalDpi = ToDpi(v, vUnit);
    }

    // Unit 1 = pixels per inch, 2 = pixels per cm.
    private static double? ToDpi(double value, int unit) => value <= 0 ? null : unit == 2 ? value * 2.54 : value;

    private static bool IsResourceSignature(ReadOnlySpan<byte> s) =>
        s.SequenceEqual("8BIM"u8) || s.SequenceEqual("MeSa"u8) || s.SequenceEqual("AgHg"u8) ||
        s.SequenceEqual("PHUT"u8) || s.SequenceEqual("DCSR"u8);
}
