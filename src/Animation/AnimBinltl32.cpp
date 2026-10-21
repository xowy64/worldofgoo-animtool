// AnimBinltl32.cpp: binary ".anim.binltl" reader/writer.

// The reader follows the offsets stored in the 52-byte BinImageAnimation
// header (pointers == file offsets, 0 == NULL).  The writer reproduces the
// byte layout of game-produced files:

// header, transform types, frame times, mXformFrames outer array,
// per-transform arrays, alpha/color/sound arrays, string table
// (padded to 4 bytes), keyframes.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include "AnimBinltl32.h"

static_assert(sizeof(float) == 4 && sizeof(int32_t) == 4 && sizeof(uint32_t) == 4,
              "this converter assumes 32-bit ints and floats");

namespace {

const size_t kHeaderSize   = 52;   // 13 x 32-bit words
const size_t kKeyframeSize = 32;   // 3 floats + 5 ints

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

// string table

// "a\0b\0" -> ["a","b"]. Trailing NUL padding becomes trailing empty strings,
// which is exactly what we want: joining them back reproduces the bytes.
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

} // namespace

// Read

bool AnimBinltl32::Read(const std::string& path, Animation& out, std::string& err)
{
    std::vector<uint8_t> d;
    if (!readFile(path, d, err)) return false;

    if (d.size() < kHeaderSize) {
        err = "'" + path + "': too small for a BinImageAnimation header (" +
              std::to_string(d.size()) + " of 52 bytes)";
        return false;
    }

    // header
    uint32_t typesOff = 0, timesOff = 0, xformOff = 0;
    uint32_t alphaOff = 0, colorOff = 0, soundOff = 0, strOff = 0;

    if (!rdI32(d, 0x00, out.hasColor)      ||
        !rdI32(d, 0x04, out.hasAlpha)      ||
        !rdI32(d, 0x08, out.hasSound)      ||
        !rdI32(d, 0x0C, out.hasTransform)  ||
        !rdI32(d, 0x10, out.numTransforms) ||
        !rdI32(d, 0x14, out.numFrames)     ||
        !rdU32(d, 0x18, typesOff) || !rdU32(d, 0x1C, timesOff) ||
        !rdU32(d, 0x20, xformOff) || !rdU32(d, 0x24, alphaOff) ||
        !rdU32(d, 0x28, colorOff) || !rdU32(d, 0x2C, soundOff) ||
        !rdU32(d, 0x30, strOff)) {
        err = "'" + path + "': truncated BinImageAnimation header";
        return false;
    }

    if (out.numTransforms < 0 || out.numFrames < 0) {
        err = "'" + path + "': negative numTransforms/numFrames (not an anim.binltl file?)";
        return false;
    }
    {   // cheap guard against corrupt counts (every element is >= 4 bytes)
        const int64_t room = (int64_t(d.size()) - int64_t(kHeaderSize)) / 4;
        if (out.numTransforms > room || out.numFrames > room) {
            err = "'" + path + "': numTransforms/numFrames too large for the file size";
            return false;
        }
    }

    // all structure offsets in the file. The string table is the only member
    // whose size is not stored anywhere, so its extent is computed as
    // "everything from pStringTable up to the next structure (or EOF)".
    // They are also kept in out.offsets for the verbose dump.
    std::vector<size_t> structOffs;
    auto addStruct = [&structOffs, &d](uint32_t o) {
        if (o != 0 && o < d.size()) structOffs.push_back(o);
    };
    addStruct(typesOff);
    addStruct(timesOff);
    addStruct(xformOff);
    out.offsets = AnimFileOffsets();     // a fresh parse: no stale offsets
    out.offsets.base = 0;
    out.offsets.transformTypes = (typesOff != 0 && typesOff < d.size()) ? size_t(typesOff) : 0;
    out.offsets.frameTimes     = (timesOff != 0 && timesOff < d.size()) ? size_t(timesOff) : 0;
    out.offsets.xformFrames    = (xformOff != 0 && xformOff < d.size()) ? size_t(xformOff) : 0;
    out.offsets.alphaFrames    = (alphaOff != 0 && alphaOff < d.size()) ? size_t(alphaOff) : 0;
    out.offsets.colorFrames    = (colorOff != 0 && colorOff < d.size()) ? size_t(colorOff) : 0;
    out.offsets.soundFrames    = (soundOff != 0 && soundOff < d.size()) ? size_t(soundOff) : 0;
    out.offsets.stringTable    = (strOff   != 0 && strOff   < d.size()) ? size_t(strOff)   : 0;

    // transform types
    out.transformTypes.clear();
    if (typesOff != 0) {
        for (int t = 0; t < out.numTransforms; ++t) {
            int32_t v;
            if (!rdI32(d, size_t(typesOff) + 4 * size_t(t), v)) {
                err = "'" + path + "': truncated mTransformTypes array";
                return false;
            }
            if (v < XFORM_SCALE || v > XFORM_TRANSLATE) {
                err = "'" + path + "': unknown transform type " + std::to_string(v) +
                      " at index " + std::to_string(t);
                return false;
            }
            out.transformTypes.push_back(TransformType(v));
        }
    } else if (out.numTransforms > 0) {
        err = "'" + path + "': mTransformTypes is NULL but numTransforms = " +
              std::to_string(out.numTransforms);
        return false;
    }

    // frame times
    out.frameTimes.assign(size_t(out.numFrames), 0.0f);
    if (timesOff != 0) {
        for (int f = 0; f < out.numFrames; ++f) {
            float v;
            if (!rdF32(d, size_t(timesOff) + 4 * size_t(f), v)) {
                err = "'" + path + "': truncated mFrameTimes array";
                return false;
            }
            out.frameTimes[size_t(f)] = v;
        }
    }

    // one track: an array of numFrames offsets to keyframes
    auto readTrack = [&](uint32_t arrayOff, const char* what,
                         std::vector<KeyframeSlot>& track,
                         std::vector<size_t>& kfOffs) -> bool {
        track.assign(size_t(out.numFrames), KeyframeSlot());
        kfOffs.assign(size_t(out.numFrames), 0);
        if (arrayOff == 0) return true;            // whole track absent
        addStruct(arrayOff);
        for (int f = 0; f < out.numFrames; ++f) {
            uint32_t kfOff;
            if (!rdU32(d, size_t(arrayOff) + 4 * size_t(f), kfOff)) {
                err = "'" + path + "': truncated " + what + " pointer array";
                return false;
            }
            if (kfOff == 0) continue;              // NULL keyframe for this frame
            addStruct(kfOff);
            kfOffs[size_t(f)] = kfOff;
            if (kfOff > d.size() || d.size() - kfOff < kKeyframeSize ||
                !rdKeyframe(d, kfOff, track[size_t(f)].kf)) {
                err = "'" + path + "': " + what + " keyframe offset " +
                      std::to_string(kfOff) + " out of range";
                return false;
            }
            track[size_t(f)].present = true;
        }
        return true;
    };

    // transform keyframes: mXformFrames[t] -> per-transform track
    out.xformFrames.assign(size_t(out.numTransforms) * size_t(out.numFrames), KeyframeSlot());
    out.offsets.xformKeyframes.assign(size_t(out.numTransforms) * size_t(out.numFrames), 0);
    if (xformOff != 0) {
        for (int t = 0; t < out.numTransforms; ++t) {
            uint32_t rowOff;
            if (!rdU32(d, size_t(xformOff) + 4 * size_t(t), rowOff)) {
                err = "'" + path + "': truncated mXformFrames pointer array";
                return false;
            }
            if (rowOff == 0) continue;             // this transform has no keyframes
            std::vector<KeyframeSlot> row;
            std::vector<size_t> rowOffs;
            if (!readTrack(rowOff, "transform", row, rowOffs)) return false;
            for (int f = 0; f < out.numFrames; ++f) {
                out.xformFrames[out.xformIndex(t, f)] = row[size_t(f)];
                out.offsets.xformKeyframes[out.xformIndex(t, f)] = rowOffs[size_t(f)];
            }
        }
    }

    if (!readTrack(alphaOff, "alpha", out.alphaFrames, out.offsets.alphaKeyframes)) return false;
    if (!readTrack(colorOff, "color", out.colorFrames, out.offsets.colorKeyframes)) return false;
    if (!readTrack(soundOff, "sound", out.soundFrames, out.offsets.soundKeyframes)) return false;

    // string table
    out.strings.clear();
    if (strOff != 0 && strOff < d.size()) {
        size_t end = d.size();
        for (size_t i = 0; i < structOffs.size(); ++i)
            if (structOffs[i] > strOff && structOffs[i] < end)
                end = structOffs[i];
        out.strings = splitStringTable(d.data() + strOff, end - strOff);
    }
    // keyframe soundStrIdx values are byte offsets into this table in every
    // binary format; the data model uses list indexes (see BinAnimation.h)
    SoundRefsOffsetsToIndexes(out);
    return true;
}

