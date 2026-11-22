#include "Reader.h"

#include <algorithm>
#include <cstring>

namespace dp
{
Reader::Reader(ByteSource& source) : m_source(source), m_length(source.Length()), m_block(kBlockSize) {}

void Reader::Seek(uint64_t position)
{
    if (position > m_length)
    {
        throw FormatError("seek past end of file");
    }
    m_pos = position;
}

void Reader::Skip(uint64_t count)
{
    if (count > m_length - m_pos)
    {
        throw FormatError("section extends past end of file");
    }
    m_pos += count;
}

void Reader::Read(void* destination, size_t count)
{
    if (count > m_length - m_pos)
    {
        throw FormatError("unexpected end of file");
    }
    auto* out = static_cast<uint8_t*>(destination);

    if (count >= kBlockSize)
    {
        size_t done = 0;
        while (done < count)
        {
            const size_t got = m_source.ReadAt(m_pos + done, out + done, count - done);
            if (got == 0)
            {
                throw FormatError("short read");
            }
            done += got;
        }
        m_pos += count;
        return;
    }

    while (count > 0)
    {
        if (m_blockStart == UINT64_MAX || m_pos < m_blockStart || m_pos >= m_blockStart + m_blockLength)
        {
            const size_t wanted = static_cast<size_t>(std::min<uint64_t>(kBlockSize, m_length - m_pos));
            const size_t got = m_source.ReadAt(m_pos, m_block.data(), wanted);
            if (got == 0)
            {
                throw FormatError("short read");
            }
            m_blockStart = m_pos;
            m_blockLength = got;
        }
        const size_t offset = static_cast<size_t>(m_pos - m_blockStart);
        const size_t chunk = std::min(count, m_blockLength - offset);
        std::memcpy(out, m_block.data() + offset, chunk);
        out += chunk;
        count -= chunk;
        m_pos += chunk;
    }
}

std::vector<uint8_t> Reader::Bytes(uint64_t count)
{
    if (count > m_length - m_pos)
    {
        throw FormatError("unexpected end of file");
    }
    std::vector<uint8_t> data(static_cast<size_t>(count));
    if (count > 0)
    {
        Read(data.data(), data.size());
    }
    return data;
}

uint8_t Reader::U8()
{
    uint8_t b = 0;
    Read(&b, 1);
    return b;
}

uint16_t Reader::U16()
{
    uint8_t b[2];
    Read(b, 2);
    return static_cast<uint16_t>((b[0] << 8) | b[1]);
}

uint32_t Reader::U32()
{
    uint8_t b[4];
    Read(b, 4);
    return (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | uint32_t(b[3]);
}

uint64_t Reader::U64()
{
    const uint64_t high = U32();
    return (high << 32) | U32();
}
} // namespace dp
