// main.cpp: command line front end for the World of Goo animation and
// movie converter.

#include <cstdio>
#include <cstring>
#include <exception>
#include <new>
#include <string>
#include <vector>

#include "AnimConvert.h"
#include "BinAnimation.h"
#include "BinMovie.h"

static const char* FlagForFormat(AnimFormat f)
{
    switch (f) {
    case ANIM_XML:      return "--xml";
    case ANIM_BINLTL32: return "--bin32";
    case ANIM_BINLTL64: return "--bin64";
    case ANIM_BINUNI:   return "--binuni";
    default:            return "(none)";
    }
}

static const char* PlainMovieExtension(MovieFormat f)
{
    switch (f) {
    case MOVIE_XML:      return ".xml";
    case MOVIE_BINLTL32: return ".binltl";
    case MOVIE_BINLTL64: return ".binltl64";
    case MOVIE_BINUNI:   return ".binuni";
    default:             return "";
    }
}

// "out.anim.xml" / "out.anim.binltl" / ... (as opposed to a plain ".xml")
static bool hasExplicitAnimExtension(const std::string& s)
{
    static const char* exts[4] = { ".anim.xml", ".anim.binltl64", ".anim.binuni", ".anim.binltl" };
    for (int i = 0; i < 4; ++i) {
        const size_t n = std::strlen(exts[i]);
        if (s.size() < n) continue;
        bool ok = true;
        for (size_t j = 0; j < n; ++j) {
            char a = s[s.size() - n + j], b = exts[i][j];
            if (a >= 'A' && a <= 'Z') a = char(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = char(b - 'A' + 'a');
            if (a != b) { ok = false; break; }
        }
        if (ok) return true;
    }
    return false;
}

static void printUsage(const char* argv0)
{
    std::printf(
        "World of Goo animation and movie converter:\n"
        "Supports: binltl, binltl64, binuni, and xml."
        "\n"
        "Usage:\n"
        "  %s [options] <input> [output]    single file\n"
        "  %s [options] <indir> [outdir]    whole folder, recursively\n"
        "\n"
        "Options:\n"
        "  -r, --recursive      Convert a folder recursively\n"
        "  -v, --verbose        Print the full contents (every actor/keyframe)\n"
        "  -q, --quiet          Print nothing except errors\n"
        "  -n, --skip-existing  Skip conversions whose output file already exists\n"
        "      --xml            Target format: anim.xml / movie.xml\n"
        "      --bin, --bin32   Target format: anim.binltl / movie.binltl (32-bit)\n"
        "      --bin64          Target format: anim.binltl64 / movie.binltl64\n"
        "      --binuni         Target format: anim.binuni / movie.binuni\n"
        "      --binuni -fix    movie.binuni in the remastered layout.\n"
        "  -h, --help           Show this help\n"
        "\n"
        "Examples:\n"
        "  %s this.anim.binltl                      -> this.anim.xml\n"
        "  %s this.movie.binltl this.movie.binuni   -> binuni\n"
        "  %s this.movie.xml --bin64                -> this.movie.binltl64\n"
        "  %s anim anim_xml                         -> whole folder to xml\n",
        argv0, argv0, argv0, argv0, argv0, argv0);
}

int main(int argc, char** argv)
{
    std::vector<std::string> args;
    bool verbose = false, quiet = false, recursive = false, skipExisting = false;
    bool binuniFix = false;
    AnimFormat target = ANIM_UNKNOWN;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if      (a == "-v" || a == "--verbose")       verbose      = true;
        else if (a == "-q" || a == "--quiet")         quiet        = true;
        else if (a == "-r" || a == "--recursive")     recursive    = true;
        else if (a == "-n" || a == "--skip-existing") skipExisting = true;
        else if (a == "--xml" || a == "--to-xml")     target       = ANIM_XML;
        else if (a == "--bin" || a == "--bin32" ||
                 a == "--to-binltl" || a == "--to-binltl32" || a == "--to-bin")
            target       = ANIM_BINLTL32;
        else if (a == "--bin64" || a == "--to-binltl64") target   = ANIM_BINLTL64;
        else if (a == "--binuni" || a == "--to-binuni") {
            target = ANIM_BINUNI;
            if (i + 1 < argc && (std::strcmp(argv[i + 1], "-fix") == 0 ||
                                 std::strcmp(argv[i + 1], "--fix") == 0)) {
                binuniFix = true;
                ++i;
            }
        }
        else if (a == "-fix" || a == "--fix") {
            std::fprintf(stderr,
                         "error: '%s' is only valid directly after --binuni\n\n",
                         a.c_str());
            printUsage(argv[0]);
            return 1;
        }
        else if (a == "-h" || a == "--help")          { printUsage(argv[0]); return 0; }
        else if (a.size() > 1 && a[0] == '-') {
            std::fprintf(stderr, "error: unknown option '%s'\n\n", a.c_str());
            printUsage(argv[0]);
            return 1;
        }
        else args.push_back(a);
    }

    if (args.empty() || args.size() > 2) {
        printUsage(argv[0]);
        return 1;
    }

    const std::string inPath = args[0];

    if (!AnimConvert::PathExists(inPath)) {
        std::fprintf(stderr, "error: '%s': No such file or directory\n", inPath.c_str());
        return 1;
    }

    // folder mode
    if (recursive || AnimConvert::IsDirectory(inPath)) {
        if (!AnimConvert::IsDirectory(inPath)) {
            std::fprintf(stderr,
                         "error: -r/--recursive given, but '%s' is a file, not a directory\n",
                         inPath.c_str());
            return 1;
        }
        std::string inDir = inPath;
        while (inDir.size() > 1 &&
               (inDir[inDir.size() - 1] == '/' || inDir[inDir.size() - 1] == '\\'))
            inDir.resize(inDir.size() - 1);
        const std::string outDir = (args.size() == 2) ? args[1] : inDir + "_out";
        const AnimFormat folderTarget = (target != ANIM_UNKNOWN) ? target : ANIM_XML;

        if (inDir == outDir && !quiet)
            std::printf("note: converting in place ('%s' -> '%s')\n", inDir.c_str(), outDir.c_str());

        AnimConvert::FolderStats stats;
        std::string err;
        bool ok = false;
        try {
            ok = AnimConvert::ConvertFolder(inDir, outDir, folderTarget, skipExisting,
                                            verbose, quiet, stats, err, binuniFix);
        } catch (const std::bad_alloc&) {
            std::fprintf(stderr, "error: out of memory\n");
            return 1;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "error: %s\n", e.what());
            return 1;
        }

        if (!ok) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }

        if (!quiet) {
            std::printf("\n%s -> %s [%s]: %d converted, %d skipped (already %s),"
                        " %d skipped (exists), %d not an animation/movie, %d failed\n",
                        inDir.c_str(), outDir.c_str(),
                        AnimConvert::FormatName(folderTarget),
                        stats.converted, stats.skippedSameFormat,
                        AnimConvert::FormatName(folderTarget),
                        stats.skippedExisting, stats.skippedUnknown, stats.failed);
        }
        return (stats.failed > 0) ? 1 : 0;
    }

    // single file mode: movies and animations are separate format families,
    // so first find out which one this is
    const MovieFormat movieIn = MovieConvert::DetectFormat(inPath);
    const AnimFormat inFormat = AnimConvert::DetectFormat(inPath);
    if (movieIn == MOVIE_UNKNOWN && inFormat == ANIM_UNKNOWN) {
        std::fprintf(stderr,
                     "error: cannot determine the input format of '%s'\n"
                     "       (expected .anim.* / .movie.* with .xml / .binltl / .binltl64 /"
                     " .binuni, or a BINUNIAN / BINUNIMO magic)\n",
                     inPath.c_str());
        return 1;
    }

    if (movieIn != MOVIE_UNKNOWN) {
        // movie
        if (args.size() == 2 && hasExplicitAnimExtension(args[1])) {
            std::fprintf(stderr,
                         "error: cannot convert the movie '%s' to an anim format.",
                         inPath.c_str());
            return 1;
        }

        MovieFormat outFormat = MovieConvert::FormatFor(target);
        if (outFormat == MOVIE_UNKNOWN && args.size() == 2) {
            const MovieFormat byExt = MovieConvert::FormatFromExtension(args[1]);
            if (byExt != MOVIE_UNKNOWN) {
                outFormat = byExt;
            } else {
                // a plain ".xml" / ".binltl" / ... output name means the
                // movie type of that format
                const AnimFormat plain = AnimConvert::FormatFromExtension(args[1]);
                if (plain != ANIM_UNKNOWN)
                    outFormat = MovieConvert::FormatFor(plain);
            }
        }
        if (outFormat == MOVIE_UNKNOWN)
            outFormat = (movieIn == MOVIE_XML) ? MOVIE_BINLTL32 : MOVIE_XML;

        if (binuniFix && outFormat != MOVIE_BINUNI)
            std::fprintf(stderr,
                         "note: --binuni -fix only affects movie.binuni output;"
                         " ignored for %s\n", MovieConvert::FormatName(outFormat));

        std::string outPath;
        if (args.size() == 2) {
            outPath = args[1];
            // an output name that does not name a format gets the target
            // extension appended ("seasonDisplay" -> "seasonDisplay.binltl")
            if (MovieConvert::FormatFromExtension(outPath) == MOVIE_UNKNOWN &&
                AnimConvert::FormatFromExtension(outPath) == ANIM_UNKNOWN) {
                outPath += PlainMovieExtension(outFormat);
                std::fprintf(stderr,
                             "note: the output name '%s' does not name a format;"
                             " writing %s as '%s'\n",
                             args[1].c_str(), MovieConvert::FormatName(outFormat),
                             outPath.c_str());
            }
        } else {
            outPath = MovieConvert::DefaultOutputPath(inPath, outFormat);
        }
        if (outPath == inPath) {
            std::fprintf(stderr,
                         "error: the output path is the same as the input ('%s');"
                         " pass an explicit output path\n",
                         inPath.c_str());
            return 1;
        }

        if (skipExisting && AnimConvert::PathExists(outPath)) {
            if (!quiet)
                std::printf("skip (exists): %s\n", outPath.c_str());
            return 0;
        }

        Movie        movie;
        std::string  err;
        bool ok = false;
        try {
            ok = MovieConvert::ConvertFile(inPath, outPath, movieIn, outFormat,
                                           &movie, err, binuniFix);
        } catch (const std::bad_alloc&) {
            std::fprintf(stderr, "error: out of memory\n");
            return 1;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "error: %s\n", e.what());
            return 1;
        }

        if (!ok) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }

        if (!quiet) {
            if (verbose) {
                MovieConvert::PrintVerboseDump(movie, inPath);
                std::printf("\nwrote %s\n", outPath.c_str());
            } else {
                MovieConvert::PrintSummary(movie, inPath, outPath);
            }
        }
        return 0;
    }

    // animation
    if (binuniFix)
        std::fprintf(stderr,
                     "note: --binuni -fix has no effect on animations.");
    if (args.size() == 2 && MovieConvert::FormatFromExtension(args[1]) != MOVIE_UNKNOWN) {
        std::fprintf(stderr,
                     "error: cannot convert the animation '%s' to a movie format.",
                     inPath.c_str());
        return 1;
    }

    // target format: explicit option > output name extension > default
    AnimFormat outFormat = target;
    if (outFormat == ANIM_UNKNOWN && args.size() == 2) {
        const AnimFormat byExt = AnimConvert::FormatFromExtension(args[1]);
        if (byExt != ANIM_UNKNOWN)
            outFormat = byExt;
    }
    if (outFormat == ANIM_UNKNOWN)
        outFormat = (inFormat == ANIM_XML) ? ANIM_BINLTL32 : ANIM_XML;

    if (args.size() == 2) {
        const AnimFormat byExt = AnimConvert::FormatFromExtension(args[1]);
        if (target != ANIM_UNKNOWN && byExt != ANIM_UNKNOWN && byExt != target) {
            std::fprintf(stderr,
                         "warning: output name '%s' suggests %s, but %s was requested;"
                         " writing %s\n",
                         args[1].c_str(), AnimConvert::FormatName(byExt),
                         FlagForFormat(target), AnimConvert::FormatName(target));
        } else if (target == ANIM_UNKNOWN && byExt == ANIM_UNKNOWN) {
            std::fprintf(stderr,
                         "note: cannot tell the format from the output name '%s';"
                         " writing %s\n",
                         args[1].c_str(), AnimConvert::FormatName(outFormat));
        }
    }

    const std::string outPath = (args.size() == 2) ? args[1]
                                                   : AnimConvert::DefaultOutputPath(inPath, outFormat);
    if (outPath == inPath) {
        std::fprintf(stderr,
                     "error: the output path is the same as the input ('%s');"
                     " pass an explicit output path\n",
                     inPath.c_str());
        return 1;
    }

    if (skipExisting && AnimConvert::PathExists(outPath)) {
        if (!quiet)
            std::printf("skip (exists): %s\n", outPath.c_str());
        return 0;
    }

    Animation   anim;
    std::string err;
    bool ok = false;
    try {
        ok = AnimConvert::ConvertFile(inPath, outPath, inFormat, outFormat, &anim, err);
    } catch (const std::bad_alloc&) {
        std::fprintf(stderr, "error: out of memory\n");
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }

    if (!ok) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }

    if (!quiet) {
        if (verbose) {
            AnimConvert::PrintVerboseDump(anim, inPath);
            std::printf("\nwrote %s\n", outPath.c_str());
        } else {
            AnimConvert::PrintSummary(anim, inPath, outPath);
        }
    }
    return 0;
}
