using System.Diagnostics;
using System.Text;
using DesignPeek.Tests;
using static DesignPeek.Tests.SampleFiles;

// usage: DesignPeek.Tests <app-folder> <samples-output-folder>
//   app-folder contains DesignPeek.Worker.exe and DesignPeek.exe
if (args.Length < 2)
{
    Console.WriteLine("usage: DesignPeek.Tests <app-folder> <samples-folder>");
    return 2;
}

string appFolder = Path.GetFullPath(args[0]);
string samples = Path.GetFullPath(args[1]);
string worker = Path.Combine(appFolder, "DesignPeek.Worker.exe");
string app = Path.Combine(appFolder, "DesignPeek.exe");
string temp = Path.Combine(samples, "_out");
Directory.CreateDirectory(temp);

// ---------------- sample files ----------------
byte[] jpeg = CreateTwoColorJpeg(ThumbWidth, ThumbHeight, storeAsBgr: false);
byte[] jpegBgr = CreateTwoColorJpeg(ThumbWidth, ThumbHeight, storeAsBgr: true);
byte[] thumb = ThumbnailResource(jpeg);
byte[] thumbBgr = ThumbnailResource(jpegBgr);
byte[] resolution = ResolutionResource(300);
byte[] merged = VersionInfoResource(true);
byte[] notMerged = VersionInfoResource(false);

List<(ushort, byte[])> Standard() => [(1005, resolution), (1036, thumb), (1057, merged)];

byte[] psd = BuildPsd(new Spec { Resources = Standard() });
byte[] layered = BuildPsd(new Spec
{
    Resources = Standard(),
    LayerInfo = LayerInfo(
        new Layer("Background"),
        new Layer("</Layer group>", SectionType: 3),
        new Layer("Text 1", Fonts: ["ArialMT", "NotoSansBengali-Regular", "AdobeInvisFont"]),
        new Layer("Logo", UnicodeName: "লোগো"),
        new Layer("Header Group", SectionType: 1),
        new Layer("Top", UnicodeName: "উপরের লেয়ার")),
});

string Save(string name, byte[] bytes)
{
    string path = Path.Combine(samples, name);
    Directory.CreateDirectory(Path.GetDirectoryName(path)!);
    File.WriteAllBytes(path, bytes);
    return path;
}

Save("psd_with_thumb.psd", psd);
Save("psd_rle.psd", BuildPsd(new Spec { Resources = Standard(), Compression = 1 }));
Save("psb_with_thumb.psb", BuildPsd(new Spec { Psb = true, Resources = Standard() }));
Save("psb_rle.psb", BuildPsd(new Spec { Psb = true, Resources = Standard(), Compression = 1 }));
Save("psd_bgr_1033.psd", BuildPsd(new Spec { Resources = [(1033, thumbBgr), (1057, notMerged)] }));
Save("psd_no_thumb.psd", BuildPsd(new Spec { Resources = [(1005, resolution)] }));
Save("psd_no_merged.psd", BuildPsd(new Spec { Resources = [(1036, thumb), (1057, notMerged)] }));
Save("psd_cmyk.psd", BuildPsd(new Spec
{
    Mode = 4,
    Channels = 4,
    Resources = Standard(),
    // Stored inverted (255 = no ink). Red = M+Y ink, blue = C+M ink.
    Pixel = (x, _) => x < DocWidth / 2 ? [255, 0, 0, 255] : [0, 0, 255, 255],
}));
Save("psd_16bit.psd", BuildPsd(new Spec { Depth = 16, Resources = Standard() }));
Save("psd_gray.psd", BuildPsd(new Spec
{
    Mode = 1,
    Channels = 1,
    Resources = Standard(),
    Pixel = (x, _) => x < DocWidth / 2 ? [30] : [220],
}));
Save("psd_layers.psd", layered);
Save("zero.psd", []);
Save("corrupt.psd", [.. "8BPS"u8, 0, 1, .. Enumerable.Range(0, 300).Select(i => (byte)(i * 37))]);
Save("truncated.psd", psd[..100]);
string unicodePath = Save(Path.Combine("unicode", "বাংলা ফোল্ডার", "ডিজাইন ফাইল.psd"), psd);
string largePath = Save("large.psb", BuildPsd(new Spec { Psb = true, Resources = Standard() }));
using (var large = new FileStream(largePath, FileMode.Open, FileAccess.Write))
{
    large.SetLength(1536L * 1024 * 1024); // 1.5 GB: proves we never read whole files
}

