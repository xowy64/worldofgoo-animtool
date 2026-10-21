//  MovieBinltl64.cpp - ".movie.binltl64" and ".movie.binuni" reader/writer.

//  BINLTL64: the same binary image as ".movie.binltl", except that every
//  pointer is 64 bits wide, which grows the BinMovie header from 20 to 32
//  bytes (length, numActors, then 3 x 8-byte offsets at 0x08/0x10/0x18).
//  BinActor contains no pointers, so it stays 32 bytes. Each ppAnims entry
//  points at a contiguous BinImageAnimation block in the ".anim.binltl64"
//  layout (80-byte header); its internal offsets are file offsets, with a
//  block-relative fallback in the reader, like MovieBinltl32.

//  BINUNI: the same as binltl64 with an 8-byte "BINUNIMO" magic in front
//  of the header. Offsets are stored relative to the start of the file,
//  exactly like in binltl64; the reader also accepts offsets relative to the
//  header (byte 8) as a fallback and notes when it has to. The writer's
//  behaviour can be switched with kMovieBinuniStructRelativeOffsets below.

//  v1.6 layout: newer .movie.binuni files keep everything
//  above unchanged but grow BinActor from 32 to 36
//  bytes, one extra int32 per actor at offset 0x20, read into
//  MovieActor::fixup. The layout is detected from the file itself (see
//  ProbeBinActorStride in BinMovie.h), never from the extension; the writer
//  only emits it when asked to (MovieBinuni::Write with fixupRemastered=true,
//  i.e. the --binuni -fix option).

//  Writer layout: header, BinActor array, ppAnims array, string table
//  (padded to 8), then one 8-aligned contiguous animation block per actor.

//  The little endian primitives and file IO helpers are deliberate copies
//  of the ones in AnimBinltl64.cpp / MovieBinltl32.cpp, so every converter
//  stays self-contained and the verified code does not have to be touched.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include "BinMovie.h"

static_assert(sizeof(float) == 4 && sizeof(int32_t) == 4 && sizeof(uint32_t) == 4 &&
                  sizeof(int64_t) == 8 && sizeof(uint64_t) == 8,
              "this converter assumes 32-bit ints/floats and 64-bit long longs");

namespace {

const size_t kMovieHeaderFields     = 32;  // float + int + 3 x 64-bit offsets
const size_t kActorSize             = 32;  // no pointers: same as 32-bit
const size_t kActorSizeRemastered   = 36;  // v1.6 layout: + one int32 (fixup)
const size_t kAnimHeaderFields      = 80;  // 6 x int32 + 7 x int64
const size_t kKeyframeSize          = 32;
const size_t kTypeSize              = 4;   // enum TransformType stays 4 bytes

// little endian primitives

bool rdU32(const std::vector<uint8_t>& d, size_t off, uint32_t& v)
{
    if (off > d.size() || d.size() - off < 4) return false;
    v = uint32_t(d[off])             | (uint32_t(d[off + 1]) << 8) |
        (uint32_t(d[off + 2]) << 16) | (uint32_t(d[off + 3]) << 24);
    return true;
}

bool rdI32(const std::vector<uint8_t>& d, size_t off, int32_t& v)
{
    uint32_t u;
    if (!rdU32(d, off, u)) return false;
    std::memcpy(&v, &u, 4);
    return true;
}

bool rdF32(const std::vector<uint8_t>& d, size_t off, float& v)
{
    uint32_t u;
    if (!rdU32(d, off, u)) return false;
    std::memcpy(&v, &u, 4);
    return true;
}

bool rdU64(const std::vector<uint8_t>& d, size_t off, uint64_t& v)
{
    if (off > d.size() || d.size() - off < 8) return false;
    v = 0;
    for (int i = 7; i >= 0; --i)
        v = (v << 8) | uint64_t(d[off + size_t(i)]);
    return true;
}

void wrU32(std::vector<uint8_t>& d, uint32_t v)
{
    d.push_back(uint8_t(v));
    d.push_back(uint8_t(v >> 8));
    d.push_back(uint8_t(v >> 16));
    d.push_back(uint8_t(v >> 24));
}

void wrI32(std::vector<uint8_t>& d, int32_t v) { wrU32(d, uint32_t(v)); }

void wrF32(std::vector<uint8_t>& d, float v)
{
    uint32_t u;
    std::memcpy(&u, &v, 4);
    wrU32(d, u);
}

void wrU64(std::vector<uint8_t>& d, uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        d.push_back(uint8_t(v >> (8 * i)));
}

void patchU64(std::vector<uint8_t>& d, size_t pos, uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        d[pos + size_t(i)] = uint8_t(v >> (8 * i));
}

bool rdKeyframe(const std::vector<uint8_t>& d, size_t off, Keyframe& kf)
{
    return rdF32(d, off,      kf.x)              &&
           rdF32(d, off + 4,  kf.y)              &&
           rdF32(d, off + 8,  kf.angle)          &&
           rdI32(d, off + 12, kf.alpha)          &&
           rdI32(d, off + 16, kf.color)          &&
           rdI32(d, off + 20, kf.nextFrameIndex) &&
           rdI32(d, off + 24, kf.soundStrIdx)    &&
           rdI32(d, off + 28, kf.interpolation);
}

// writes a keyframe, converting the data model's soundStrIdx list index to
// the on-disk byte offset convention (see BinAnimation.h).
void wrKeyframe(std::vector<uint8_t>& d, const Keyframe& kf,
                const std::vector<std::string>& strings)
{
    wrF32(d, kf.x);
    wrF32(d, kf.y);
    wrF32(d, kf.angle);
    wrI32(d, kf.alpha);
    wrI32(d, kf.color);
    wrI32(d, kf.nextFrameIndex);
    wrI32(d, SoundIndexToOffset(kf.soundStrIdx, strings));
    wrI32(d, kf.interpolation);
}

// whole file io

bool readFile(const std::string& path, std::vector<uint8_t>& data, std::string& err)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { err = "cannot open '" + path + "' for reading"; return false; }
    if (std::fseek(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        err = "seek failed on '" + path + "'";
        return false;
    }
    const long size = std::ftell(f);
    if (size < 0) {
        std::fclose(f);
        err = "cannot determine size of '" + path + "'";
        return false;
    }
    std::rewind(f);
    data.resize(size_t(size));
    if (size > 0 && std::fread(data.data(), 1, size_t(size), f) != size_t(size)) {
        std::fclose(f);
        data.clear();
        err = "read error on '" + path + "'";
        return false;
    }
    std::fclose(f);
    return true;
}

bool writeFile(const std::string& path, const std::vector<uint8_t>& data, std::string& err)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { err = "cannot open '" + path + "' for writing"; return false; }
    const bool ok = data.empty() || std::fwrite(data.data(), 1, data.size(), f) == data.size();
    std::fclose(f);
    if (!ok) err = "write error on '" + path + "'";
    return ok;
}

