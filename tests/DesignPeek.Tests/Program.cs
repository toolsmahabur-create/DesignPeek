using System.Diagnostics;
using DesignPeek.Core;
using DesignPeek.Core.Imaging;
using DesignPeek.Core.Psd;
using DesignPeek.Tests;

// usage: DesignPeek.Tests [samples-output-folder]
string samples = args.Length > 0 ? args[0] : Path.Combine(AppContext.BaseDirectory, "samples");
Directory.CreateDirectory(samples);

// ---------- build sample files ----------
byte[] jpeg = SampleFiles.CreateTwoColorJpeg(SampleFiles.ThumbWidth, SampleFiles.ThumbHeight, storeAsBgr: false);
byte[] jpegBgr = SampleFiles.CreateTwoColorJpeg(SampleFiles.ThumbWidth, SampleFiles.ThumbHeight, storeAsBgr: true);
byte[] thumb = SampleFiles.ThumbnailResource(jpeg, SampleFiles.ThumbWidth, SampleFiles.ThumbHeight);
byte[] thumbBgr = SampleFiles.ThumbnailResource(jpegBgr, SampleFiles.ThumbWidth, SampleFiles.ThumbHeight);
byte[] resolution = SampleFiles.ResolutionResource(300);

byte[] psd = SampleFiles.BuildPsd(psb: false, (1005, resolution), (1036, thumb));
byte[] psb = SampleFiles.BuildPsd(psb: true, (1005, resolution), (1036, thumb));
byte[] psdBgr = SampleFiles.BuildPsd(psb: false, (1033, thumbBgr));
byte[] psdNoThumb = SampleFiles.BuildPsd(psb: false, (1005, resolution));
byte[] corrupt = [.. "8BPS"u8, 0, 1, .. Enumerable.Range(0, 300).Select(i => (byte)(i * 37))];
byte[] truncated = psd[..100];

string Save(string name, byte[] bytes)
{
    string path = Path.Combine(samples, name);
    Directory.CreateDirectory(Path.GetDirectoryName(path)!);
    File.WriteAllBytes(path, bytes);
    return path;
}

Save("psd_with_thumb.psd", psd);
Save("psb_with_thumb.psb", psb);
Save("psd_bgr_1033.psd", psdBgr);
Save("psd_no_thumb.psd", psdNoThumb);
Save("zero.psd", []);
Save("corrupt.psd", corrupt);
Save("truncated.psd", truncated);
string unicodePath = Save(Path.Combine("unicode", "বাংলা ফোল্ডার", "ডিজাইন ফাইল.psd"), psd);

// A 1.5 GB PSB: proves we only read the beginning of huge files.
string largePath = Save("large.psb", psb);
using (var large = new FileStream(largePath, FileMode.Open, FileAccess.Write))
{
    large.SetLength(1536L * 1024 * 1024);
}

// ---------- tests ----------
int failures = 0;

void Check(string name, Func<bool> test)
{
    try
    {
        bool ok = test();
        Console.WriteLine($"{(ok ? "PASS" : "FAIL")}  {name}");
        if (!ok)
        {
            failures++;
        }
    }
    catch (Exception ex)
    {
        Console.WriteLine($"FAIL  {name}: {ex.GetType().Name}: {ex.Message}");
        failures++;
    }
}

RenderedImage? RenderFile(string path, int size)
{
    using var file = File.OpenRead(path);
    return ThumbnailRenderer.Render(file, size);
}

RenderedImage? RenderBytes(byte[] bytes, int size) => ThumbnailRenderer.Render(new MemoryStream(bytes), size);

bool LooksRed(RenderedImage image, int x, int y)
{
    int i = (y * image.Width + x) * 4;
    return image.Bgra[i + 2] > 180 && image.Bgra[i] < 80;
}

bool LooksBlue(RenderedImage image, int x, int y)
{
    int i = (y * image.Width + x) * 4;
    return image.Bgra[i] > 180 && image.Bgra[i + 2] < 80;
}

Check("PSD header is parsed", () =>
{
    PsdDocument doc = PsdReader.Read(new MemoryStream(psd));
    return !doc.IsPsb && doc.Width == SampleFiles.DocWidth && doc.Height == SampleFiles.DocHeight &&
           doc.Channels == 3 && doc.BitDepth == 8 && doc.ColorMode == PsdColorMode.Rgb &&
           doc.HorizontalDpi is > 299.9 and < 300.1 && doc.Thumbnail is { Width: 160, Height: 120, IsBgr: false };
});

Check("PSB header is parsed", () => PsdReader.Read(new MemoryStream(psb)) is { IsPsb: true, Thumbnail: not null });

Check("PSD thumbnail enlarged to 256 keeps aspect ratio and colours", () =>
{
    var image = RenderBytes(psd, 256)!;
    return image.Width == 256 && Math.Abs(image.Height - 192) <= 1 &&
           LooksRed(image, 20, image.Height / 2) && LooksBlue(image, image.Width - 20, image.Height / 2);
});

Check("PSD thumbnail shrunk to 96 while decoding", () =>
{
    var image = RenderBytes(psd, 96)!;
    return image.Width == 96 && Math.Abs(image.Height - 72) <= 1;
});

Check("PSB thumbnail renders", () => RenderBytes(psb, 256) is { Width: 256 });

Check("Photoshop 4 (BGR, resource 1033) colours are corrected", () =>
{
    var image = RenderBytes(psdBgr, 256)!;
    return LooksRed(image, 20, image.Height / 2) && LooksBlue(image, image.Width - 20, image.Height / 2);
});

Check("PSD without thumbnail returns nothing (no crash)", () => RenderBytes(psdNoThumb, 256) is null);
Check("Zero-byte file returns nothing", () => RenderBytes([], 256) is null);

Check("Corrupt file is rejected cleanly", () => RejectsCleanly(corrupt));
Check("Truncated file is rejected cleanly", () => RejectsCleanly(truncated));

Check("Bangla folder and file names work", () => RenderFile(unicodePath, 256) is { Width: 256 });

Check("1.5 GB PSB renders quickly without reading the whole file", () =>
{
    var watch = Stopwatch.StartNew();
    var image = RenderFile(largePath, 256);
    watch.Stop();
    Console.WriteLine($"      large.psb took {watch.ElapsedMilliseconds} ms");
    return image is { Width: 256 } && watch.ElapsedMilliseconds < 2000;
});

Console.WriteLine();
Console.WriteLine(failures == 0 ? "All tests passed." : $"{failures} test(s) failed.");
Console.WriteLine($"Samples written to {samples}");
return failures == 0 ? 0 : 1;

static bool RejectsCleanly(byte[] bytes)
{
    try
    {
        return ThumbnailRenderer.Render(new MemoryStream(bytes), 256) is null;
    }
    catch (Exception ex) when (ex is InvalidDataException or EndOfStreamException)
    {
        return true;
    }
}
