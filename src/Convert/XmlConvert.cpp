// XmlConvert.cpp: anim.xml reader/writer (TinyXML-2).
//  RULES
//  -----
//  * <transform> elements keep their document order; scale / rotate /
//     translate are the transform tracks (any number, any order - exactly
//     the binary mTransformTypes order).  At most one each of alpha, color
//     and sound; they map to the binary's separate mAlphaFrames /
//     mColorFrames / mSoundFrames arrays.
//   * Every keyframe writes only the fields its type uses:
//         scale: x, y      rotate: angle    translate: x, y
//         alpha: alpha     color: color     sound: soundStrIdx
//     Other fields may still be written explicitly; when omitted they get
//     the defaults the game's own converter produces (verified against real
//     .anim.binltl files):
//         x = y = angle = alpha = color = -1,  soundStrIdx = 0
//     Relevant fields omitted by hand default to their identity value
//     instead: scale x/y = 1, angle = 0, translate x/y = 0, alpha = 255,
//     color = 0, soundStrIdx = 0.
//   * nextFrameIndex: when omitted it is computed automatically as the
//     frameIndex of the next keyframe of the same track (-1 for the last) -
//     the same chaining the game's files follow.
//   * interpolation: "linear" when omitted.
//   * Only non-NULL keyframes are written; a frame without a <keyframe>
//     element is a NULL slot in the binary pointer array.
//   * Sound keyframes may use sound="NAME" instead of soundStrIdx="N"; the
//     name is looked up in <strings> and appended if missing.
//   * <strings> is only written when the table contains at least one
//     non-empty entry.  Game files carry a single empty string when no
//     sounds are used; the reader recreates that automatically, keeping
//     binary round trips byte-identical.  When <strings> is present its
//     entries match the original file's table exactly (indices included).
//   * The has* flags accept 0/1/true/false and are derived from the
//     <transform> elements present when omitted.
//   * <frameTimes> may be omitted; the frame count is then derived from the
//     highest keyframe frameIndex + 1 (all times 0).  When present, the
//     <time> entries must cover frames 0..N-1 exactly (frameIndex may be
//     omitted and then counts sequentially).

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "tinyxml2.h"
#include "XmlConvert.h"

