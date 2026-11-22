using DesignPeek.Core.Imaging;
using DesignPeek.Core.Psd;

namespace DesignPeek.Core;

/// <summary>Picks the right fast path for a file and produces a thumbnail no larger than the requested size.</summary>
public static class ThumbnailRenderer
{
    /// <summary>Returns null when the file type is unknown or the file has nothing we can show.</summary>
    /// <exception cref="InvalidDataException">The file is damaged.</exception>
    /// <exception cref="EndOfStreamException">The file is truncated.</exception>
    public static RenderedImage? Render(Stream stream, int maxSize)
    {
        if (stream.Length < 4)
        {
            return null; // zero-byte or tiny file
        }

        Span<byte> header = stackalloc byte[4];
        stream.Position = 0;
        stream.ReadExactly(header);

        if (PsdReader.IsPsd(header))
        {
            return RenderPsd(stream, maxSize);
        }

        return null;
    }

    private static RenderedImage? RenderPsd(Stream stream, int maxSize)
    {
        PsdDocument document = PsdReader.Read(stream);
        if (document.Thumbnail is not { } thumbnail)
        {
            return null; // Later steps: fall back to the merged composite image.
        }

        return ImageDecoder.DecodeToFit(thumbnail.JpegData, maxSize, swapRedBlue: thumbnail.IsBgr);
    }
}