std::vector<std::string> splitStringTable(const uint8_t* p, size_t n)
{
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < n) {
        size_t end = pos;
        while (end < n && p[end] != 0) ++end;
        out.push_back(std::string(reinterpret_cast<const char*>(p + pos), end - pos));
        pos = end + 1;
    }
    return out;
}

// embedded BinImageAnimation

// 'hdr' is the file offset of the hasColor field; every stored offset maps
// to 'stored + delta' (0 for file relative, hdr for block relative) and all
// reads are bounded by 'limit' (the end of the block).
bool ParseAnim64(const std::vector<uint8_t>& d, size_t hdr, uint64_t delta,
                 size_t limit, Animation& out, std::string& err)
{
    if (limit > d.size()) limit = d.size();
    const size_t headerEnd = hdr + kAnimHeaderFields;
    if (hdr > d.size() || limit < headerEnd) {
        err = "animation block too small for the header";
        return false;
    }

    Animation a;
    a.offsets.base = hdr;    // callers adjust when a magic precedes the header
    if (!rdI32(d, hdr + 0x00, a.hasColor)     ||
        !rdI32(d, hdr + 0x04, a.hasAlpha)     ||
        !rdI32(d, hdr + 0x08, a.hasSound)     ||
        !rdI32(d, hdr + 0x0C, a.hasTransform) ||
        !rdI32(d, hdr + 0x10, a.numTransforms)||
        !rdI32(d, hdr + 0x14, a.numFrames)) {
        err = "truncated header";
        return false;
    }

    uint64_t typesOff = 0, timesOff = 0, xformOff = 0;
    uint64_t alphaOff = 0, colorOff = 0, soundOff = 0, strOff = 0;
    if (!rdU64(d, hdr + 0x18, typesOff) || !rdU64(d, hdr + 0x20, timesOff) ||
        !rdU64(d, hdr + 0x28, xformOff) || !rdU64(d, hdr + 0x30, alphaOff) ||
        !rdU64(d, hdr + 0x38, colorOff) || !rdU64(d, hdr + 0x40, soundOff) ||
        !rdU64(d, hdr + 0x48, strOff)) {
        err = "truncated header";
        return false;
    }

    if (a.numTransforms < 0 || a.numFrames < 0) {
        err = "negative numTransforms/numFrames";
        return false;
    }
    {
        const int64_t room = (int64_t(limit) - int64_t(headerEnd)) / 4;
        if (a.numTransforms > room || a.numFrames > room) {
            err = "numTransforms/numFrames too large for the animation block";
            return false;
        }
    }

    auto deref = [&](uint64_t stored, size_t& pos) -> bool {
        if (stored == 0) return false;
        const uint64_t p = stored + delta;
        if (p < headerEnd || p >= limit) return false;
        pos = size_t(p);
        return true;
    };

    std::vector<size_t> structOffs;

    // transform types
    {
        size_t pos = 0;
        if (deref(typesOff, pos)) {
            structOffs.push_back(pos);
            a.offsets.transformTypes = pos;
            if (pos + kTypeSize * size_t(a.numTransforms) > limit) {
                err = "truncated mTransformTypes array";
                return false;
            }
            for (int t = 0; t < a.numTransforms; ++t) {
                int32_t v;
                if (!rdI32(d, pos + kTypeSize * size_t(t), v)) {
                    err = "truncated mTransformTypes array";
                    return false;
                }
                if (v < XFORM_SCALE || v > XFORM_TRANSLATE) {
                    err = "unknown transform type " + std::to_string(v) +
                          " at index " + std::to_string(t);
                    return false;
                }
                a.transformTypes.push_back(TransformType(v));
            }
        } else if (typesOff != 0) {
            err = "mTransformTypes offset out of range";
            return false;
        } else if (a.numTransforms > 0) {
            err = "mTransformTypes is NULL but numTransforms = " +
                  std::to_string(a.numTransforms);
            return false;
        }
    }

    // frame times
    a.frameTimes.assign(size_t(a.numFrames), 0.0f);
    {
        size_t pos = 0;
        if (deref(timesOff, pos)) {
            structOffs.push_back(pos);
            a.offsets.frameTimes = pos;
            if (pos + 4 * size_t(a.numFrames) > limit) {
                err = "truncated mFrameTimes array";
                return false;
            }
            for (int f = 0; f < a.numFrames; ++f) {
                if (!rdF32(d, pos + 4 * size_t(f), a.frameTimes[size_t(f)])) {
                    err = "truncated mFrameTimes array";
                    return false;
                }
            }
        } else if (timesOff != 0) {
            err = "mFrameTimes offset out of range";
            return false;
        }
    }

    // one track: int64[numFrames] -> keyframes.  Also reports the file
    // position of the pointer array and of every keyframe in it.
    auto readTrack = [&](uint64_t arrayOff, const char* what,
                         std::vector<KeyframeSlot>& track,
                         std::vector<size_t>& kfOffs, size_t* arrayPosOut) -> bool {
        track.assign(size_t(a.numFrames), KeyframeSlot());
        kfOffs.assign(size_t(a.numFrames), 0);
        if (arrayOff == 0) return true;               // whole track absent
        size_t arrayPos = 0;
        if (!deref(arrayOff, arrayPos)) {
            err = std::string(what) + " pointer array offset out of range";
            return false;
        }
        if (arrayPosOut) *arrayPosOut = arrayPos;
        if (arrayPos + 8 * size_t(a.numFrames) > limit) {
            err = std::string("truncated ") + what + " pointer array";
            return false;
        }
        structOffs.push_back(arrayPos);
        for (int f = 0; f < a.numFrames; ++f) {
            uint64_t stored = 0;
            if (!rdU64(d, arrayPos + 8 * size_t(f), stored)) {
                err = std::string("truncated ") + what + " pointer array";
                return false;
            }
            if (stored == 0) continue;                // NULL keyframe for this frame
            size_t kfPos = 0;
            if (!deref(stored, kfPos) || kfPos + kKeyframeSize > limit) {
                err = std::string(what) + " keyframe offset out of range";
                return false;
            }
            structOffs.push_back(kfPos);
            kfOffs[size_t(f)] = kfPos;
            if (!rdKeyframe(d, kfPos, track[size_t(f)].kf)) {
                err = std::string(what) + " keyframe is truncated";
                return false;
            }
            track[size_t(f)].present = true;
        }
        return true;
    };

    // transform keyframes: mXformFrames[t] -> per-transform track
    a.xformFrames.assign(size_t(a.numTransforms) * size_t(a.numFrames), KeyframeSlot());
    a.offsets.xformKeyframes.assign(size_t(a.numTransforms) * size_t(a.numFrames), 0);
    if (xformOff != 0) {
        size_t xformPos = 0;
        if (!deref(xformOff, xformPos)) {
            err = "mXformFrames offset out of range";
            return false;
        }
        if (xformPos + 8 * size_t(a.numTransforms) > limit) {
            err = "truncated mXformFrames pointer array";
            return false;
        }
        structOffs.push_back(xformPos);
        a.offsets.xformFrames = xformPos;
        for (int t = 0; t < a.numTransforms; ++t) {
            uint64_t rowOff = 0;
            if (!rdU64(d, xformPos + 8 * size_t(t), rowOff)) {
                err = "truncated mXformFrames pointer array";
                return false;
            }
            if (rowOff == 0) continue;                // this transform has no keyframes
            std::vector<KeyframeSlot> row;
            std::vector<size_t> rowOffs;
            if (!readTrack(rowOff, "transform", row, rowOffs, nullptr)) return false;
            for (int f = 0; f < a.numFrames; ++f) {
                a.xformFrames[a.xformIndex(t, f)] = row[size_t(f)];
                a.offsets.xformKeyframes[a.xformIndex(t, f)] = rowOffs[size_t(f)];
            }
        }
    }

    if (!readTrack(alphaOff, "alpha", a.alphaFrames, a.offsets.alphaKeyframes, &a.offsets.alphaFrames)) return false;
    if (!readTrack(colorOff, "color", a.colorFrames, a.offsets.colorKeyframes, &a.offsets.colorFrames)) return false;
    if (!readTrack(soundOff, "sound", a.soundFrames, a.offsets.soundKeyframes, &a.offsets.soundFrames)) return false;

    // string table
    if (strOff != 0) {
        size_t strPos = 0;
        if (!deref(strOff, strPos)) {
            err = "pStringTable offset out of range";
            return false;
        }
        size_t end = limit;
        for (size_t i = 0; i < structOffs.size(); ++i)
            if (structOffs[i] > strPos && structOffs[i] < end)
                end = structOffs[i];
        a.offsets.stringTable = strPos;
        a.strings = splitStringTable(d.data() + strPos, end - strPos);
    }

    // keyframe soundStrIdx values are byte offsets into this table in every
    // binary format; the data model uses list indexes (see BinAnimation.h)
    SoundRefsOffsetsToIndexes(a);

    out = a;
    return true;
}

