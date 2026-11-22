using System.Buffers.Binary;
using DesignPeek.Core.Imaging;

namespace DesignPeek.Worker;

/// <summary>
/// Binary protocol with the native shim over stdin/stdout (all integers little-endian):
/// <list type="bullet">
/// <item>worker → shim  'R' int64 offset, int32 count   — read bytes from the file</item>
/// <item>shim → worker  int32 n, n bytes                 — n = -1 on error, fewer than count at end of file</item>
/// <item>worker → shim  'B' int32 w, int32 h, int32 alpha, w*h*4 BGRA bytes (top-down) — success</item>
/// <item>worker → shim  'E' int32 code                   — nothing to show / failed</item>
/// </list>
/// The shim never gives the worker a file path: the worker only sees bytes, which keeps Unicode paths,
/// permissions and Explorer's own stream handling out of the risky process.
/// </summary>
internal sealed class HostChannel(Stream fromHost, Stream toHost)
{
    public const int MaxReadRequest = 4 * 1024 * 1024;

    public int ReadAt(long offset, Span<byte> destination)
    {
        if (destination.Length > MaxReadRequest)
        {
            destination = destination[..MaxReadRequest];
        }

        Span<byte> request = stackalloc byte[13];
        request[0] = (byte)'R';
        BinaryPrimitives.WriteInt64LittleEndian(request[1..], offset);
        BinaryPrimitives.WriteInt32LittleEndian(request[9..], destination.Length);
        toHost.Write(request);
        toHost.Flush();

        Span<byte> header = stackalloc byte[4];
        fromHost.ReadExactly(header);
        int count = BinaryPrimitives.ReadInt32LittleEndian(header);
        if (count < 0 || count > destination.Length)
        {
            throw new IOException("Host could not read the file.");
        }

        fromHost.ReadExactly(destination[..count]);
        return count;
    }

    public void SendImage(RenderedImage image)
    {
        Span<byte> header = stackalloc byte[13];
        header[0] = (byte)'B';
        BinaryPrimitives.WriteInt32LittleEndian(header[1..], image.Width);
        BinaryPrimitives.WriteInt32LittleEndian(header[5..], image.Height);
        BinaryPrimitives.WriteInt32LittleEndian(header[9..], image.HasAlpha ? 1 : 0);
        toHost.Write(header);
        toHost.Write(image.Bgra);
        toHost.Flush();
    }

    public void SendError(int code)
    {
        Span<byte> message = stackalloc byte[5];
        message[0] = (byte)'E';
        BinaryPrimitives.WriteInt32LittleEndian(message[1..], code);
        toHost.Write(message);
        toHost.Flush();
    }
}
