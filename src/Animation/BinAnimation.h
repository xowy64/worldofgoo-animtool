//  BinAnimation.h

//  Shared data model for World of Goo animations, mirroring the game's
//  BinImageAnimation / keyframe structures.  Used by:
//      AnimBinltl32  - binary ".anim.binltl" reader/writer
//      XmlConvert    - "anim.xml" reader/writer (TinyXML-2)
//      AnimConvert   - high level conversion + reporting glue
//      main          - command line front end

//  ---------------------------------------------------------------------------
//  ON-DISK LAYOUT OF ".anim.binltl" (32-bit, little endian)
//  ---------------------------------------------------------------------------
//  The file is a flat image of the in-memory BinImageAnimation struct: every
//  pointer is stored as a 32-bit file offset (0 == NULL).  The struct itself
//  is the 52-byte header:
//
//      offset  field
//      0x00    mHasColor, mHasAlpha, mHasSound, mHasTransform   (4 x int32)
//      0x10    mNumTransforms, mNumFrames                       (2 x int32)
//      0x18    offset -> mTransformTypes : int32[mNumTransforms]
//      0x1C    offset -> mFrameTimes     : float32[mNumFrames]
//      0x20    offset -> mXformFrames    : offset[mNumTransforms], each entry
//                                          an offset to offset[mNumFrames],
//                                          each entry an offset to a keyframe
//      0x24    offset -> mAlphaFrames    : offset[mNumFrames] -> keyframe
//      0x28    offset -> mColorFrames    : offset[mNumFrames] -> keyframe
//      0x2C    offset -> mSoundFrames    : offset[mNumFrames] -> keyframe
//      0x30    offset -> pStringTable    : NUL separated strings
//
//  A 0 entry in a frame-pointer array means "no keyframe for this frame".
//  Game-produced files keep the alpha/color/sound arrays (filled with NULLs)
//  even when the corresponding has* flag is 0, so the writer does the same.
//
//  keyframe (32 bytes): float32 x, y, angle; int32 alpha, color,
//  nextFrameIndex, soundStrIdx, interpolation (0 = none, 1 = linear).
//
//  Data behind the header is laid out in this order by the game's writer
//  (and by AnimBinltl32::Write): transform types, frame times, the
//  mXformFrames outer array, the per-transform arrays, the alpha/color/sound
//  arrays, the string table (padded to a 4-byte multiple), then the keyframes.

#ifndef BINANIMATION_H
#define BINANIMATION_H

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Enums

enum TransformType {
    XFORM_SCALE     = 0,
    XFORM_ROTATE    = 1,
    XFORM_TRANSLATE = 2
};

enum InterpolationType {
    INTERPOLATION_NONE   = 0,
    INTERPOLATION_LINEAR = 1
};

// Keyframe

struct Keyframe {
    float   x              = 0.0f;
    float   y              = 0.0f;
    float   angle          = 0.0f;
    int32_t alpha          = 0;
    int32_t color          = 0;
    int32_t nextFrameIndex = 0;
    int32_t soundStrIdx    = -1;
    int32_t interpolation  = INTERPOLATION_NONE;
};

// one keyframe slot of a track.  present == false mirrors a NULL keyframe
// pointer in the binary file (e.g. the alpha/color/sound arrays of an anim
// that does not use those tracks are arrays of NULL pointers).
struct KeyframeSlot {
    bool     present = false;
    Keyframe kf;
};

// Animation

// ---------------------------------------------------------------------------
// Verbose-dump offsets
// ---------------------------------------------------------------------------
// The binary readers record where each structure lives in the file, so the
// verbose console dump can show it.  Everything stays zeroed after an XML
// read (the XML format has no offsets); a 0 entry anywhere means "no
// information".

struct AnimFileOffsets {
    size_t base           = 0;   // first byte of the animation image
                                 // (0 also suppresses the dump line)
    size_t transformTypes = 0;   // mTransformTypes array
    size_t frameTimes     = 0;   // mFrameTimes array
    size_t xformFrames    = 0;   // mXformFrames outer pointer array
    size_t alphaFrames    = 0;   // mAlphaFrames pointer array
    size_t colorFrames    = 0;   // mColorFrames pointer array
    size_t soundFrames    = 0;   // mSoundFrames pointer array
    size_t stringTable    = 0;   // pStringTable

    // per-slot keyframe positions, parallel to the frame arrays
    // (0 = NULL slot or no information)
    std::vector<size_t> xformKeyframes;   // numTransforms*numFrames, transform-major
    std::vector<size_t> alphaKeyframes;   // numFrames
    std::vector<size_t> colorKeyframes;   // numFrames
    std::vector<size_t> soundKeyframes;   // numFrames
};

