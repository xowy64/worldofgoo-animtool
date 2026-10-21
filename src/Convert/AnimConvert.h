// AnimConvert.h: format detection, any-to-any conversion, recursive folder
// conversion and console reporting.  Ties AnimBinltl32, AnimBinltl64 and
// XmlConvert together so main.cpp only deals with command line handling.

#ifndef ANIMCONVERT_H
#define ANIMCONVERT_H

#include <string>
#include <vector>

#include "BinAnimation.h"

namespace AnimConvert {

// Formats (The AnimFormat enum itself lives in BinAnimation.h.)

const char* FormatName(AnimFormat f);                 // "xml", "binltl", ...
AnimFormat FormatFromExtension(const std::string& path);
std::string ExtensionFor(AnimFormat f);               // ".xml", ".binltl", ...
AnimFormat DetectFormat(const std::string& path);     // magic + extension + content

bool PathExists(const std::string& path);             // file OR directory
bool IsDirectory(const std::string& path);

// "foo.anim.binltl64" + ANIM_XML -> "foo.anim.xml"
std::string DefaultOutputPath(const std::string& inPath, AnimFormat outFormat);

// reads/writes any supported format (dispatch to the right converter).
bool ReadAny (const std::string& path, AnimFormat fmt, Animation& out, std::string& err);
bool WriteAny(const std::string& path, AnimFormat fmt, const Animation& anim, std::string& err);

// converts one file. inFormat ANIM_UNKNOWN -> detected from the file.
// outFormat ANIM_UNKNOWN -> XML for binary inputs, binltl for XML input.
// on success *loadedAnim (if not null) receives the animation.
bool ConvertFile(const std::string& inPath, const std::string& outPath,
                 AnimFormat inFormat, AnimFormat outFormat,
                 Animation* loadedAnim, std::string& err);

// Recursive folder conversion

struct FolderStats {
    int converted;
    int skippedSameFormat;     // input already in the target format
    int skippedExisting;       // output already on disk and skipExisting=true
    int skippedUnknown;        // not an animation file at all
    int failed;
    std::vector<std::string> failedFiles;

    FolderStats()
        : converted(0), skippedSameFormat(0), skippedExisting(0),
        skippedUnknown(0), failed(0) {}
};

// converts every animation file found under inDir (recursively) to outFormat,
// recreating the directory structure under outDir. binuniFix selects the
// v1.6 layout for movie.binuni output (see MovieBinuni::Write); anim.binuni
// has no v1.6 variant and is always written in the classic layout.

// Output naming / collisions:
//   "this.anim.binltl" -> "this.anim.xml".  If several source files share
//   one base name (this.anim.binltl, this.anim.binltl64, this.anim.binuni),
//   the first in sorted order keeps the plain name and the others get the
//   source format inserted: "this.anim.binltl64.xml", "this.anim.binuni.xml".

// Overwriting:
//   Existing output files are overwritten by default; with skipExisting=true
//   conversions whose output file already exists are skipped instead.
//   Files already in the target format and non-animation files are always
//   skipped (not copied).  A file that fails to convert is reported on stderr
//   and does not abort the run (see FolderStats).
bool ConvertFolder(const std::string& inDir, const std::string& outDir,
                   AnimFormat outFormat, bool skipExisting, bool verbose, bool quiet,
                   FolderStats& stats, std::string& err, bool binuniFix = false);

// Console reporting

void PrintSummary(const Animation& a, const std::string& inPath, const std::string& outPath);
void PrintVerboseDump(const Animation& a, const std::string& title);

} // namespace AnimConvert

#endif // ANIMCONVERT_H