// <sounds> handling: see MovieBinltl32.cpp (same logic, 64-bit file).
bool AttachMovieSounds(const std::vector<MovieSound>& sounds, Animation& anim,
                       std::string& err)
{
    for (size_t i = 0; i < sounds.size(); ++i) {
        if (anim.numFrames <= 0) {
            anim.numFrames = 1;
            anim.frameTimes.assign(1, sounds[i].time);
            anim.xformFrames.assign(size_t(anim.numTransforms), KeyframeSlot());
            anim.alphaFrames.assign(1, KeyframeSlot());
            anim.colorFrames.assign(1, KeyframeSlot());
            anim.soundFrames.assign(1, KeyframeSlot());
        }
        int32_t best = 0;
        float bestDiff = -1.0f;
        for (int32_t f = 0; f < anim.numFrames; ++f) {
            float diff = anim.frameTimes[size_t(f)] - sounds[i].time;
            if (diff < 0.0f) diff = -diff;
            if (bestDiff < 0.0f || diff < bestDiff) { bestDiff = diff; best = f; }
        }
        if (anim.soundFrames[size_t(best)].present) {
            err = "two movie sounds map to frame " + std::to_string(best) +
                  " of the first actor's animation; give them distinct times";
            return false;
        }
        KeyframeSlot& s = anim.soundFrames[size_t(best)];
        s.present = true;
        s.kf.x = -1.0f; s.kf.y = -1.0f; s.kf.angle = -1.0f;    // the game's
        s.kf.alpha = -1; s.kf.color = -1;                      // unused-field
        s.kf.interpolation = INTERPOLATION_LINEAR;             // defaults
        s.kf.soundStrIdx = -1;
        for (size_t k = 0; k < anim.strings.size(); ++k)
            if (anim.strings[k] == sounds[i].id) { s.kf.soundStrIdx = int32_t(k); break; }
        if (s.kf.soundStrIdx < 0) {
            anim.strings.push_back(sounds[i].id);
            s.kf.soundStrIdx = int32_t(anim.strings.size() - 1);
        }
    }
    for (int32_t f = 0; f < anim.numFrames; ++f) {
        if (!anim.soundFrames[size_t(f)].present) continue;
        int32_t next = -1;
        for (int32_t g = f + 1; g < anim.numFrames; ++g)
            if (anim.soundFrames[size_t(g)].present) { next = g; break; }
        anim.soundFrames[size_t(f)].kf.nextFrameIndex = next;
    }
    return true;
}

