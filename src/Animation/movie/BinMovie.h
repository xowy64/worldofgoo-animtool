//  BinMovie.h

//  Shared data model for World of Goo movies, mirroring the game's
//  BinMovie / BinActor structures.  Used by:
//      MovieBinltl32  - binary ".movie.binltl" reader/writer
//      MovieBinltl64  - ".movie.binltl64" / ".movie.binuni" reader/writer
//      XmlConvert     - "movie.xml" reader/writer (TinyXML-2)
//      MovieConvert   - high level conversion + reporting glue
//      AnimConvert    - folder conversion dispatch (movie files found in a
//                       folder are converted as movies, never as anims)
//      main           - command line front end

//  ON-DISK LAYOUT OF ".movie.binltl" (32-bit, little endian)
//  The file is a flat image of the in-memory BinMovie struct; every pointer
//  is stored as a 32-bit file offset (0 == NULL):

//      offset  field
//      0x00    length                              (float32)
//      0x04    numActors                           (int32)
//      0x08    offset -> BinActor[numActors]
//      0x0C    offset -> offset[numActors]          (ppAnims), each entry an
//              offset to one complete BinImageAnimation image (BinAnimation.h;
//              the extra indirection lets every animation be one contiguous
//              block, which is how 2D Boy stored the original files)
//      0x10    offset -> pStringTable               (NUL separated strings)

//  BinActor (32 bytes), all string indexes into the BinMovie string table:
//      0x00    mType                (0 = image, 1 = text)
//      0x04    mImageStrIdx
//      0x08    mLabelTextStrIdx
//      0x0C    mFontStrIdx
//      0x10    mLabelMaxWidth       (float32)
//      0x14    mLabelWrapWidth      (float32)
//      0x18    mLabelJustification  (0 = left, 1 = center, 2 = right)
//      0x1C    mDepth               (float32)

//  ".movie.binltl64" is the same image with 64-bit pointers: the header grows
//  to 32 bytes (length, numActors, then 3 x 8-byte offsets at 0x08/0x10/0x18)
//  and the ppAnims entries are 8 bytes each. BinActor contains no pointers,
//  so it stays 32 bytes. ".movie.binuni" is movie.binltl64 with an 8-byte
//  "BINUNIMO" magic in front of the header.

//  Remastered binuni layout: Newer files (v1.6) keep the magic, the movie
//  header and every animation block unchanged, but grow BinActor from 32 to
//  36 bytes: one extra int32 field at offset 0x20, behind mDepth (always 0
//  in the files tested; the data model stores it in MovieActor::fixup and
//  round trips it verbatim). The two layouts share one magic, so they are
//  told apart from the on-disk geometry, not from the extension: the game
//  packs the BinActor array directly in front of the ppAnims array, so
//  (ppAnimsOffset - pActorsOffset) == numActors * 32 (v1.5) or
//  numActors * 36 (v1.6) - see ProbeBinActorStride() below. The reader
//  detects the layout automatically; the writer emits the classic v1.5
//  layout unless the --binuni -fix option selects the v1.6 one.

//  String references: BinActor string fields are byte offsets into the
//  BinMovie string table, the game resolves them as pStringTable + idx
//  (verified against gootool, which produced the reference dumps of the
//  original files). The table starts with one empty string; unused BinActor
//  fields reference it at byte offset 0. Each embedded BinImageAnimation
//  carries its own string table, and its keyframes soundStrIdx is likewise
//  a byte offset into that table (0 = "no sound"); the data model converts
//  these to/from list indexes, see BinAnimation.h.

//  String table padding: Every string table blob (movie-level and per
//  animation) is NUL-aligned by the game's writers - to 4 bytes in the
//  32-bit image, to 8 bytes in the 64-bit ones (the v1.6 type pads the
//  movie table to 4 but keeps animation tables at 8). The padding parses
//  back as trailing empty entries, so the binary writers serialize trimmed
//  tables (see TrimmedMovieStrings / TrimmedAnimStrings in BinAnimation.h)
//  and let their own alignment restore the target format's byte layout -
//  that is what keeps direct binary-to-binary conversions byte-identical
//  to the game's own files in either direction.