struct Animation {
    int32_t hasColor      = 0;
    int32_t hasAlpha      = 0;
    int32_t hasSound      = 0;
    int32_t hasTransform  = 0;
    int32_t numTransforms = 0;
    int32_t numFrames     = 0;

    std::vector<TransformType> transformTypes;   // numTransforms entries
    std::vector<float>         frameTimes;       // numFrames entries

    std::vector<KeyframeSlot> xformFrames;   // numTransforms*numFrames, transform-major
    std::vector<KeyframeSlot> alphaFrames;   // numFrames
    std::vector<KeyframeSlot> colorFrames;   // numFrames
    std::vector<KeyframeSlot> soundFrames;   // numFrames

    std::vector<std::string> strings;        // string table entries

    AnimFileOffsets offsets;                 // binary readers only, see above

    size_t xformIndex(int t, int f) const {
        return size_t(t) * size_t(numFrames) + size_t(f);
    }
    KeyframeSlot&       xformAt(int t, int f)       { return xformFrames[xformIndex(t, f)]; }
    const KeyframeSlot& xformAt(int t, int f) const { return xformFrames[xformIndex(t, f)]; }
};

// Container formats
// the data model above is shared by all container formats; the formats
// differ only in how they serialize it to disk.

enum AnimFormat {
    ANIM_XML,        // anim.xml      (TinyXML-2 text format)
    ANIM_BINLTL32,   // anim.binltl   (binary image, 32-bit offsets; 52-byte header)
    ANIM_BINLTL64,   // anim.binltl64 (binary image, 64-bit offsets; 80-byte header)
    ANIM_BINUNI,     // anim.binuni   (BINUNIAN magic + binltl64; 88-byte header)
    ANIM_UNKNOWN
};

// the 8-byte magic at the start of every .anim.binuni file (no NUL terminator).
static const char BINUNI_MAGIC[8] = { 'B', 'I', 'N', 'U', 'N', 'I', 'A', 'N' };

// Shared helpers

inline std::string TransformTypeName(int t)
{
    switch (t) {
    case XFORM_SCALE:     return "scale";
    case XFORM_ROTATE:    return "rotate";
    case XFORM_TRANSLATE: return "translate";
    default:              return "unknown";
    }
}

inline std::string InterpolationTypeName(int i)
{
    if (i == INTERPOLATION_NONE)   return "none";
    if (i == INTERPOLATION_LINEAR) return "linear";
    return std::to_string(i);          // unknown values survive XML round trips
}

// shortest decimal text that reads back as the exact same 32-bit float.
inline std::string FormatFloat(float v)
{
    char buf[64] = "";
    for (int prec = 6; prec <= 9; prec += 3) {   // %.9g always round-trips a float
        std::snprintf(buf, sizeof(buf), "%.*g", prec, double(v));
        float back = std::strtof(buf, nullptr);
        if (std::memcmp(&back, &v, sizeof(float)) == 0)
            break;
    }
    return std::string(buf);
}

inline size_t CountPresent(const std::vector<KeyframeSlot>& track)
{
    size_t n = 0;
    for (size_t i = 0; i < track.size(); ++i)
        if (track[i].present) ++n;
    return n;
}