// Appends one contiguous animation block, laid out exactly like a
// standalone animation file: every offset is relative to the start of the
// block.  withAnimMagic picks the type:
//   true  -> ".anim.binuni": 8-byte BINUNIAN magic, then the 80-byte
//             header; offsets are relative to the magic.  This is how the
//             game's own .movie.binuni files embed their animations.
//   false -> ".anim.binltl64": the 80-byte header only; offsets relative
//             to the block start.  This is how .movie.binltl64 files
//             embed them.
bool BuildAnimBlock64(const Animation& a, bool withAnimMagic,
                      std::vector<uint8_t>& o, std::string& err)
{
    const int64_t nT = a.numTransforms;
    const int64_t nF = a.numFrames;

    if (nT < 0 || nF < 0) {
        err = "negative numTransforms/numFrames in an animation block";
        return false;
    }
    if (int64_t(a.transformTypes.size()) != nT) {
        err = "transform type count does not match numTransforms";
        return false;
    }
    if (int64_t(a.frameTimes.size()) != nF) {
        err = "frame time count does not match numFrames";
        return false;
    }
    if (int64_t(a.xformFrames.size()) != nT * nF) {
        err = "transform keyframe slot count mismatch";
        return false;
    }
    if (int64_t(a.alphaFrames.size()) != nF ||
        int64_t(a.colorFrames.size()) != nF ||
        int64_t(a.soundFrames.size()) != nF) {
        err = "track keyframe slot count mismatch";
        return false;
    }

    // trailing empty entries are the source format's table padding (see
    // TrimmedAnimStrings) - drop them and let the 8-byte alignment below
    // restore whatever this format expects
    const std::vector<std::string> strings = TrimmedAnimStrings(a);

    const bool needTable = !strings.empty() ||
                           AnySoundReference(a.xformFrames) ||
                           AnySoundReference(a.alphaFrames) ||
                           AnySoundReference(a.colorFrames) ||
                           AnySoundReference(a.soundFrames);

    // the block starts here - at the magic when one is written - and every
    // stored offset is measured from this position
    const size_t blockBase = o.size();
    if (withAnimMagic)
        o.insert(o.end(), BINUNI_MAGIC, BINUNI_MAGIC + 8);

    // header (offset words are patched in at the end)
    wrI32(o, a.hasColor);
    wrI32(o, a.hasAlpha);
    wrI32(o, a.hasSound);
    wrI32(o, a.hasTransform);
    wrI32(o, a.numTransforms);
    wrI32(o, a.numFrames);
    const size_t hTypes = o.size(); wrU64(o, 0);   // 0x18 (+magic)
    const size_t hTimes = o.size(); wrU64(o, 0);   // 0x20
    const size_t hXform = o.size(); wrU64(o, 0);   // 0x28
    const size_t hAlpha = o.size(); wrU64(o, 0);   // 0x30
    const size_t hColor = o.size(); wrU64(o, 0);   // 0x38
    const size_t hSound = o.size(); wrU64(o, 0);   // 0x40
    const size_t hStr   = o.size(); wrU64(o, 0);   // 0x48

    // transform types
    uint64_t typesOff = 0;
    if (nT > 0) {
        typesOff = uint64_t(o.size() - blockBase);
        for (size_t i = 0; i < a.transformTypes.size(); ++i)
            wrI32(o, int32_t(a.transformTypes[i]));
    }

    // frame times
    uint64_t timesOff = 0;
    if (nF > 0) {
        timesOff = uint64_t(o.size() - blockBase);
        for (size_t i = 0; i < a.frameTimes.size(); ++i)
            wrF32(o, a.frameTimes[i]);
    }

    // mXformFrames: outer array, then one pointer array per transform
    uint64_t xformOff = 0;
    std::vector<size_t> outerPatch;
    std::vector<std::vector<size_t> > kfPatch;
    if (nT > 0) {
        xformOff = uint64_t(o.size() - blockBase);
        for (int64_t t = 0; t < nT; ++t) { outerPatch.push_back(o.size()); wrU64(o, 0); }
        kfPatch.resize(size_t(nT));
        for (int64_t t = 0; t < nT; ++t) {
            patchU64(o, outerPatch[size_t(t)], uint64_t(o.size() - blockBase));
            for (int64_t f = 0; f < nF; ++f) {
                kfPatch[size_t(t)].push_back(o.size());
                wrU64(o, 0);
            }
        }
    }

    // alpha / color / sound pointer arrays (always present, like game files)
    uint64_t alphaOff = 0, colorOff = 0, soundOff = 0;
    std::vector<size_t> alphaPatch, colorPatch, soundPatch;
    if (nF > 0) {
        alphaOff = uint64_t(o.size() - blockBase);
        for (int64_t f = 0; f < nF; ++f) { alphaPatch.push_back(o.size()); wrU64(o, 0); }
        colorOff = uint64_t(o.size() - blockBase);
        for (int64_t f = 0; f < nF; ++f) { colorPatch.push_back(o.size()); wrU64(o, 0); }
        soundOff = uint64_t(o.size() - blockBase);
        for (int64_t f = 0; f < nF; ++f) { soundPatch.push_back(o.size()); wrU64(o, 0); }
    }

    // string table (padded to 8 bytes, like a 64-bit memory image; the
    // block base is 8-aligned, so aligning the absolute position also
    // aligns the block-relative one)
    uint64_t strOff = 0;
    if (needTable) {
        strOff = uint64_t(o.size() - blockBase);
        if (strings.empty()) {
            o.push_back(0);                          // a single empty string
        } else {
            for (size_t i = 0; i < strings.size(); ++i) {
                o.insert(o.end(), strings[i].begin(), strings[i].end());
                o.push_back(0);
            }
        }
        while (o.size() % 8 != 0) o.push_back(0);
    }

    // keyframes (transforms first, then alpha, color, sound)
    for (int32_t t = 0; t < a.numTransforms; ++t) {
        for (int32_t f = 0; f < a.numFrames; ++f) {
            const KeyframeSlot& s = a.xformAt(t, f);
            if (!s.present) continue;
            patchU64(o, kfPatch[size_t(t)][size_t(f)], uint64_t(o.size() - blockBase));
            wrKeyframe(o, s.kf, strings);
        }
    }
    for (int32_t f = 0; f < a.numFrames; ++f) {
        const KeyframeSlot& s = a.alphaFrames[size_t(f)];
        if (s.present) { patchU64(o, alphaPatch[size_t(f)], uint64_t(o.size() - blockBase)); wrKeyframe(o, s.kf, strings); }
    }
    for (int32_t f = 0; f < a.numFrames; ++f) {
        const KeyframeSlot& s = a.colorFrames[size_t(f)];
        if (s.present) { patchU64(o, colorPatch[size_t(f)], uint64_t(o.size() - blockBase)); wrKeyframe(o, s.kf, strings); }
    }
    for (int32_t f = 0; f < a.numFrames; ++f) {
        const KeyframeSlot& s = a.soundFrames[size_t(f)];
        if (s.present) { patchU64(o, soundPatch[size_t(f)], uint64_t(o.size() - blockBase)); wrKeyframe(o, s.kf, strings); }
    }

    // patch the header offsets
    patchU64(o, hTypes, typesOff);
    patchU64(o, hTimes, timesOff);
    patchU64(o, hXform, xformOff);
    patchU64(o, hAlpha, alphaOff);
    patchU64(o, hColor, colorOff);
    patchU64(o, hSound, soundOff);
    patchU64(o, hStr,   strOff);
    return true;    // 64-bit offsets cannot realistically overflow
}