// ---------------- helpers ----------------
int failures = 0;

void Check(string name, Func<bool> test)
{
    bool ok;
    try
    {
        ok = test();
    }
    catch (Exception ex)
    {
        Console.WriteLine($"      {ex.GetType().Name}: {ex.Message}");
        ok = false;
    }

    Console.WriteLine($"{(ok ? "PASS" : "FAIL")}  {name}");
    if (!ok)
    {
        failures++;
    }
}

(int Exit, string Output, long Ms) Run(string exe, params string[] arguments)
{
    var start = new ProcessStartInfo(exe)
    {
        RedirectStandardOutput = true,
        StandardOutputEncoding = Encoding.UTF8,
        UseShellExecute = false,
        CreateNoWindow = true,
    };
    foreach (string argument in arguments)
    {
        start.ArgumentList.Add(argument);
    }

    var watch = Stopwatch.StartNew();
    using var process = Process.Start(start)!;
    string output = process.StandardOutput.ReadToEnd();
    if (!process.WaitForExit(15000))
    {
        process.Kill();
        return (-1, output, watch.ElapsedMilliseconds);
    }

    return (process.ExitCode, output, watch.ElapsedMilliseconds);
}

(int Exit, string Output, long Ms, Bmp? Image) Render(string file, int size, string mode = "thumb")
{
    string bmp = Path.Combine(temp, Guid.NewGuid().ToString("N") + ".bmp");
    var (exit, output, ms) = Run(worker, "render-file", file, size.ToString(), bmp, mode);
    Console.WriteLine($"      {Path.GetFileName(file)} {mode} {size}: {output.Split('\n')[0].Trim()} ({ms} ms)");
    return (exit, output, ms, File.Exists(bmp) ? Bmp.Load(bmp) : null);
}

string S(string name) => Path.Combine(samples, name);

bool RedLeftBlueRight(Bmp? image) =>
    image is not null && image.IsRed(image.Width / 8, image.Height / 2) && image.IsBlue(image.Width * 7 / 8, image.Height / 2);

// ---------------- tests ----------------
Check("Small thumbnail uses the embedded JPEG (96px)", () =>
{
    var r = Render(S("psd_with_thumb.psd"), 96);
    return r.Output.StartsWith("OK 96x72") && RedLeftBlueRight(r.Image);
});

Check("Large thumbnail is rendered sharp from the composite (256px)", () =>
{
    var r = Render(S("psd_with_thumb.psd"), 256);
    return r.Output.StartsWith("OK 256x192") && RedLeftBlueRight(r.Image);
});

Check("RLE-compressed composite", () =>
{
    var r = Render(S("psd_rle.psd"), 1000, "preview");
    return r.Output.StartsWith("OK 320x240") && RedLeftBlueRight(r.Image); // never upscaled
});

Check("PSB raw + RLE composite", () =>
    Render(S("psb_with_thumb.psb"), 200).Output.StartsWith("OK 200x150") &&
    RedLeftBlueRight(Render(S("psb_rle.psb"), 200, "preview").Image));

Check("Photoshop 4 thumbnail (BGR) colours corrected", () =>
    RedLeftBlueRight(Render(S("psd_bgr_1033.psd"), 128).Image));

Check("No embedded thumbnail: composite still works", () =>
    RedLeftBlueRight(Render(S("psd_no_thumb.psd"), 256).Image));

Check("Saved without Maximize Compatibility: embedded preview + note", () =>
{
    var r = Render(S("psd_no_merged.psd"), 256, "preview");
    return r.Output.StartsWith("OK 256x192") && r.Output.Contains("note\tno-composite") && RedLeftBlueRight(r.Image);
});

