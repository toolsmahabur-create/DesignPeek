namespace DesignPeek.Worker;

/// <summary>
/// Best-effort error log in %USERPROFILE%\AppData\LocalLow\DesignPeek\logs (LocalLow because Explorer's
/// thumbnail host runs at low integrity and cannot write anywhere else). Never throws.
/// </summary>
internal static class WorkerLog
{
    private const long MaxLogBytes = 1024 * 1024;

    public static void Error(string context, Exception exception)
    {
        try
        {
            string local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            string directory = Path.Combine(Path.GetDirectoryName(local)!, "LocalLow", "DesignPeek", "logs");
            Directory.CreateDirectory(directory);
            string path = Path.Combine(directory, "worker.log");

            var info = new FileInfo(path);
            if (info.Exists && info.Length > MaxLogBytes)
            {
                File.Move(path, path + ".old", overwrite: true);
            }

            File.AppendAllText(path, $"{DateTime.Now:O} [{Environment.ProcessId}] {context}: {exception}{Environment.NewLine}");
        }
        catch
        {
            // Logging must never break thumbnail generation.
        }
    }
}
