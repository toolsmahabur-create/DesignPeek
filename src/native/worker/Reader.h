#pragma once
#include <cstdint>
#include <vector>

#include "ByteSource.h"

namespace dp
{
// Big-endian, bounds-checked cursor over a ByteSource with a small read-ahead cache.
class Reader
{
public:
    explicit Reader(ByteSource& source);

    uint64_t Length() const { return m_length; }
    uint64_t Pos() const { return m_pos; }
    void Seek(uint64_t position);
    void Skip(uint64_t count);

    void Read(void* destination, size_t count);
    std::vector<uint8_t> Bytes(uint64_t count);

    uint8_t U8();
    uint16_t U16();
    uint32_t U32();
    uint64_t U64();
    int16_t I16() { return static_cast<int16_t>(U16()); }
    int32_t I32() { return static_cast<int32_t>(U32()); }

private:
    static constexpr size_t kBlockSize = 64 * 1024;

    ByteSource& m_source;
    uint64_t m_length;
    uint64_t m_pos = 0;
    std::vector<uint8_t> m_block;
    uint64_t m_blockStart = UINT64_MAX;
    size_t m_blockLength = 0;
};
} // namespace dp