// whole movie

// 'hdr' is the file offset of the length field (0 for binltl64, 8 for
// binuni); 'delta' is added to every stored movie-level offset.
// 'actorStride' is the BinActor record size: 32 for the classic layout,
// 36 for the v1.6 one (which appends one int32 per actor, read into
// MovieActor::fixup).
bool ParseMovie64(const std::vector<uint8_t>& d, size_t hdr, uint64_t delta,
                  size_t actorStride, Movie& out, std::string& err)
{
    const size_t headerEnd = hdr + kMovieHeaderFields;
    if (d.size() < headerEnd) {
        err = "too small for a BinMovie header (" + std::to_string(d.size()) +
              " bytes, need " + std::to_string(headerEnd) + ")";
        return false;
    }
    if (actorStride != kActorSize && actorStride != kActorSizeRemastered) {
        err = "internal error: bad BinActor stride " + std::to_string(actorStride);
        return false;
    }

    Movie m;
    int32_t numActors = 0;
    uint64_t actorsOff = 0, animsOff = 0, strOff = 0;
    if (!rdF32(d, hdr + 0x00, m.length) ||
        !rdI32(d, hdr + 0x04, numActors) ||
        !rdU64(d, hdr + 0x08, actorsOff) ||
        !rdU64(d, hdr + 0x10, animsOff)  ||
        !rdU64(d, hdr + 0x18, strOff)) {
        err = "truncated BinMovie header";
        return false;
    }
    if (numActors < 0) {
        err = "negative numActors";
        return false;
    }
    if (size_t(numActors) > (d.size() - headerEnd) / actorStride) {
        err = "numActors too large for the file size";
        return false;
    }

    auto deref = [&](uint64_t stored, size_t& pos) -> bool {
        if (stored == 0) return false;
        const uint64_t p = stored + delta;
        if (p < headerEnd || p >= d.size()) return false;
        pos = size_t(p);
        return true;
    };

    std::vector<size_t> structOffs;
    size_t actorsPos = 0;
    if (numActors > 0) {
        if (!deref(actorsOff, actorsPos)) {
            err = "pActors offset out of range";
            return false;
        }
        if (actorsPos + actorStride * size_t(numActors) > d.size()) {
            err = "truncated BinActor array";
            return false;
        }
        structOffs.push_back(actorsPos);
    }

    // ppAnims
    std::vector<size_t> animBases(size_t(numActors), 0);
    size_t animsPos = 0;
    if (numActors > 0) {
        if (!deref(animsOff, animsPos)) {
            err = "ppAnims offset out of range";
            return false;
        }
        if (animsPos + 8 * size_t(numActors) > d.size()) {
            err = "truncated ppAnims array";
            return false;
        }
        structOffs.push_back(animsPos);
        for (int32_t i = 0; i < numActors; ++i) {
            uint64_t entry = 0;
            if (!rdU64(d, animsPos + 8 * size_t(i), entry)) {
                err = "truncated ppAnims array";
                return false;
            }
            if (entry == 0) {
                err = "actor " + std::to_string(i) +
                      " has a NULL animation pointer (BinMovie has exactly one"
                      " BinImageAnimation per BinActor)";
                return false;
            }
            size_t base = 0;
            if (!deref(entry, base)) {
                err = "actor " + std::to_string(i) + " animation offset out of range";
                return false;
            }
            animBases[size_t(i)] = base;
            structOffs.push_back(base);
        }
    }

    // string table (the raw bytes are kept: BinActor references are
    // resolved against them in several ways, see str() below)
    const uint8_t* rawTable = nullptr;
    size_t rawTableLen = 0;
    size_t strPos = 0;
    if (strOff != 0) {
        if (!deref(strOff, strPos)) {
            err = "pStringTable offset out of range";
            return false;
        }
        size_t end = d.size();
        for (size_t i = 0; i < structOffs.size(); ++i)
            if (structOffs[i] > strPos && structOffs[i] < end)
                end = structOffs[i];
        rawTable = d.data() + strPos;
        rawTableLen = end - strPos;
        m.offsets.stringTable = strPos;
        m.strings = splitStringTable(rawTable, rawTableLen);
    }

    // Resolves a BinActor string reference. Real files store byte offsets
    // into pStringTable, the game resolves them as pStringTable + idx
    // (verified against gootool, whose dumps of the original files this
    // format description is based on); unused fields reference the table's
    // leading empty string at byte offset 0. List-index interpretations
    // are kept as a fallback for hypothetical other writers, but they must
    // NOT run first: for byte-offset files they silently resolve early
    // references to unrelated table entries.
    auto str = [&](int32_t idx) -> std::string {
        if (idx < 0) return "";
        if (rawTable && size_t(idx) < rawTableLen) {   // byte offset
            size_t end = size_t(idx);
            while (end < rawTableLen && rawTable[end] != 0) ++end;
            return std::string(reinterpret_cast<const char*>(rawTable + idx),
                               end - size_t(idx));
        }
        if (size_t(idx) < m.strings.size() && !m.strings[size_t(idx)].empty())
            return m.strings[size_t(idx)];
        size_t seen = 0;                       // index counting only non-empties
        for (size_t i = 0; i < m.strings.size(); ++i) {
            if (m.strings[i].empty()) continue;
            if (seen == size_t(idx)) return m.strings[i];
            ++seen;
        }
        return "";
    };

    // actors
    m.actors.resize(size_t(numActors));
    for (int32_t i = 0; i < numActors; ++i) {
        const size_t base = actorsPos + actorStride * size_t(i);
        MovieActor& ac = m.actors[size_t(i)];
        int32_t imageIdx = 0, textIdx = 0, fontIdx = 0;
        if (!rdI32(d, base + 0x00, ac.type)              ||
            !rdI32(d, base + 0x04, imageIdx)              ||
            !rdI32(d, base + 0x08, textIdx)               ||
            !rdI32(d, base + 0x0C, fontIdx)               ||
            !rdF32(d, base + 0x10, ac.labelMaxWidth)     ||
            !rdF32(d, base + 0x14, ac.labelWrapWidth)     ||
            !rdI32(d, base + 0x18, ac.labelJustification)  ||
            !rdF32(d, base + 0x1C, ac.depth)) {
            err = "truncated BinActor " + std::to_string(i);
            return false;
        }
        // the v1.6 layout appends one int32 behind mDepth
        if (actorStride > 0x20 && !rdI32(d, base + 0x20, ac.fixup)) {
            err = "truncated BinActor " + std::to_string(i);
            return false;
        }
        if (ac.type != eActorType_Image && ac.type != eActorType_Text) {
            err = "actor " + std::to_string(i) + " has unknown type " +
                  std::to_string(ac.type);
            return false;
        }
        if (ac.labelJustification < ALIGN_LEFT || ac.labelJustification > ALIGN_RIGHT) {
            err = "actor " + std::to_string(i) + " has unknown justification " +
                  std::to_string(ac.labelJustification);
            return false;
        }
        ac.image     = str(imageIdx);
        ac.labelText = str(textIdx);
        ac.font      = str(fontIdx);
    }

    // animation blocks. 2D Boy stored each one as a standalone contiguous
    // image with offsets relative to the start of the block (that is what
    // the ppAnims indirection exists for): an anim.binuni image (BINUNIAN
    // magic, then the header, offsets relative to the magic) in
    // .movie.binuni files, an anim.binltl64 image (no magic) in
    // .movie.binltl64 files. Those interpretations are tried first;
    // file-relative offsets are the fallback.
    for (int32_t i = 0; i < numActors; ++i) {
        const size_t animBase = animBases[size_t(i)];
        size_t blockEnd = d.size();
        for (int32_t j = 0; j < numActors; ++j)
            if (animBases[size_t(j)] > animBase && animBases[size_t(j)] < blockEnd)
                blockEnd = animBases[size_t(j)];
        if (strPos > animBase && strPos < blockEnd)
            blockEnd = strPos;

        Animation& a = m.actors[size_t(i)].anim;
        // a standalone anim.binuni image starts with the BINUNIAN magic;
        // the 80-byte header follows it and every stored offset is
        // relative to the magic. The game's own .movie.binuni files embed
        // their animation blocks this way, so it is tried first.
        const bool animMagic = animBase + 8 <= blockEnd &&
                               std::memcmp(d.data() + animBase, BINUNI_MAGIC, 8) == 0;
        Animation tmp;
        std::string e;
        if (animMagic &&
            ParseAnim64(d, animBase + 8, uint64_t(animBase), blockEnd, tmp, e)) {
            a = tmp;                     // anim.binuni image (the game's binuni movies)
            a.offsets.base = animBase;   // the ppAnims entry points at the magic
        } else {
            Animation tmp2;
            std::string e2;
            if (ParseAnim64(d, animBase, uint64_t(animBase), blockEnd, tmp2, e2)) {
                a = tmp2;            // block-relative anim.binltl64 image
            } else {
                Animation tmp3;
                std::string e3;
                if (ParseAnim64(d, animBase, delta, blockEnd, tmp3, e3)) {
                    a = tmp3;        // file/header-relative
                    std::fprintf(stderr,
                                 "note: actor %d's animation block uses file-relative"
                                 " offsets, not block-relative ones\n", int(i));
                } else {
                    err = "actor " + std::to_string(i) +
                          ": the animation block is not a valid BinImageAnimation (" +
                          (animMagic ? e : e2) + ")";
                    return false;
                }
            }
        }
    }

    // where things live, for the verbose dump
    if (numActors > 0) {
        m.offsets.actors = actorsPos;
        m.offsets.anims  = size_t(animsPos);
    }
    m.offsets.animBlocks.assign(size_t(numActors), 0);
    for (int32_t i = 0; i < numActors; ++i)
        m.offsets.animBlocks[size_t(i)] = animBases[size_t(i)];
    m.readAsRemastered = (actorStride == kActorSizeRemastered);

    out = m;
    return true;
}