namespace {

const int32_t kMaxFrames = INT32_MAX;  // limits
const int32_t kMaxStrings = 1 << 20;   // sanity cap for gap-filled tables

char asciiLower(char c)
{
    return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
}

std::string toLower(const char* s)
{
    std::string r(s ? s : "");
    for (size_t i = 0; i < r.size(); ++i)
        r[i] = asciiLower(r[i]);
    return r;
}

enum TrackKind {
    TRACK_XFORM,     // scale / rotate / translate
    TRACK_ALPHA,
    TRACK_COLOR,
    TRACK_SOUND
};

// "scale"/"rotate"/"translate" (+ XFORM_ prefix, + 0/1/2) -> TRACK_XFORM,
// "alpha"/"color"/"sound" -> the special tracks.
bool parseTrackKind(const char* s, TrackKind& kind, TransformType& xtype)
{
    if (!s || !*s) return false;
    std::string v = toLower(s);
    if (v.compare(0, 6, "xform_") == 0) v.erase(0, 6);
    if      (v == "scale"     || v == "0") { kind = TRACK_XFORM; xtype = XFORM_SCALE;     return true; }
    else if (v == "rotate"    || v == "1") { kind = TRACK_XFORM; xtype = XFORM_ROTATE;    return true; }
    else if (v == "translate" || v == "2") { kind = TRACK_XFORM; xtype = XFORM_TRANSLATE; return true; }
    else if (v == "alpha")  { kind = TRACK_ALPHA; return true; }
    else if (v == "color")  { kind = TRACK_COLOR; return true; }
    else if (v == "sound")  { kind = TRACK_SOUND; return true; }
    return false;
}

bool parseInterpolation(const char* s, int32_t& out)
{
    if (!s || !*s) return false;
    const std::string v = toLower(s);
    if      (v == "none"   || v == "interpolation_none")   { out = INTERPOLATION_NONE;   return true; }
    else if (v == "linear" || v == "interpolation_linear") { out = INTERPOLATION_LINEAR; return true; }
    char* end = nullptr;
    const long n = std::strtol(s, &end, 10);
    if (end && *end == '\0') { out = int32_t(n); return true; }
    return false;
}

bool parseFlag(const char* s, int32_t& out)
{
    if (!s || !*s) return false;
    const std::string v = toLower(s);
    if (v == "0" || v == "false" || v == "no")  { out = 0; return true; }
    if (v == "1" || v == "true"  || v == "yes") { out = 1; return true; }
    return false;
}

// finds s in the string table, appending it if missing; returns its index.
int32_t internString(std::vector<std::string>& strings, const char* s)
{
    for (size_t i = 0; i < strings.size(); ++i)
        if (strings[i] == s) return int32_t(i);
    strings.push_back(s);
    return int32_t(strings.size() - 1);
}


// <strings> tables:
// The XML formats write only the non-empty entries of a string table, with
// their original indexes, so that tables with gaps (and the trailing
// padding that reads back as empty entries) stay readable. Reading fills
// every index that is not present with an empty string, which rebuilds the
// binary tables byte for byte: middle gaps become empty entries again and
// trailing empties are re-created by the binary writers' table padding.

// Reads the <string> children of one <strings> element into 'strings'.
// Indexes must not repeat or go backwards; 'next' is the lowest index this
// element may use (one past the previous element's last entry when a file
// has several <strings> sections) and is advanced past the last entry read.
bool readStringsEntries(const tinyxml2::XMLElement* st,
                        std::vector<std::string>& strings, int32_t& next,
                        const std::string& path, std::string& err)
{
    for (const tinyxml2::XMLElement* se = st->FirstChildElement("string");
         se; se = se->NextSiblingElement("string")) {
        const int32_t declared = se->IntAttribute("index", next);
        if (declared < next) {
            err = "'" + path + "': <string> entries must be in ascending index order"
                  " (expected index " + std::to_string(next) + " or higher, got " +
                  std::to_string(declared) + ")";
            return false;
        }
        if (declared >= kMaxStrings) {
            err = "'" + path + "': <string> index " + std::to_string(declared) +
                  " is too large";
            return false;
        }
        if (size_t(declared) >= strings.size())
            strings.resize(size_t(declared) + 1);   // gaps become empty strings
        const char* v = se->Attribute("value");
        if (!v) v = se->GetText();          // <string>NAME</string> works too
        strings[size_t(declared)] = (v ? v : "");
        next = declared + 1;
    }
    return true;
}

// true when the table has at least one non-empty entry (otherwise the
// writers omit <strings> entirely and the readers rebuild it)
bool anyNonEmptyString(const std::vector<std::string>& strings)
{
    for (size_t i = 0; i < strings.size(); ++i)
        if (!strings[i].empty()) return true;
    return false;
}

// keyfield defaults per track kind

struct KfDefaults {
    float   x, y, angle;
    int32_t alpha, color, soundStrIdx;
};

// omitted "unused" fields get the game's values (-1 / soundStrIdx 0),
// omitted "relevant" fields get an identity value instead (see header).
KfDefaults DefaultsFor(TrackKind kind, TransformType xtype)
{
    KfDefaults d;
    d.x = -1.0f; d.y = -1.0f; d.angle = -1.0f;
    d.alpha = -1; d.color = -1; d.soundStrIdx = 0;
    switch (kind) {
        case TRACK_XFORM:
            if      (xtype == XFORM_SCALE)     { d.x = 1.0f; d.y = 1.0f; }
            else if (xtype == XFORM_ROTATE)    { d.angle = 0.0f; }
            else /* XFORM_TRANSLATE */         { d.x = 0.0f; d.y = 0.0f; }
            break;
        case TRACK_ALPHA: d.alpha = 255; break;
        case TRACK_COLOR: d.color = 0;   break;
        case TRACK_SOUND: d.soundStrIdx = 0; break;
    }
    return d;
}

// Reading

struct RawKeyframe {
    int32_t  frameIndex = 0;
    Keyframe kf;
    bool     hasNext = false;    // nextFrameIndex attribute was present
};

struct RawTransform {
    TrackKind                  kind  = TRACK_XFORM;
    TransformType              xtype = XFORM_SCALE;
    std::vector<RawKeyframe>   kfs;
};

bool readKeyframeElement(const tinyxml2::XMLElement* e, TrackKind kind,
                         TransformType xtype, std::vector<std::string>& strings,
                         RawKeyframe& rk, const std::string& path, std::string& err)
{
    const KfDefaults def = DefaultsFor(kind, xtype);
    Keyframe& kf = rk.kf;

    if (const char* s = e->Attribute("frameIndex")) {
        rk.frameIndex = e->IntAttribute("frameIndex", -1);
        if (rk.frameIndex < 0 || rk.frameIndex >= kMaxFrames) {
            err = "'" + path + "': invalid frameIndex '" + s + "' on <keyframe>";
            return false;
        }
    } else {
        err = "'" + path + "': <keyframe> is missing the frameIndex attribute";
        return false;
    }
    const std::string ctx = "'" + path + "', frame " + std::to_string(rk.frameIndex);

    kf.x     = e->Attribute("x")     ? e->FloatAttribute("x", def.x)         : def.x;
    kf.y     = e->Attribute("y")     ? e->FloatAttribute("y", def.y)         : def.y;
    kf.angle = e->Attribute("angle") ? e->FloatAttribute("angle", def.angle) : def.angle;
    kf.alpha = e->Attribute("alpha") ? e->IntAttribute("alpha", def.alpha)   : def.alpha;
    kf.color = e->Attribute("color") ? e->IntAttribute("color", def.color)   : def.color;

    if (e->Attribute("nextFrameIndex")) {
        kf.nextFrameIndex = e->IntAttribute("nextFrameIndex", 0);
        rk.hasNext = true;
    } else {
        kf.nextFrameIndex = 0;       // computed after the whole file is read
        rk.hasNext = false;
    }

    if (e->Attribute("soundStrIdx")) {
        kf.soundStrIdx = e->IntAttribute("soundStrIdx", 0);
    } else if (const char* nm = e->Attribute("sound")) {
        kf.soundStrIdx = internString(strings, nm);
    } else {
        kf.soundStrIdx = def.soundStrIdx;
    }

    if (const char* ip = e->Attribute("interpolation")) {
        if (!parseInterpolation(ip, kf.interpolation)) {
            err = ctx + ": invalid interpolation '" + ip + "'";
            return false;
        }
    } else {
        kf.interpolation = INTERPOLATION_LINEAR;
    }
    return true;
}

bool readTransformElement(const tinyxml2::XMLElement* e, std::vector<RawTransform>& raws,
                          std::vector<std::string>& strings, const std::string& path,
                          std::string& err)
{
    RawTransform rt;
    if (!parseTrackKind(e->Attribute("type"), rt.kind, rt.xtype)) {
        err = "'" + path + "': <transform> with missing/invalid 'type' attribute"
              " (expected scale, rotate, translate, alpha, color or sound)";
        return false;
    }

    for (const tinyxml2::XMLElement* k = e->FirstChildElement(); k; k = k->NextSiblingElement()) {
        const char* kn = k->Name();
        if (std::strcmp(kn, "transformFrames") == 0) {
            for (const tinyxml2::XMLElement* ke = k->FirstChildElement();
                 ke; ke = ke->NextSiblingElement()) {
                if (std::strcmp(ke->Name(), "keyframe") != 0) {
                    err = "'" + path + "': unexpected element <" + ke->Name() +
                          "> inside <transformFrames>";
                    return false;
                }
                RawKeyframe rk;
                if (!readKeyframeElement(ke, rt.kind, rt.xtype, strings, rk, path, err))
                    return false;
                rt.kfs.push_back(rk);
            }
        } else if (std::strcmp(kn, "keyframe") == 0) {   // direct child: allowed
            RawKeyframe rk;
            if (!readKeyframeElement(k, rt.kind, rt.xtype, strings, rk, path, err))
                return false;
            rt.kfs.push_back(rk);
        } else {
            err = "'" + path + "': unexpected element <" + kn + "> inside <transform>";
            return false;
        }
    }
    raws.push_back(rt);
    return true;
}

bool readTimeElement(const tinyxml2::XMLElement* e, std::vector<float>& times,
                     std::vector<char>& seen, const std::string& path, std::string& err)
{
    int32_t fi;
    if (const char* s = e->Attribute("frameIndex")) {
        fi = e->IntAttribute("frameIndex", -1);
        if (fi < 0 || fi >= kMaxFrames) {
            err = "'" + path + "': invalid frameIndex '" + s + "' on <time>";
            return false;
        }
    } else {
        fi = int32_t(times.size());   // sequential when omitted
    }
    if (size_t(fi) >= times.size()) {
        times.resize(size_t(fi) + 1, 0.0f);
        seen.resize(size_t(fi) + 1, 0);
    }
    if (seen[size_t(fi)]) {
        err = "'" + path + "': duplicate <time frameIndex=\"" + std::to_string(fi) + "\">";
        return false;
    }
    times[size_t(fi)] = e->Attribute("start") ? e->FloatAttribute("start", 0.0f) : 0.0f;
    seen[size_t(fi)] = 1;
    return true;
}

// Writing

// writes frameIndex/nextFrameIndex/interpolation plus the fields the track
// kind uses; unused fields are only written when they differ from the
// default, so nothing is ever lost but normal files stay clean.
void writeKeyframeElement(tinyxml2::XMLElement* e, TrackKind kind, TransformType xtype,
                          int32_t frameIndex, const Keyframe& kf)
{
    const KfDefaults def = DefaultsFor(kind, xtype);

    e->SetAttribute("frameIndex",     int(frameIndex));
    e->SetAttribute("nextFrameIndex", int(kf.nextFrameIndex));
    e->SetAttribute("interpolation",  InterpolationTypeName(kf.interpolation).c_str());

    const bool isScale     = (kind == TRACK_XFORM && xtype == XFORM_SCALE);
    const bool isTranslate = (kind == TRACK_XFORM && xtype == XFORM_TRANSLATE);
    const bool isRotate    = (kind == TRACK_XFORM && xtype == XFORM_ROTATE);

    if (isScale || isTranslate || kf.x != def.x)
        e->SetAttribute("x", FormatFloat(kf.x).c_str());
    if (isScale || isTranslate || kf.y != def.y)
        e->SetAttribute("y", FormatFloat(kf.y).c_str());
    if (isRotate || kf.angle != def.angle)
        e->SetAttribute("angle", FormatFloat(kf.angle).c_str());
    if (kind == TRACK_ALPHA || kf.alpha != def.alpha)
        e->SetAttribute("alpha", int(kf.alpha));
    if (kind == TRACK_COLOR || kf.color != def.color)
        e->SetAttribute("color", int(kf.color));
    if (kind == TRACK_SOUND || kf.soundStrIdx != def.soundStrIdx)
        e->SetAttribute("soundStrIdx", int(kf.soundStrIdx));
}

} // namespace

// Read

