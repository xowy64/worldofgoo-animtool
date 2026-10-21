// XmlConvert.h - anim.xml and movie.xml reader/writer (TinyXML-2).

#ifndef XMLCONVERT_H
#define XMLCONVERT_H

#include <string>

#include "BinAnimation.h"
#include "BinMovie.h"

class XmlConvert {
public:
    static bool Read (const std::string& path, Animation& out, std::string& err);
    static bool Write(const std::string& path, const Animation& anim, std::string& err);

    static bool ReadMovie (const std::string& path, Movie& out, std::string& err);
    static bool WriteMovie(const std::string& path, const Movie& movie, std::string& err);
};

#endif // XMLCONVERT_H