// If a movie.binuni file turns out to store offsets relative to the header
// (the first byte after the magic) instead of the start of the file, set
// this to true. The reader accepts both variants either way.
const bool kMovieBinuniStructRelativeOffsets = false;

bool BuildMovie64(const Movie& m, bool withMagic, bool fixupRemastered,
                  std::vector<uint8_t>& o, std::string& err)
{
    if (m.actors.size() > size_t(INT32_MAX)) {
        err = "too many actors";
        return false;
    }
    const int32_t numActors = int32_t(m.actors.size());
    const size_t actorStride = fixupRemastered ? kActorSizeRemastered : kActorSize;
    const uint64_t adj = (withMagic && kMovieBinuniStructRelativeOffsets) ? 8 : 0;

    // The string table: the movie's table with any trailing pad-empty
    // entries dropped (they are the source format's table padding, see
    // TrimmedMovieStrings, the alignment below restores this format's
    // own: 8 bytes for the classic layout, 4 for the v1.6 one), with
    // strings not in it appended in actor order (image, text, font).
    // Actor references are stored as byte offsets into the table.  Unused
    // fields reference the table's empty-string entry (appended on
    // demand) - the game's own files do it this way, not with -1.
    std::vector<std::string> strings = TrimmedMovieStrings(m.strings);
    std::vector<size_t> entryOff;
    size_t tableBytes = 0;
    entryOff.reserve(strings.size());
    for (size_t i = 0; i < strings.size(); ++i) {
        entryOff.push_back(tableBytes);
        tableBytes += strings[i].size() + 1;
    }
    auto internOffset = [&](const std::string& s) -> int32_t {
        for (size_t i = 0; i < strings.size(); ++i)
            if (strings[i] == s) return int32_t(entryOff[i]);
        entryOff.push_back(tableBytes);
        strings.push_back(s);
        tableBytes += s.size() + 1;
        return int32_t(entryOff.back());
    };

    // the per-actor animations (the first one gets the movie-level sounds)
    std::vector<Animation> anims;
    anims.reserve(m.actors.size());
    for (size_t i = 0; i < m.actors.size(); ++i)
        anims.push_back(m.actors[i].anim);
    if (!m.sounds.empty()) {
        if (anims.empty()) {
            err = "the movie has <sounds> but no actors to attach them to";
            return false;
        }
        if (!AttachMovieSounds(m.sounds, anims[0], err)) return false;
    }

    o.clear();
    o.reserve((withMagic ? 8 : 0) + kMovieHeaderFields +
              actorStride * size_t(numActors) + 8 * size_t(numActors) + 64);

    if (withMagic)
        o.insert(o.end(), BINUNIMO_MAGIC, BINUNIMO_MAGIC + 8);

    // header
    wrF32(o, m.length);
    wrI32(o, numActors);
    const size_t hActors = o.size(); wrU64(o, 0);   // 0x08 (+magic)
    const size_t hAnims  = o.size(); wrU64(o, 0);   // 0x10
    const size_t hStr    = o.size(); wrU64(o, 0);   // 0x18

    // BinActor array (36 bytes per actor in the v1.6 layout, with the extra
    // fixup int32 behind mDepth; 32 bytes otherwise)
    const uint64_t actorsOff = uint64_t(o.size()) - adj;
    for (size_t i = 0; i < m.actors.size(); ++i) {
        const MovieActor& ac = m.actors[i];
        wrI32(o, ac.type);
        wrI32(o, internOffset(ac.image));
        wrI32(o, internOffset(ac.labelText));
        wrI32(o, internOffset(ac.font));
        wrF32(o, ac.labelMaxWidth);
        wrF32(o, ac.labelWrapWidth);
        wrI32(o, ac.labelJustification);
        wrF32(o, ac.depth);
        if (fixupRemastered)
            wrI32(o, ac.fixup);
    }

    // ppAnims array (patched below)
    const uint64_t animsOff = (numActors > 0) ? uint64_t(o.size()) - adj : 0;
    std::vector<size_t> animPatch;
    animPatch.resize(size_t(numActors));    // numActors placeholder positions
    for (size_t i = 0; i < animPatch.size(); ++i) { animPatch[i] = o.size(); wrU64(o, 0); }

    // string table (the classic layout pads it to 8 bytes; the v1.6 layout
    // to 4 - both observed in the game's own files, where the padding NULs
    // parse back as trailing empty entries)
    uint64_t strOff = 0;
    if (!strings.empty()) {
        strOff = uint64_t(o.size()) - adj;
        for (size_t i = 0; i < strings.size(); ++i) {
            o.insert(o.end(), strings[i].begin(), strings[i].end());
            o.push_back(0);
        }
        if (fixupRemastered)
            while (o.size() % 4 != 0) o.push_back(0);
        else
            while (o.size() % 8 != 0) o.push_back(0);
    }

    // one 8-aligned contiguous animation block per actor: a standalone
    // anim.binuni image (BINUNIAN magic) in binuni files, a standalone
    // anim.binltl64 image in binltl64 files. Offsets are block-relative
    // either way (no adj); the ppAnims entry points at the block start,
    // i.e. at the magic.
    for (size_t i = 0; i < anims.size(); ++i) {
        while (o.size() % 8 != 0) o.push_back(0);
        patchU64(o, animPatch[i], uint64_t(o.size()) - adj);
        if (!BuildAnimBlock64(anims[i], withMagic, o, err)) return false;
    }

    // patch the header offsets
    patchU64(o, hActors, actorsOff);
    patchU64(o, hAnims,  animsOff);
    patchU64(o, hStr,    strOff);
    return true;
}

} // namespace