bool XmlConvert::Read(const std::string& path, Animation& out, std::string& err)
{
    tinyxml2::XMLDocument doc;
    if (doc.LoadFile(path.c_str()) != tinyxml2::XML_SUCCESS) {
        err = "'" + path + "': XML parse error: " + doc.ErrorStr();
        return false;
    }

    const tinyxml2::XMLElement* root = doc.FirstChildElement("anim");
    if (!root) {
        err = "'" + path + "': missing <anim> root element";
        return false;
    }

    // allowed children + old format detection
    for (const tinyxml2::XMLElement* c = root->FirstChildElement(); c; c = c->NextSiblingElement()) {
        const char* n = c->Name();
        if (std::strcmp(n, "frame") == 0) {
            err = "'" + path + "': this anim.xml uses the old <frame>-based format from"
                  " earlier versions, which is no longer supported - reconvert"
                  " the file from its .binltl / .binltl64 / .binuni original";
            return false;
        }
        if (std::strcmp(n, "frameTimes") && std::strcmp(n, "time") &&
            std::strcmp(n, "transforms") && std::strcmp(n, "transform") &&
            std::strcmp(n, "strings")) {
            err = "'" + path + "': unexpected element <" + n + "> inside <anim>";
            return false;
        }
    }

    // has* flags: explicit attribute wins, otherwise derived below
    int32_t hasTransform = -1, hasColor = -1, hasAlpha = -1, hasSound = -1;
    {
        int32_t*    flags[4] = { &hasTransform, &hasColor, &hasAlpha, &hasSound };
        const char* names[4] = { "hasTransform", "hasColor", "hasAlpha", "hasSound" };
        for (int i = 0; i < 4; ++i) {
            if (const char* v = root->Attribute(names[i])) {
                if (!parseFlag(v, *flags[i])) {
                    err = "'" + path + "': invalid " + names[i] + " value '" + v + "'";
                    return false;
                }
            }
        }
    }

    // strings (read first: keyframes can add entries via sound="...")
    // Only non-empty entries are written; gaps rebuild as empty strings.
    out.strings.clear();
    {
        int32_t next = 0;
        for (const tinyxml2::XMLElement* st = root->FirstChildElement("strings");
             st; st = st->NextSiblingElement("strings")) {
            if (!readStringsEntries(st, out.strings, next, path, err)) return false;
        }
    }
    if (out.strings.empty())
        out.strings.push_back("");   // the game always stores one empty string

    // frame times + transforms
    std::vector<RawTransform> raws;
    std::vector<float>        times;
    std::vector<char>         timeSeen;

    for (const tinyxml2::XMLElement* c = root->FirstChildElement(); c; c = c->NextSiblingElement()) {
        const char* n = c->Name();

        if (std::strcmp(n, "strings") == 0) {
            continue;                                  // already read
        } else if (std::strcmp(n, "frameTimes") == 0) {
            for (const tinyxml2::XMLElement* te = c->FirstChildElement();
                 te; te = te->NextSiblingElement()) {
                if (std::strcmp(te->Name(), "time") != 0) {
                    err = "'" + path + "': unexpected element <" + te->Name() +
                          "> inside <frameTimes>";
                    return false;
                }
                if (!readTimeElement(te, times, timeSeen, path, err)) return false;
            }
        } else if (std::strcmp(n, "time") == 0) {      // direct child: allowed
            if (!readTimeElement(c, times, timeSeen, path, err)) return false;
        } else if (std::strcmp(n, "transforms") == 0) {
            for (const tinyxml2::XMLElement* te = c->FirstChildElement();
                 te; te = te->NextSiblingElement()) {
                if (std::strcmp(te->Name(), "transform") != 0) {
                    err = "'" + path + "': unexpected element <" + te->Name() +
                          "> inside <transforms>";
                    return false;
                }
                if (!readTransformElement(te, raws, out.strings, path, err)) return false;
            }
        } else {                                       // direct <transform>
            if (!readTransformElement(c, raws, out.strings, path, err)) return false;
        }
    }

    // frame count
    const bool framesFromTimes = !times.empty();
    int32_t numFrames = int32_t(times.size());
    if (!framesFromTimes) {
        for (size_t i = 0; i < raws.size(); ++i)
            for (size_t k = 0; k < raws[i].kfs.size(); ++k)
                if (raws[i].kfs[k].frameIndex + 1 > numFrames)
                    numFrames = raws[i].kfs[k].frameIndex + 1;
    } else {
        for (int32_t i = 0; i < numFrames; ++i) {
            if (!timeSeen[size_t(i)]) {
                err = "'" + path + "': <frameTimes> must cover frames 0.." +
                      std::to_string(numFrames - 1) + " exactly, no <time> for frame " +
                      std::to_string(i);
                return false;
            }
        }
    }

    // at most one alpha / color / soundtrack
    int alphaTracks = 0, colorTracks = 0, soundTracks = 0;
    for (size_t i = 0; i < raws.size(); ++i) {
        switch (raws[i].kind) {
            case TRACK_ALPHA: ++alphaTracks; break;
            case TRACK_COLOR: ++colorTracks; break;
            case TRACK_SOUND: ++soundTracks; break;
            default: break;
        }
    }
    if (alphaTracks > 1) { err = "'" + path + "': more than one <transform type=\"alpha\">"; return false; }
    if (colorTracks > 1) { err = "'" + path + "': more than one <transform type=\"color\">"; return false; }
    if (soundTracks > 1) { err = "'" + path + "': more than one <transform type=\"sound\">"; return false; }

    // place everything into the Animation
    out.transformTypes.clear();
    for (size_t i = 0; i < raws.size(); ++i)
        if (raws[i].kind == TRACK_XFORM)
            out.transformTypes.push_back(raws[i].xtype);
    out.numTransforms = int32_t(out.transformTypes.size());
    out.numFrames     = numFrames;
    out.frameTimes.assign(size_t(numFrames), 0.0f);
    for (size_t i = 0; i < times.size() && i < out.frameTimes.size(); ++i)
        out.frameTimes[i] = times[i];
    out.xformFrames.assign(size_t(out.numTransforms) * size_t(numFrames), KeyframeSlot());
    out.alphaFrames.assign(size_t(numFrames), KeyframeSlot());
    out.colorFrames.assign(size_t(numFrames), KeyframeSlot());
    out.soundFrames.assign(size_t(numFrames), KeyframeSlot());

    int32_t nextXform = 0;
    for (size_t i = 0; i < raws.size(); ++i) {
        RawTransform& rt = raws[i];

        // order within a track does not matter, sort by frameIndex so the
        // automatic nextFrameIndex chaining is well-defined
        std::sort(rt.kfs.begin(), rt.kfs.end(),
                  [](const RawKeyframe& a, const RawKeyframe& b)
                  { return a.frameIndex < b.frameIndex; });

        for (size_t k = 0; k < rt.kfs.size(); ++k) {
            if (rt.kfs[k].hasNext) continue;
            rt.kfs[k].kf.nextFrameIndex = (k + 1 < rt.kfs.size())
                                        ? rt.kfs[k + 1].frameIndex : -1;
        }

        if (rt.kind == TRACK_XFORM) {
            const int32_t t = nextXform++;
            for (size_t k = 0; k < rt.kfs.size(); ++k) {
                const int32_t f = rt.kfs[k].frameIndex;
                if (f >= numFrames) {
                    err = "'" + path + "': keyframe frameIndex " + std::to_string(f) +
                          " is out of range (the animation has " + std::to_string(numFrames) +
                          (framesFromTimes ? " frames from <frameTimes>)"
                                           : " frames from its keyframes)");
                    return false;
                }
                KeyframeSlot& s = out.xformAt(t, f);
                if (s.present) {
                    err = "'" + path + "': duplicate keyframe for frame " + std::to_string(f) +
                          " in transform " + std::to_string(t);
                    return false;
                }
                s.kf = rt.kfs[k].kf;
                s.present = true;
            }
        } else {
            std::vector<KeyframeSlot>& track =
                (rt.kind == TRACK_ALPHA) ? out.alphaFrames :
                (rt.kind == TRACK_COLOR) ? out.colorFrames : out.soundFrames;
            const char* tname = (rt.kind == TRACK_ALPHA) ? "alpha" :
                                (rt.kind == TRACK_COLOR) ? "color" : "sound";
            for (size_t k = 0; k < rt.kfs.size(); ++k) {
                const int32_t f = rt.kfs[k].frameIndex;
                if (f >= numFrames) {
                    err = "'" + path + "': keyframe frameIndex " + std::to_string(f) +
                          " is out of range (the animation has " + std::to_string(numFrames) +
                          " frames)";
                    return false;
                }
                KeyframeSlot& s = track[size_t(f)];
                if (s.present) {
                    err = "'" + path + "': duplicate keyframe for frame " + std::to_string(f) +
                          " in the " + tname + " track";
                    return false;
                }
                s.kf = rt.kfs[k].kf;
                s.present = true;
            }
        }
    }

    // validate sound references
    {
        const int32_t nStr = int32_t(out.strings.size());
        for (size_t i = 0; i < out.xformFrames.size(); ++i) {
            if (!out.xformFrames[i].present) continue;
            const int32_t idx = out.xformFrames[i].kf.soundStrIdx;
            if (idx < -1 || idx >= nStr) {
                const int32_t t = int32_t(i / size_t(out.numFrames));
                const int32_t f = int32_t(i % size_t(out.numFrames));
                err = "'" + path + "': transform " + std::to_string(t) + " keyframe of frame " +
                      std::to_string(f) + " has soundStrIdx=" + std::to_string(idx) +
                      " but the string table has " + std::to_string(nStr) + " entries";
                return false;
            }
        }
        const std::vector<KeyframeSlot>* tracks[3] =
            { &out.alphaFrames, &out.colorFrames, &out.soundFrames };
        const char* names[3] = { "alpha", "color", "sound" };
        for (int i = 0; i < 3; ++i) {
            for (int32_t f = 0; f < out.numFrames; ++f) {
                if (!tracks[i]->at(size_t(f)).present) continue;
                const int32_t idx = tracks[i]->at(size_t(f)).kf.soundStrIdx;
                if (idx < -1 || idx >= nStr) {
                    err = "'" + path + "': " + names[i] + " keyframe of frame " +
                          std::to_string(f) + " has soundStrIdx=" + std::to_string(idx) +
                          " but the string table has " + std::to_string(nStr) + " entries";
                    return false;
                }
            }
        }
    }

    // flags
    if (hasTransform < 0) hasTransform = (out.numTransforms > 0) ? 1 : 0;
    if (hasAlpha     < 0) hasAlpha     = (alphaTracks > 0) ? 1 : 0;
    if (hasColor     < 0) hasColor     = (colorTracks > 0) ? 1 : 0;
    if (hasSound     < 0) hasSound     = (soundTracks > 0) ? 1 : 0;

    out.hasTransform = hasTransform;
    out.hasColor     = hasColor;
    out.hasAlpha     = hasAlpha;
    out.hasSound     = hasSound;
    return true;
}

