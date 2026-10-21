//  AnimBinltl64.cpp - ".anim.binltl64" and ".anim.binuni" reader/writer.
//
//  BINLTL64: the same binary image as ".anim.binltl", except that every
//  pointer is 64 bits wide, which grows the header from 52 to 80 bytes
//  (7 offsets x 4 extra bytes each).  Only the offsets grow: transform
//  types stay 4-byte enums, frame times stay 4-byte floats, keyframes stay
//  32 bytes, and a stored offset of 0 still means NULL.

//  BINUNI: the same as binltl64 with an 8-byte "BINUNIAN" magic in front
//  of the header (88-byte header).  Offsets are stored relative to the
//  start of the file, exactly like in binltl64; the reader also accepts
//  offsets relative to the header (byte 8) as a fallback and prints a note
//  when it has to. The writer's behaviour can be switched with
//  kBinuniStructRelativeOffsets below.

//  Writer data layout (same order as AnimBinltl32::Write): header, transform
//  types, frame times, mXformFrames outer array, per-transform arrays,
//  alpha/color/sound arrays, string table (padded to 8 bytes), keyframes.

//  The little endian primitives and file IO helpers are deliberate copies
//  of the ones in AnimBinltl32.cpp, so both converters stay self-contained
//  and the verified 32-bit code does not have to be touched.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include "AnimBinltl64.h"

static_assert(sizeof(float) == 4 && sizeof(int32_t) == 4 && sizeof(uint32_t) == 4 &&
                  sizeof(int64_t) == 8 && sizeof(uint64_t) == 8,
              "this converter assumes 32-bit ints/floats and 64-bit long longs");

