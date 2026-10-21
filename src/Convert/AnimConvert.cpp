// AnimConvert.cpp: format detection, conversion dispatch, recursive folder
// conversion (portable, no <filesystem> used) and console reporting.

// Output file policy (both modes), decided as follows:
//  * Existing output files are OVERWRITTEN by default, so re-running a
//  conversion is idempotent.  skipExisting (--skip-existing) skips
//  conversions whose output file already exists instead.
//  * Two inputs converted in one folder run never overwrite each other:
//  if several source files share one base name, the first in sorted
//  order keeps the plain output name and the others get the source
//  format inserted (this.anim.binuni -> this.anim.binuni.xml).
//  * Files already in the target format are skipped, not copied.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "AnimConvert.h"
#include "XmlConvert.h"
#include "AnimBinltl32.h"
#include "AnimBinltl64.h"
#include "BinMovie.h"

#ifdef _WIN32
#  include <direct.h>      // _mkdir
#  include <io.h>          // _findfirst, _findnext, _findclose
#  include <sys/stat.h>
#  include <sys/types.h>
#else
#  include <dirent.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#endif

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

// File system

bool makeOneDir(const std::string& path)
{
#ifdef _WIN32
    return _mkdir(path.c_str()) == 0;
#else
    return mkdir(path.c_str(), 0755) == 0;
#endif
}

// creates 'path' and all missing parent directories.
bool makeDirs(const std::string& path, std::string& err)
{
    for (size_t i = 1; i <= path.size(); ++i) {
        if (i < path.size() && path[i] != '/' && path[i] != '\\')
            continue;
        const std::string part = path.substr(0, i);
        if (part.empty() || part == "/") continue;
        if (part.size() == 2 && part[1] == ':') continue;                   // "C:"
        if (part.size() == 3 && part[1] == ':' &&
            (part[2] == '/' || part[2] == '\\')) continue;                  // "C:\"
        if (!AnimConvert::IsDirectory(part) && !makeOneDir(part)) {
            err = "cannot create directory '" + part + "'";
            return false;
        }
    }
    return true;
}

// lists the entries of 'dir' (names only, no "." / "..").
bool listDir(const std::string& dir, std::vector<std::string>& names,
             std::vector<bool>& isDirs, std::string& err)
{
    names.clear();
    isDirs.clear();
#ifdef _WIN32
    std::string pattern = dir + "/*";
    for (size_t i = 0; i < pattern.size(); ++i)
        if (pattern[i] == '/') pattern[i] = '\\';
    _finddata_t fd;
    const intptr_t h = _findfirst(pattern.c_str(), &fd);
    if (h == -1) {
        err = "cannot read directory '" + dir + "'";
        return false;
    }
    do {
        const std::string name(fd.name);
        if (name == "." || name == "..") continue;
        names.push_back(name);
        isDirs.push_back((fd.attrib & _A_SUBDIR) != 0);
    } while (_findnext(h, &fd) == 0);
    _findclose(h);
    return true;
#else
    DIR* dp = opendir(dir.c_str());
    if (!dp) {
        err = "cannot read directory '" + dir + "'";
        return false;
    }
    for (;;) {
        struct dirent* de = readdir(dp);
        if (!de) break;
        const std::string name(de->d_name);
        if (name == "." || name == "..") continue;
        struct stat st;
        const std::string full = dir + "/" + name;
        names.push_back(name);
        isDirs.push_back(stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
    }
    closedir(dp);
    return true;
#endif
}

// collects the relative paths of all files under root/rel, recursively.
bool collectFiles(const std::string& root, const std::string& rel,
                  std::vector<std::string>& files, std::string& err)
{
    const std::string dir = rel.empty() ? root : root + "/" + rel;
    std::vector<std::string> names;
    std::vector<bool> isDirs;
    if (!listDir(dir, names, isDirs, err)) return false;
    for (size_t i = 0; i < names.size(); ++i) {
        const std::string child = rel.empty() ? names[i] : rel + "/" + names[i];
        if (isDirs[i]) {
            if (!collectFiles(root, child, files, err)) return false;
        } else {
            files.push_back(child);
        }
    }
    return true;
}

std::string stripTrailingSlashes(std::string s)
{
    while (s.size() > 1 && (s[s.size() - 1] == '/' || s[s.size() - 1] == '\\'))
        s.resize(s.size() - 1);
    return s;
}

// Content sniffing

bool hasBinuniMagic(const std::string& path)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[8];
    const size_t n = std::fread(buf, 1, 8, f);
    std::fclose(f);
    return n == 8 && std::memcmp(buf, BINUNI_MAGIC, 8) == 0;
}