// Write

bool XmlConvert::Write(const std::string& path, const Animation& a, std::string& err)
{
    const int64_t nT = a.numTransforms;
    const int64_t nF = a.numFrames;
    if (int64_t(a.transformTypes.size()) != nT ||
        int64_t(a.frameTimes.size())     != nF ||
        int64_t(a.xformFrames.size())    != nT * nF ||
        int64_t(a.alphaFrames.size())    != nF ||
        int64_t(a.colorFrames.size())    != nF ||
        int64_t(a.soundFrames.size())    != nF) {
        err = "internal error: inconsistent animation data";
        return false;
    }

    tinyxml2::XMLDocument doc;
    doc.InsertFirstChild(doc.NewDeclaration("xml version=\"1.0\" encoding=\"UTF-8\""));

    tinyxml2::XMLElement* root = doc.NewElement("anim");
    doc.InsertEndChild(root);
    root->SetAttribute("hasColor",     int(a.hasColor));
    root->SetAttribute("hasAlpha",     int(a.hasAlpha));
    root->SetAttribute("hasSound",     int(a.hasSound));
    root->SetAttribute("hasTransform", int(a.hasTransform));

    // frame times
    tinyxml2::XMLElement* ft = doc.NewElement("frameTimes");
    root->InsertEndChild(ft);
    for (int32_t f = 0; f < a.numFrames; ++f) {
        tinyxml2::XMLElement* te = doc.NewElement("time");
        te->SetAttribute("frameIndex", int(f));
        te->SetAttribute("start", FormatFloat(a.frameTimes[size_t(f)]).c_str());
        ft->InsertEndChild(te);
    }

    // transforms
    tinyxml2::XMLElement* ts = doc.NewElement("transforms");
    root->InsertEndChild(ts);

    for (int32_t t = 0; t < a.numTransforms; ++t) {
        tinyxml2::XMLElement* te = doc.NewElement("transform");
        te->SetAttribute("type", TransformTypeName(int(a.transformTypes[size_t(t)])).c_str());
        ts->InsertEndChild(te);

        bool any = false;   // declared track with no keyframes?
        for (int32_t f = 0; f < a.numFrames && !any; ++f)
            any = a.xformAt(t, f).present;
        if (!any) continue;

        tinyxml2::XMLElement* tfe = doc.NewElement("transformFrames");
        te->InsertEndChild(tfe);
        for (int32_t f = 0; f < a.numFrames; ++f) {
            const KeyframeSlot& s = a.xformAt(t, f);
            if (!s.present) continue;
            tinyxml2::XMLElement* ke = doc.NewElement("keyframe");
            writeKeyframeElement(ke, TRACK_XFORM, a.transformTypes[size_t(t)], f, s.kf);
            tfe->InsertEndChild(ke);
        }
    }

    // alpha / color / soundtracks: only when they actually carry keyframes
    struct SpecialTrack {
        const char*                       type;
        TrackKind                         kind;
        const std::vector<KeyframeSlot>*  track;
    };
    const SpecialTrack specials[3] = {
        { "alpha", TRACK_ALPHA, &a.alphaFrames },
        { "color", TRACK_COLOR, &a.colorFrames },
        { "sound", TRACK_SOUND, &a.soundFrames },
    };
    for (int i = 0; i < 3; ++i) {
        if (CountPresent(*specials[i].track) == 0) continue;
        tinyxml2::XMLElement* te = doc.NewElement("transform");
        te->SetAttribute("type", specials[i].type);
        ts->InsertEndChild(te);
        tinyxml2::XMLElement* tfe = doc.NewElement("transformFrames");
        te->InsertEndChild(tfe);
        for (int32_t f = 0; f < a.numFrames; ++f) {
            const KeyframeSlot& s = specials[i].track->at(size_t(f));
            if (!s.present) continue;
            tinyxml2::XMLElement* ke = doc.NewElement("keyframe");
            writeKeyframeElement(ke, specials[i].kind, XFORM_SCALE, f, s.kf);
            tfe->InsertEndChild(ke);
        }
    }

    // strings: only the non-empty entries, with their original indexes (see
    // readStringsEntries for how the gaps rebuild on read)
    if (anyNonEmptyString(a.strings)) {
        tinyxml2::XMLElement* st = doc.NewElement("strings");
        root->InsertEndChild(st);
        for (size_t i = 0; i < a.strings.size(); ++i) {
            if (a.strings[i].empty()) continue;
            tinyxml2::XMLElement* se = doc.NewElement("string");
            se->SetAttribute("index", int(i));
            se->SetAttribute("value", a.strings[i].c_str());
            st->InsertEndChild(se);
        }
    }

    if (doc.SaveFile(path.c_str()) != tinyxml2::XML_SUCCESS) {
        err = "'" + path + "': " + doc.ErrorStr();
        return false;
    }
    return true;
}

//  movie XML (.movie.xml)

//  <movie length="0.055555556">
//    <actor type="image" id="IMAGE_MOVIE_SEASONDISPLAY_BLACK32"
//            depth="0" align="center">
//      <animation>
//        <keyframe time="0.0" scale="34.21753,18.750015" angle="0.0"
//                  x="547.5" y="300.0" alpha="255" interpolation="none"/>
//      </animation>
//    </actor>
//    <actor type="text" font="FONT_OUTLINE_26" text="$SEASON_TEXT"
//            depth="1.0" align="center">
//      <animation>...</animation>
//    </actor>
//    <sounds>
//      <sound time="0.0" id="SOUND_MOVIE_EXAMPLE"/>
//    </sounds>
//  </movie>

//  Every BinMovie/BinActor field is represented: length; per actor the
//  type, image id / font / text label (attributes, only when set), depth,
//  align, and max-width / wrap-width (only when used; -1 is the game's
//  "unused" sentinel and is never written).

//  <strings> (optional, both at movie level and inside <animation>): a
//  verbatim copy of the corresponding binary string table, so that xml
//  round trips reproduce the original tables byte for byte. At movie
//  level it is the BinMovie table the actor attributes reference; inside
//  <animation> it is the actor animation's own table (the game always
//  stores one leading empty string there, then the sound names). When
//  <strings> is omitted the tables are rebuilt from the references: the
//  movie table from the actor attributes (in actor order) and the
//  animation tables from the sound= attributes (in first-reference order,
//  with the game's leading empty string).