namespace {

const size_t kHeaderFields = 80;   // 6 x int32 + 7 x int64
const size_t kKeyframeSize = 32;
const size_t kTypeSize     = 4;    // enum TransformType stays 4 bytes in 64-bit builds

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

// "a\0b\0" -> ["a","b"]; trailing NUL padding becomes trailing empty strings
// so that writing them back reproduces the original bytes.
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

// parser

// parses the 64-bit image.  'hdr' is the file offset of the hasColor field
// (0 for binltl64, 8 for binuni).  'delta' is added to every stored offset
// before it is used as a file position (0 normally; 8 when trying binuni
// offsets that are relative to the header instead of the file).  'out' is
// only touched when the whole parse succeeds.
bool ParseBin64(const std::vector<uint8_t>& d, size_t hdr, uint64_t delta,
                Animation& out, std::string& err)
{
    const size_t headerEnd = hdr + kHeaderFields;
    if (d.size() < headerEnd) {
        err = "too small for the header (" + std::to_string(d.size()) +
              " bytes, need " + std::to_string(headerEnd) + ")";
        return false;
    }

    Animation a;
    a.offsets.base = hdr;   // 8 for a binuni image whose header follows the magic

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
    {   // cheap guard against corrupt counts (every counted element needs
        // at least 4 bytes somewhere behind the header)
        const int64_t room = (int64_t(d.size()) - int64_t(headerEnd)) / 4;
        if (a.numTransforms > room || a.numFrames > room) {
            err = "numTransforms/numFrames too large for the file size";
            return false;
        }
    }

    // stored offset -> file position; rejects NULL and anything that does
    // not point behind the header / inside the file.  (The "behind the
    // header" rule is also what lets the binuni reader distinguish file
    // relative from header relative offsets.)
    auto deref = [&](uint64_t stored, size_t& pos) -> bool {
        if (stored == 0) return false;
        const uint64_t p = stored + delta;
        if (p < headerEnd || p > d.size()) return false;
        pos = size_t(p);
        return true;
    };

    // every structure position, for computing the string table extent
    // (the table's size is not stored anywhere)
    std::vector<size_t> structOffs;

    // transform types
    {
        size_t pos = 0;
        if (deref(typesOff, pos)) {
            structOffs.push_back(pos);
            a.offsets.transformTypes = pos;
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
            for (int f = 0; f < a.numFrames; ++f) {
                float v;
                if (!rdF32(d, pos + 4 * size_t(f), v)) {
                    err = "truncated mFrameTimes array";
                    return false;
                }
                a.frameTimes[size_t(f)] = v;
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
        structOffs.push_back(arrayPos);
        for (int f = 0; f < a.numFrames; ++f) {
            uint64_t stored = 0;
            if (!rdU64(d, arrayPos + 8 * size_t(f), stored)) {
                err = std::string("truncated ") + what + " pointer array";
                return false;
            }
            if (stored == 0) continue;                // NULL keyframe for this frame
            size_t kfPos = 0;
            if (!deref(stored, kfPos)) {
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
        size_t end = d.size();
        for (size_t i = 0; i < structOffs.size(); ++i)
            if (structOffs[i] > strPos && structOffs[i] < end)
                end = structOffs[i];
        a.offsets.stringTable = strPos;
        a.strings = splitStringTable(d.data() + strPos, end - strPos);
    }

    // keyframe soundStrIdx values are byte offsets into this table in every
    // binary format; the data model uses list indexes (see BinAnimation.h)
    SoundRefsOffsetsToIndexes(a);

    out = a;    // only on success, so failed attempts leave 'out' untouched
    return true;
}

// writer

// If a binuni file turns out to store offsets relative to the header (the
// first byte after the magic) instead of the start of the file, set this to
// true.  The reader accepts both variants either way.
const bool kBinuniStructRelativeOffsets = false;

bool BuildBin64(const Animation& a, bool withMagic, std::vector<uint8_t>& o, std::string& err)
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
    // TrimmedAnimStrings) - drop them and let the 8-byte alignment below
    // restore whatever this format expects
    const std::vector<std::string> strings = TrimmedAnimStrings(a);

    const bool needTable = !strings.empty() ||
                           AnySoundReference(a.xformFrames) ||
                           AnySoundReference(a.alphaFrames) ||
                           AnySoundReference(a.colorFrames) ||
                           AnySoundReference(a.soundFrames);

    const int64_t kfCount = int64_t(CountPresent(a.xformFrames)) +
                            int64_t(CountPresent(a.alphaFrames)) +
                            int64_t(CountPresent(a.colorFrames)) +
                            int64_t(CountPresent(a.soundFrames));

    const uint64_t adj = (withMagic && kBinuniStructRelativeOffsets) ? 8 : 0;

    o.clear();
    o.reserve(size_t((withMagic ? 8 : 0) + int64_t(kHeaderFields) +
                     4 * nT + 4 * nF + 8 * nT + 8 * nT * nF + 24 * nF + 8 +
                     int64_t(strings.size()) + kfCount * int64_t(kKeyframeSize)));

    if (withMagic)
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
        typesOff = uint64_t(o.size()) - adj;
        for (size_t i = 0; i < a.transformTypes.size(); ++i)
            wrI32(o, int32_t(a.transformTypes[i]));
    }

    // frame times
    uint64_t timesOff = 0;
    if (nF > 0) {
        timesOff = uint64_t(o.size()) - adj;
        for (size_t i = 0; i < a.frameTimes.size(); ++i)
            wrF32(o, a.frameTimes[i]);
    }

    // mXformFrames: outer array, then one pointer array per transform
    uint64_t xformOff = 0;
    std::vector<size_t> outerPatch;
    std::vector<std::vector<size_t> > kfPatch;
    if (nT > 0) {
        xformOff = uint64_t(o.size()) - adj;
        for (int64_t t = 0; t < nT; ++t) { outerPatch.push_back(o.size()); wrU64(o, 0); }
        kfPatch.resize(size_t(nT));
        for (int64_t t = 0; t < nT; ++t) {
            patchU64(o, outerPatch[size_t(t)], uint64_t(o.size()) - adj);
            for (int64_t f = 0; f < nF; ++f) {
                kfPatch[size_t(t)].push_back(o.size());
                wrU64(o, 0);
            }
        }
    }

    // alpha / color / sound pointer arrays (written even when the has* flag is 0,
    // filled with NULLs, matching game files)
    uint64_t alphaOff = 0, colorOff = 0, soundOff = 0;
    std::vector<size_t> alphaPatch, colorPatch, soundPatch;
    if (nF > 0) {
        alphaOff = uint64_t(o.size()) - adj;
        for (int64_t f = 0; f < nF; ++f) { alphaPatch.push_back(o.size()); wrU64(o, 0); }
        colorOff = uint64_t(o.size()) - adj;
        for (int64_t f = 0; f < nF; ++f) { colorPatch.push_back(o.size()); wrU64(o, 0); }
        soundOff = uint64_t(o.size()) - adj;
        for (int64_t f = 0; f < nF; ++f) { soundPatch.push_back(o.size()); wrU64(o, 0); }
    }

    // string table (padded to 8 bytes, like a 64-bit memory image)
    uint64_t strOff = 0;
    if (needTable) {
        strOff = uint64_t(o.size()) - adj;
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
            patchU64(o, kfPatch[size_t(t)][size_t(f)], uint64_t(o.size()) - adj);
            wrKeyframe(o, s.kf, strings);
        }
    }
    for (int32_t f = 0; f < a.numFrames; ++f) {
        const KeyframeSlot& s = a.alphaFrames[size_t(f)];
        if (s.present) { patchU64(o, alphaPatch[size_t(f)], uint64_t(o.size()) - adj); wrKeyframe(o, s.kf, strings); }
    }
    for (int32_t f = 0; f < a.numFrames; ++f) {
        const KeyframeSlot& s = a.colorFrames[size_t(f)];
        if (s.present) { patchU64(o, colorPatch[size_t(f)], uint64_t(o.size()) - adj); wrKeyframe(o, s.kf, strings); }
    }
    for (int32_t f = 0; f < a.numFrames; ++f) {
        const KeyframeSlot& s = a.soundFrames[size_t(f)];
        if (s.present) { patchU64(o, soundPatch[size_t(f)], uint64_t(o.size()) - adj); wrKeyframe(o, s.kf, strings); }
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

} // namespace

// AnimBinltl64 (.anim.binltl64)

bool AnimBinltl64::Read(const std::string& path, Animation& out, std::string& err)
{
    std::vector<uint8_t> d;
    if (!readFile(path, d, err)) return false;

    if (d.size() >= 8 && std::memcmp(d.data(), BINUNI_MAGIC, 8) == 0) {
        err = "'" + path + "': this is an anim.binuni file (BINUNIAN magic)";
        return false;
    }

    std::string e;
    if (!ParseBin64(d, 0, 0, out, e)) {
        err = "'" + path + "': " + e;
        return false;
    }
    return true;
}

bool AnimBinltl64::Write(const std::string& path, const Animation& a, std::string& err)
{
    std::vector<uint8_t> o;
    if (!BuildBin64(a, false, o, err)) return false;
    return writeFile(path, o, err);
}

//  AnimBinuni (.anim.binuni)

bool AnimBinuni::Read(const std::string& path, Animation& out, std::string& err)
{
    std::vector<uint8_t> d;
    if (!readFile(path, d, err)) return false;

    if (d.size() < 8 || std::memcmp(d.data(), BINUNI_MAGIC, 8) != 0) {
        err = "'" + path + "': missing the 'BINUNIAN' magic (not an anim.binuni file?)";
        return false;
    }

    // Primary interpretation: offsets are file-relative, like in binltl64.
    {
        std::string e1;
        if (ParseBin64(d, 8, 0, out, e1)) {
            out.offsets.base = 0;   // the image starts at the magic
            return true;
        }
    }
    // Fallback: offsets relative to the header (the byte after the magic).
    {
        Animation tmp;
        std::string e2;
        if (ParseBin64(d, 8, 8, tmp, e2)) {
            out = std::move(tmp);
            out.offsets.base = 0;
            std::fprintf(stderr,
                         "note: '%s': offsets are relative to the header (after the"
                         " magic), not to the start of the file\n", path.c_str());
            return true;
        }
    }
    err = "'" + path + "': not a valid anim.binuni file";
    return false;
}

bool AnimBinuni::Write(const std::string& path, const Animation& a, std::string& err)
{
    std::vector<uint8_t> o;
    if (!BuildBin64(a, true, o, err)) return false;
    return writeFile(path, o, err);
}