Check("CMYK composite colours", () => RedLeftBlueRight(Render(S("psd_cmyk.psd"), 300, "preview").Image));
Check("16-bit composite colours", () => RedLeftBlueRight(Render(S("psd_16bit.psd"), 300, "preview").Image));

Check("Grayscale composite", () =>
{
    var image = Render(S("psd_gray.psd"), 300, "preview").Image;
    return image is not null && image.Gray(image.Width / 8, 10) is > 20 and < 40 &&
           image.Gray(image.Width * 7 / 8, 10) is > 210 and < 230;
});

Check("Design info: size, DPI, mode, layers, groups, Bangla names, fonts", () =>
{
    string info = Render(S("psd_layers.psd"), 300, "preview").Output;
    string[] lines = info.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
    string[] top = lines.Where(l => l.StartsWith("layer\t") || l.StartsWith("group\t")).ToArray();
    return lines.Contains("width\t320") && lines.Contains("height\t240") && lines.Contains("dpi\t300") &&
           lines.Contains("mode\tRGB") && lines.Contains("depth\t8") &&
           lines.Contains("layers\t5") && lines.Contains("groups\t1") && lines.Contains("textlayers\t1") &&
           top.SequenceEqual(["layer\tউপরের লেয়ার", "group\tHeader Group", "layer\tBackground"]) &&
           lines.Contains("font\tArialMT") && lines.Contains("font\tNotoSansBengali-Regular") &&
           !info.Contains("AdobeInvisFont") && !info.Contains("Normal RGB");
});

foreach (string bad in new[] { "zero.psd", "corrupt.psd", "truncated.psd" })
{
    Check($"Rejects {bad} quickly without crashing", () =>
    {
        var r = Render(S(bad), 256);
        return r.Exit == 1 && r.Output.StartsWith("FAIL") && r.Ms < 2000;
    });
}

Check("Bangla folder and file names", () => Render(unicodePath, 256).Output.StartsWith("OK 256x192"));

Check("1.5 GB PSB renders quickly", () =>
{
    var r = Render(largePath, 256);
    return r.Output.StartsWith("OK 256x192") && r.Ms < 2000;
});

Check("DesignPeek.exe renders a preview through the sandboxed worker", () =>
{
    string report = Path.Combine(temp, "app-report.txt");
    var (exit, _, ms) = Run(app, "--render-test", S("psd_layers.psd"), report);
    string text = File.Exists(report) ? File.ReadAllText(report).Trim() : "(no report)";
    Console.WriteLine($"      app: {text} ({ms} ms)");
    return exit == 0 && text == "OK 320x240 layers=5 fonts=2";
});

Console.WriteLine();
Console.WriteLine(failures == 0 ? "All tests passed." : $"{failures} test(s) failed.");
return failures == 0 ? 0 : 1;

internal sealed class Bmp
{
    private readonly byte[] _pixels;

    private Bmp(int width, int height, byte[] pixels)
    {
        Width = width;
        Height = height;
        _pixels = pixels;
    }

    public int Width { get; }
    public int Height { get; }

    public static Bmp Load(string path)
    {
        byte[] data = File.ReadAllBytes(path);
        int offset = BitConverter.ToInt32(data, 10);
        int width = BitConverter.ToInt32(data, 18);
        int height = Math.Abs(BitConverter.ToInt32(data, 22));
        return new Bmp(width, height, data[offset..]);
    }

    private (byte B, byte G, byte R) At(int x, int y)
    {
        int i = (y * Width + x) * 4;
        return (_pixels[i], _pixels[i + 1], _pixels[i + 2]);
    }

    public bool IsRed(int x, int y) => At(x, y) is { R: > 180, B: < 80, G: < 80 };
    public bool IsBlue(int x, int y) => At(x, y) is { B: > 180, R: < 80, G: < 80 };
    public int Gray(int x, int y) => At(x, y).G;
}