//  The <animation> element is a flat view of the actor's BinImageAnimation:
//  one <keyframe> per animation frame, in order, whose 'time' is the frame
//  time and whose attributes merge the per-track keyframes:
//      x, y           translate track keyframe
//      scale="sx,sy"  scale track keyframe (scale-x / scale-y are also
//                     accepted when reading)
//      angle          rotate track keyframe
//      alpha / color  the alpha / color track keyframes
//      sound          the soundtrack keyframe (string table entry)
//  An attribute group is only present when that track has a keyframe at the
//  frame (a NULL pointer in the binary).  Which transform tracks exist is
//  derived from the attributes used; an explicit transforms="scale,..."
//  attribute (and hasTransform/hasColor/hasAlpha/hasSound flags) is written
//  only when the binary values cannot be derived that way.
//  <sounds> has no counterpart in BinMovie: the game plays movie sounds
//  from the actor animations' soundtracks, so the binary writers attach
//  them to the first actor's animation as sound keyframes at the frame with
//  the closest time.  Converting that file back shows them as sound
//  attributes of that actor's keyframes - same sound, same time.

namespace {

bool parseActorType(const char* s, int32_t& out)
{
    if (!s || !*s) return false;
    const std::string v = toLower(s);
    if (v == "image" || v == "0") { out = eActorType_Image; return true; }
    if (v == "text"  || v == "1") { out = eActorType_Text;  return true; }
    return false;
}

bool parseAlignment(const char* s, int32_t& out)
{
    if (!s || !*s) return false;
    const std::string v = toLower(s);
    if (v == "left"   || v == "0") { out = ALIGN_LEFT;   return true; }
    if (v == "center" || v == "1") { out = ALIGN_CENTER; return true; }
    if (v == "right"  || v == "2") { out = ALIGN_RIGHT;  return true; }
    return false;
}

// "scale,rotate,translate" (+ 0/1/2 entries) -> the transform type list.
// transforms="" means no transform tracks.
bool parseTransformList(const char* s, std::vector<TransformType>& out)
{
    out.clear();
    if (!s) return false;
    std::string item;
    const std::string list(s);
    for (size_t i = 0; i <= list.size(); ++i) {
        if (i == list.size() || list[i] == ',') {
            if (!item.empty()) {
                TrackKind kind;
                TransformType xt;
                if (!parseTrackKind(item.c_str(), kind, xt) || kind != TRACK_XFORM)
                    return false;
                out.push_back(xt);
                item.clear();
            }
        } else {
            item += list[i];
        }
    }
    return true;
}

// "34.2,18.75" (a single value is accepted as a uniform scale)
bool parseScalePair(const char* s, float& sx, float& sy)
{
    if (!s || !*s) return false;
    const std::string v(s);
    char* end = nullptr;
    sx = std::strtof(v.c_str(), &end);
    if (end == v.c_str()) return false;
    const size_t comma = v.find(',');
    if (comma == std::string::npos) { sy = sx; return true; }
    const char* second = v.c_str() + comma + 1;
    sy = std::strtof(second, &end);
    return end != second;
}

struct RawMovieKeyframe {
    float        time = 0.0f;
    bool         hasScale = false, hasRotate = false, hasTranslate = false;
    float        sx = 1.0f, sy = 1.0f, angle = 0.0f, x = 0.0f, y = 0.0f;
    bool         hasAlpha = false, hasColor = false, hasSound = false;
    int32_t      alpha = 255, color = 0;
    std::string  sound;
    bool         hasInterp = false;
    int32_t      interp = INTERPOLATION_NONE;
    bool         hasSoundInterp = false;    // soundtrack's own interpolation
    int32_t      soundInterp = INTERPOLATION_NONE;
    bool         hasNext = false;
    int32_t      next = 0;
};

// Reading: <animation>

bool readMovieAnimation(const tinyxml2::XMLElement* e, Animation& out,
                        const std::string& path, std::string& err)
{
    // has* flags: explicit attribute wins, otherwise derived below
    int32_t hasTransform = -1, hasColor = -1, hasAlpha = -1, hasSound = -1;
    {
        int32_t*    flags[4] = { &hasTransform, &hasColor, &hasAlpha, &hasSound };
        const char* names[4] = { "hasTransform", "hasColor", "hasAlpha", "hasSound" };
        for (int i = 0; i < 4; ++i) {
            if (const char* v = e->Attribute(names[i])) {
                if (!parseFlag(v, *flags[i])) {
                    err = "'" + path + "': invalid " + names[i] + " value '" + v +
                          "' on <animation>";
                    return false;
                }
            }
        }
    }

    std::vector<TransformType> explicitTypes;
    bool hasExplicitTypes = false;
    if (const char* t = e->Attribute("transforms")) {
        if (!parseTransformList(t, explicitTypes)) {
            err = "'" + path + "': invalid transforms='" + std::string(t) +
                  "' on <animation> (expected a comma separated list of"
                  " scale, rotate and/or translate)";
            return false;
        }
        hasExplicitTypes = true;
    }

    // <strings>: a verbatim copy of the animation's binary string table.
    // Read before the keyframes: sound="..." attributes intern into it, so
    // an explicit table must already be in place (and soundStrIdx="N"
    // indexes into it). Only non-empty entries are written; gaps rebuild
    // as empty strings. When the element is absent, the table is rebuilt
    // from the sound references (the game always stores a leading empty
    // string, which the seeding at the end of this function provides).
    std::vector<std::string> animStrings;
    bool hasStringsElement = false;
    {
        int32_t next = 0;
        for (const tinyxml2::XMLElement* st = e->FirstChildElement("strings");
             st; st = st->NextSiblingElement("strings")) {
            hasStringsElement = true;
            if (!readStringsEntries(st, animStrings, next, path, err)) return false;
        }
    }

    std::vector<RawMovieKeyframe> frames;
    for (const tinyxml2::XMLElement* ke = e->FirstChildElement(); ke;
         ke = ke->NextSiblingElement()) {
        if (std::strcmp(ke->Name(), "strings") == 0)
            continue;                        // already read above
        if (std::strcmp(ke->Name(), "keyframe") != 0) {
            err = "'" + path + "': unexpected element <" + ke->Name() +
                  "> inside <animation>";
            return false;
        }
        RawMovieKeyframe rk;
        if (!ke->Attribute("time")) {
            err = "'" + path + "': <keyframe> is missing the time attribute";
            return false;
        }
        rk.time = ke->FloatAttribute("time", 0.0f);
        if (const char* sc = ke->Attribute("scale")) {
            if (!parseScalePair(sc, rk.sx, rk.sy)) {
                err = "'" + path + "': invalid scale '" + sc + "'";
                return false;
            }
            rk.hasScale = true;
        }
        // legacy forms (files from earlier converter versions)
        if (ke->Attribute("scale-x")) { rk.hasScale = true; rk.sx = ke->FloatAttribute("scale-x", 1.0f); }
        if (ke->Attribute("scale-y")) { rk.hasScale = true; rk.sy = ke->FloatAttribute("scale-y", 1.0f); }
        if (ke->Attribute("angle"))   { rk.hasRotate = true;    rk.angle = ke->FloatAttribute("angle", 0.0f); }
        if (ke->Attribute("x"))       { rk.hasTranslate = true; rk.x = ke->FloatAttribute("x", 0.0f); }
        if (ke->Attribute("y"))       { rk.hasTranslate = true; rk.y = ke->FloatAttribute("y", 0.0f); }
        if (ke->Attribute("alpha"))   { rk.hasAlpha = true;     rk.alpha = ke->IntAttribute("alpha", 255); }
        if (ke->Attribute("color"))  { rk.hasColor = true;      rk.color = ke->IntAttribute("color", 0); }
        if (const char* snd = ke->Attribute("sound")) { rk.hasSound = true; rk.sound = snd; }
        if (const char* ip = ke->Attribute("interpolation")) {
            if (!parseInterpolation(ip, rk.interp)) {
                err = "'" + path + "': invalid interpolation '" + ip + "'";
                return false;
            }
            rk.hasInterp = true;
        }
        // the soundtrack's own interpolation, for frames where it differs
        // from the visual tracks (the game writes LINEAR sound keyframes
        // even at frames whose transform tracks are NONE)
        if (const char* sip = ke->Attribute("soundInterpolation")) {
            if (!parseInterpolation(sip, rk.soundInterp)) {
                err = "'" + path + "': invalid soundInterpolation '" + sip + "'";
                return false;
            }
            rk.hasSoundInterp = true;
        }
        if (ke->Attribute("nextFrame") || ke->Attribute("next-frame")) {
            rk.hasNext = true;
            rk.next = ke->Attribute("nextFrame")
                    ? ke->IntAttribute("nextFrame", -1)
                    : ke->IntAttribute("next-frame", -1);
        }
        frames.push_back(rk);
    }

    Animation a;
    if (hasStringsElement)
        a.strings = animStrings;   // verbatim table (see above)
    if (hasExplicitTypes) {
        a.transformTypes = explicitTypes;
    } else {
        // derived: the tracks whose attributes are used, in s/r/t order
        bool anyS = false, anyR = false, anyT = false;
        for (size_t i = 0; i < frames.size(); ++i) {
            anyS = anyS || frames[i].hasScale;
            anyR = anyR || frames[i].hasRotate;
            anyT = anyT || frames[i].hasTranslate;
        }
        if (anyS) a.transformTypes.push_back(XFORM_SCALE);
        if (anyR) a.transformTypes.push_back(XFORM_ROTATE);
        if (anyT) a.transformTypes.push_back(XFORM_TRANSLATE);
    }
    a.numTransforms = int32_t(a.transformTypes.size());
    a.numFrames = int32_t(frames.size());
    a.frameTimes.resize(frames.size());
    for (size_t i = 0; i < frames.size(); ++i)
        a.frameTimes[i] = frames[i].time;
    a.xformFrames.assign(size_t(a.numTransforms) * size_t(a.numFrames), KeyframeSlot());
    a.alphaFrames.assign(size_t(a.numFrames), KeyframeSlot());
    a.colorFrames.assign(size_t(a.numFrames), KeyframeSlot());
    a.soundFrames.assign(size_t(a.numFrames), KeyframeSlot());

    auto trackIndex = [&](TransformType tt) -> int32_t {
        for (size_t i = 0; i < a.transformTypes.size(); ++i)
            if (a.transformTypes[i] == tt) return int32_t(i);
        return -1;
    };

    for (size_t f = 0; f < frames.size(); ++f) {
        const RawMovieKeyframe& rk = frames[f];

        // base keyframe: the game's unused-field defaults (x=y=angle=
        // alpha=color=-1, soundStrIdx 0), matching the anim.xml reader
        Keyframe kf;
        kf.x = -1.0f; kf.y = -1.0f; kf.angle = -1.0f;
        kf.alpha = -1; kf.color = -1; kf.soundStrIdx = 0;
        kf.interpolation = rk.hasInterp ? rk.interp : INTERPOLATION_NONE;

        struct XformUse { bool used; TransformType tt; const char* attr; };
        const XformUse uses[3] = {
                                  { rk.hasScale,     XFORM_SCALE,     "scale" },
                                  { rk.hasRotate,    XFORM_ROTATE,    "angle" },
                                  { rk.hasTranslate, XFORM_TRANSLATE, "x/y"   },
                                  };
        for (int u = 0; u < 3; ++u) {
            if (!uses[u].used) continue;
            const int32_t t = trackIndex(uses[u].tt);
            if (t < 0) {
                err = "'" + path + "': frame " + std::to_string(f) + " uses " +
                      uses[u].attr + " but there is no " +
                      TransformTypeName(int(uses[u].tt)) +
                      " track (check the transforms attribute)";
                return false;
            }
            KeyframeSlot& s = a.xformAt(t, int32_t(f));
            if (s.present) {
                err = "'" + path + "': duplicate " + TransformTypeName(int(uses[u].tt)) +
                      " keyframe for frame " + std::to_string(f);
                return false;
            }
            s.present = true;
            s.kf = kf;
            if      (uses[u].tt == XFORM_SCALE)     { s.kf.x = rk.sx;    s.kf.y = rk.sy;  }
            else if (uses[u].tt == XFORM_ROTATE)    { s.kf.angle = rk.angle;             }
            else /* XFORM_TRANSLATE */              { s.kf.x = rk.x;     s.kf.y = rk.y;   }
        }
        if (rk.hasAlpha) {
            KeyframeSlot& s = a.alphaFrames[f];
            if (s.present) { err = "'" + path + "': duplicate alpha keyframe for frame " +
                      std::to_string(f); return false; }
            s.present = true; s.kf = kf; s.kf.alpha = rk.alpha;
        }
        if (rk.hasColor) {
            KeyframeSlot& s = a.colorFrames[f];
            if (s.present) { err = "'" + path + "': duplicate color keyframe for frame " +
                      std::to_string(f); return false; }
            s.present = true; s.kf = kf; s.kf.color = rk.color;
        }
        if (rk.hasSound) {
            KeyframeSlot& s = a.soundFrames[f];
            if (s.present) { err = "'" + path + "': duplicate sound keyframe for frame " +
                      std::to_string(f); return false; }
            s.present = true; s.kf = kf;
            if (rk.hasSoundInterp) s.kf.interpolation = rk.soundInterp;
            s.kf.soundStrIdx = internString(a.strings, rk.sound.c_str());
        }
    }

    // nextFrameIndex: the game chains to the next keyframe of the same track
    for (int32_t t = 0; t < a.numTransforms; ++t) {
        int32_t next = -1;
        for (int32_t f = a.numFrames - 1; f >= 0; --f) {
            KeyframeSlot& s = a.xformAt(t, f);
            if (s.present) { s.kf.nextFrameIndex = next; next = f; }
        }
    }
    {
        std::vector<KeyframeSlot>* tracks[3] =
            { &a.alphaFrames, &a.colorFrames, &a.soundFrames };
        for (int i = 0; i < 3; ++i) {
            int32_t next = -1;
            for (int32_t f = int32_t(tracks[i]->size()) - 1; f >= 0; --f) {
                KeyframeSlot& s = tracks[i]->at(size_t(f));
                if (s.present) { s.kf.nextFrameIndex = next; next = f; }
            }
        }
    }
    // an explicit next-frame overrides every keyframe present at the frame
    for (size_t f = 0; f < frames.size(); ++f) {
        if (!frames[f].hasNext) continue;
        for (int32_t t = 0; t < a.numTransforms; ++t)
            if (a.xformAt(t, int32_t(f)).present)
                a.xformAt(t, int32_t(f)).kf.nextFrameIndex = frames[f].next;
        if (a.alphaFrames[f].present) a.alphaFrames[f].kf.nextFrameIndex = frames[f].next;
        if (a.colorFrames[f].present) a.colorFrames[f].kf.nextFrameIndex = frames[f].next;
        if (a.soundFrames[f].present) a.soundFrames[f].kf.nextFrameIndex = frames[f].next;
    }

    // flags
    if (hasTransform < 0) hasTransform = (a.numTransforms > 0) ? 1 : 0;
    if (hasAlpha     < 0) hasAlpha     = (CountPresent(a.alphaFrames) > 0) ? 1 : 0;
    if (hasColor     < 0) hasColor     = (CountPresent(a.colorFrames) > 0) ? 1 : 0;
    if (hasSound     < 0) hasSound     = (CountPresent(a.soundFrames) > 0) ? 1 : 0;
    a.hasTransform = hasTransform;
    a.hasColor     = hasColor;
    a.hasAlpha     = hasAlpha;
    a.hasSound     = hasSound;

    if (a.strings.empty())
        a.strings.push_back("");   // the game always stores one empty string
    out = a;
    return true;
}

// Reading: <actor>

bool readMovieActor(const tinyxml2::XMLElement* e, MovieActor& out,
                    const std::string& path, std::string& err)
{
    if (const char* t = e->Attribute("type")) {
        if (!parseActorType(t, out.type)) {
            err = "'" + path + "': invalid actor type '" + t + "' (expected image or text)";
            return false;
        }
    } else {
        err = "'" + path + "': <actor> is missing the type attribute";
        return false;
    }

    if (const char* id = e->Attribute("id"))   out.image     = id;
    if (const char* f  = e->Attribute("font")) out.font      = f;
    if (const char* tx = e->Attribute("text")) out.labelText = tx;

    out.depth = e->FloatAttribute("depth", 0.0f);
    // the extra per-actor field of the v1.6 binuni layout (written back to
    // that layout only; 0 in the files tested)
    out.fixup = e->IntAttribute("fixup", 0);
    if (const char* al = e->Attribute("align")) {
        if (!parseAlignment(al, out.labelJustification)) {
            err = "'" + path + "': invalid align '" + al +
                  "' (expected left, center or right)";
            return false;
        }
    } else {
        out.labelJustification = ALIGN_CENTER;
    }
    out.labelMaxWidth  = e->FloatAttribute("maxWidth",  -1.0f);
    out.labelWrapWidth = e->FloatAttribute("wrapWidth", -1.0f);

    bool hasAnim = false;
    for (const tinyxml2::XMLElement* c = e->FirstChildElement(); c;
         c = c->NextSiblingElement()) {
        const char* n = c->Name();
        if (std::strcmp(n, "animation") == 0) {
            if (hasAnim) {
                err = "'" + path + "': more than one <animation> in an <actor>"
                                   " (BinMovie has exactly one BinImageAnimation per BinActor)";
                return false;
            }
            hasAnim = true;
            if (!readMovieAnimation(c, out.anim, path, err)) return false;
        } else if (std::strcmp(n, "image") == 0 || std::strcmp(n, "text") == 0) {
            err = "'" + path + "': this movie.xml uses the old <" + std::string(n) +
                  "> element format, which is no longer supported - reconvert"
                  " the file from its original binary";
            return false;
        } else {
            err = "'" + path + "': unexpected element <" + n + "> inside <actor>";
            return false;
        }
    }
    if (out.type == eActorType_Image && out.image.empty()) {
        err = "'" + path + "': an image actor is missing its id attribute";
        return false;
    }
    if (out.type == eActorType_Text &&
        (out.font.empty() || out.labelText.empty())) {
        err = "'" + path + "': a text actor needs both the font and text attributes";
        return false;
    }
    if (!hasAnim) {
        err = "'" + path + "': <actor> is missing its <animation>"
                           " (BinMovie has exactly one BinImageAnimation per BinActor)";
        return false;
    }
    return true;
}

// Writing: <animation>

void writeMovieAnimation(tinyxml2::XMLDocument& doc, tinyxml2::XMLElement* parent,
                         const Animation& a)
{
    tinyxml2::XMLElement* ae = doc.NewElement("animation");
    parent->InsertEndChild(ae);

    // transform list: written only when it cannot be derived from the
    // attributes used (scale -> scale, angle -> rotate, x/y -> translate)
    bool anyScale = false, anyRotate = false, anyTranslate = false;
    for (int32_t t = 0; t < a.numTransforms && !(anyScale && anyRotate && anyTranslate); ++t) {
        for (int32_t f = 0; f < a.numFrames; ++f) {
            if (!a.xformAt(t, f).present) continue;
            switch (a.transformTypes[size_t(t)]) {
            case XFORM_SCALE:      anyScale = true;     break;
            case XFORM_ROTATE:    anyRotate = true;    break;
            case XFORM_TRANSLATE: anyTranslate = true; break;
            }
            if (anyScale && anyRotate && anyTranslate) break;
        }
    }
    {
        std::vector<TransformType> derived;
        if (anyScale)     derived.push_back(XFORM_SCALE);
        if (anyRotate)    derived.push_back(XFORM_ROTATE);
        if (anyTranslate) derived.push_back(XFORM_TRANSLATE);
        if (a.transformTypes != derived) {
            std::string list;
            for (size_t i = 0; i < a.transformTypes.size(); ++i) {
                if (!list.empty()) list += ',';
                list += TransformTypeName(int(a.transformTypes[i]));
            }
            ae->SetAttribute("transforms", list.c_str());
        }
    }

    // has* flags: written when they differ from the derived values
    if (a.hasTransform != (a.numTransforms > 0 ? 1 : 0))
        ae->SetAttribute("hasTransform", int(a.hasTransform));
    if (a.hasColor != (CountPresent(a.colorFrames) > 0 ? 1 : 0))
        ae->SetAttribute("hasColor", int(a.hasColor));
    if (a.hasAlpha != (CountPresent(a.alphaFrames) > 0 ? 1 : 0))
        ae->SetAttribute("hasAlpha", int(a.hasAlpha));
    if (a.hasSound != (CountPresent(a.soundFrames) > 0 ? 1 : 0))
        ae->SetAttribute("hasSound", int(a.hasSound));

    auto nextXform = [&](int32_t t, int32_t f) -> int32_t {
        for (int32_t g = f + 1; g < a.numFrames; ++g)
            if (a.xformAt(t, g).present) return g;
        return -1;
    };
    auto nextTrack = [&](const std::vector<KeyframeSlot>& tr, int32_t f) -> int32_t {
        for (int32_t g = f + 1; g < int32_t(tr.size()); ++g)
            if (tr[size_t(g)].present) return g;
        return -1;
    };

    for (int32_t f = 0; f < a.numFrames; ++f) {
        tinyxml2::XMLElement* ke = doc.NewElement("keyframe");
        ke->SetAttribute("time", FormatFloat(a.frameTimes[size_t(f)]).c_str());

        for (int32_t t = 0; t < a.numTransforms; ++t) {
            const KeyframeSlot& s = a.xformAt(t, f);
            if (!s.present) continue;
            switch (a.transformTypes[size_t(t)]) {
            case XFORM_SCALE: {
                const std::string sc = FormatFloat(s.kf.x) + "," + FormatFloat(s.kf.y);
                ke->SetAttribute("scale", sc.c_str());
                break;
            }
            case XFORM_ROTATE:
                ke->SetAttribute("angle", FormatFloat(s.kf.angle).c_str());
                break;
            case XFORM_TRANSLATE:
                ke->SetAttribute("x", FormatFloat(s.kf.x).c_str());
                ke->SetAttribute("y", FormatFloat(s.kf.y).c_str());
                break;
            }
        }
        if (a.alphaFrames[size_t(f)].present)
            ke->SetAttribute("alpha", int(a.alphaFrames[size_t(f)].kf.alpha));
        if (a.colorFrames[size_t(f)].present)
            ke->SetAttribute("color", int(a.colorFrames[size_t(f)].kf.color));
        if (a.soundFrames[size_t(f)].present) {
            const int32_t idx = a.soundFrames[size_t(f)].kf.soundStrIdx;
            if (idx >= 0 && size_t(idx) < a.strings.size())
                ke->SetAttribute("sound", a.strings[size_t(idx)].c_str());
        }
        // interpolation: written only when it is actually used; omitted
        // means none. The soundtrack carries its own interpolation in the
        // binary, so when it disagrees with the visual tracks of the same
        // frame it gets a separate soundInterpolation attribute.
        {
            int32_t value = -1;
            bool uniform = true;
            auto consider = [&](const KeyframeSlot& s) {
                if (!s.present) return;
                if (value < 0) value = s.kf.interpolation;
                else if (s.kf.interpolation != value) uniform = false;
            };
            for (int32_t t = 0; t < a.numTransforms; ++t) consider(a.xformAt(t, f));
            consider(a.alphaFrames[size_t(f)]);
            consider(a.colorFrames[size_t(f)]);
            if (value >= 0 && uniform && value != INTERPOLATION_NONE)
                ke->SetAttribute("interpolation", InterpolationTypeName(value).c_str());
            if (a.soundFrames[size_t(f)].present) {
                const int32_t snd = a.soundFrames[size_t(f)].kf.interpolation;
                if (snd != value && (value >= 0 || snd != INTERPOLATION_NONE))
                    ke->SetAttribute("soundInterpolation",
                                     InterpolationTypeName(snd).c_str());
            }
        }
        // next-frame: only when the automatic chaining does not reproduce it
        {
            bool needs = false, haveCommon = false, same = true;
            int32_t common = 0;
            auto consider = [&](const KeyframeSlot& s, int32_t autoNext) {
                if (!s.present) return;
                if (s.kf.nextFrameIndex != autoNext) needs = true;
                if (!haveCommon) { haveCommon = true; common = s.kf.nextFrameIndex; }
                else if (s.kf.nextFrameIndex != common) same = false;
            };
            for (int32_t t = 0; t < a.numTransforms; ++t)
                consider(a.xformAt(t, f), nextXform(t, f));
            consider(a.alphaFrames[size_t(f)], nextTrack(a.alphaFrames, f));
            consider(a.colorFrames[size_t(f)], nextTrack(a.colorFrames, f));
            consider(a.soundFrames[size_t(f)], nextTrack(a.soundFrames, f));
            if (needs && same)
                ke->SetAttribute("nextFrame", int(common));
        }
        ae->InsertEndChild(ke);
    }

    // strings: a verbatim copy of the animation's binary string table, but
    // only the non-empty entries (a table of just the game's leading empty
    // string is rebuilt on read; gaps become empty strings again).  Written
    // after the keyframes; the reader picks it up wherever it appears.
    if (anyNonEmptyString(a.strings)) {
        tinyxml2::XMLElement* st = doc.NewElement("strings");
        ae->InsertEndChild(st);
        for (size_t i = 0; i < a.strings.size(); ++i) {
            if (a.strings[i].empty()) continue;
            tinyxml2::XMLElement* se = doc.NewElement("string");
            se->SetAttribute("index", int(i));
            se->SetAttribute("value", a.strings[i].c_str());
            st->InsertEndChild(se);
        }
    }
}

} // namespace