// Write

bool AnimBinltl32::Write(const std::string& path, const Animation& a, std::string& err)
{
    const int64_t nT = a.numTransforms;
    const int64_t nF = a.numFrames;

    if (nT < 0 || nF < 0) {
        err = "negative numTransforms/numFrames";
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

    // the game always stores a string table when a keyframe references one
    // (soundStrIdx >= 0); an unused table is a single empty string.
    const bool needTable = !strings.empty() ||
                           AnySoundReference(a.xformFrames) ||
                           AnySoundReference(a.alphaFrames) ||
                           AnySoundReference(a.colorFrames) ||
                           AnySoundReference(a.soundFrames);

    std::vector<uint8_t> o;
    o.reserve(size_t(kHeaderSize + 8 * nT + 8 * nF + 4 * nT * nF + 12 * nF +
                     4 + (nT * nF + 3 * nF) * int64_t(kKeyframeSize)));

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
        typesOff = uint32_t(o.size());
        for (size_t i = 0; i < a.transformTypes.size(); ++i)
            wrI32(o, int32_t(a.transformTypes[i]));
    }

    // frame times
    uint32_t timesOff = 0;
    if (nF > 0) {
        timesOff = uint32_t(o.size());
        for (size_t i = 0; i < a.frameTimes.size(); ++i)
            wrF32(o, a.frameTimes[i]);
    }

    // mXformFrames: outer array, then one pointer array per transform
    uint32_t xformOff = 0;
    std::vector<size_t> outerPatch;
    std::vector<std::vector<size_t> > kfPatch;
    if (nT > 0) {
        xformOff = uint32_t(o.size());
        for (int64_t t = 0; t < nT; ++t) { outerPatch.push_back(o.size()); wrU32(o, 0); }
        kfPatch.resize(size_t(nT));
        for (int64_t t = 0; t < nT; ++t) {
            patchU32(o, outerPatch[size_t(t)], uint32_t(o.size()));
            for (int64_t f = 0; f < nF; ++f) {
                kfPatch[size_t(t)].push_back(o.size());
                wrU32(o, 0);
            }
        }
    }

    // alpha / color / sound pointer arrays
    // game files keep these arrays (filled with NULLs) even when the has* flag
    // is 0, so they are written unconditionally.
    uint32_t alphaOff = 0, colorOff = 0, soundOff = 0;
    std::vector<size_t> alphaPatch, colorPatch, soundPatch;
    if (nF > 0) {
        alphaOff = uint32_t(o.size());
        for (int64_t f = 0; f < nF; ++f) { alphaPatch.push_back(o.size()); wrU32(o, 0); }
        colorOff = uint32_t(o.size());
        for (int64_t f = 0; f < nF; ++f) { colorPatch.push_back(o.size()); wrU32(o, 0); }
        soundOff = uint32_t(o.size());
        for (int64_t f = 0; f < nF; ++f) { soundPatch.push_back(o.size()); wrU32(o, 0); }
    }

    // string table (padded to a 4-byte multiple, like game files)
    uint32_t strOff = 0;
    if (needTable) {
        strOff = uint32_t(o.size());
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
            patchU32(o, kfPatch[size_t(t)][size_t(f)], uint32_t(o.size()));
            wrKeyframe(o, s.kf, strings);
        }
    }
    for (int32_t f = 0; f < a.numFrames; ++f) {
        const KeyframeSlot& s = a.alphaFrames[size_t(f)];
        if (s.present) { patchU32(o, alphaPatch[size_t(f)], uint32_t(o.size())); wrKeyframe(o, s.kf, strings); }
    }
    for (int32_t f = 0; f < a.numFrames; ++f) {
        const KeyframeSlot& s = a.colorFrames[size_t(f)];
        if (s.present) { patchU32(o, colorPatch[size_t(f)], uint32_t(o.size())); wrKeyframe(o, s.kf, strings); }
    }
    for (int32_t f = 0; f < a.numFrames; ++f) {
        const KeyframeSlot& s = a.soundFrames[size_t(f)];
        if (s.present) { patchU32(o, soundPatch[size_t(f)], uint32_t(o.size())); wrKeyframe(o, s.kf, strings); }
    }

    if (o.size() > size_t(UINT32_MAX)) {
        err = "result is larger than 4 GB; binltl32 offsets are 32-bit";
        return false;
    }

    // patch the header offsets
    patchU32(o, hTypes, typesOff);
    patchU32(o, hTimes, timesOff);
    patchU32(o, hXform, xformOff);
    patchU32(o, hAlpha, alphaOff);
    patchU32(o, hColor, colorOff);
    patchU32(o, hSound, soundOff);
    patchU32(o, hStr,   strOff);

    return writeFile(path, o, err);
}