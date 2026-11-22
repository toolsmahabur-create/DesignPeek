using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace DesignPeek.Core.Imaging;

/// <summary>Decodes compressed images (via Windows Imaging Component) straight to the requested size.</summary>
public static class ImageDecoder
{
    /// <summary>
    /// Decodes <paramref name="encoded"/> so that it fits inside a <paramref name="maxSize"/> square, keeping aspect ratio.
    /// Shrinking happens inside the decoder (DecodePixelWidth), so big images are never fully decoded.
    /// </summary>
    public static RenderedImage DecodeToFit(byte[] encoded, int maxSize, bool swapRedBlue = false)
    {
        ArgumentNullException.ThrowIfNull(encoded);
        ArgumentOutOfRangeException.ThrowIfLessThan(maxSize, 1);

        int sourceWidth, sourceHeight;
        using (var probeStream = new MemoryStream(encoded, writable: false))
        {
            var probe = BitmapDecoder.Create(probeStream, BitmapCreateOptions.DelayCreation, BitmapCacheOption.None);
            sourceWidth = probe.Frames[0].PixelWidth;
            sourceHeight = probe.Frames[0].PixelHeight;
        }

        (int targetWidth, int targetHeight) = FitInside(sourceWidth, sourceHeight, maxSize);

        BitmapSource source;
        if (targetWidth < sourceWidth || targetHeight < sourceHeight)
        {
            var image = new BitmapImage();
            image.BeginInit();
            image.StreamSource = new MemoryStream(encoded, writable: false);
            image.CacheOption = BitmapCacheOption.OnLoad;
            if (sourceWidth >= sourceHeight)
            {
                image.DecodePixelWidth = targetWidth;
            }
            else
            {
                image.DecodePixelHeight = targetHeight;
            }

            image.EndInit();
            source = image;
        }
        else
        {
            source = BitmapFrame.Create(new MemoryStream(encoded, writable: false), BitmapCreateOptions.None, BitmapCacheOption.OnLoad);
            if (targetWidth > sourceWidth)
            {
                // Small embedded thumbnails get enlarged so Explorer's tile is filled.
                source = new TransformedBitmap(source, new ScaleTransform(
                    (double)targetWidth / sourceWidth, (double)targetHeight / sourceHeight));
            }
        }

        var bgra = new FormatConvertedBitmap(source, PixelFormats.Bgra32, null, 0);
        int width = bgra.PixelWidth;
        int height = bgra.PixelHeight;
        int stride = width * 4;
        byte[] pixels = new byte[stride * height];
        bgra.CopyPixels(pixels, stride, 0);

        if (swapRedBlue)
        {
            for (int i = 0; i < pixels.Length; i += 4)
            {
                (pixels[i], pixels[i + 2]) = (pixels[i + 2], pixels[i]);
            }
        }

        return new RenderedImage(width, height, pixels, HasAlpha: false);
    }

    public static (int Width, int Height) FitInside(int width, int height, int maxSize)
    {
        double scale = Math.Min((double)maxSize / width, (double)maxSize / height);
        int w = Math.Clamp((int)Math.Round(width * scale), 1, maxSize);
        int h = Math.Clamp((int)Math.Round(height * scale), 1, maxSize);
        return (w, h);
    }
}