// Read

bool XmlConvert::ReadMovie(const std::string& path, Movie& out, std::string& err)
{
    tinyxml2::XMLDocument doc;
    if (doc.LoadFile(path.c_str()) != tinyxml2::XML_SUCCESS) {
        err = "'" + path + "': XML parse error: " + doc.ErrorStr();
        return false;
    }

    const tinyxml2::XMLElement* root = doc.FirstChildElement("movie");
    if (!root) {
        err = "'" + path + "': missing <movie> root element";
        return false;
    }

    Movie m;
    m.length = root->FloatAttribute("length", 0.0f);
    int32_t movieStringNext = 0;   // next free index of the movie string table

    for (const tinyxml2::XMLElement* c = root->FirstChildElement(); c;
         c = c->NextSiblingElement()) {
        const char* n = c->Name();
        if (std::strcmp(n, "actor") == 0) {
            MovieActor ac;
            if (!readMovieActor(c, ac, path, err)) return false;
            m.actors.push_back(ac);
        } else if (std::strcmp(n, "strings") == 0) {
            // a verbatim copy of the BinMovie string table the actor
            // attributes reference (byte offsets into it are recomputed on
            // binary write). Only non-empty entries are written; gaps
            // rebuild as empty strings. Several <strings> elements in one
            // file continue the same table.
            if (!readStringsEntries(c, m.strings, movieStringNext, path, err)) return false;
        } else if (std::strcmp(n, "sounds") == 0) {
            for (const tinyxml2::XMLElement* se = c->FirstChildElement(); se;
                 se = se->NextSiblingElement()) {
                if (std::strcmp(se->Name(), "sound") != 0) {
                    err = "'" + path + "': unexpected element <" + se->Name() +
                          "> inside <sounds>";
                    return false;
                }
                if (!se->Attribute("id")) {
                    err = "'" + path + "': <sound> is missing the id attribute";
                    return false;
                }
                MovieSound s;
                s.time = se->FloatAttribute("time", 0.0f);
                s.id   = se->Attribute("id");
                m.sounds.push_back(s);
            }
        } else {
            err = "'" + path + "': unexpected element <" + n + "> inside <movie>";
            return false;
        }
    }

    // the game always stores a leading empty string in the BinMovie table
    // (unused BinActor fields reference it at byte offset 0)
    if (m.strings.empty())
        m.strings.push_back("");

    out = m;
    return true;
}

