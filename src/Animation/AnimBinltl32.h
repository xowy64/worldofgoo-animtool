// AnimBinltl32.h: binary ".anim.binltl" reader/writer.
// See BinAnimation.h for the on-disk layout.

#ifndef ANIMBINLTL32_H
#define ANIMBINLTL32_H

#include <string>
#include "BinAnimation.h"

class AnimBinltl32 {
public:
    // Reads a 32-bit little endian anim.binltl file into 'out'.
    static bool Read(const std::string& path, Animation& out, std::string& err);

    // Writes 'anim' as a 32-bit little endian anim.binltl file, using the
    // same data layout as the game's own writer.
    static bool Write(const std::string& path, const Animation& anim, std::string& err);
};

#endif // ANIMBINLTL32_H
