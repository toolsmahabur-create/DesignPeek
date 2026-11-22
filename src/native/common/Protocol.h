#pragma once
#include <cstdint>

// Pipe protocol between a host (shim DLL or DesignPeek.exe) and DesignPeek.Worker.exe.
// All integers are little-endian.
//
//   worker -> host  'R' int64 offset, int32 count     read bytes of the file
//   host -> worker  int32 n, n bytes                   n = -1 on error, fewer than count at end of file
//   worker -> host  'I' int32 len, len bytes (UTF-8)   design info, "key\tvalue\n" lines (preview mode only)
//   worker -> host  'B' int32 w, int32 h, int32 alpha, w*h*4 bytes   top-down BGRA image -> done
//   worker -> host  'E' int32 code                     no image -> done
//
// The worker never receives a file path from Explorer: it only sees bytes it asks for.
namespace dp::protocol
{
constexpr uint8_t kRead = 'R';
constexpr uint8_t kInfo = 'I';
constexpr uint8_t kBitmap = 'B';
constexpr uint8_t kError = 'E';

constexpr int32_t kMaxReadRequest = 8 * 1024 * 1024;
constexpr int32_t kMaxInfoBytes = 1024 * 1024;
constexpr int kMaxImageDimension = 4096;
} // namespace dp::protocol
