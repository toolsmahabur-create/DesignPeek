#pragma once
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace dp
{
// Thrown for damaged, truncated or unreadable files. Always caught inside the worker.
struct FormatError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

class ByteSource
{
public:
    virtual ~ByteSource() = default;
    virtual uint64_t Length() const = 0;
    // Reads up to `count` bytes at `offset`. Returns bytes read (0 = end of file). Throws on I/O failure.
    virtual size_t ReadAt(uint64_t offset, void* buffer, size_t count) = 0;
};
} // namespace dp