bool looksLikeXml(const std::string& path)
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
    return i < n && buf[i] == '<';
}

} // namespace

// Formats

const char* AnimConvert::FormatName(AnimFormat f)
{
    switch (f) {
    case ANIM_XML:      return "xml";
    case ANIM_BINLTL32: return "binltl";
    case ANIM_BINLTL64: return "binltl64";
    case ANIM_BINUNI:   return "binuni";
    default:            return "unknown";
    }
}

AnimFormat AnimConvert::FormatFromExtension(const std::string& path)
{
    // movie files are a separate format family (foo.movie.binltl etc) and
    // are handled by MovieConvert, this makes sure the checks below do not
    // swallow them ("foo.movie.binltl" also ends with ".binltl")
    if (endsWithCI(path, ".movie.binltl64") ||
        endsWithCI(path, ".movie.binuni")   ||
        endsWithCI(path, ".movie.binltl")   ||
        endsWithCI(path, ".movie.xml"))
        return ANIM_UNKNOWN;

    if (endsWithCI(path, ".binltl64")) return ANIM_BINLTL64;   // before .binltl!
    if (endsWithCI(path, ".binuni"))   return ANIM_BINUNI;
    if (endsWithCI(path, ".binltl"))   return ANIM_BINLTL32;
    if (endsWithCI(path, ".xml"))      return ANIM_XML;
    return ANIM_UNKNOWN;
}

std::string AnimConvert::ExtensionFor(AnimFormat f)
{
    switch (f) {
    case ANIM_XML:      return ".xml";
    case ANIM_BINLTL32: return ".binltl";
    case ANIM_BINLTL64: return ".binltl64";
    case ANIM_BINUNI:   return ".binuni";
    default:            return "";
    }
}

AnimFormat AnimConvert::DetectFormat(const std::string& path)
{
    if (hasBinuniMagic(path)) return ANIM_BINUNI;   // the magic always wins
    const AnimFormat byExt = FormatFromExtension(path);
    if (byExt != ANIM_UNKNOWN) return byExt;
    if (looksLikeXml(path)) return ANIM_XML;
    return ANIM_UNKNOWN;
}

bool AnimConvert::PathExists(const std::string& path)
{
    if (path.empty()) return false;
#ifdef _WIN32
    struct _stat st;
    return _stat(path.c_str(), &st) == 0;
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0;
#endif
}

bool AnimConvert::IsDirectory(const std::string& path)
{
    if (path.empty()) return false;
#ifdef _WIN32
    struct _stat st;
    return _stat(path.c_str(), &st) == 0 && (st.st_mode & _S_IFDIR) != 0;
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

std::string AnimConvert::DefaultOutputPath(const std::string& inPath, AnimFormat outFormat)
{
    std::string base = inPath;
    const size_t slash = base.find_last_of("/\\");
    const size_t afterSlash = (slash == std::string::npos) ? 0 : slash + 1;
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot > afterSlash)
        base.resize(dot);
    return base + ExtensionFor(outFormat);
}

// Conversion

bool AnimConvert::ReadAny(const std::string& path, AnimFormat fmt, Animation& out, std::string& err)
{
    switch (fmt) {
    case ANIM_XML:      return XmlConvert::Read(path, out, err);
    case ANIM_BINLTL32: return AnimBinltl32::Read(path, out, err);
    case ANIM_BINLTL64: return AnimBinltl64::Read(path, out, err);
    case ANIM_BINUNI:   return AnimBinuni::Read(path, out, err);
    default:
        err = "internal error: unknown input format";
        return false;
    }
}

bool AnimConvert::WriteAny(const std::string& path, AnimFormat fmt, const Animation& anim, std::string& err)
{
    switch (fmt) {
    case ANIM_XML:      return XmlConvert::Write(path, anim, err);
    case ANIM_BINLTL32: return AnimBinltl32::Write(path, anim, err);
    case ANIM_BINLTL64: return AnimBinltl64::Write(path, anim, err);
    case ANIM_BINUNI:   return AnimBinuni::Write(path, anim, err);
    default:
        err = "internal error: unknown output format";
        return false;
    }
}

