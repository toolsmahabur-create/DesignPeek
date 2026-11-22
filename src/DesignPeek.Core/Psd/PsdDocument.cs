namespace DesignPeek.Core.Psd;

public enum PsdColorMode : ushort
{
    Bitmap = 0,
    Grayscale = 1,
    Indexed = 2,
    Rgb = 3,
    Cmyk = 4,
    Multichannel = 7,
    Duotone = 8,
    Lab = 9,
}

/// <summary>JPEG thumbnail that Photoshop stores inside the file (image resource 1036, or 1033 in very old files).</summary>
/// <param name="IsBgr">True for resource 1033 (Photoshop 4), whose JPEG has red and blue swapped.</param>
public sealed record PsdEmbeddedThumbnail(int Width, int Height, bool IsBgr, byte[] JpegData);

/// <summary>Everything we learn from the start of a PSD/PSB file without touching layer or pixel data.</summary>
public sealed class PsdDocument
{
    public required bool IsPsb { get; init; }
    public required int Channels { get; init; }
    public required int Width { get; init; }
    public required int Height { get; init; }
    public required int BitDepth { get; init; }
    public required PsdColorMode ColorMode { get; init; }

    public double? HorizontalDpi { get; internal set; }
    public double? VerticalDpi { get; internal set; }
    public PsdEmbeddedThumbnail? Thumbnail { get; internal set; }

    /// <summary>File offset where the "Layer and Mask Information" section starts (used by later steps).</summary>
    public long LayerSectionOffset { get; internal set; }
}
