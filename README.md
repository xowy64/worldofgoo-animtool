# World of Goo Animation Tool
- What is it?
    - This is a simple CLI tool for converting World of Goo movies and animations.

- What formats are supported?
    - Currently: `binltl`, `binltl64`, `binuni`, and `xml`.

- Does it work with the Goovie or GooTool XML formats?
    - Not currently. In order to edit these, you would have to convert a bin file produced by Gootool/Goovie to xml with this tool.

## Usage

```
./animtool [options] <input> [output]

Options:
  -r, --recursive      Convert a folder recursively
  -v, --verbose        Print the full contents (every actor/keyframe)
  -q, --quiet          Print nothing except errors
  -n, --skip-existing  Skip conversions whose output file already exists
      --xml            Target format: anim.xml / movie.xml
      --bin, --bin32   Target format: anim.binltl / movie.binltl (32-bit)
      --bin64          Target format: anim.binltl64 / movie.binltl64
      --binuni         Target format: anim.binuni / movie.binuni
      --binuni -fix    movie.binuni in the remastered layout.
```

## How to build

- First, clone the repository: `git clone --recursive https://github.com/xowy64/worldofgoo-animtool`.

- Then open a terminal inside the repo directory and run this command: `cmake -B build -DCMAKE_BUILD_TYPE=MinSizeRel && cmake --build build -j8` 
    - Default build is `MinSizeRel`, but it can be changed to these: `Release`, `RelWithDebInfo`, and `Debug`.

## Acknowledgments

- [TinyXML-2](https://github.com/leethomason/tinyxml2) [Zlib license]
