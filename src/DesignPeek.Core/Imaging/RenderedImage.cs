namespace DesignPeek.Core.Imaging;

/// <summary>A decoded image as top-down 32-bit BGRA pixels (stride = Width * 4).</summary>
public sealed record RenderedImage(int Width, int Height, byte[] Bgra, bool HasAlpha);
