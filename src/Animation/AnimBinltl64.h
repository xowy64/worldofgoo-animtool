// AnimBinltl64.h - ".anim.binltl64" and ".anim.binuni" reader/writer.
// See AnimBinltl64.cpp for the on-disk layouts.

#ifndef ANIMBINLTL64_H
#define ANIMBINLTL64_H

#include <string>
#include "BinAnimation.h"

// ".anim.binltl64": the binltl format with 64-bit offsets (80-byte header).
class AnimBinltl64 {
public:
    static bool Read (const std::string& path, Animation& out, std::string& err);
    static bool Write(const std::string& path, const Animation& anim, std::string& err);
};

// ".anim.binuni": binltl64 with an 8-byte "BINUNIAN" magic in front of the
// header (88-byte header).
class AnimBinuni {
public:
    static bool Read (const std::string& path, Animation& out, std::string& err);
    static bool Write(const std::string& path, const Animation& anim, std::string& err);
};

#endif // ANIMBINLTL64_H