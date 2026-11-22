using System.Globalization;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using DesignPeek.Core;
using DesignPeek.Core.Imaging;

namespace DesignPeek.Worker;

internal static class Program
{
    private const int ExitOk = 0;
    private const int ExitFailed = 1;
    private const int ExitUsage = 2;

    private const int ErrorNothingToShow = 1;
    private const int ErrorDamagedFile = 2;

    [STAThread]
    private static int Main(string[] args)
    {
        try
        {
            return args switch
            {
                ["thumb", ..] => RunThumbnailForHost(args),
                ["thumb-file", var path, var size, var output] => RunThumbnailForFile(path, int.Parse(size, CultureInfo.InvariantCulture), output),
                _ => Usage(),
            };
        }
        catch (Exception ex)
        {
            WorkerLog.Error("Main", ex);
            return ExitFailed;
        }
    }

    /// <summary>Mode used by Explorer (through the shim): file bytes arrive over stdin/stdout.</summary>
    private static int RunThumbnailForHost(string[] args)
    {
        int size = int.Parse(GetOption(args, "--size"), CultureInfo.InvariantCulture);
        long length = long.Parse(GetOption(args, "--length"), CultureInfo.InvariantCulture);

        using Stream fromHost = Console.OpenStandardInput();
        using Stream toHost = Console.OpenStandardOutput();
        var channel = new HostChannel(fromHost, toHost);

        try
        {
            using var file = new HostStream(channel, length);
            RenderedImage? image = ThumbnailRenderer.Render(file, size);
            if (image is null)
            {
                channel.SendError(ErrorNothingToShow);
            }
            else
            {
                channel.SendImage(image);
            }

            return ExitOk;
        }
        catch (Exception ex)
        {
            WorkerLog.Error("thumb", ex);
            try
            {
                channel.SendError(ErrorDamagedFile);
            }
            catch (IOException)
            {
                // Host already gone.
            }

            return ExitFailed;
        }
    }

    /// <summary>Developer mode: DesignPeek.Worker.exe thumb-file &lt;input&gt; &lt;size&gt; &lt;output.png&gt;</summary>
    private static int RunThumbnailForFile(string path, int size, string outputPng)
    {
        using var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        RenderedImage? image = ThumbnailRenderer.Render(file, size);
        if (image is null)
        {
            Console.WriteLine("NO-THUMBNAIL");
            return ExitFailed;
        }

        var bitmap = BitmapSource.Create(image.Width, image.Height, 96, 96, PixelFormats.Bgra32, null, image.Bgra, image.Width * 4);
        var encoder = new PngBitmapEncoder();
        encoder.Frames.Add(BitmapFrame.Create(bitmap));
        using (var output = File.Create(outputPng))
        {
            encoder.Save(output);
        }

        Console.WriteLine($"OK {image.Width}x{image.Height}");
        return ExitOk;
    }

    private static string GetOption(string[] args, string name)
    {
        int index = Array.IndexOf(args, name);
        if (index < 0 || index + 1 >= args.Length)
        {
            throw new ArgumentException($"Missing {name}");
        }

        return args[index + 1];
    }

    private static int Usage()
    {
        Console.Error.WriteLine("DesignPeek worker. This program is started by Windows Explorer; you don't need to run it.");
        Console.Error.WriteLine("Developer use: DesignPeek.Worker.exe thumb-file <file> <size> <output.png>");
        return ExitUsage;
    }
}