bool AnimConvert::ConvertFile(const std::string& inPath, const std::string& outPath,
                              AnimFormat inFormat, AnimFormat outFormat,
                              Animation* loadedAnim, std::string& err)
{
    if (inFormat == ANIM_UNKNOWN) {
        inFormat = DetectFormat(inPath);
        if (inFormat == ANIM_UNKNOWN) {
            err = "cannot determine the input format of '" + inPath +
                  "' (expected .xml / .binltl / .binltl64 / .binuni)";
            return false;
        }
    }
    if (outFormat == ANIM_UNKNOWN)
        outFormat = (inFormat == ANIM_XML) ? ANIM_BINLTL32 : ANIM_XML;

    Animation anim;
    if (!ReadAny(inPath, inFormat, anim, err)) return false;
    if (!WriteAny(outPath, outFormat, anim, err)) return false;
    if (loadedAnim) *loadedAnim = std::move(anim);
    return true;
}

bool AnimConvert::ConvertFolder(const std::string& inDirIn, const std::string& outDirIn,
                                AnimFormat outFormat, bool skipExisting, bool verbose, bool quiet,
                                FolderStats& stats, std::string& err, bool binuniFix)
{
    const std::string inDir  = stripTrailingSlashes(inDirIn);
    const std::string outDir = stripTrailingSlashes(outDirIn);

    if (!IsDirectory(inDir)) {
        err = "'" + inDir + "' is not a directory";
        return false;
    }
    if (PathExists(outDir) && !IsDirectory(outDir)) {
        err = "output path '" + outDir + "' exists and is not a directory";
        return false;
    }
    if (outFormat == ANIM_UNKNOWN) outFormat = ANIM_XML;

    std::vector<std::string> files;
    if (!collectFiles(inDir, "", files, err)) return false;
    std::sort(files.begin(), files.end());        // deterministic processing order
        // (also decides who keeps the
        // plain output name on collisions)
    if (!makeDirs(outDir, err)) return false;

    // output paths claimed by this run. A later input whose natural output
    // name is already claimed gets the source format inserted into its name,
    // so two conversions never overwrite each other.
    std::set<std::string> usedOutputs;

    for (size_t i = 0; i < files.size(); ++i) {
        const std::string& rel = files[i];
        const std::string inPath = inDir + "/" + rel;

        // movies are converted as movies (movie.binltl -> movie.xml etc.),
        // never as animations - the two format families never mix
        const MovieFormat inMovie = MovieConvert::DetectFormat(inPath);
        AnimFormat inFormat = ANIM_UNKNOWN;
        if (inMovie == MOVIE_UNKNOWN) {
            inFormat = DetectFormat(inPath);
            if (inFormat == ANIM_UNKNOWN) {
                ++stats.skippedUnknown;   // not an animation or movie file
                continue;
            }
        }

        const MovieFormat outMovie = MovieConvert::FormatFor(outFormat);
        bool sameFormat = (inMovie != MOVIE_UNKNOWN)
                              ? (inMovie == outMovie)
                              : (inFormat == outFormat);
        // movie.binuni has two on-disk layouts (the classic v1.5 one and the
        // 36-byte-BinActor v1.6 one), so a binuni -> binuni conversion still
        // runs when the layouts differ (e.g. --binuni -fix upgrading v1.5
        // files, or a plain --binuni normalizing v1.6 files back); layouts
        // are detected from the file, never from the extension.
        if (sameFormat && inMovie == MOVIE_BINUNI && outMovie == MOVIE_BINUNI)
            sameFormat = (MovieConvert::DetectBinuniLayoutRemastered(inPath) == binuniFix);
        if (sameFormat) {
            ++stats.skippedSameFormat;
            if (!quiet)
                std::printf("skip (already %s): %s\n", FormatName(outFormat), inPath.c_str());
            continue;
        }

        // natural output name: same place in the tree, target extension
        std::string base = rel;
        const size_t slash = base.find_last_of("/\\");
        const size_t afterSlash = (slash == std::string::npos) ? 0 : slash + 1;
        const size_t dot = base.find_last_of('.');
        if (dot != std::string::npos && dot > afterSlash)
            base.resize(dot);

        // movies: the target extension already carries the ".movie" marker
        // (this.movie.binltl -> this.movie.xml, not this.movie.movie.xml)
        if (inMovie != MOVIE_UNKNOWN)
            while (base.size() > 6 && endsWithCI(base, ".movie"))
                base.resize(base.size() - 6);

        std::string outPath = outDir + "/" + base +
                              ((inMovie != MOVIE_UNKNOWN) ? MovieConvert::ExtensionFor(outMovie)
                                                          : ExtensionFor(outFormat));
        if (usedOutputs.count(outPath) != 0) {
            // collision: insert the source format (this.movie.binuni.xml),
            // then numeric suffixes if even that is taken
            outPath = outDir + "/" + base + "." +
                      ((inMovie != MOVIE_UNKNOWN) ? MovieConvert::FormatName(inMovie)
                                                  : FormatName(inFormat)) +
                      ((inMovie != MOVIE_UNKNOWN) ? MovieConvert::ExtensionFor(outMovie)
                                                  : ExtensionFor(outFormat));
            int n = 2;
            while (usedOutputs.count(outPath) != 0) {
                outPath = outDir + "/" + base + "." +
                          ((inMovie != MOVIE_UNKNOWN) ? MovieConvert::FormatName(inMovie)
                                                      : FormatName(inFormat)) +
                          "." + std::to_string(n) +
                          ((inMovie != MOVIE_UNKNOWN) ? MovieConvert::ExtensionFor(outMovie)
                                                      : ExtensionFor(outFormat));
                ++n;
            }
        }
        // the name stays reserved even if the conversion is skipped below,
        // so a later input cannot silently take over this slot
        usedOutputs.insert(outPath);

        if (skipExisting && PathExists(outPath)) {
            ++stats.skippedExisting;
            if (!quiet)
                std::printf("skip (exists): %s\n", outPath.c_str());
            continue;
        }

        const size_t outSlash = outPath.find_last_of("/\\");
        if (outSlash != std::string::npos && !makeDirs(outPath.substr(0, outSlash), err))
            return false;

        if (inMovie != MOVIE_UNKNOWN) {
            Movie movie;
            std::string ferr;
            try {
                if (!MovieConvert::ReadAny(inPath, inMovie, movie, ferr) ||
                    !MovieConvert::WriteAny(outPath, outMovie, movie, ferr, binuniFix)) {
                    ++stats.failed;
                    stats.failedFiles.push_back(inPath);
                    std::fprintf(stderr, "error: %s\n", ferr.c_str());
                    continue;
                }
            } catch (const std::bad_alloc&) {
                ++stats.failed;
                stats.failedFiles.push_back(inPath);
                std::fprintf(stderr, "error: out of memory while converting '%s'\n",
                             inPath.c_str());
                continue;
            }
            ++stats.converted;
            if (verbose) {
                MovieConvert::PrintVerboseDump(movie, inPath);
                std::printf("wrote %s\n\n", outPath.c_str());
            } else if (!quiet) {
                MovieConvert::PrintSummary(movie, inPath, outPath);
            }
            continue;
        }

        Animation anim;
        std::string ferr;
        try {
            if (!ReadAny(inPath, inFormat, anim, ferr) ||
                !WriteAny(outPath, outFormat, anim, ferr)) {
                ++stats.failed;
                stats.failedFiles.push_back(inPath);
                std::fprintf(stderr, "error: %s\n", ferr.c_str());
                continue;
            }
        } catch (const std::bad_alloc&) {
            ++stats.failed;
            stats.failedFiles.push_back(inPath);
            std::fprintf(stderr, "error: out of memory while converting '%s'\n", inPath.c_str());
            continue;
        }
        ++stats.converted;

        if (verbose) {
            PrintVerboseDump(anim, inPath);
            std::printf("wrote %s\n\n", outPath.c_str());
        } else if (!quiet) {
            PrintSummary(anim, inPath, outPath);
        }
    }
    return true;
}