//  The embedded BinImageAnimation offsets are relative to the start of the
//  animation block (the blocks are standalone BinImageAnimation images,
//  which is what the ppAnims indirection is for). Movie-level pointers are
//  offsets from the start of the file. (The readers also accept anim blocks
//  whose internal offsets are file-relative, and note it when they see one.)

#ifndef BINMOVIE_H
#define BINMOVIE_H

#include <cstdint>
#include <string>
#include <vector>

#include "BinAnimation.h"

// Enums (names as in the game headers)

enum ActorType {
    eActorType_Image = 0,
    eActorType_Text  = 1
};

enum AlignmentH {
    ALIGN_LEFT   = 0,
    ALIGN_CENTER = 1,
    ALIGN_RIGHT  = 2
};

// Data model

// one animated scene element; strings are resolved from the BinMovie string
// table on read and interned back into it on write.
struct MovieActor {
    int32_t     type                = eActorType_Image;  // ActorType
    std::string image;                                   // mImageStrIdx
    std::string labelText;                               // mLabelTextStrIdx
    std::string font;                                    // mFontStrIdx
    float       labelMaxWidth       = -1.0f;
    float       labelWrapWidth      = -1.0f;
    int32_t     labelJustification  = ALIGN_CENTER;      // AlignmentH
    float       depth               = 0.0f;

    // the extra int32 in the v1.6 binuni layout appends to every BinActor
    // (offset 0x20 of the 36-byte record; 0 in the files tested). Only
    // written by the v1.6 type of the binuni writer, and only carried
    // through XML when non-zero (fixup="N" attribute on <actor>).
    int32_t     fixup               = 0;

    Animation   anim;   // exactly one per actor
};

// a movie-level sound. BinMovie has no field for these: the game plays
// movie sounds from the actor animations soundtracks, so the binary
// writers attach them to the first actor's animation as sound keyframes
// (see MovieBinltl32.cpp / MovieBinltl64.cpp and XmlConvert.cpp).
struct MovieSound {
    float       time = 0.0f;
    std::string id;
};

// Verbose-dump offsets (binary readers only; all zero after an XML read)

struct MovieFileOffsets {
    size_t actors       = 0;   // BinActor array
    size_t anims        = 0;   // ppAnims array
    size_t stringTable  = 0;   // pStringTable
    std::vector<size_t> animBlocks;   // per actor: first byte of its anim image
};

struct Movie {
    float                        length = 0.0f;
    std::vector<MovieActor>      actors;
    std::vector<std::string>     strings;   // BinMovie string table, verbatim
    std::vector<MovieSound>      sounds;    // xml-level only (see MovieSound)

    MovieFileOffsets offsets;           // binary readers only, see above
    bool readAsRemastered = false;      // read with the 36-byte BinActor layout
};

// Container formats (the model is shared, only the serialization differs)

enum MovieFormat {
    MOVIE_XML,       // movie.xml      (TinyXML-2 text format)
    MOVIE_BINLTL32,  // movie.binltl   (binary image, 32-bit offsets; 20-byte header)
    MOVIE_BINLTL64,  // movie.binltl64 (binary image, 64-bit offsets; 32-byte header)
    MOVIE_BINUNI,    // movie.binuni   (BINUNIMO magic + binltl64; 40-byte header)
    MOVIE_UNKNOWN
};

// the 8-byte magic at the start of every .movie.binuni file.
static const char BINUNIMO_MAGIC[8] = { 'B', 'I', 'N', 'U', 'N', 'I', 'M', 'O' };

// Shared helpers

inline std::string ActorTypeName(int t)
{
    switch (t) {
    case eActorType_Image: return "image";
    case eActorType_Text:  return "text";
    default:               return std::to_string(t);   // unknown values survive round trips
    }
}

inline std::string AlignmentName(int a)
{
    switch (a) {
    case ALIGN_LEFT:   return "left";
    case ALIGN_CENTER: return "center";
    case ALIGN_RIGHT:  return "right";
    default:           return std::to_string(a);
    }
}