// Write

bool XmlConvert::WriteMovie(const std::string& path, const Movie& m, std::string& err)
{
    for (size_t i = 0; i < m.actors.size(); ++i) {
        const Animation& a = m.actors[i].anim;
        const int64_t nT = a.numTransforms, nF = a.numFrames;
        if (int64_t(a.transformTypes.size()) != nT ||
            int64_t(a.frameTimes.size())     != nF ||
            int64_t(a.xformFrames.size())    != nT * nF ||
            int64_t(a.alphaFrames.size())    != nF ||
            int64_t(a.colorFrames.size())    != nF ||
            int64_t(a.soundFrames.size())    != nF) {
            err = "internal error: inconsistent animation data in actor " +
                  std::to_string(i);
            return false;
        }
    }

    tinyxml2::XMLDocument doc;
    doc.InsertFirstChild(doc.NewDeclaration("xml version=\"1.0\" encoding=\"UTF-8\""));

    tinyxml2::XMLElement* root = doc.NewElement("movie");
    doc.InsertEndChild(root);
    root->SetAttribute("length", FormatFloat(m.length).c_str());

    for (size_t i = 0; i < m.actors.size(); ++i) {
        const MovieActor& ac = m.actors[i];
        tinyxml2::XMLElement* ae = doc.NewElement("actor");
        root->InsertEndChild(ae);
        ae->SetAttribute("type", ActorTypeName(int(ac.type)).c_str());
        if (!ac.image.empty())     ae->SetAttribute("id",   ac.image.c_str());
        if (!ac.font.empty())      ae->SetAttribute("font", ac.font.c_str());
        if (!ac.labelText.empty()) ae->SetAttribute("text", ac.labelText.c_str());
        ae->SetAttribute("depth", FormatFloat(ac.depth).c_str());
        ae->SetAttribute("align", AlignmentName(int(ac.labelJustification)).c_str());
        if (ac.fixup != 0)
            ae->SetAttribute("fixup", int(ac.fixup));
        if (ac.labelMaxWidth  != -1.0f)
            ae->SetAttribute("maxWidth",  FormatFloat(ac.labelMaxWidth).c_str());
        if (ac.labelWrapWidth != -1.0f)
            ae->SetAttribute("wrapWidth", FormatFloat(ac.labelWrapWidth).c_str());

        writeMovieAnimation(doc, ae, ac.anim);
    }

    // strings: a verbatim copy of the BinMovie string table, but only the
    // non-empty entries (the seeding empty string and any trailing padding
    // empties are rebuilt on read / by the binary writers' padding).
    // Written after the actors, before <sounds>.
    if (anyNonEmptyString(m.strings)) {
        tinyxml2::XMLElement* st = doc.NewElement("strings");
        root->InsertEndChild(st);
        for (size_t i = 0; i < m.strings.size(); ++i) {
            if (m.strings[i].empty()) continue;
            tinyxml2::XMLElement* se = doc.NewElement("string");
            se->SetAttribute("index", int(i));
            se->SetAttribute("value", m.strings[i].c_str());
            st->InsertEndChild(se);
        }
    }

    if (!m.sounds.empty()) {
        tinyxml2::XMLElement* ss = doc.NewElement("sounds");
        root->InsertEndChild(ss);
        for (size_t i = 0; i < m.sounds.size(); ++i) {
            tinyxml2::XMLElement* se = doc.NewElement("sound");
            se->SetAttribute("time", FormatFloat(m.sounds[i].time).c_str());
            se->SetAttribute("id", m.sounds[i].id.c_str());
            ss->InsertEndChild(se);
        }
    }

    if (doc.SaveFile(path.c_str()) != tinyxml2::XML_SUCCESS) {
        err = "'" + path + "': " + doc.ErrorStr();
        return false;
    }
    return true;
}