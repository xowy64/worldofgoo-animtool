// MovieBinltl32.cpp: binary ".movie.binltl" reader/writer.
// See BinMovie.h for the on-disk layout.

// The reader follows the offsets stored in the 20-byte BinMovie header
// (pointers == file offsets, 0 == NULL). Each ppAnims entry points at a
// contiguous BinImageAnimation block; its internal offsets are file offsets
// too, with a fallback to block-relative offsets (a standalone anim.binltl
// image), which is noted on stderr when used. The writer layout: header,
// BinActor array, ppAnims array, string table (padded to 4), then one
// contiguous animation block per actor (each laid out exactly like
// AnimBinltl32::Write lays out a whole file).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include "BinMovie.h"

static_assert(sizeof(float) == 4 && sizeof(int32_t) == 4 && sizeof(uint32_t) == 4,
              "this converter assumes 32-bit ints and floats");

namespace {

const size_t kMovieHeaderSize = 20;   // float + int + 3 x 32-bit offsets
const size_t kActorSize       = 32;   // 4 x int32 + 2 x float + int32 + float
const size_t kAnimHeaderSize  = 52;   // BinImageAnimation (BinAnimation.h)
const size_t kKeyframeSize    = 32;   // 3 floats + 5 ints

// little endian primitives (same as AnimBinltl32.cpp)

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

void patchU32(std::vector<uint8_t>& d, size_t pos, uint32_t v)
{
    d[pos]     = uint8_t(v);
    d[pos + 1] = uint8_t(v >> 8);
    d[pos + 2] = uint8_t(v >> 16);
    d[pos + 3] = uint8_t(v >> 24);
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

// "a\0b\0" -> ["a","b"]; trailing NUL padding becomes trailing empty strings.
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

// Parses one animation block.  The 52-byte header starts at hdr; every
// stored offset maps to the file position 'stored + delta' (0 for file
// relative offsets, hdr for offsets relative to the block start), and all
// reads are bounded by 'limit' (the end of the block).  'out' is only
// touched when the whole parse succeeds.
bool ParseAnim32(const std::vector<uint8_t>& d, size_t hdr, uint32_t delta,
                 size_t limit, Animation& out, std::string& err)
{
    if (limit > d.size()) limit = d.size();
    const size_t headerEnd = hdr + kAnimHeaderSize;
    if (hdr > d.size() || limit < headerEnd) {
        err = "animation block too small for a BinImageAnimation header";
        return false;
    }

    Animation a;
    a.offsets.base = hdr;    // callers adjust when the block has a leading magic
    uint32_t typesOff = 0, timesOff = 0, xformOff = 0;
    uint32_t alphaOff = 0, colorOff = 0, soundOff = 0, strOff = 0;

    if (!rdI32(d, hdr + 0x00, a.hasColor)      ||
        !rdI32(d, hdr + 0x04, a.hasAlpha)      ||
        !rdI32(d, hdr + 0x08, a.hasSound)      ||
        !rdI32(d, hdr + 0x0C, a.hasTransform)  ||
        !rdI32(d, hdr + 0x10, a.numTransforms) ||
        !rdI32(d, hdr + 0x14, a.numFrames)     ||
        !rdU32(d, hdr + 0x18, typesOff) || !rdU32(d, hdr + 0x1C, timesOff) ||
        !rdU32(d, hdr + 0x20, xformOff) || !rdU32(d, hdr + 0x24, alphaOff) ||
        !rdU32(d, hdr + 0x28, colorOff) || !rdU32(d, hdr + 0x2C, soundOff) ||
        !rdU32(d, hdr + 0x30, strOff)) {
        err = "truncated BinImageAnimation header";
        return false;
    }

    if (a.numTransforms < 0 || a.numFrames < 0) {
        err = "negative numTransforms/numFrames in an animation block";
        return false;
    }
    {   // cheap guard against corrupt counts
        const int64_t room = (int64_t(limit) - int64_t(headerEnd)) / 4;
        if (a.numTransforms > room || a.numFrames > room) {
            err = "numTransforms/numFrames too large for the animation block";
            return false;
        }
    }

    auto deref = [&](uint32_t stored, size_t& pos) -> bool {
        if (stored == 0) return false;
        const uint64_t p = uint64_t(stored) + uint64_t(delta);
        if (p < headerEnd || p >= limit) return false;
        pos = size_t(p);
        return true;
    };

    std::vector<size_t> structOffs;   // for the string table extent

    // transform types
    {
        size_t pos = 0;
        if (deref(typesOff, pos)) {
            structOffs.push_back(pos);
            a.offsets.transformTypes = pos;
            if (pos + 4 * size_t(a.numTransforms) > limit) {
                err = "truncated mTransformTypes array";
                return false;
            }
            for (int t = 0; t < a.numTransforms; ++t) {
                int32_t v;
                if (!rdI32(d, pos + 4 * size_t(t), v)) {
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

    // one track: an array of numFrames offsets to keyframes.  Also reports
    // the file position of the pointer array and of every keyframe in it.
    auto readTrack = [&](uint32_t arrayOff, const char* what,
                         std::vector<KeyframeSlot>& track,
                         std::vector<size_t>& kfOffs, size_t* arrayPosOut) -> bool {
        track.assign(size_t(a.numFrames), KeyframeSlot());
        kfOffs.assign(size_t(a.numFrames), 0);
        if (arrayOff == 0) return true;              // whole track absent
        size_t arrayPos = 0;
        if (!deref(arrayOff, arrayPos)) {
            err = std::string(what) + " pointer array offset out of range";
            return false;
        }
        if (arrayPosOut) *arrayPosOut = arrayPos;
        if (arrayPos + 4 * size_t(a.numFrames) > limit) {
            err = std::string("truncated ") + what + " pointer array";
            return false;
        }
        structOffs.push_back(arrayPos);
        for (int f = 0; f < a.numFrames; ++f) {
            uint32_t kfOff;
            if (!rdU32(d, arrayPos + 4 * size_t(f), kfOff)) {
                err = std::string("truncated ") + what + " pointer array";
                return false;
            }
            if (kfOff == 0) continue;                // NULL keyframe for this frame
            size_t kfPos = 0;
            if (!deref(kfOff, kfPos) || kfPos + kKeyframeSize > limit) {
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
    // (keyframe soundStrIdx values are converted from byte offsets to list
    // indexes after the string table is parsed below)

    // transform keyframes: mXformFrames[t] -> per-transform track
    a.xformFrames.assign(size_t(a.numTransforms) * size_t(a.numFrames), KeyframeSlot());
    a.offsets.xformKeyframes.assign(size_t(a.numTransforms) * size_t(a.numFrames), 0);
    if (xformOff != 0) {
        size_t xformPos = 0;
        if (!deref(xformOff, xformPos)) {
            err = "mXformFrames offset out of range";
            return false;
        }
        if (xformPos + 4 * size_t(a.numTransforms) > limit) {
            err = "truncated mXformFrames pointer array";
            return false;
        }
        structOffs.push_back(xformPos);
        a.offsets.xformFrames = xformPos;
        for (int t = 0; t < a.numTransforms; ++t) {
            uint32_t rowOff;
            if (!rdU32(d, xformPos + 4 * size_t(t), rowOff)) {
                err = "truncated mXformFrames pointer array";
                return false;
            }
            if (rowOff == 0) continue;               // this transform has no keyframes
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

    out = a;    // only on success
    return true;
}

// <sounds> entries have no BinMovie field to live in; the game plays movie
// sounds from the actor animations soundtracks, so they become sound
// keyframes on the first actor's animation, at the frame with the closest
// time. See XmlConvert.cpp for the XML side.
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
                  " of the first actor's animation, give them distinct times";
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
    // re-chain the soundtrack
    for (int32_t f = 0; f < anim.numFrames; ++f) {
        if (!anim.soundFrames[size_t(f)].present) continue;
        int32_t next = -1;
        for (int32_t g = f + 1; g < anim.numFrames; ++g)
            if (anim.soundFrames[size_t(g)].present) { next = g; break; }
        anim.soundFrames[size_t(f)].kf.nextFrameIndex = next;
    }
    return true;
}

// Appends one contiguous animation block to 'o', laid out exactly like a
// standalone .anim.binltl file: every offset is relative to the start of
// the block, which is how the game's own movie files embed them.
bool BuildAnimBlock32(const Animation& a, std::vector<uint8_t>& o, std::string& err)
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
    // TrimmedAnimStrings) - drop them and let the 4-byte alignment below
    // restore whatever this format expects
    const std::vector<std::string> strings = TrimmedAnimStrings(a);

    const bool needTable = !strings.empty() ||
                           AnySoundReference(a.xformFrames) ||
                           AnySoundReference(a.alphaFrames) ||
                           AnySoundReference(a.colorFrames) ||
                           AnySoundReference(a.soundFrames);

    const size_t blockBase = o.size();   // the block starts here

    // header (offset words are patched in at the end)
    wrI32(o, a.hasColor);
    wrI32(o, a.hasAlpha);
    wrI32(o, a.hasSound);
    wrI32(o, a.hasTransform);
    wrI32(o, a.numTransforms);
    wrI32(o, a.numFrames);
    const size_t hTypes = o.size(); wrU32(o, 0);   // 0x18
    const size_t hTimes = o.size(); wrU32(o, 0);   // 0x1C
    const size_t hXform = o.size(); wrU32(o, 0);   // 0x20
    const size_t hAlpha = o.size(); wrU32(o, 0);   // 0x24
    const size_t hColor = o.size(); wrU32(o, 0);   // 0x28
    const size_t hSound = o.size(); wrU32(o, 0);   // 0x2C
    const size_t hStr   = o.size(); wrU32(o, 0);   // 0x30

    // transform types
    uint32_t typesOff = 0;
    if (nT > 0) {
        typesOff = uint32_t(o.size() - blockBase);
        for (size_t i = 0; i < a.transformTypes.size(); ++i)
            wrI32(o, int32_t(a.transformTypes[i]));
    }

    // frame times
    uint32_t timesOff = 0;
    if (nF > 0) {
        timesOff = uint32_t(o.size() - blockBase);
        for (size_t i = 0; i < a.frameTimes.size(); ++i)
            wrF32(o, a.frameTimes[i]);
    }

    // mXformFrames: outer array, then one pointer array per transform
    uint32_t xformOff = 0;
    std::vector<size_t> outerPatch;
    std::vector<std::vector<size_t> > kfPatch;
    if (nT > 0) {
        xformOff = uint32_t(o.size() - blockBase);
        for (int64_t t = 0; t < nT; ++t) { outerPatch.push_back(o.size()); wrU32(o, 0); }
        kfPatch.resize(size_t(nT));
        for (int64_t t = 0; t < nT; ++t) {
            patchU32(o, outerPatch[size_t(t)], uint32_t(o.size() - blockBase));
            for (int64_t f = 0; f < nF; ++f) {
                kfPatch[size_t(t)].push_back(o.size());
                wrU32(o, 0);
            }
        }
    }

    // alpha / color / sound pointer arrays (always present, like game files)
    uint32_t alphaOff = 0, colorOff = 0, soundOff = 0;
    std::vector<size_t> alphaPatch, colorPatch, soundPatch;
    if (nF > 0) {
        alphaOff = uint32_t(o.size() - blockBase);
        for (int64_t f = 0; f < nF; ++f) { alphaPatch.push_back(o.size()); wrU32(o, 0); }
        colorOff = uint32_t(o.size() - blockBase);
        for (int64_t f = 0; f < nF; ++f) { colorPatch.push_back(o.size()); wrU32(o, 0); }
        soundOff = uint32_t(o.size() - blockBase);
        for (int64_t f = 0; f < nF; ++f) { soundPatch.push_back(o.size()); wrU32(o, 0); }
    }

    // string table (padded to a 4-byte multiple)
    uint32_t strOff = 0;
    if (needTable) {
        strOff = uint32_t(o.size() - blockBase);
        if (strings.empty()) {
            o.push_back(0);                        // a single empty string
        } else {
            for (size_t i = 0; i < strings.size(); ++i) {
                o.insert(o.end(), strings[i].begin(), strings[i].end());
                o.push_back(0);
            }
        }
        while (o.size() % 4 != 0) o.push_back(0);
    }

    // keyframes (transforms first, then alpha, color, sound)
    for (int32_t t = 0; t < a.numTransforms; ++t) {
        for (int32_t f = 0; f < a.numFrames; ++f) {
            const KeyframeSlot& s = a.xformAt(t, f);
            if (!s.present) continue;
            patchU32(o, kfPatch[size_t(t)][size_t(f)], uint32_t(o.size() - blockBase));
            wrKeyframe(o, s.kf, strings);
        }
    }
    for (int32_t f = 0; f < a.numFrames; ++f) {
        const KeyframeSlot& s = a.alphaFrames[size_t(f)];
        if (s.present) { patchU32(o, alphaPatch[size_t(f)], uint32_t(o.size() - blockBase)); wrKeyframe(o, s.kf, strings); }
    }
    for (int32_t f = 0; f < a.numFrames; ++f) {
        const KeyframeSlot& s = a.colorFrames[size_t(f)];
        if (s.present) { patchU32(o, colorPatch[size_t(f)], uint32_t(o.size() - blockBase)); wrKeyframe(o, s.kf, strings); }
    }
    for (int32_t f = 0; f < a.numFrames; ++f) {
        const KeyframeSlot& s = a.soundFrames[size_t(f)];
        if (s.present) { patchU32(o, soundPatch[size_t(f)], uint32_t(o.size() - blockBase)); wrKeyframe(o, s.kf, strings); }
    }

    // patch the header offsets
    patchU32(o, hTypes, typesOff);
    patchU32(o, hTimes, timesOff);
    patchU32(o, hXform, xformOff);
    patchU32(o, hAlpha, alphaOff);
    patchU32(o, hColor, colorOff);
    patchU32(o, hSound, soundOff);
    patchU32(o, hStr,   strOff);
    return true;    // the 4 GB check happens in BuildMovie32
}

// whole movie

bool ParseMovie32(const std::vector<uint8_t>& d, Movie& out, std::string& err)
{
    if (d.size() < kMovieHeaderSize) {
        err = "too small for a BinMovie header (" + std::to_string(d.size()) +
              " of 20 bytes)";
        return false;
    }

    Movie m;
    int32_t numActors = 0;
    uint32_t actorsOff = 0, animsOff = 0, strOff = 0;
    if (!rdF32(d, 0x00, m.length) ||
        !rdI32(d, 0x04, numActors) ||
        !rdU32(d, 0x08, actorsOff) ||
        !rdU32(d, 0x0C, animsOff)  ||
        !rdU32(d, 0x10, strOff)) {
        err = "truncated BinMovie header";
        return false;
    }
    if (numActors < 0) {
        err = "negative numActors (not a movie.binltl file?)";
        return false;
    }
    if (size_t(numActors) > (d.size() - kMovieHeaderSize) / kActorSize) {
        err = "numActors too large for the file size";
        return false;
    }

    // every structure position, for computing the string table extent
    std::vector<size_t> structOffs;
    if (numActors > 0) {
        if (actorsOff == 0 || actorsOff >= d.size()) {
            err = "pActors offset out of range";
            return false;
        }
        if (size_t(actorsOff) + kActorSize * size_t(numActors) > d.size()) {
            err = "truncated BinActor array";
            return false;
        }
        structOffs.push_back(actorsOff);
    }

    // ppAnims
    std::vector<size_t> animBases(size_t(numActors), 0);
    if (numActors > 0) {
        if (animsOff == 0 || animsOff >= d.size()) {
            err = "ppAnims offset out of range";
            return false;
        }
        if (size_t(animsOff) + 4 * size_t(numActors) > d.size()) {
            err = "truncated ppAnims array";
            return false;
        }
        structOffs.push_back(animsOff);
        for (int32_t i = 0; i < numActors; ++i) {
            uint32_t p;
            if (!rdU32(d, size_t(animsOff) + 4 * size_t(i), p)) {
                err = "truncated ppAnims array";
                return false;
            }
            if (p == 0) {
                err = "actor " + std::to_string(i) +
                      " has a NULL animation pointer (BinMovie has exactly one"
                      " BinImageAnimation per BinActor)";
                return false;
            }
            if (p >= d.size()) {
                err = "actor " + std::to_string(i) + " animation offset out of range";
                return false;
            }
            animBases[size_t(i)] = p;
            structOffs.push_back(p);
        }
    }

    // string table (the raw bytes are kept: BinActor references are
    // resolved against them in several ways, see str() below)
    const uint8_t* rawTable = nullptr;
    size_t rawTableLen = 0;
    if (strOff != 0) {
        if (strOff >= d.size()) {
            err = "pStringTable offset out of range";
            return false;
        }
        size_t end = d.size();
        for (size_t i = 0; i < structOffs.size(); ++i)
            if (structOffs[i] > strOff && structOffs[i] < end)
                end = structOffs[i];
        rawTable = d.data() + strOff;
        rawTableLen = end - strOff;
        m.offsets.stringTable = strOff;
        m.strings = splitStringTable(rawTable, rawTableLen);
    }

    // Resolves a BinActor string reference. Real files store BYTE OFFSETS
    // into pStringTable - the game resolves them as pStringTable + idx
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
        const size_t base = size_t(actorsOff) + kActorSize * size_t(i);
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
        ac.image      = str(imageIdx);
        ac.labelText  = str(textIdx);
        ac.font       = str(fontIdx);
    }

    // animation blocks.  2D Boy stored each one as a standalone contiguous
    // image with offsets relative to the start of the block (that is what
    // the ppAnims indirection exists for), so that interpretation is tried
    // first; file-relative offsets are the fallback.
    for (int32_t i = 0; i < numActors; ++i) {
        const size_t animBase = animBases[size_t(i)];
        size_t blockEnd = d.size();
        for (int32_t j = 0; j < numActors; ++j)
            if (animBases[size_t(j)] > animBase && animBases[size_t(j)] < blockEnd)
                blockEnd = animBases[size_t(j)];
        if (strOff > animBase && size_t(strOff) < blockEnd)
            blockEnd = strOff;

        Animation& a = m.actors[size_t(i)].anim;
        Animation tmp;
        std::string e;
        if (ParseAnim32(d, animBase, uint32_t(animBase), blockEnd, tmp, e)) {
            a = tmp;                     // block-relative (the game's own files)
        } else {
            Animation tmp2;
            std::string e2;
            if (ParseAnim32(d, animBase, 0, blockEnd, tmp2, e2)) {
                a = tmp2;                // file-relative
                std::fprintf(stderr,
                             "note: actor %d's animation block uses file-relative"
                             " offsets, not block-relative ones\n", int(i));
            } else {
                err = "actor " + std::to_string(i) + ": the animation block is not a"
                                                     " valid BinImageAnimation (" + e + ")";
                return false;
            }
        }
    }

    // where things live, for the verbose dump (the 32-bit format has no
    // v1.6 variant: BinActor is always 32 bytes here)
    if (numActors > 0) {
        m.offsets.actors = actorsOff;
        m.offsets.anims  = animsOff;
    }
    m.offsets.animBlocks.assign(size_t(numActors), 0);
    for (int32_t i = 0; i < numActors; ++i)
        m.offsets.animBlocks[size_t(i)] = animBases[size_t(i)];
    m.readAsRemastered = false;

    out = m;    // only on success
    return true;
}

bool BuildMovie32(const Movie& m, std::vector<uint8_t>& o, std::string& err)
{
    if (m.actors.size() > size_t(INT32_MAX)) {
        err = "too many actors";
        return false;
    }
    const int32_t numActors = int32_t(m.actors.size());

    // The string table: the movie's table with any trailing pad-empty
    // entries dropped (they are the source format's table padding, see
    // TrimmedMovieStrings, the 4-byte alignment below restores this
    // format's own), with strings not in it appended in actor order
    // (image, text, font). Actor references are stored as byte offsets
    // into the table. Unused fields reference the table's empty-string
    // entry (appended on demand) - the game's own files do it this way,
    // not with -1.
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
    o.reserve(kMovieHeaderSize + kActorSize * size_t(numActors) +
              4 * size_t(numActors) + 64);

    // header
    wrF32(o, m.length);
    wrI32(o, numActors);
    const size_t hActors = o.size(); wrU32(o, 0);   // 0x08
    const size_t hAnims  = o.size(); wrU32(o, 0);   // 0x0C
    const size_t hStr    = o.size(); wrU32(o, 0);   // 0x10

    // BinActor array
    const uint32_t actorsOff = uint32_t(o.size());
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
    }

    // ppAnims array (patched below)
    const uint32_t animsOff = (numActors > 0) ? uint32_t(o.size()) : 0;
    std::vector<size_t> animPatch;
    animPatch.resize(size_t(numActors));    // numActors placeholder positions
    for (size_t i = 0; i < animPatch.size(); ++i) { animPatch[i] = o.size(); wrU32(o, 0); }

    // string table (padded to a 4-byte multiple)
    uint32_t strOff = 0;
    if (!strings.empty()) {
        strOff = uint32_t(o.size());
        for (size_t i = 0; i < strings.size(); ++i) {
            o.insert(o.end(), strings[i].begin(), strings[i].end());
            o.push_back(0);
        }
        while (o.size() % 4 != 0) o.push_back(0);
    }

    // one contiguous animation block per actor (block-relative offsets)
    for (size_t i = 0; i < anims.size(); ++i) {
        patchU32(o, animPatch[i], uint32_t(o.size()));
        if (!BuildAnimBlock32(anims[i], o, err)) return false;
    }

    if (o.size() > size_t(UINT32_MAX)) {
        err = "result is larger than 4 GB; binltl32 offsets are 32-bit";
        return false;
    }

    // patch the header offsets
    patchU32(o, hActors, actorsOff);
    patchU32(o, hAnims,  animsOff);
    patchU32(o, hStr,    strOff);
    return true;
}

} // namespace

// MovieBinltl32 (.movie.binltl)

bool MovieBinltl32::Read(const std::string& path, Movie& out, std::string& err)
{
    std::vector<uint8_t> d;
    if (!readFile(path, d, err)) return false;

    std::string e;
    if (!ParseMovie32(d, out, e)) {
        err = "'" + path + "': " + e;
        return false;
    }
    return true;
}

bool MovieBinltl32::Write(const std::string& path, const Movie& m, std::string& err)
{
    std::vector<uint8_t> o;
    if (!BuildMovie32(m, o, err)) return false;
    return writeFile(path, o, err);
}