// MovieBinltl64 (.movie.binltl64)

bool MovieBinltl64::Read(const std::string& path, Movie& out, std::string& err)
{
    std::vector<uint8_t> d;
    if (!readFile(path, d, err)) return false;

    if (d.size() >= 8 && std::memcmp(d.data(), BINUNIMO_MAGIC, 8) == 0) {
        err = "'" + path + "': this is a movie.binuni file (BINUNIMO magic)";
        return false;
    }

    // The BinActor stride (32 = classic, 36 = v1.6) is detected from the
    // file, never from the extension; the header's own pointers usually
    // identify it (see ProbeBinActorStride).
    const size_t probed = ProbeBinActorStride(d.data(), d.size(), 0);
    size_t strides[2] = { kActorSize, kActorSizeRemastered };
    if (probed == kActorSizeRemastered) { strides[0] = kActorSizeRemastered; strides[1] = kActorSize; }

    std::string lastError;
    for (int s = 0; s < 2; ++s) {
        std::string e;
        if (ParseMovie64(d, 0, 0, strides[s], out, e)) {
            if (strides[s] == kActorSizeRemastered && probed != kActorSizeRemastered)
                std::fprintf(stderr,
                             "note: '%s': 36-byte BinActor (v1.6 layout) detected"
                             " by parsing, not from the header\n", path.c_str());
            return true;
        }
        if (s == 0) lastError = e;
    }
    err = "'" + path + "': " + lastError;
    return false;
}

