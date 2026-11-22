namespace DesignPeek.Worker;

/// <summary>
/// Read-only, seekable view of the file that Explorer handed to the shim. Bytes are fetched on demand,
/// so a 2 GB PSB never has to be copied — only the parts the parser actually touches cross the pipe.
/// </summary>
internal sealed class HostStream(HostChannel channel, long length) : Stream
{
    private const int BlockSize = 64 * 1024;

    private readonly byte[] _block = new byte[BlockSize];
    private long _blockStart = -1;
    private int _blockLength;
    private long _position;

    public override bool CanRead => true;
    public override bool CanSeek => true;
    public override bool CanWrite => false;
    public override long Length => length;

    public override long Position
    {
        get => _position;
        set => _position = value >= 0 ? value : throw new ArgumentOutOfRangeException(nameof(value));
    }

    public override int Read(byte[] buffer, int offset, int count) => Read(buffer.AsSpan(offset, count));

    public override int Read(Span<byte> buffer)
    {
        if (buffer.IsEmpty || _position >= length)
        {
            return 0;
        }

        int wanted = (int)Math.Min(buffer.Length, length - _position);

        // Large reads (e.g. image data) go straight through without the cache.
        if (wanted >= BlockSize)
        {
            int total = 0;
            while (total < wanted)
            {
                int read = channel.ReadAt(_position, buffer.Slice(total, wanted - total));
                if (read == 0)
                {
                    break;
                }

                total += read;
                _position += read;
            }

            return total;
        }

        if (_blockStart < 0 || _position < _blockStart || _position >= _blockStart + _blockLength)
        {
            int blockLength = (int)Math.Min(BlockSize, length - _position);
            _blockLength = channel.ReadAt(_position, _block.AsSpan(0, blockLength));
            _blockStart = _position;
            if (_blockLength == 0)
            {
                return 0;
            }
        }

        int offsetInBlock = (int)(_position - _blockStart);
        int count = Math.Min(wanted, _blockLength - offsetInBlock);
        _block.AsSpan(offsetInBlock, count).CopyTo(buffer);
        _position += count;
        return count;
    }

    public override long Seek(long offset, SeekOrigin origin)
    {
        Position = origin switch
        {
            SeekOrigin.Begin => offset,
            SeekOrigin.Current => _position + offset,
            SeekOrigin.End => length + offset,
            _ => throw new ArgumentOutOfRangeException(nameof(origin)),
        };
        return _position;
    }

    public override void Flush()
    {
    }

    public override void SetLength(long value) => throw new NotSupportedException();

    public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();
}
