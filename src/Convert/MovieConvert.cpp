// MovieConvert.cpp: movie format detection, conversion dispatch and console
// reporting. Mirrors the anim side of AnimConvert; folder conversion lives
// in AnimConvert::ConvertFolder, which routes the movie files it finds
// through the functions here (movie.binltl -> movie.xml etc., anims and
// movies are never converted into each other).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "BinMovie.h"
#include "XmlConvert.h"
#include "AnimConvert.h"     // PrintVerboseDump for the actor animations

namespace {

char asciiLower(char c)
{
    return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
}

bool endsWithCI(const std::string& s, const char* suffix)
{
    const size_t n = std::strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (asciiLower(s[s.size() - n + i]) != asciiLower(suffix[i]))
            return false;
    return true;
}

bool hasBinunimoMagic(const std::string& path)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[8];
    const size_t n = std::fread(buf, 1, 8, f);
    std::fclose(f);
    return n == 8 && std::memcmp(buf, BINUNIMO_MAGIC, 8) == 0;
}

// " (at 0x1a)" / " (kf at 0x1a)" - empty when off is 0 (no information)
std::string AtSuffix(size_t off, const char* what = nullptr)
{
    if (!off) return "";
    char buf[64];
    if (what) std::snprintf(buf, sizeof(buf), " (%s at 0x%zx)", what, off);
    else      std::snprintf(buf, sizeof(buf), " (at 0x%zx)", off);
    return std::string(buf);
}

// a plain "*.xml" that starts with "<movie" (movie XML with a plain name).
bool looksLikeMovieXml(const std::string& path)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    unsigned char buf[64];
    const size_t n = std::fread(buf, 1, sizeof(buf), f);
    std::fclose(f);
    size_t i = 0;
    if (n >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) i = 3;  // UTF-8 BOM
    while (i < n && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n'))
        ++i;
    if (n - i < 7 || std::memcmp(buf + i, "<movie", 6) != 0) return false;
    const unsigned char c = buf[i + 6];
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '>' || c == '/';
}

} // namespace

// Formats

const char* MovieConvert::FormatName(MovieFormat f)
{
    switch (f) {
    case MOVIE_XML:      return "xml";
    case MOVIE_BINLTL32: return "binltl";
    case MOVIE_BINLTL64: return "binltl64";
    case MOVIE_BINUNI:   return "binuni";
    default:             return "unknown";
    }
}

MovieFormat MovieConvert::FormatFromExtension(const std::string& path)
{
    if (endsWithCI(path, ".movie.binltl64")) return MOVIE_BINLTL64;   // before .movie.binltl!
    if (endsWithCI(path, ".movie.binuni"))   return MOVIE_BINUNI;
    if (endsWithCI(path, ".movie.binltl"))   return MOVIE_BINLTL32;
    if (endsWithCI(path, ".movie.xml"))      return MOVIE_XML;
    return MOVIE_UNKNOWN;   // plain .xml / .binltl / ... are the anim family
}

std::string MovieConvert::ExtensionFor(MovieFormat f)
{
    switch (f) {
    case MOVIE_XML:      return ".movie.xml";
    case MOVIE_BINLTL32: return ".movie.binltl";
    case MOVIE_BINLTL64: return ".movie.binltl64";
    case MOVIE_BINUNI:   return ".movie.binuni";
    default:             return "";
    }
}

MovieFormat MovieConvert::FormatFor(AnimFormat f)
{
    switch (f) {
    case ANIM_XML:      return MOVIE_XML;
    case ANIM_BINLTL32: return MOVIE_BINLTL32;
    case ANIM_BINLTL64: return MOVIE_BINLTL64;
    case ANIM_BINUNI:   return MOVIE_BINUNI;
    default:            return MOVIE_UNKNOWN;
    }
}

MovieFormat MovieConvert::DetectFormat(const std::string& path)
{
    if (hasBinunimoMagic(path)) return MOVIE_BINUNI;   // the magic always wins
    const MovieFormat byExt = FormatFromExtension(path);
    if (byExt != MOVIE_UNKNOWN) return byExt;
    if (looksLikeMovieXml(path)) return MOVIE_XML;
    return MOVIE_UNKNOWN;
}

bool MovieConvert::DetectBinuniLayoutRemastered(const std::string& path)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t buf[0x20];
    const size_t n = std::fread(buf, 1, sizeof(buf), f);
    std::fclose(f);
    if (n < sizeof(buf) || std::memcmp(buf, BINUNIMO_MAGIC, 8) != 0) return false;
    return ProbeBinActorStride(buf, n, 8) == 36;
}

std::string MovieConvert::DefaultOutputPath(const std::string& inPath, MovieFormat outFormat)
{
    std::string base = inPath;
    const size_t slash = base.find_last_of("/\\");
    const size_t afterSlash = (slash == std::string::npos) ? 0 : slash + 1;
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot > afterSlash)
        base.resize(dot);
    // ExtensionFor() already carries the ".movie" family marker, so a
    // trailing one in the base name must go: "seasonDisplay.movie.binltl"
    // -> "seasonDisplay.movie.xml", not "seasonDisplay.movie.movie.xml".
    while (base.size() > 6 && endsWithCI(base, ".movie"))
        base.resize(base.size() - 6);
    return base + ExtensionFor(outFormat);
}

// Conversion

