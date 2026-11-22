using System.Buffers.Binary;

namespace DesignPeek.Core.IO;

/// <summary>Big-endian primitive readers. Photoshop files store all numbers big-endian.</summary>
internal static class BigEndianStreamExtensions
{
    public static ushort ReadUInt16BE(this Stream stream)
    {
        Span<byte> buffer = stackalloc byte[2];
        stream.ReadExactly(buffer);
        return BinaryPrimitives.ReadUInt16BigEndian(buffer);
    }

    public static uint ReadUInt32BE(this Stream stream)
    {
        Span<byte> buffer = stackalloc byte[4];
        stream.ReadExactly(buffer);
        return BinaryPrimitives.ReadUInt32BigEndian(buffer);
    }

    public static int ReadInt32BE(this Stream stream)
    {
        Span<byte> buffer = stackalloc byte[4];
        stream.ReadExactly(buffer);
        return BinaryPrimitives.ReadInt32BigEndian(buffer);
    }

    /// <summary>Moves forward by <paramref name="count"/> bytes, refusing to jump past the end of the stream.</summary>
    public static void Skip(this Stream stream, long count)
    {
        if (count < 0 || stream.Position + count > stream.Length)
        {
            throw new InvalidDataException("Section extends past the end of the file.");
        }

        stream.Seek(count, SeekOrigin.Current);
    }
}