// Console reporting

void AnimConvert::PrintSummary(const Animation& a, const std::string& inPath,
                               const std::string& outPath)
{
    std::string types;
    for (size_t i = 0; i < a.transformTypes.size(); ++i) {
        if (i) types += ",";
        types += TransformTypeName(int(a.transformTypes[i]));
    }
    std::printf("%s -> %s: %d frame(s), %d transform(s) [%s], keyframes: xform=%d alpha=%d color=%d sound=%d, %d string(s)\n",
                inPath.c_str(), outPath.c_str(),
                int(a.numFrames), int(a.numTransforms),
                types.empty() ? "-" : types.c_str(),
                int(CountPresent(a.xformFrames)),
                int(CountPresent(a.alphaFrames)),
                int(CountPresent(a.colorFrames)),
                int(CountPresent(a.soundFrames)),
                int(a.strings.size()));
}

void AnimConvert::PrintVerboseDump(const Animation& a, const std::string& title)
{
    // " (at 0x1a)" / " (kf at 0x1a)" - empty when off is 0 (no information;
    // the XML formats carry no offsets, so XML inputs dump without them)
    auto AtSuffix = [](size_t off, const char* what) -> std::string {
        if (!off) return "";
        char buf[72];
        if (what) std::snprintf(buf, sizeof(buf), " (%s at 0x%zx)", what, off);
        else      std::snprintf(buf, sizeof(buf), " (at 0x%zx)", off);
        return std::string(buf);
    };
    auto kfOff = [](const std::vector<size_t>& v, size_t i) -> size_t {
        return (i < v.size()) ? v[i] : 0;
    };

    std::printf("--- %s ---\n", title.c_str());
    std::printf("  flags         : hasTransform=%d hasColor=%d hasAlpha=%d hasSound=%d\n",
                int(a.hasTransform), int(a.hasColor), int(a.hasAlpha), int(a.hasSound));
    if (a.offsets.base)
        std::printf("  image base    : 0x%zx\n", a.offsets.base);
    std::printf("  numTransforms : %d (", int(a.numTransforms));
    for (size_t i = 0; i < a.transformTypes.size(); ++i)
        std::printf("%s%s", i ? ", " : "",
                    TransformTypeName(int(a.transformTypes[i])).c_str());
    std::printf(")%s\n", AtSuffix(a.offsets.transformTypes, "types").c_str());
    std::printf("  numFrames     : %d%s\n", int(a.numFrames),
                AtSuffix(a.offsets.frameTimes, "times").c_str());
    {
        // the per-track keyframe pointer arrays, when their positions are known
        std::string tracks;
        auto append = [&](size_t off, const char* name) {
            if (!off) return;
            char buf[48];
            std::snprintf(buf, sizeof(buf), "%s%s at 0x%zx",
                          tracks.empty() ? "" : ", ", name, off);
            tracks += buf;
        };
        append(a.offsets.xformFrames, "xform");
        append(a.offsets.alphaFrames, "alpha");
        append(a.offsets.colorFrames, "color");
        append(a.offsets.soundFrames, "sound");
        if (!tracks.empty())
            std::printf("  track arrays  : %s\n", tracks.c_str());
    }
    std::printf("  string table  : %d string(s)%s\n", int(a.strings.size()),
                AtSuffix(a.offsets.stringTable, nullptr).c_str());
    for (size_t i = 0; i < a.strings.size(); ++i)
        std::printf("    [%d] \"%s\"\n", int(i), a.strings[i].c_str());

    auto dumpKf = [&](const char* label, const Keyframe& k, size_t off) {
        std::string snd;
        if (k.soundStrIdx >= 0 && size_t(k.soundStrIdx) < a.strings.size()) {
            snd = " \"";
            snd += a.strings[size_t(k.soundStrIdx)];
            snd += "\"";
        }
        std::printf("    %-20s x=%-12s y=%-12s angle=%-12s alpha=%-6d color=%-6d(0x%08X) "
                    "next=%-4d snd=%-4d%s interp=%s%s\n",
                    label,
                    FormatFloat(k.x).c_str(), FormatFloat(k.y).c_str(),
                    FormatFloat(k.angle).c_str(),
                    int(k.alpha), int(k.color), unsigned(k.color),
                    int(k.nextFrameIndex), int(k.soundStrIdx),
                    snd.c_str(), InterpolationTypeName(k.interpolation).c_str(),
                    AtSuffix(off, "kf").c_str());
    };
    auto dumpSlot = [&](const char* label, const KeyframeSlot& s, size_t off) {
        if (s.present) dumpKf(label, s.kf, off);
        else           std::printf("    %-20s NULL\n", label);
    };

    for (int32_t f = 0; f < a.numFrames; ++f) {
        std::printf("  frame %d (t=%s)\n", int(f),
                    FormatFloat(a.frameTimes[size_t(f)]).c_str());
        for (int32_t t = 0; t < a.numTransforms; ++t) {
            char label[48];
            std::snprintf(label, sizeof(label), "xform[%d] %s", int(t),
                          TransformTypeName(int(a.transformTypes[size_t(t)])).c_str());
            dumpSlot(label, a.xformAt(t, f), kfOff(a.offsets.xformKeyframes, a.xformIndex(t, f)));
        }
        dumpSlot("alpha", a.alphaFrames[size_t(f)], kfOff(a.offsets.alphaKeyframes, size_t(f)));
        dumpSlot("color", a.colorFrames[size_t(f)], kfOff(a.offsets.colorKeyframes, size_t(f)));
        dumpSlot("sound", a.soundFrames[size_t(f)], kfOff(a.offsets.soundKeyframes, size_t(f)));
    }
}