bool MovieConvert::ReadAny(const std::string& path, MovieFormat fmt, Movie& out, std::string& err)
{
    switch (fmt) {
    case MOVIE_XML:      return XmlConvert::ReadMovie(path, out, err);
    case MOVIE_BINLTL32: return MovieBinltl32::Read(path, out, err);
    case MOVIE_BINLTL64: return MovieBinltl64::Read(path, out, err);
    case MOVIE_BINUNI:   return MovieBinuni::Read(path, out, err);
    default:
        err = "internal error: unknown movie input format";
        return false;
    }
}

bool MovieConvert::WriteAny(const std::string& path, MovieFormat fmt, const Movie& movie,
                            std::string& err, bool binuniFix)
{
    switch (fmt) {
    case MOVIE_XML:      return XmlConvert::WriteMovie(path, movie, err);
    case MOVIE_BINLTL32: return MovieBinltl32::Write(path, movie, err);
    case MOVIE_BINLTL64: return MovieBinltl64::Write(path, movie, err);
    case MOVIE_BINUNI:   return MovieBinuni::Write(path, movie, err, binuniFix);
    default:
        err = "internal error: unknown movie output format";
        return false;
    }
}

bool MovieConvert::ConvertFile(const std::string& inPath, const std::string& outPath,
                               MovieFormat inFormat, MovieFormat outFormat,
                               Movie* loadedMovie, std::string& err, bool binuniFix)
{
    if (inFormat == MOVIE_UNKNOWN) {
        inFormat = DetectFormat(inPath);
        if (inFormat == MOVIE_UNKNOWN) {
            err = "cannot determine the input format of '" + inPath +
                  "' (expected .movie.xml / .movie.binltl / .movie.binltl64 /"
                  " .movie.binuni)";
            return false;
        }
    }
    if (outFormat == MOVIE_UNKNOWN)
        outFormat = (inFormat == MOVIE_XML) ? MOVIE_BINLTL32 : MOVIE_XML;

    Movie movie;
    if (!ReadAny(inPath, inFormat, movie, err)) return false;
    if (!WriteAny(outPath, outFormat, movie, err, binuniFix)) return false;
    if (loadedMovie) *loadedMovie = std::move(movie);
    return true;
}

// Console reporting

void MovieConvert::PrintSummary(const Movie& m, const std::string& inPath,
                                const std::string& outPath)
{
    int images = 0, texts = 0;
    for (size_t i = 0; i < m.actors.size(); ++i)
        (m.actors[i].type == eActorType_Text) ? ++texts : ++images;
    std::printf("%s -> %s: movie, length=%s, %d actor(s) (%d image, %d text), %d string(s)",
                inPath.c_str(), outPath.c_str(), FormatFloat(m.length).c_str(),
                int(m.actors.size()), images, texts, int(m.strings.size()));
    if (!m.sounds.empty())
        std::printf(", %d movie sound(s)", int(m.sounds.size()));
    std::printf("\n");
}

void MovieConvert::PrintVerboseDump(const Movie& m, const std::string& title)
{
    std::printf("--- %s ---\n", title.c_str());
    std::printf("  length      : %s\n", FormatFloat(m.length).c_str());
    std::printf("  string table: %d string(s)%s\n", int(m.strings.size()),
                AtSuffix(m.offsets.stringTable).c_str());
    for (size_t i = 0; i < m.strings.size(); ++i)
        std::printf("    [%d] \"%s\"\n", int(i), m.strings[i].c_str());

    if (m.offsets.actors || m.offsets.anims) {
        std::printf("  actors      : %d", int(m.actors.size()));
        if (m.offsets.actors) std::printf(", BinActor array at 0x%zx", m.offsets.actors);
        if (m.offsets.anims)  std::printf(", ppAnims at 0x%zx", m.offsets.anims);
        if (m.readAsRemastered)     std::printf(", v1.6 layout (36-byte BinActor)");
        std::printf("\n");
    }

    for (size_t i = 0; i < m.actors.size(); ++i) {
        const MovieActor& a = m.actors[i];
        std::printf("  actor %d: %s depth=%s align=%s", int(i),
                    ActorTypeName(int(a.type)).c_str(),
                    FormatFloat(a.depth).c_str(),
                    AlignmentName(int(a.labelJustification)).c_str());
        if (a.fixup != 0)
            std::printf(" fixup=%d", int(a.fixup));
        std::printf("\n");
        if (a.type == eActorType_Text || !a.font.empty() || !a.labelText.empty() ||
            a.labelMaxWidth != 0.0f || a.labelWrapWidth != 0.0f) {
            std::printf("    text : font=\"%s\" max-width=%s wrap-width=%s\n",
                        a.font.c_str(), FormatFloat(a.labelMaxWidth).c_str(),
                        FormatFloat(a.labelWrapWidth).c_str());
            std::printf("    label: \"%s\"\n", a.labelText.c_str());
        }
        if (a.type == eActorType_Image || !a.image.empty())
            std::printf("    image: \"%s\"\n", a.image.c_str());
        AnimConvert::PrintVerboseDump(a.anim,
                                      "actor " + std::to_string(i) + " animation");
    }

    if (!m.sounds.empty()) {
        std::printf("  movie sounds:\n");
        for (size_t i = 0; i < m.sounds.size(); ++i)
            std::printf("    time=%s id=\"%s\"\n",
                        FormatFloat(m.sounds[i].time).c_str(),
                        m.sounds[i].id.c_str());
    }
}