inline bool AnySoundReference(const std::vector<KeyframeSlot>& track)
{
    for (size_t i = 0; i < track.size(); ++i)
        if (track[i].present && track[i].kf.soundStrIdx >= 0) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Keyframe soundStrIdx conventions
// ---------------------------------------------------------------------------
// In every binary format the keyframe soundStrIdx is a BYTE OFFSET into the
// animation's own string table: the game resolves it as
// pStringTable + soundStrIdx (verified against real game files and gootool's
// reader).  A value of 0 is the game's "no sound" (byte offset 0 is the
// table's leading empty string); the table itself always starts with that
// empty string, then the sound names.

// The shared data model (and the XML formats) use plain LIST INDEXES into
// Animation::strings instead, with -1 for "none".  The binary readers convert
// byte offsets -> list indexes, the binary writers list indexes -> byte
// offsets, so every representation stays internally consistent.

// on-disk byte offset -> data model list index (-1 when unresolvable)
inline int32_t SoundOffsetToIndex(int32_t raw, const std::vector<std::string>& strings)
{
    if (raw < 0) return -1;                  // negative: already "none"
    if (strings.empty()) return -1;          // no table to resolve against
    size_t off = 0;
    for (size_t i = 0; i < strings.size(); ++i) {
        if (off == size_t(raw)) return int32_t(i);
        off += strings[i].size() + 1;
    }
    return -1;   // mid-entry or past the end: not a resolvable reference
}

// data model list index -> on-disk byte offset (0 = the game's "no sound")
inline int32_t SoundIndexToOffset(int32_t idx, const std::vector<std::string>& strings)
{
    if (idx < 0 || strings.empty()) return 0;
    size_t off = 0;
    for (size_t i = 0; i < strings.size(); ++i) {
        if (i == size_t(idx)) return int32_t(off);
        off += strings[i].size() + 1;
    }
    return 0;    // out of range: treat as none
}

// converts every present keyframe's soundStrIdx from the on-disk byte offset
// convention to the data model's list index convention (call after the
// animation's string table has been parsed).
inline void SoundRefsOffsetsToIndexes(Animation& a)
{
    for (size_t i = 0; i < a.xformFrames.size(); ++i)
        if (a.xformFrames[i].present)
            a.xformFrames[i].kf.soundStrIdx =
                SoundOffsetToIndex(a.xformFrames[i].kf.soundStrIdx, a.strings);
    for (size_t i = 0; i < a.alphaFrames.size(); ++i)
        if (a.alphaFrames[i].present)
            a.alphaFrames[i].kf.soundStrIdx =
                SoundOffsetToIndex(a.alphaFrames[i].kf.soundStrIdx, a.strings);
    for (size_t i = 0; i < a.colorFrames.size(); ++i)
        if (a.colorFrames[i].present)
            a.colorFrames[i].kf.soundStrIdx =
                SoundOffsetToIndex(a.colorFrames[i].kf.soundStrIdx, a.strings);
    for (size_t i = 0; i < a.soundFrames.size(); ++i)
        if (a.soundFrames[i].present)
            a.soundFrames[i].kf.soundStrIdx =
                SoundOffsetToIndex(a.soundFrames[i].kf.soundStrIdx, a.strings);
}

// ---------------------------------------------------------------------------
// String table padding
// ---------------------------------------------------------------------------
// The game's writers NUL-align every string table blob - the movie-level
// table and each animation's own table - by appending empty string entries
// (4-byte alignment in the 32-bit image, 8-byte in the 64-bit ones).  The
// padding parses back as trailing empty entries, so a table that is copied
// between formats through the data model carries the source format's
// padding into the target file and misaligns it (e.g. an 8-entry all-empty
// animation table from a binuni becomes a 4-entry one in binltl).

// The binary writers therefore serialize a TRIMMED copy: trailing empty
// entries are dropped, but never below the highest referenced entry (a
// keyframe's soundStrIdx is a list index here, so referenced empties keep
// their index) and never below one entry for a table that exists at all.
// The writers' own alignment padding then restores whatever the target
// format expects, byte for byte.

// the trimmed copy of an animation's string table for binary serialization
inline std::vector<std::string> TrimmedAnimStrings(const Animation& a)
{
    size_t keep = a.strings.empty() ? 0 : 1;   // an existing table keeps >= 1 entry
    for (size_t i = 0; i < a.xformFrames.size(); ++i)
        if (a.xformFrames[i].present && a.xformFrames[i].kf.soundStrIdx >= 0)
            keep = (size_t(a.xformFrames[i].kf.soundStrIdx) + 1 > keep)
                       ? size_t(a.xformFrames[i].kf.soundStrIdx) + 1 : keep;
    for (size_t i = 0; i < a.alphaFrames.size(); ++i)
        if (a.alphaFrames[i].present && a.alphaFrames[i].kf.soundStrIdx >= 0)
            keep = (size_t(a.alphaFrames[i].kf.soundStrIdx) + 1 > keep)
                       ? size_t(a.alphaFrames[i].kf.soundStrIdx) + 1 : keep;
    for (size_t i = 0; i < a.colorFrames.size(); ++i)
        if (a.colorFrames[i].present && a.colorFrames[i].kf.soundStrIdx >= 0)
            keep = (size_t(a.colorFrames[i].kf.soundStrIdx) + 1 > keep)
                       ? size_t(a.colorFrames[i].kf.soundStrIdx) + 1 : keep;
    for (size_t i = 0; i < a.soundFrames.size(); ++i)
        if (a.soundFrames[i].present && a.soundFrames[i].kf.soundStrIdx >= 0)
            keep = (size_t(a.soundFrames[i].kf.soundStrIdx) + 1 > keep)
                       ? size_t(a.soundFrames[i].kf.soundStrIdx) + 1 : keep;

    std::vector<std::string> strings = a.strings;
    while (strings.size() > keep && strings.back().empty())
        strings.pop_back();
    return strings;
}

// the trimmed copy of a movie-level string table for binary serialization
// (actor references are re-interned by value on write, so nothing can point
// into the dropped region; the game's own files reference the empty entry
// at the first byte behind the real content, which is where the writers
// re-intern an empty string on demand)
inline std::vector<std::string> TrimmedMovieStrings(const std::vector<std::string>& strings)
{
    std::vector<std::string> out = strings;
    while (!out.empty() && out.back().empty())
        out.pop_back();
    return out;
}

#endif // BINANIMATION_H