// little endian readers for the header probe below
inline uint32_t rdLe32(const uint8_t* p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

inline uint64_t rdLe64(const uint8_t* p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | uint64_t(p[i]);
    return v;
}

// Guesses the BinActor stride of a 64-bit movie image (binltl64 or binuni;
// 'hdr' is the file offset of the length field) from its own header
// pointers: the game packs the BinActor array directly in front of the
// ppAnims array, so (ppAnims - pActors) == numActors * stride. Returns 32
// (the classic layout), 36 (the v1.6 layout) or 0 when the geometry does
// not identify one. Both offset conventions (file and header-relative)
// give the same answer because only the difference is used.
inline size_t ProbeBinActorStride(const uint8_t* d, size_t n, size_t hdr)
{
    if (n < hdr + 0x18) return 0;
    const int32_t  numActors = int32_t(rdLe32(d + hdr + 0x04));
    const uint64_t actorsOff = rdLe64(d + hdr + 0x08);
    const uint64_t animsOff  = rdLe64(d + hdr + 0x10);
    if (numActors <= 0 || actorsOff == 0 || animsOff <= actorsOff) return 0;
    const uint64_t gap = animsOff - actorsOff;
    if (gap == uint64_t(numActors) * 32) return 32;
    if (gap == uint64_t(numActors) * 36) return 36;
    return 0;
}

// Binary reader/writers (declarations; the .cpps are self-contained)

class MovieBinltl32 {
public:
    static bool Read (const std::string& path, Movie& out, std::string& err);
    static bool Write(const std::string& path, const Movie& movie, std::string& err);
};

class MovieBinltl64 {
public:
    static bool Read (const std::string& path, Movie& out, std::string& err);
    static bool Write(const std::string& path, const Movie& movie, std::string& err);
};

class MovieBinuni {
public:
    static bool Read (const std::string& path, Movie& out, std::string& err);
    // Writes the classic (v1.5) layout by default; fixupRemastered=true writes
    // the v1.6 layout with 36-byte BinActors (the --binuni -fix option).
    static bool Write(const std::string& path, const Movie& movie,
                      std::string& err, bool fixupRemastered = false);
};

// MovieConvert: format detection + dispatch, mirroring AnimConvert for
// movies. Folder conversion lives in AnimConvert::ConvertFolder, which
// routes the movie files it finds through these functions.

namespace MovieConvert {

const char* FormatName(MovieFormat f);                 // "xml", "binltl", ...
MovieFormat FormatFromExtension(const std::string& path);
std::string ExtensionFor(MovieFormat f);               // ".movie.xml", ...
MovieFormat FormatFor(AnimFormat animFormat);          // ANIM_XML -> MOVIE_XML, ...
MovieFormat DetectFormat(const std::string& path);     // magic + extension + content

// true when 'path' is a movie.binuni file whose BinActor array has the
// 36-byte stride of the v1.6 layout (false for anything else, including
// unreadable files - use DetectFormat/Read first to know it is a binuni).
bool DetectBinuniLayoutRemastered(const std::string& path);

// "foo.movie.binltl64" + MOVIE_XML -> "foo.movie.xml"
std::string DefaultOutputPath(const std::string& inPath, MovieFormat outFormat);

bool ReadAny (const std::string& path, MovieFormat fmt, Movie& out, std::string& err);
bool WriteAny(const std::string& path, MovieFormat fmt, const Movie& movie,
              std::string& err, bool binuniFix = false);

// converts one file. inFormat MOVIE_UNKNOWN -> detected from the file.
// outFormat MOVIE_UNKNOWN -> XML for binary inputs, binltl for XML input.
// binuniFix only affects movie.binuni output (v1.6 layout, see MovieBinuni::Write).
bool ConvertFile(const std::string& inPath, const std::string& outPath,
                 MovieFormat inFormat, MovieFormat outFormat,
                 Movie* loadedMovie, std::string& err, bool binuniFix = false);

// Console reporting

void PrintSummary(const Movie& m, const std::string& inPath, const std::string& outPath);
void PrintVerboseDump(const Movie& m, const std::string& title);

} // namespace MovieConvert

#endif // BINMOVIE_H