bool MovieBinltl64::Write(const std::string& path, const Movie& m, std::string& err)
{
    std::vector<uint8_t> o;
    if (!BuildMovie64(m, false, false, o, err)) return false;
    return writeFile(path, o, err);
}

// MovieBinuni (.movie.binuni)

bool MovieBinuni::Read(const std::string& path, Movie& out, std::string& err)
{
    std::vector<uint8_t> d;
    if (!readFile(path, d, err)) return false;

    if (d.size() < 8 || std::memcmp(d.data(), BINUNIMO_MAGIC, 8) != 0) {
        err = "'" + path + "': missing the 'BINUNIMO' magic (not a movie.binuni file?)";
        return false;
    }

    // The v1.5 and v1.6 binuni layouts share the magic and the movie header
    // and only differ in the BinActor stride (32 vs 36 bytes), so the layout
    // is detected from the on-disk geometry, never from the extension: the
    // header's own pointers identify the stride when the actor array is
    // packed in front of the ppAnims array (as the game writes it), and the
    // other layout is kept as a parse fallback. Offsets are file-relative
    // like in binltl64 (primary) or header-relative (fallback).
    const size_t probed = ProbeBinActorStride(d.data(), d.size(), 8);
    size_t strides[2] = { kActorSize, kActorSizeRemastered };
    if (probed == kActorSizeRemastered) { strides[0] = kActorSizeRemastered; strides[1] = kActorSize; }

    std::string firstErr;
    for (int pass = 0; pass < 2; ++pass) {
        const uint64_t delta = (pass == 0) ? 0 : 8;   // file-relative, then header-relative
        for (int s = 0; s < 2; ++s) {
            Movie tmp;
            std::string e;
            if (ParseMovie64(d, 8, delta, strides[s], tmp, e)) {
                out = std::move(tmp);
                if (strides[s] == kActorSizeRemastered && probed != kActorSizeRemastered)
                    std::fprintf(stderr,
                                 "note: '%s': 36-byte BinActor (v1.6 layout) detected"
                                 " by parsing, not from the header\n", path.c_str());
                if (pass != 0)
                    std::fprintf(stderr,
                                 "note: '%s': offsets are relative to the header (after the"
                                 " magic), not to the start of the file\n", path.c_str());
                return true;
            }
            if (pass == 0 && s == 0) firstErr = e;
        }
    }
    err = "'" + path + "': not a valid movie.binuni file (" + firstErr + ")";
    return false;
}

bool MovieBinuni::Write(const std::string& path, const Movie& m, std::string& err,
                        bool fixupRemastered)
{
    std::vector<uint8_t> o;
    if (!BuildMovie64(m, true, fixupRemastered, o, err)) return false;
    return writeFile(path, o, err);
}