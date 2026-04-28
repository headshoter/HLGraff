

#define NOMINMAX
#include <windows.h>

// Provide our own min/max templates to avoid conflicts between the Windows.h
// macros (suppressed by NOMINMAX) and the standard library templates used
// throughout this file.
template <typename T>
inline T min(T a, T b) {
    return (a < b) ? a : b;
}

template <typename T>
inline T max(T a, T b) {
    return (a > b) ? a : b;
}

#include <gdiplus.h>
#include <shlwapi.h>

// Standard-library headers used across the various subsystems below.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

// Link against the GDI+ and Shell utility libraries at build time.
#pragma comment(lib, "Gdiplus.lib")
#pragma comment(lib, "Shlwapi.lib")

// Alias std::filesystem to a short namespace for readability.
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// BASIC DATA STRUCTURES
// ---------------------------------------------------------------------------

// Represents a single pixel with red, green, blue and alpha (transparency)
// channels, each stored as an unsigned 8-bit integer (0–255).
struct RGBA {
    std::uint8_t r;
    std::uint8_t g;
    std::uint8_t b;
    std::uint8_t a;
};

// Opaque (no alpha) RGB pixel used exclusively for the WAD palette entries,
// which do not carry per-entry transparency information.
struct RGB {
    std::uint8_t r;
    std::uint8_t g;
    std::uint8_t b;
};

// Container returned by loadImageFile(): holds the raw decoded pixel data
// together with the image dimensions required for later processing steps.
struct LoadedImage {
    std::vector<RGBA> pixels;
    std::size_t width = 0;
    std::size_t height = 0;
};

// Associates a unique opaque RGB color with the number of times it appears
// in an image.  Used as the input data structure for the median-cut color
// quantization algorithm.
struct ColorCount {
    RGB          color;
    std::uint32_t count = 0;
};

// Represents a partition ("box") of the color space during median-cut
// quantization.  Tracks the index range within the sorted color array as
// well as the axis-aligned bounding box of the colors it contains.
struct ColorBox {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::uint64_t total = 0;
    int r_min = 255;
    int r_max = 0;
    int g_min = 255;
    int g_max = 0;
    int b_min = 255;
    int b_max = 0;
};

// ---------------------------------------------------------------------------
// GLOBAL PATHS
// ---------------------------------------------------------------------------

// Returns the directory that contains the currently running executable.
// This is used as the root from which all other paths are derived so that
// the tool works regardless of the current working directory.
fs::path getExeDir() {
    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    return fs::path(buffer).parent_path();
}

// All path constants are computed once at program start-up from the exe
// directory so they are consistent throughout the entire execution.
const fs::path BASE_DIR = getExeDir();
// Folder where the user places spray files (.wad / .png / .jpg / .jpeg).
const fs::path SPRAYS_DIR = BASE_DIR / "sprays";
// Sub-folder where image sprays that have been converted to .wad are cached.
const fs::path CONVERTED_DIR = SPRAYS_DIR / "converted";
// INI configuration file read on every launch.
const fs::path INI_PATH = BASE_DIR / "spraysettings.ini";
// Persistent state file that records the name of the last spray that was
// applied, and (in random mode) the shuffled rotation list.
const fs::path STATE_FILE = BASE_DIR / "last_spray.txt";
// File that stores SHA-256 hashes of every source image so that the tool
// can detect when an image has been modified and its cached .wad must be
// rebuilt.
const fs::path HASH_FILE = BASE_DIR / "spray_hashes.txt";

// ---------------------------------------------------------------------------
// STRING UTILITIES
// ---------------------------------------------------------------------------

// Removes leading and trailing whitespace characters (space, tab, carriage
// return, newline) from the string s and returns the result.
std::string trim(const std::string& s) {
    std::size_t start = s.find_first_not_of(" \t\r\n");
    std::size_t end = s.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    return s.substr(start, end - start + 1);
}

// Returns a copy of s with every ASCII letter converted to lower-case.
// Used throughout for case-insensitive comparisons.
std::string toLowerCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

// Returns true when strings a and b are equal ignoring case differences.
bool equalsIgnoreCase(const std::string& a, const std::string& b) {
    return toLowerCopy(a) == toLowerCopy(b);
}

// Returns the file extension of path as a lower-case string (e.g. ".wad").
std::string getLowerExtension(const fs::path& path) {
    return toLowerCopy(path.extension().string());
}

// Returns true when path has an extension that the spray system can handle
// (.wad, .png, .jpg or .jpeg).
bool isSupportedSprayExtension(const fs::path& path) {
    std::string ext = getLowerExtension(path);
    return ext == ".wad" || ext == ".png" || ext == ".jpg" || ext == ".jpeg"
        || ext == ".bmp" || ext == ".gif" || ext == ".tiff" || ext == ".tif"
        || ext == ".tga";
}

// Returns true when path refers to an image file that must be converted to
// a .wad before it can be used as a spray (.png, .jpg or .jpeg).
// Returns true when path refers to an image file that must be converted to
// a .wad before it can be used as a spray.
bool isImageSprayExtension(const fs::path& path) {
    std::string ext = getLowerExtension(path);
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg"
        || ext == ".bmp" || ext == ".gif" || ext == ".tiff" || ext == ".tif"
        || ext == ".tga";
}

// Displays a modal error dialog with the given message.  Used for all
// user-facing error reporting because the application runs without a console
// window.
void showError(const std::string& message) {
    MessageBoxA(NULL, message.c_str(), "hlgraff", MB_OK | MB_ICONERROR);
}

// ---------------------------------------------------------------------------
// NAME FLAGS
// ---------------------------------------------------------------------------
// Flags are underscore-prefixed tokens appended to a filename stem in any
// order, before the extension.  All recognised flags are parsed in a single
// right-to-left pass; unrecognised tokens are left in the clean stem so
// forward-compatible filenames work without modification.
//
// Recognised flags:
//   _large / _medium / _small   — size override (overrides global size=)
//   _static                     — this spray is always used (ignores method=)
//   _game-<modname>             — restrict spray to a specific mod/game
//   _f<N>                       — frame index for GIF or multi-texture WAD
//
// Examples:
//   "tag_large.png"              → size=large
//   "logo_small_f2.gif"          → size=small, frame=2
//   "spray_game-cstrike.png"     → only applied in cstrike
//   "main_static_game-valve.png" → static spray, valve only
//   "art_f3_medium_game-ts.gif"  → frame 3, medium, ts only

// Holds all flags parsed from a single filename.
struct SprayFlags {
    std::string sizeMode;    // "large" / "medium" / "small" / "" (use global)
    bool        isStatic = false;
    std::string gameTarget;  // mod name without "game-", or "" (universal)
    int         frameIndex = 0; // 0-based; -1 = "not specified" (use first)
    std::string cleanStem;   // filename stem with all recognised flags removed
};

// Parses all name flags from the stem of `filename` (no extension) and
// returns a populated SprayFlags.  Tokens are consumed right-to-left so
// flags can appear in any order.
SprayFlags parseNameFlags(const fs::path& filename) {
    SprayFlags flags;
    flags.frameIndex = 0; // default: first frame

    std::string stem = filename.stem().string();
    // Split the stem on underscores right-to-left, consuming recognised
    // tokens until we hit one that is not a known flag.
    // We rebuild the clean stem from what remains.
    std::vector<std::string> tokens;
    {
        // Tokenise on '_', preserving case for the clean stem.
        std::string buf;
        for (char c : stem) {
            if (c == '_') {
                if (!buf.empty()) { tokens.push_back(buf); buf.clear(); }
            }
            else {
                buf += c;
            }
        }
        if (!buf.empty()) tokens.push_back(buf);
    }

    // Walk tokens right-to-left, eating flags until a non-flag token is found.
    // Everything left of the first unrecognised token is the clean stem.
    std::size_t consumed = 0; // number of tokens eaten from the right
    for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(tokens.size()) - 1; i >= 0; --i) {
        std::string tok = toLowerCopy(tokens[static_cast<std::size_t>(i)]);

        if (tok == "large" || tok == "medium" || tok == "small") {
            if (flags.sizeMode.empty()) flags.sizeMode = tok;
            consumed++;
        }
        else if (tok == "static") {
            flags.isStatic = true;
            consumed++;
        }
        else if (tok.size() > 5 && tok.substr(0, 5) == "game-") {
            // _game-<modname>: store the mod name part (after "game-").
            if (flags.gameTarget.empty())
                flags.gameTarget = tok.substr(5);
            consumed++;
        }
        else if (tok.size() > 1 && tok[0] == 'f') {
            // _f<N>: frame index.  Must be purely numeric after the 'f'.
            std::string num = tok.substr(1);
            bool isNum = !num.empty() && std::all_of(num.begin(), num.end(), ::isdigit);
            if (isNum) {
                flags.frameIndex = std::stoi(num);
                consumed++;
            }
            else {
                // Not a frame flag — stop consuming.
                break;
            }
        }
        else {
            // Unrecognised token — stop consuming flags here.
            break;
        }
    }

    // Rebuild the clean stem from the tokens that were NOT consumed.
    std::size_t keepCount = tokens.size() - consumed;
    std::string clean;
    for (std::size_t i = 0; i < keepCount; ++i) {
        if (i > 0) clean += '_';
        clean += tokens[i];
    }
    flags.cleanStem = clean.empty() ? stem : clean;
    return flags;
}

// Thin wrapper kept for internal callers that only need the size mode and
// clean stem (avoids touching every call site from the old single-flag API).
std::string extractNameFlag(const fs::path& filename, std::string& cleanStem) {
    SprayFlags f = parseNameFlags(filename);
    cleanStem = f.cleanStem;
    return f.sizeMode;
}

// ---------------------------------------------------------------------------
// BINARY WRITE HELPERS
// ---------------------------------------------------------------------------

// Appends exactly `size` raw bytes starting at `data` to the byte vector
// `out`.  This is the low-level building block for WAD file construction.
void appendBytes(std::vector<std::uint8_t>& out, const void* data, std::size_t size) {
    const auto* begin = static_cast<const std::uint8_t*>(data);
    out.insert(out.end(), begin, begin + size);
}

// Appends a single value of any trivially-copyable type T to `out` using its
// raw memory representation (little-endian on Windows x86/x64).
template <typename T>
void appendValue(std::vector<std::uint8_t>& out, T value) {
    appendBytes(out, &value, sizeof(T));
}

// ---------------------------------------------------------------------------
// WINDOWS NATURAL SORT
// ---------------------------------------------------------------------------

// Compares two UTF-8 filenames using the same ordering algorithm that Windows
// Explorer applies (StrCmpLogicalW), so the spray list matches the order a
// user sees when they open the sprays folder.  Returns true when a precedes b.
bool windowsNaturalSort(const std::string& a, const std::string& b) {
    // Convert both UTF-8 strings to wide strings required by StrCmpLogicalW.
    int lenA = MultiByteToWideChar(CP_UTF8, 0, a.c_str(), -1, NULL, 0);
    int lenB = MultiByteToWideChar(CP_UTF8, 0, b.c_str(), -1, NULL, 0);

    std::wstring wa(lenA, L'\0');
    std::wstring wb(lenB, L'\0');

    MultiByteToWideChar(CP_UTF8, 0, a.c_str(), -1, &wa[0], lenA);
    MultiByteToWideChar(CP_UTF8, 0, b.c_str(), -1, &wb[0], lenB);

    // StrCmpLogicalW returns negative / zero / positive like strcmp.
    return StrCmpLogicalW(wa.c_str(), wb.c_str()) < 0;
}

// ---------------------------------------------------------------------------
// GAME DIRECTORY DETECTION
// ---------------------------------------------------------------------------

// Searches several likely locations for hl.exe (or the configured exe) so
// that the tool can be placed either in the game root or in a mod sub-folder
// and still find the launcher.  The first directory that contains hl.exe is
// returned; if none is found the executable's own directory is used as a
// safe fallback.
fs::path resolveGameDir() {
    std::vector<fs::path> candidates = { BASE_DIR };

    // Also check the parent of the exe directory in case the tool lives
    // inside a mod folder (e.g. Half-Life/valve/hlgraff.exe).
    if (BASE_DIR.has_parent_path())
        candidates.push_back(BASE_DIR.parent_path());

    // Include the process working directory and its parent for completeness.
    try {
        fs::path cwd = fs::current_path();
        candidates.push_back(cwd);

        if (cwd.has_parent_path())
            candidates.push_back(cwd.parent_path());
    }
    catch (...) {
        // fs::current_path can throw if the working directory was deleted;
        // silently ignore and continue with the other candidates.
    }

    for (const auto& candidate : candidates) {
        if (!candidate.empty() && fs::exists(candidate / "hl.exe"))
            return candidate;
    }

    return BASE_DIR;
}

// ---------------------------------------------------------------------------
// GDI+ INITIALISATION
// ---------------------------------------------------------------------------

// Initialises the GDI+ subsystem exactly once per process using a local
// static.  Returns true on success.  All image loading goes through GDI+,
// so this must succeed before any image file is opened.
bool ensureGdiplus() {
    static bool    attempted = false;
    static bool    ready = false;
    static ULONG_PTR token = 0;

    if (!attempted) {
        attempted = true;
        Gdiplus::GdiplusStartupInput input;
        // GdiplusStartup fills `token` which would be needed for shutdown,
        // but we intentionally never shut down GDI+ because the process is
        // short-lived and ExitProcess handles cleanup.
        ready = Gdiplus::GdiplusStartup(&token, &input, NULL) == Gdiplus::Ok;
    }

    return ready;
}

// ---------------------------------------------------------------------------
// PIXEL SAMPLING
// ---------------------------------------------------------------------------

// Returns the pixel of the source image that is nearest to the continuous
// image-space coordinate (x, y).  Coordinates are clamped so that sampling
// outside the image boundary returns the closest edge pixel.
RGBA sampleNearest(const std::vector<RGBA>& pixels, std::size_t width, std::size_t height, double x, double y) {
    int ix = static_cast<int>(std::floor(x + 0.5));
    int iy = static_cast<int>(std::floor(y + 0.5));
    // Clamp to valid pixel range.
    ix = std::clamp(ix, 0, static_cast<int>(width) - 1);
    iy = std::clamp(iy, 0, static_cast<int>(height) - 1);
    return pixels[static_cast<std::size_t>(iy) * width + static_cast<std::size_t>(ix)];
}

// Returns a bilinearly interpolated pixel for the continuous coordinate
// (x, y).  Alpha is interpolated linearly; colour channels use premultiplied
// alpha so that transparent pixels do not bleed colour into neighbouring
// opaque ones.
RGBA sampleBilinear(const std::vector<RGBA>& pixels, std::size_t width, std::size_t height, double x, double y) {
    // Clamp continuous coordinate to the valid pixel range.
    x = std::clamp(x, 0.0, static_cast<double>(width - 1));
    y = std::clamp(y, 0.0, static_cast<double>(height - 1));

    // Four surrounding pixel indices.
    int x0 = static_cast<int>(std::floor(x));
    int y0 = static_cast<int>(std::floor(y));
    int x1 = std::min(x0 + 1, static_cast<int>(width) - 1);
    int y1 = std::min(y0 + 1, static_cast<int>(height) - 1);

    // Fractional offsets within the 2×2 pixel neighbourhood.
    double tx = x - x0;
    double ty = y - y0;

    // Fetch the four surrounding pixels.
    const RGBA& p00 = pixels[static_cast<std::size_t>(y0) * width + static_cast<std::size_t>(x0)];
    const RGBA& p10 = pixels[static_cast<std::size_t>(y0) * width + static_cast<std::size_t>(x1)];
    const RGBA& p01 = pixels[static_cast<std::size_t>(y1) * width + static_cast<std::size_t>(x0)];
    const RGBA& p11 = pixels[static_cast<std::size_t>(y1) * width + static_cast<std::size_t>(x1)];

    // Linear interpolation helper.
    auto lerp = [](double a, double b, double t) {
        return a + (b - a) * t;
    };

    // Interpolate the alpha channel independently before colour channels,
    // because the premultiplied colour calculation needs it.
    auto weightedAlpha = [&](std::uint8_t a00, std::uint8_t a10, std::uint8_t a01, std::uint8_t a11) {
        double top = lerp(static_cast<double>(a00), static_cast<double>(a10), tx);
        double bottom = lerp(static_cast<double>(a01), static_cast<double>(a11), tx);
        return lerp(top, bottom, ty);
    };

    double alpha = weightedAlpha(p00.a, p10.a, p01.a, p11.a);

    // If the resulting alpha is zero the pixel is fully transparent; return a
    // canonical transparent blue pixel (the WAD transparency key colour).
    if (alpha <= 0.0) {
        return { 0, 0, 0xff, 0 };
    }

    // Interpolate each colour channel using premultiplied alpha arithmetic to
    // avoid colour fringing at transparency edges.
    auto samplePremultipliedChannel = [&](std::uint8_t c00, std::uint8_t c10, std::uint8_t c01, std::uint8_t c11) {
        double top = lerp(
            static_cast<double>(c00) * static_cast<double>(p00.a) / 255.0,
            static_cast<double>(c10) * static_cast<double>(p10.a) / 255.0,
            tx
        );
        double bottom = lerp(
            static_cast<double>(c01) * static_cast<double>(p01.a) / 255.0,
            static_cast<double>(c11) * static_cast<double>(p11.a) / 255.0,
            tx
        );
        // Divide out the accumulated alpha to convert back to straight alpha.
        double premultiplied = lerp(top, bottom, ty);
        double value = premultiplied * 255.0 / alpha;
        long   rounded = std::lround(value);
        rounded = std::clamp(rounded, 0L, 255L);
        return static_cast<std::uint8_t>(rounded);
    };

    RGBA pixel = {};
    pixel.r = samplePremultipliedChannel(p00.r, p10.r, p01.r, p11.r);
    pixel.g = samplePremultipliedChannel(p00.g, p10.g, p01.g, p11.g);
    pixel.b = samplePremultipliedChannel(p00.b, p10.b, p01.b, p11.b);
    pixel.a = static_cast<std::uint8_t>(std::clamp(std::lround(alpha), 0L, 255L));
    return pixel;
}

// ---------------------------------------------------------------------------
// TRANSPARENCY NORMALISATION
// ---------------------------------------------------------------------------

// The Half-Life WAD format only supports binary (on/off) transparency.
// This function converts any partial transparency to a fully opaque pixel
// (alpha >= 128) or to a fully transparent one (alpha < 128).  Transparent
// pixels are forced to opaque blue (0,0,255) which is the WAD palette index
// 255 transparency key colour used by GoldSrc.
void normalizeBinaryTransparency(std::vector<RGBA>& pixels) {
    for (RGBA& pixel : pixels) {
        if (pixel.a >= 0x80) {
            // Opaque: keep colour, force alpha to full.
            pixel.a = 0xff;
        }
        else {
            // Transparent: replace with the WAD transparency key colour.
            pixel.r = 0;
            pixel.g = 0;
            pixel.b = 0xff;
            pixel.a = 0x00;
        }
    }
}

// ---------------------------------------------------------------------------
// TEXTURE RESIZE
// ---------------------------------------------------------------------------

// Scales srcWidth×srcHeight pixel data to dstWidth×dstHeight using either
// nearest-neighbour (usePointResample=true) or bilinear interpolation.
// The result also applies a binary alpha threshold so every output pixel is
// either fully opaque or fully transparent.
std::vector<RGBA> resizeTexture(
    const std::vector<RGBA>& pixels,
    std::size_t srcWidth,
    std::size_t srcHeight,
    std::size_t dstWidth,
    std::size_t dstHeight,
    bool usePointResample
) {
    // Pre-fill output with transparent blue so uncovered pixels use the key
    // colour rather than black.
    std::vector<RGBA> resized(dstWidth * dstHeight, { 0, 0, 0xff, 0 });

    for (std::size_t y = 0; y < dstHeight; ++y) {
        // Map destination Y coordinate back to a continuous source coordinate
        // using the centre-aligned mapping that avoids half-pixel shift artefacts.
        double srcY = ((static_cast<double>(y) + 0.5) * static_cast<double>(srcHeight) / static_cast<double>(dstHeight)) - 0.5;
        for (std::size_t x = 0; x < dstWidth; ++x) {
            double srcX = ((static_cast<double>(x) + 0.5) * static_cast<double>(srcWidth) / static_cast<double>(dstWidth)) - 0.5;
            // Choose the interpolation method based on the caller's preference.
            RGBA pixel = usePointResample
                ? sampleNearest(pixels, srcWidth, srcHeight, srcX, srcY)
                : sampleBilinear(pixels, srcWidth, srcHeight, srcX, srcY);
            // Threshold alpha to binary after interpolation.
            pixel.a = (pixel.a >= 0x80) ? 0xff : 0x00;
            resized[y * dstWidth + x] = pixel;
        }
    }

    return resized;
}

// ---------------------------------------------------------------------------
// SIZE ADJUSTMENT  (size setting: large / medium / small)
// ---------------------------------------------------------------------------

// Finds the largest 16-aligned width and height that preserve the aspect ratio
// of (srcWidth x srcHeight) and whose pixel area does not exceed areaBudget.
// Both output dimensions are always at least 16.
//
// Strategy: compute the maximum uniform scale factor from the area constraint,
// then step down from that scale in 0.5 % increments until both scaled
// dimensions snap to a multiple of 16.  This guarantees the result is as
// large as possible while remaining WAD-legal and proportional.
static void bestFitProportional(
    std::size_t srcWidth,
    std::size_t srcHeight,
    std::size_t areaBudget,
    std::size_t& outWidth,
    std::size_t& outHeight
) {
    // Maximum scale so that (srcW * s) * (srcH * s) <= areaBudget.
    // No cap at 1.0 — we intentionally upscale small images to fill the budget.
    double maxScale = std::sqrt(
        static_cast<double>(areaBudget) /
        (static_cast<double>(srcWidth) * static_cast<double>(srcHeight))
    );

    // Walk down from maxScale until both rounded-down dimensions are multiples of 16.
    for (double s = maxScale; s > 0.01; s -= 0.005) {
        std::size_t w = (static_cast<std::size_t>(std::floor(static_cast<double>(srcWidth) * s)) / 16) * 16;
        std::size_t h = (static_cast<std::size_t>(std::floor(static_cast<double>(srcHeight) * s)) / 16) * 16;
        if (w >= 16 && h >= 16) {
            outWidth = w;
            outHeight = h;
            return;
        }
    }
    // Absolute fallback: 16x16.
    outWidth = 16;
    outHeight = 16;
}

// Computes the target WAD resolution for a source image, honouring the
// GoldSrc pixel-area limit and the user's size setting.
//
// GoldSrc renders decals at (texture_pixels / 8) world-units per side, so
// the texture resolution directly controls the in-game decal size.
// The engine hard limit is 112x128 = 14 336 pixels total.
//
// Size mode semantics (all relative to the "large" output, not the source):
//   "large"  -> fill the WAD budget as fully as possible (~112x112 for a square).
//   "medium" -> 1/2 the area of large  (~0.71x linear, e.g. ~64x64 for a square).
//   "small"  -> 1/4 the area of large  (~0.5x linear,  e.g. ~48x48 for a square).
//
// Because the reference point is always "large", switching modes produces a
// consistent and visible size difference regardless of the source resolution.
// Small source images are upscaled to fill the budget; large ones are downscaled.
//
// Both upscaling (small source images) and downscaling (large source images)
// are performed so that the chosen size mode always has a consistent and
// visible effect regardless of the source resolution.
void adjustSize(
    const std::vector<RGBA>& texture,
    std::size_t width,
    std::size_t height,
    bool usePointResample,
    const std::string& sizeMode,
    std::vector<RGBA>& adjusted,
    std::size_t& outWidth,
    std::size_t& outHeight
) {
    // GoldSrc hard pixel-area limit.
    const std::size_t WAD_MAX_AREA = 112 * 128; // 14 336

    // Step 1: compute the "large" target dimensions — the biggest 16-aligned
    // proportional fit for this image inside the WAD area limit.
    std::size_t largeW = 0, largeH = 0;
    bestFitProportional(width, height, WAD_MAX_AREA, largeW, largeH);

    // Step 2: derive the actual target from the large dimensions according to
    // the size mode.  medium = half the linear size, small = quarter.
    // Halving linear dimensions = dividing area by 4; quartering = dividing by 16.
    if (sizeMode == "medium") {
        // Target area = largeW * largeH / 2  (~0.71x linear scale).
        // Produces a clearly smaller but still readable decal in-game.
        bestFitProportional(largeW, largeH, (largeW * largeH) / 2, outWidth, outHeight);
    }
    else if (sizeMode == "small") {
        // Target area = largeW * largeH / 4  (0.5x linear scale).
        // Produces a noticeably small corner-stamp decal.
        bestFitProportional(largeW, largeH, (largeW * largeH) / 4, outWidth, outHeight);
    }
    else {
        // "large" (default): use the large dimensions directly.
        outWidth = largeW;
        outHeight = largeH;
    }

    // If the source already has the exact target dimensions, copy as-is.
    if (outWidth == width && outHeight == height) {
        adjusted = texture;
        return;
    }

    // Resize (up or down) to the computed proportional target.
    adjusted = resizeTexture(texture, width, height, outWidth, outHeight, usePointResample);
}
// ---------------------------------------------------------------------------
// TGA IMAGE LOADER
// ---------------------------------------------------------------------------
// GDI+ does not support Targa (.tga) natively.  This loader handles the two
// variants used in practice: uncompressed (type 2 = RGB, type 3 = greyscale)
// and RLE-compressed (type 10 = RGB RLE, type 11 = greyscale RLE).  Indexed
// (colour-mapped) TGA files are not supported and will throw.
//
// The output is always a 32-bit RGBA LoadedImage, consistent with the rest
// of the pipeline.  Bottom-up images (the TGA default) are flipped to
// top-down on load.

LoadedImage loadTgaFile(const fs::path& tgaPath) {
    std::ifstream f(tgaPath, std::ios::binary);
    if (!f)
        throw std::runtime_error("Cannot open TGA: " + tgaPath.string());

    // ---- TGA header (18 bytes) ----
    std::uint8_t idLength = 0; // length of image ID field
    std::uint8_t colorMapType = 0; // 0 = no colour map
    std::uint8_t imageType = 0; // 2=RGB, 3=grey, 10=RGB RLE, 11=grey RLE
    // Colour map spec (5 bytes, ignored)
    std::uint8_t cmSpec[5] = {};
    // Image spec
    std::uint16_t xOrigin = 0;
    std::uint16_t yOrigin = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint8_t  bpp = 0; // bits per pixel: 24 or 32
    std::uint8_t  descriptor = 0; // bit 5: top-down flag

    f.read(reinterpret_cast<char*>(&idLength), 1);
    f.read(reinterpret_cast<char*>(&colorMapType), 1);
    f.read(reinterpret_cast<char*>(&imageType), 1);
    f.read(reinterpret_cast<char*>(cmSpec), 5);
    f.read(reinterpret_cast<char*>(&xOrigin), 2);
    f.read(reinterpret_cast<char*>(&yOrigin), 2);
    f.read(reinterpret_cast<char*>(&width), 2);
    f.read(reinterpret_cast<char*>(&height), 2);
    f.read(reinterpret_cast<char*>(&bpp), 1);
    f.read(reinterpret_cast<char*>(&descriptor), 1);

    // Validate image type — only true-colour and greyscale are supported.
    if (imageType != 2 && imageType != 3 && imageType != 10 && imageType != 11)
        throw std::runtime_error("Unsupported TGA type " + std::to_string(imageType)
            + " in: " + tgaPath.string());

    if (width == 0 || height == 0)
        throw std::runtime_error("TGA has zero dimensions: " + tgaPath.string());

    // Bytes per pixel in the file: 3 (BGR) or 4 (BGRA) for colour;
    // 1 byte for greyscale regardless of bpp field.
    bool isGrey = (imageType == 3 || imageType == 11);
    bool isRLE = (imageType == 10 || imageType == 11);
    int  bytesPerPx = isGrey ? 1 : (bpp == 32 ? 4 : 3);

    // Skip the image ID field.
    if (idLength > 0)
        f.seekg(idLength, std::ios::cur);

    std::size_t pixelCount = static_cast<std::size_t>(width) * height;
    std::vector<RGBA> pixels(pixelCount);

    // ---- Decode pixels ----
    auto readPixel = [&](RGBA& out) {
        // Reads one pixel from the file stream into `out`.
        std::uint8_t buf[4] = {};
        if (isGrey) {
            f.read(reinterpret_cast<char*>(buf), 1);
            out = { buf[0], buf[0], buf[0], 0xff };
        }
        else if (bytesPerPx == 4) {
            f.read(reinterpret_cast<char*>(buf), 4);
            // TGA stores BGRA.
            out = { buf[2], buf[1], buf[0], buf[3] };
        }
        else {
            f.read(reinterpret_cast<char*>(buf), 3);
            // TGA stores BGR.
            out = { buf[2], buf[1], buf[0], 0xff };
        }
    };

    if (!isRLE) {
        // Uncompressed: one pixel per bytesPerPx bytes, row by row.
        for (std::size_t i = 0; i < pixelCount; ++i)
            readPixel(pixels[i]);
    }
    else {
        // RLE: each packet starts with a header byte.
        // High bit = 1 → run-length packet (repeat next pixel N+1 times).
        // High bit = 0 → raw packet (read next N+1 pixels verbatim).
        std::size_t i = 0;
        while (i < pixelCount) {
            std::uint8_t header = 0;
            f.read(reinterpret_cast<char*>(&header), 1);
            std::size_t count = static_cast<std::size_t>(header & 0x7f) + 1;
            if (header & 0x80) {
                // Run-length packet.
                RGBA colour = {};
                readPixel(colour);
                for (std::size_t j = 0; j < count && i < pixelCount; ++j, ++i)
                    pixels[i] = colour;
            }
            else {
                // Raw packet.
                for (std::size_t j = 0; j < count && i < pixelCount; ++j, ++i)
                    readPixel(pixels[i]);
            }
        }
    }

    // ---- Flip bottom-up images to top-down ----
    // TGA images are bottom-up by default (origin at bottom-left).
    // Bit 5 of the descriptor byte indicates top-down origin.
    bool topDown = (descriptor & 0x20) != 0;
    if (!topDown) {
        // Flip rows in-place.
        for (std::size_t row = 0; row < static_cast<std::size_t>(height) / 2; ++row) {
            std::swap_ranges(
                pixels.begin() + static_cast<std::ptrdiff_t>(row * width),
                pixels.begin() + static_cast<std::ptrdiff_t>(row * width + width),
                pixels.begin() + static_cast<std::ptrdiff_t>((height - 1 - row) * width)
            );
        }
    }

    LoadedImage image;
    image.width = width;
    image.height = height;
    image.pixels = std::move(pixels);
    return image;
}

// ---------------------------------------------------------------------------
// IMAGE LOADING (GDI+)
// ---------------------------------------------------------------------------

// Loads any image format supported by GDI+ (JPEG, PNG, BMP, GIF, TIFF …)
// from `imagePath` and returns the decoded pixel data as a LoadedImage.
// Every image is internally converted to 32-bit ARGB so that subsequent
// processing always operates on a consistent pixel format regardless of the
// source encoding.
// Loads any GDI+-supported image from `imagePath`.
// `frameIndex` selects the frame for animated GIFs (or any multi-frame
// format GDI+ supports via SelectActiveFrame).  For single-frame images
// the parameter is ignored.  If `frameIndex` exceeds the available frame
// count, the last frame is used.
LoadedImage loadImageFile(const fs::path& imagePath, int frameIndex = 0) {
    // TGA is not supported by GDI+; dispatch to the dedicated TGA loader.
    if (getLowerExtension(imagePath) == ".tga")
        return loadTgaFile(imagePath);  // frameIndex ignored — TGA is single-frame

    if (!ensureGdiplus()) {
        throw std::runtime_error("Could not initialise GDI+ for image loading.");
    }

    // GDI+ requires a wide-character path.
    std::wstring path = imagePath.wstring();
    Gdiplus::Bitmap source(path.c_str());
    if (source.GetLastStatus() != Gdiplus::Ok) {
        throw std::runtime_error("Could not open image: " + imagePath.string());
    }

    // ---- Frame selection for animated GIFs and other multi-frame formats ----
    // GDI+ exposes animation frames through the "time" dimension.
    // Query all available dimensions and select the time dimension if present.
    UINT dimCount = source.GetFrameDimensionsCount();
    if (dimCount > 0 && frameIndex != 0) {
        std::vector<GUID> dimIDs(dimCount);
        source.GetFrameDimensionsList(dimIDs.data(), dimCount);

        // Walk the dimension list looking for the time dimension (animation).
        for (UINT d = 0; d < dimCount; ++d) {
            UINT totalFrames = source.GetFrameCount(&dimIDs[d]);
            if (totalFrames > 1) {
                // Clamp to the last available frame if index is out of range.
                UINT safeFrame = static_cast<UINT>(
                    std::min(frameIndex, static_cast<int>(totalFrames) - 1)
                    );
                source.SelectActiveFrame(&dimIDs[d], safeFrame);
                break;
            }
        }
    }

    UINT width = source.GetWidth();
    UINT height = source.GetHeight();
    if (width == 0 || height == 0) {
        throw std::runtime_error("Image has zero dimensions: " + imagePath.string());
    }

    // Create a 32-bit ARGB destination bitmap and blit the source into it.
    // This handles palette, RGB-24, greyscale, and all other source formats.
    Gdiplus::Bitmap converted(width, height, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics graphics(&converted);
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
        if (graphics.DrawImage(&source, 0, 0, static_cast<INT>(width), static_cast<INT>(height)) != Gdiplus::Ok) {
            throw std::runtime_error("Could not convert image to 32bpp: " + imagePath.string());
        }
    }

    // Lock the bitmap bits for direct memory access.
    Gdiplus::Rect       rect(0, 0, static_cast<INT>(width), static_cast<INT>(height));
    Gdiplus::BitmapData bitmapData = {};
    if (converted.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bitmapData) != Gdiplus::Ok) {
        throw std::runtime_error("Could not lock image bits: " + imagePath.string());
    }

    LoadedImage image;
    image.width = width;
    image.height = height;
    image.pixels.resize(image.width * image.height);

    // GDI+ stores pixels as BGRA in memory; reorder to RGBA.
    const auto* base = static_cast<const std::uint8_t*>(bitmapData.Scan0);
    for (std::size_t y = 0; y < image.height; ++y) {
        const auto* row = base + static_cast<std::size_t>(bitmapData.Stride) * y;
        for (std::size_t x = 0; x < image.width; ++x) {
            const auto* src = row + x * 4;
            image.pixels[y * image.width + x] = { src[2], src[1], src[0], src[3] };
        }
    }

    converted.UnlockBits(&bitmapData);
    return image;
}

// ---------------------------------------------------------------------------
// MEDIAN-CUT COLOUR QUANTIZATION
// ---------------------------------------------------------------------------

// Re-computes the cached statistics (total pixel count, RGB bounding box)
// stored in `box` by iterating over its assigned colour subset.  Must be
// called after the order array is modified to keep the box data consistent.
void updateColorBox(ColorBox& box, const std::vector<ColorCount>& colors, const std::vector<std::size_t>& order) {
    box.total = 0;
    box.r_min = box.g_min = box.b_min = 255;
    box.r_max = box.g_max = box.b_max = 0;

    for (std::size_t i = box.begin; i < box.end; ++i) {
        const ColorCount& entry = colors[order[i]];
        box.total += entry.count;
        box.r_min = std::min(box.r_min, static_cast<int>(entry.color.r));
        box.r_max = std::max(box.r_max, static_cast<int>(entry.color.r));
        box.g_min = std::min(box.g_min, static_cast<int>(entry.color.g));
        box.g_max = std::max(box.g_max, static_cast<int>(entry.color.g));
        box.b_min = std::min(box.b_min, static_cast<int>(entry.color.b));
        box.b_max = std::max(box.b_max, static_cast<int>(entry.color.b));
    }
}

// Returns true when the box can still be split (i.e. it contains at least
// two distinct colours).  Boxes with a single colour must be kept intact.
bool isBoxSplittable(const ColorBox& box) {
    return box.end - box.begin > 1 &&
        (box.r_min != box.r_max || box.g_min != box.g_max || box.b_min != box.b_max);
}

// Computes a priority score for the box.  Boxes with a larger total pixel
// count multiplied by the widest colour-space extent are split first, which
// tends to produce the most perceptually uniform palette.
std::uint64_t colorBoxScore(const ColorBox& box) {
    int maxRange = std::max({ box.r_max - box.r_min, box.g_max - box.g_min, box.b_max - box.b_min });
    return isBoxSplittable(box) ? box.total * static_cast<std::uint64_t>(maxRange + 1) : 0;
}

// Splits `source` into two halves along its widest colour-space axis.
// The split point is chosen so that each half contains approximately the same
// total number of pixels (median cut).  `order` is sorted in-place for the
// colours in the source range.  Returns false if the box cannot be split.
bool splitColorBox(
    const ColorBox& source,
    const std::vector<ColorCount>& colors,
    std::vector<std::size_t>& order,
    ColorBox& left,
    ColorBox& right
) {
    if (!isBoxSplittable(source))
        return false;

    // Identify the axis with the greatest colour spread to sort along.
    enum class Channel { Red, Green, Blue };
    int redRange = source.r_max - source.r_min;
    int greenRange = source.g_max - source.g_min;
    int blueRange = source.b_max - source.b_min;

    Channel channel = Channel::Red;
    if (greenRange > redRange && greenRange >= blueRange) {
        channel = Channel::Green;
    }
    else if (blueRange > redRange && blueRange > greenRange) {
        channel = Channel::Blue;
    }

    // Stable-sort the index slice for this box by the chosen channel first,
    // then by the remaining channels as tie-breakers for reproducibility.
    auto begin = order.begin() + static_cast<std::ptrdiff_t>(source.begin);
    auto end = order.begin() + static_cast<std::ptrdiff_t>(source.end);
    std::stable_sort(begin, end, [&](std::size_t lhs, std::size_t rhs) {
        const ColorCount& a = colors[lhs];
        const ColorCount& b = colors[rhs];
        switch (channel) {
        case Channel::Red:
            if (a.color.r != b.color.r) return a.color.r < b.color.r;
            if (a.color.g != b.color.g) return a.color.g < b.color.g;
            return a.color.b < b.color.b;
        case Channel::Green:
            if (a.color.g != b.color.g) return a.color.g < b.color.g;
            if (a.color.r != b.color.r) return a.color.r < b.color.r;
            return a.color.b < b.color.b;
        case Channel::Blue:
        default:
            if (a.color.b != b.color.b) return a.color.b < b.color.b;
            if (a.color.r != b.color.r) return a.color.r < b.color.r;
            return a.color.g < b.color.g;
        }
        });

    // Walk from the start accumulating pixel counts until the running total
    // reaches half of the box total (the median cut point).
    std::uint64_t target = source.total / 2;
    std::uint64_t accum = 0;
    std::size_t   mid = source.begin;
    for (; mid < source.end; ++mid) {
        accum += colors[order[mid]].count;
        if (accum >= target) {
            ++mid;
            break;
        }
    }

    // Clamp mid so that neither child box is empty.
    if (mid <= source.begin || mid >= source.end) {
        mid = source.begin + (source.end - source.begin) / 2;
    }

    if (mid <= source.begin || mid >= source.end) {
        return false;
    }

    // Materialise the two child boxes and update their statistics.
    left = source;
    right = source;
    left.end = mid;
    right.begin = mid;
    updateColorBox(left, colors, order);
    updateColorBox(right, colors, order);
    return true;
}

// Runs the full median-cut quantization pipeline on the texture, producing a
// 256-entry palette and an 8-bit index map that maps each pixel to its
// nearest palette colour.  Palette entry 255 is reserved for the GoldSrc
// transparency key (opaque blue = 0,0,255); all other entries hold the
// quantized opaque colours.
std::pair<std::array<RGB, 256>, std::vector<std::uint8_t>> remapToIndexedColor(
    const std::vector<RGBA>& texture,
    std::size_t width,
    std::size_t height
) {
    std::array<RGB, 256> palette = {};
    // Initialise every pixel as transparent (index 255).
    std::vector<std::uint8_t> indexMap(width * height, 0xff);
    // Reserve palette[255] for the GoldSrc WAD transparency key colour.
    palette[255] = { 0, 0, 0xff };

    // Build a frequency histogram of all opaque colours in the texture.
    std::unordered_map<std::uint32_t, std::uint32_t> histogram;
    histogram.reserve(texture.size());

    for (const RGBA& pixel : texture) {
        // Skip transparent pixels; they will stay mapped to index 255.
        if (pixel.a == 0)
            continue;
        // Encode RGB as a packed 24-bit integer to use as the map key.
        std::uint32_t key =
            (static_cast<std::uint32_t>(pixel.r) << 16) |
            (static_cast<std::uint32_t>(pixel.g) << 8) |
            static_cast<std::uint32_t>(pixel.b);
        ++histogram[key];
    }

    if (histogram.empty()) {
        return { palette, indexMap };
    }

    // Flatten the histogram into a sortable vector of (colour, count) pairs.
    std::vector<ColorCount> colors;
    colors.reserve(histogram.size());
    for (const auto& entry : histogram) {
        colors.push_back({
            {
                static_cast<std::uint8_t>((entry.first >> 16) & 0xff),
                static_cast<std::uint8_t>((entry.first >> 8) & 0xff),
                static_cast<std::uint8_t>(entry.first & 0xff),
            },
            entry.second
            });
    }

    // Index array used for sorted partitioning without moving the color data.
    std::vector<std::size_t> order(colors.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }

    // Initialise the box list with a single box covering all colours.
    std::vector<ColorBox> boxes;
    boxes.push_back({ 0, order.size() });
    updateColorBox(boxes.back(), colors, order);

    // Iteratively split the highest-priority box until we have 255 boxes
    // (palette entries 0-254; entry 255 is the transparency key).
    while (boxes.size() < 255) {
        // Find the box with the highest split priority score.
        auto best = std::max_element(boxes.begin(), boxes.end(), [](const ColorBox& a, const ColorBox& b) {
            return colorBoxScore(a) < colorBoxScore(b);
            });

        if (best == boxes.end() || !isBoxSplittable(*best))
            break;

        ColorBox left;
        ColorBox right;
        if (!splitColorBox(*best, colors, order, left, right))
            break;

        // Replace the selected box with the left child; append the right child.
        *best = left;
        boxes.push_back(right);
    }

    // Compute each palette entry as the pixel-count-weighted average colour of
    // its box, which minimises the expected quantization error.
    std::size_t paletteCount = 0;
    for (const ColorBox& box : boxes) {
        if (box.begin >= box.end)
            continue;

        std::uint64_t sumR = 0;
        std::uint64_t sumG = 0;
        std::uint64_t sumB = 0;
        std::uint64_t total = 0;

        for (std::size_t i = box.begin; i < box.end; ++i) {
            const ColorCount& entry = colors[order[i]];
            sumR += static_cast<std::uint64_t>(entry.color.r) * entry.count;
            sumG += static_cast<std::uint64_t>(entry.color.g) * entry.count;
            sumB += static_cast<std::uint64_t>(entry.color.b) * entry.count;
            total += entry.count;
        }

        if (total == 0 || paletteCount >= 255)
            continue;

        // Round to nearest with integer arithmetic (add half of divisor before dividing).
        palette[paletteCount++] = {
            static_cast<std::uint8_t>((sumR + total / 2) / total),
            static_cast<std::uint8_t>((sumG + total / 2) / total),
            static_cast<std::uint8_t>((sumB + total / 2) / total),
        };
    }

    if (paletteCount == 0) {
        return { palette, indexMap };
    }

    // ---- Floyd-Steinberg dithering ----
    //
    // Standard nearest-colour mapping produces visible "banding" on gradients
    // because every pixel in a smooth ramp snaps to the same palette entry
    // for a range of input values.  Floyd-Steinberg dithering eliminates this
    // by tracking the quantization error at each pixel and distributing it to
    // the four neighbouring pixels that have not yet been processed, using
    // the classic 7/16 · 3/16 · 5/16 · 1/16 kernel:
    //
    //            current  →  7/16
    //   3/16  ←  5/16     →  1/16   (next row)
    //
    // The algorithm operates on a floating-point working buffer so that
    // accumulated errors are not rounded prematurely.  Transparent pixels
    // (alpha == 0) are skipped entirely: they receive no error and do not
    // contribute error to their neighbours, ensuring that the transparency
    // boundary stays crisp.
    //
    // Serpentine (boustrophedon) scanning — alternating left-to-right and
    // right-to-left rows — is used to prevent the directional error bias that
    // standard left-to-right-only scanning introduces on large uniform areas.

    // Working buffer: one float triplet (R, G, B) per pixel, initialised from
    // the normalised texture.  Only opaque pixels are populated; transparent
    // slots are left at zero and never read.
    struct FloatRGB { float r, g, b; };
    std::vector<FloatRGB> buf(width * height, { 0.f, 0.f, 0.f });

    // Pre-fill the buffer with the original (pre-error) colour values.
    for (std::size_t i = 0; i < texture.size(); ++i) {
        if (texture[i].a != 0) {
            buf[i] = {
                static_cast<float>(texture[i].r),
                static_cast<float>(texture[i].g),
                static_cast<float>(texture[i].b),
            };
        }
    }

    // Helper: find the nearest palette entry index for a clamped float colour.
    // Uses squared Euclidean distance in RGB space, identical to the original
    // nearest-colour mapping so dithering does not change the palette selection.
    auto nearestPaletteIndex = [&](float fr, float fg, float fb) -> std::size_t {
        // Clamp the accumulated colour to the valid byte range before matching.
        int qr = std::clamp(static_cast<int>(std::round(fr)), 0, 255);
        int qg = std::clamp(static_cast<int>(std::round(fg)), 0, 255);
        int qb = std::clamp(static_cast<int>(std::round(fb)), 0, 255);

        std::size_t bestIndex = 0;
        int         bestDistance = std::numeric_limits<int>::max();
        for (std::size_t pi = 0; pi < paletteCount; ++pi) {
            int dr = qr - static_cast<int>(palette[pi].r);
            int dg = qg - static_cast<int>(palette[pi].g);
            int db = qb - static_cast<int>(palette[pi].b);
            int dist = dr * dr + dg * dg + db * db;
            if (dist < bestDistance) {
                bestDistance = dist;
                bestIndex = pi;
            }
        }
        return bestIndex;
    };

    // Helper: add a weighted fraction of (er, eg, eb) to buf[idx] if the
    // target pixel is opaque.  Transparent pixels absorb no error so the
    // key-colour boundary is never contaminated by neighbouring colour noise.
    auto spreadError = [&](std::size_t idx, float er, float eg, float eb, float w) {
        if (idx < buf.size() && texture[idx].a != 0) {
            buf[idx].r += er * w;
            buf[idx].g += eg * w;
            buf[idx].b += eb * w;
        }
    };

    // Serpentine Floyd-Steinberg scan.
    for (std::size_t row = 0; row < height; ++row) {
        // Alternate scan direction each row to suppress directional bias.
        bool leftToRight = (row % 2 == 0);

        // Column iterator bounds and step for both directions.
        std::size_t colStart = leftToRight ? 0 : width - 1;
        std::size_t colEnd = leftToRight ? width : static_cast<std::size_t>(-1);
        std::ptrdiff_t colStep = leftToRight ? 1 : -1;

        for (std::size_t col = colStart; col != colEnd;
            col = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(col) + colStep))
        {
            std::size_t idx = row * width + col;

            // Transparent pixels are not quantized and do not spread error.
            if (texture[idx].a == 0) {
                indexMap[idx] = 0xff;
                continue;
            }

            // Quantize the current (error-accumulated) colour value.
            float fr = buf[idx].r;
            float fg = buf[idx].g;
            float fb = buf[idx].b;

            std::size_t best = nearestPaletteIndex(fr, fg, fb);
            indexMap[idx] = static_cast<std::uint8_t>(best);

            // Compute the quantization error: difference between the
            // floating-point colour and the palette entry we snapped to.
            float er = fr - static_cast<float>(palette[best].r);
            float eg = fg - static_cast<float>(palette[best].g);
            float eb = fb - static_cast<float>(palette[best].b);

            // Distribute the error to the four neighbours using the
            // Floyd-Steinberg kernel, mirrored horizontally on right-to-left
            // rows so the diffusion direction matches the scan direction.
            //
            //  left-to-right:          right-to-left (mirrored):
            //   cur  [+7/16]            [7/16]  cur
            //  [3/16][5/16][1/16]      [1/16][5/16][3/16]
            bool hasLeft = (col > 0);
            bool hasRight = (col + 1 < width);
            bool hasBelow = (row + 1 < height);

            if (leftToRight) {
                // Right neighbour on the same row (7/16).
                if (hasRight)
                    spreadError(idx + 1, er, eg, eb, 7.f / 16.f);
                // Row below: left-diagonal (3/16), directly below (5/16),
                // right-diagonal (1/16).
                if (hasBelow) {
                    if (hasLeft)  spreadError(idx + width - 1, er, eg, eb, 3.f / 16.f);
                    spreadError(idx + width, er, eg, eb, 5.f / 16.f);
                    if (hasRight) spreadError(idx + width + 1, er, eg, eb, 1.f / 16.f);
                }
            }
            else {
                // Mirrored: left neighbour on the same row (7/16).
                if (hasLeft)
                    spreadError(idx - 1, er, eg, eb, 7.f / 16.f);
                // Row below: right-diagonal (3/16), directly below (5/16),
                // left-diagonal (1/16).
                if (hasBelow) {
                    if (hasRight) spreadError(idx + width + 1, er, eg, eb, 3.f / 16.f);
                    spreadError(idx + width, er, eg, eb, 5.f / 16.f);
                    if (hasLeft)  spreadError(idx + width - 1, er, eg, eb, 1.f / 16.f);
                }
            }
        }
    }

    return { palette, indexMap };
}

// ---------------------------------------------------------------------------
// WAD FILE CONSTRUCTION
// ---------------------------------------------------------------------------

// Assembles a valid GoldSrc WAD3 file (tempdecal.wad) in memory from the
// already-quantized palette and index map.  The WAD file contains exactly one
// texture named "{LOGO" with four mip levels; mip 1-3 are filled with index
// 0xff (transparent) because GoldSrc generates the mips at load time.
std::vector<std::uint8_t> makeTempdecal(
    const std::array<RGB, 256>& palette,
    const std::vector<std::uint8_t>& indexMap,
    std::size_t width,
    std::size_t height
) {
    // Texture name exactly as expected by the GoldSrc decal system.
    static constexpr std::uint8_t NAME[16] = { '{', 'L', 'O', 'G', 'O', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

    // Mip level sizes: mip0 = full resolution; each successive level is
    // one quarter of the previous.
    std::size_t m0size = width * height;
    std::size_t m1size = m0size / 4;
    std::size_t m2size = m0size / 16;
    std::size_t m3size = m0size / 64;

    // Total texture entry size before alignment padding.
    std::size_t textureSize = 0x28 + (m0size + m1size + m2size + m3size) + 2 + 0x300;
    // Pad to a 16-byte boundary as required by the WAD3 format.
    std::size_t paddingSize = (16 - textureSize % 16) % 16;
    std::size_t alignedTextureSize = textureSize + paddingSize;
    // Directory entry starts immediately after the 16-byte WAD header and the
    // single aligned texture block.
    std::uint32_t directoryOffset = static_cast<std::uint32_t>(0x10 + alignedTextureSize);

    // Pre-allocate the output buffer to avoid repeated reallocations.
    std::vector<std::uint8_t> wad;
    wad.reserve(directoryOffset + 0x20);

    // ---- WAD3 file header (16 bytes) ----
    // Magic "WAD3" identifier.
    appendBytes(wad, "WAD3", 4);
    // Number of directory entries: exactly 1 (the tempdecal texture).
    appendValue<std::uint32_t>(wad, 1);
    // Byte offset to the directory block.
    appendValue<std::uint32_t>(wad, directoryOffset);
    // Unused padding field; zero.
    appendValue<std::uint32_t>(wad, 0);

    // ---- Texture entry header (0x28 = 40 bytes) ----
    // Texture name (null-padded to 16 bytes).
    appendBytes(wad, NAME, sizeof(NAME));
    // Texture dimensions in pixels.
    appendValue<std::uint32_t>(wad, static_cast<std::uint32_t>(width));
    appendValue<std::uint32_t>(wad, static_cast<std::uint32_t>(height));
    // Byte offsets to each mip level, relative to the start of this texture
    // entry (0x28 is immediately after the 40-byte header).
    appendValue<std::uint32_t>(wad, 0x28);
    appendValue<std::uint32_t>(wad, static_cast<std::uint32_t>(0x28 + m0size));
    appendValue<std::uint32_t>(wad, static_cast<std::uint32_t>(0x28 + m0size + m1size));
    appendValue<std::uint32_t>(wad, static_cast<std::uint32_t>(0x28 + m0size + m1size + m2size));

    // ---- Mip level 0: full-resolution index map ----
    appendBytes(wad, indexMap.data(), indexMap.size());
    // ---- Mip levels 1, 2, 3: filled with 0xff (transparent) ----
    // GoldSrc re-generates these at load time from mip 0.
    wad.insert(wad.end(), m1size, 0xff);
    wad.insert(wad.end(), m2size, 0xff);
    wad.insert(wad.end(), m3size, 0xff);

    // ---- Palette: 256 RGB entries preceded by a 2-byte count ----
    appendValue<std::uint16_t>(wad, 256);
    for (const RGB& color : palette) {
        wad.push_back(color.r);
        wad.push_back(color.g);
        wad.push_back(color.b);
    }

    // ---- Alignment padding to 16-byte boundary ----
    wad.insert(wad.end(), paddingSize, 0);

    // ---- WAD3 directory entry (one entry, 32 bytes) ----
    // Offset to the texture entry in the file (starts after the 16-byte header).
    appendValue<std::uint32_t>(wad, 0x10);
    // On-disk size of the texture entry (compressed == uncompressed since we
    // do not apply WAD compression).
    appendValue<std::uint32_t>(wad, static_cast<std::uint32_t>(textureSize));
    appendValue<std::uint32_t>(wad, static_cast<std::uint32_t>(textureSize));
    // Entry type byte: 0x43 = mip texture.
    wad.push_back(0x43);
    // Compression flag (0 = uncompressed).
    wad.push_back(0x00);
    // Two padding bytes.
    wad.push_back(0x00);
    wad.push_back(0x00);
    // Texture name repeated in the directory entry.
    appendBytes(wad, NAME, sizeof(NAME));

    return wad;
}

// Chains adjustSize → normalizeBinaryTransparency → remapToIndexedColor →
// makeTempdecal to convert a raw RGBA pixel buffer into a complete WAD3 file.
// The `sizeMode` parameter is passed through to adjustSize so the caller can
// control the target resolution class.
std::vector<std::uint8_t> convertTextureToTempdecal(
    const std::vector<RGBA>& texture,
    std::size_t width,
    std::size_t height,
    bool usePointResample,
    const std::string& sizeMode
) {
    std::vector<RGBA> adjusted;
    std::size_t adjustedWidth = 0;
    std::size_t adjustedHeight = 0;
    // Step 1: resize / pad to a valid WAD resolution.
    adjustSize(texture, width, height, usePointResample, sizeMode, adjusted, adjustedWidth, adjustedHeight);
    // Step 2: convert partial alpha to binary transparency.
    normalizeBinaryTransparency(adjusted);
    // Step 3: quantize to 256 colours.
    auto [palette, indexMap] = remapToIndexedColor(adjusted, adjustedWidth, adjustedHeight);
    // Step 4: build the WAD3 byte stream.
    return makeTempdecal(palette, indexMap, adjustedWidth, adjustedHeight);
}

// ---------------------------------------------------------------------------
// SHA-256 IMPLEMENTATION (portable, no external dependency)
// ---------------------------------------------------------------------------
// A minimal self-contained SHA-256 implementation used to generate stable
// content hashes for image files.  These hashes let the tool detect when an
// image has been modified on disk so that its cached .wad can be rebuilt.

namespace sha256 {

    // Initial hash values (first 32 bits of the fractional parts of the square
    // roots of the first 8 primes).
    static constexpr std::uint32_t H0[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };

    // Round constants (first 32 bits of the fractional parts of the cube roots of
    // the first 64 primes).
    static constexpr std::uint32_t K[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
        0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
        0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
        0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
    };

    // Right-rotate a 32-bit value by n bits.
    static inline std::uint32_t rotr(std::uint32_t x, unsigned n) {
        return (x >> n) | (x << (32u - n));
    }

    // Computes the SHA-256 digest of the byte array [data, data+size) and writes
    // the 32-byte binary result into `digest`.
    void compute(const std::uint8_t* data, std::size_t size, std::uint8_t digest[32]) {
        // Initialise working hash state from the standard IV.
        std::uint32_t h[8];
        for (int i = 0; i < 8; ++i) h[i] = H0[i];

        // Process the message in 64-byte (512-bit) blocks.  The final block
        // requires padding to append the 1-bit, zero-pad to 448 mod 512, and
        // a 64-bit big-endian message length.
        auto processBlock = [&](const std::uint8_t block[64]) {
            // Expand the 16-word message schedule to 64 words.
            std::uint32_t w[64];
            for (int t = 0; t < 16; ++t) {
                w[t] = (static_cast<std::uint32_t>(block[t * 4]) << 24) |
                    (static_cast<std::uint32_t>(block[t * 4 + 1]) << 16) |
                    (static_cast<std::uint32_t>(block[t * 4 + 2]) << 8) |
                    static_cast<std::uint32_t>(block[t * 4 + 3]);
            }
            for (int t = 16; t < 64; ++t) {
                std::uint32_t s0 = rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
                std::uint32_t s1 = rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
                w[t] = w[t - 16] + s0 + w[t - 7] + s1;
            }

            // Initialise working variables from the current hash state.
            std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
            std::uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];

            // 64 compression rounds.
            for (int t = 0; t < 64; ++t) {
                std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
                std::uint32_t ch = (e & f) ^ (~e & g);
                std::uint32_t temp1 = hh + S1 + ch + K[t] + w[t];
                std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
                std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
                std::uint32_t temp2 = S0 + maj;

                hh = g; g = f; f = e; e = d + temp1;
                d = c; c = b; b = a; a = temp1 + temp2;
            }

            // Add compressed chunk to the current hash value.
            h[0] += a; h[1] += b; h[2] += c; h[3] += d;
            h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
        };

        // Feed complete 64-byte blocks.
        std::size_t i = 0;
        for (; i + 64 <= size; i += 64) {
            processBlock(data + i);
        }

        // Build the final padded block(s).
        std::uint8_t lastBlock[128] = {};
        std::size_t  rem = size - i;
        // Copy remaining bytes.
        for (std::size_t j = 0; j < rem; ++j)
            lastBlock[j] = data[i + j];
        // Append the mandatory 0x80 byte.
        lastBlock[rem] = 0x80;

        // The 64-bit length field must end at the last byte of a 64-byte block.
        std::size_t blocks = (rem < 56) ? 1 : 2;
        std::uint64_t bitLength = static_cast<std::uint64_t>(size) * 8;
        std::size_t   lenOffset = blocks * 64 - 8;
        // Write length as big-endian 64-bit integer.
        for (int b = 7; b >= 0; --b) {
            lastBlock[lenOffset + static_cast<std::size_t>(7 - b)] =
                static_cast<std::uint8_t>((bitLength >> (b * 8)) & 0xff);
        }

        // Process the final 1 or 2 padded blocks.
        processBlock(lastBlock);
        if (blocks == 2)
            processBlock(lastBlock + 64);

        // Write the 256-bit digest as 8 big-endian 32-bit words.
        for (int j = 0; j < 8; ++j) {
            digest[j * 4] = static_cast<std::uint8_t>((h[j] >> 24) & 0xff);
            digest[j * 4 + 1] = static_cast<std::uint8_t>((h[j] >> 16) & 0xff);
            digest[j * 4 + 2] = static_cast<std::uint8_t>((h[j] >> 8) & 0xff);
            digest[j * 4 + 3] = static_cast<std::uint8_t>(h[j] & 0xff);
        }
    }

    // Computes the SHA-256 digest of the file at `path` and returns it as a
    // lowercase 64-character hexadecimal string.  Returns an empty string if the
    // file cannot be read.
    std::string hashFile(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return "";

        // Read the entire file into memory.  Spray images are at most a few MB
        // so this is fine.
        std::vector<std::uint8_t> bytes(
            (std::istreambuf_iterator<char>(in)),
            std::istreambuf_iterator<char>()
        );

        std::uint8_t digest[32] = {};
        compute(bytes.data(), bytes.size(), digest);

        // Format as a lowercase hex string.
        static constexpr char HEX[] = "0123456789abcdef";
        std::string hex;
        hex.reserve(64);
        for (int i = 0; i < 32; ++i) {
            hex.push_back(HEX[(digest[i] >> 4) & 0xf]);
            hex.push_back(HEX[digest[i] & 0xf]);
        }
        return hex;
    }

} // namespace sha256

// ---------------------------------------------------------------------------
// HASH STORE  (persistent SHA-256 cache for source images)
// ---------------------------------------------------------------------------
// The hash store is a plain-text file (HASH_FILE) where each line has the
// format:   <filename>:<sizemode>\t<sha256hex>
// The key intentionally includes the size mode so that changing the `size`
// setting in the INI produces a key miss and forces an unconditional rebuild
// of the cached .wad — even when the source image bytes are identical.
// It is loaded once at startup and written back whenever a change is detected.

// Loads the entire hash store from disk into a map from filename → hex hash.
// Returns an empty map if the file does not yet exist.
std::unordered_map<std::string, std::string> loadHashStore() {
    std::unordered_map<std::string, std::string> store;
    if (!fs::exists(HASH_FILE)) return store;

    std::ifstream in(HASH_FILE);
    std::string   line;
    while (std::getline(in, line)) {
        // Each non-empty line must contain a tab separator.
        std::size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        std::string filename = line.substr(0, tab);
        std::string hash = line.substr(tab + 1);
        if (!filename.empty() && !hash.empty())
            store[filename] = hash;
    }
    return store;
}

// Writes the in-memory hash store map back to HASH_FILE, overwriting any
// previous content.
void saveHashStore(const std::unordered_map<std::string, std::string>& store) {
    std::ofstream out(HASH_FILE);
    for (const auto& entry : store) {
        out << entry.first << '\t' << entry.second << '\n';
    }
}

// ---------------------------------------------------------------------------
// WAD MULTI-TEXTURE FRAME EXTRACTION
// ---------------------------------------------------------------------------

// Parses a WAD3 file and extracts one texture by index as a LoadedImage.
// `frameIndex` selects which texture entry to use (0 = first).  If the
// index exceeds the number of textures in the file, the last texture is used.
// Only WAD3 mip-texture entries (type 0x43) are counted; other entry types
// (fonts, etc.) are skipped.
//
// The extracted texture is returned as a 32-bit RGBA image with alpha=255
// for all pixels except the WAD transparency key colour (0,0,255) which is
// mapped to alpha=0.
LoadedImage loadWadFrame(const fs::path& wadPath, int frameIndex) {
    std::ifstream f(wadPath, std::ios::binary);
    if (!f)
        throw std::runtime_error("Cannot open WAD: " + wadPath.string());

    // ---- WAD3 file header ----
    char magic[4] = {};
    f.read(magic, 4);
    if (std::string(magic, 4) != "WAD3")
        throw std::runtime_error("Not a WAD3 file: " + wadPath.string());

    std::uint32_t numEntries = 0;
    std::uint32_t dirOffset = 0;
    f.read(reinterpret_cast<char*>(&numEntries), 4);
    f.read(reinterpret_cast<char*>(&dirOffset), 4);

    if (numEntries == 0)
        throw std::runtime_error("WAD contains no entries: " + wadPath.string());

    // ---- Read directory ----
    struct DirEntry {
        std::uint32_t offset;
        std::uint32_t diskSize;
        std::uint32_t size;
        std::uint8_t  type;
        std::uint8_t  compressed;
        std::uint16_t pad;
        char          name[16];
    };

    f.seekg(dirOffset);
    std::vector<DirEntry> dir(numEntries);
    for (auto& e : dir) {
        f.read(reinterpret_cast<char*>(&e.offset), 4);
        f.read(reinterpret_cast<char*>(&e.diskSize), 4);
        f.read(reinterpret_cast<char*>(&e.size), 4);
        f.read(reinterpret_cast<char*>(&e.type), 1);
        f.read(reinterpret_cast<char*>(&e.compressed), 1);
        f.read(reinterpret_cast<char*>(&e.pad), 2);
        f.read(e.name, 16);
    }

    // Collect only mip-texture entries (type 0x43).
    std::vector<const DirEntry*> textures;
    for (const auto& e : dir)
        if (e.type == 0x43) textures.push_back(&e);

    if (textures.empty())
        throw std::runtime_error("WAD has no mip-texture entries: " + wadPath.string());

    // Sort textures alphabetically by their internal name (case-insensitive)
    // so that _f0 always refers to the alphabetically first texture, _f1 to
    // the second, and so on — regardless of the order in which textures were
    // inserted into the WAD file.
    // Example: a WAD containing "wall01" and "dirt_renewed" always maps
    //   _f0 → "dirt_renewed"  (d < w)
    //   _f1 → "wall01"
    std::sort(textures.begin(), textures.end(), [](const DirEntry* a, const DirEntry* b) {
        // name fields are null-padded char[16]; compare as C strings,
        // case-insensitively so "Grass" and "grass" sort together.
        return _stricmp(a->name, b->name) < 0;
        });

    // Clamp frame index to last available texture.
    int safeFrame = std::min(frameIndex, static_cast<int>(textures.size()) - 1);
    const DirEntry* entry = textures[static_cast<std::size_t>(safeFrame)];

    // ---- Read mip-texture header ----
    f.seekg(entry->offset);
    char texName[16] = {};
    f.read(texName, 16);
    std::uint32_t texWidth = 0;
    std::uint32_t texHeight = 0;
    f.read(reinterpret_cast<char*>(&texWidth), 4);
    f.read(reinterpret_cast<char*>(&texHeight), 4);

    if (texWidth == 0 || texHeight == 0)
        throw std::runtime_error("WAD texture has zero dimensions.");

    std::uint32_t mip0Offset = 0;
    f.read(reinterpret_cast<char*>(&mip0Offset), 4);
    // Skip mip offsets 1-3 (not needed).
    f.seekg(entry->offset + mip0Offset);

    // ---- Read mip level 0 index map ----
    std::size_t pixelCount = static_cast<std::size_t>(texWidth) * texHeight;
    std::vector<std::uint8_t> indices(pixelCount);
    f.read(reinterpret_cast<char*>(indices.data()), static_cast<std::streamsize>(pixelCount));

    // Skip mip1, mip2, mip3 to reach the palette.
    std::size_t mip1 = pixelCount / 4;
    std::size_t mip2 = pixelCount / 16;
    std::size_t mip3 = pixelCount / 64;
    f.seekg(static_cast<std::streamoff>(mip1 + mip2 + mip3), std::ios::cur);

    // ---- Read 256-entry RGB palette ----
    std::uint16_t palCount = 0;
    f.read(reinterpret_cast<char*>(&palCount), 2);
    std::vector<RGB> palette(256);
    for (int i = 0; i < 256; ++i) {
        f.read(reinterpret_cast<char*>(&palette[i].r), 1);
        f.read(reinterpret_cast<char*>(&palette[i].g), 1);
        f.read(reinterpret_cast<char*>(&palette[i].b), 1);
    }

    // ---- Decode index map to RGBA ----
    // Palette index 255 is the GoldSrc transparency key (opaque blue → alpha 0).
    LoadedImage image;
    image.width = texWidth;
    image.height = texHeight;
    image.pixels.resize(pixelCount);
    for (std::size_t i = 0; i < pixelCount; ++i) {
        std::uint8_t idx = indices[i];
        if (idx == 255) {
            image.pixels[i] = { 0, 0, 0xff, 0 };
        }
        else {
            image.pixels[i] = { palette[idx].r, palette[idx].g, palette[idx].b, 0xff };
        }
    }

    return image;
}

// ---------------------------------------------------------------------------
// IMAGE-TO-WAD CONVERSION WITH HASH-BASED CHANGE DETECTION
// ---------------------------------------------------------------------------

// Checks whether the .wad cached in CONVERTED_DIR for the given source image
// is up to date.  If the source image's SHA-256 hash has changed since the
// .wad was last built (or no .wad exists yet), the WAD is rebuilt and the
// hash store is updated.
//
// The function also handles `convert-autodeletion`: when enabled it removes
// any .wad from CONVERTED_DIR whose corresponding source image no longer
// exists in SPRAYS_DIR.
fs::path convertImageSprayToWad(
    const fs::path& imagePath,
    const std::string& sizeMode,
    int frameIndex,
    bool autoDeleteOrphans,
    std::unordered_map<std::string, std::string>& hashStore
) {
    // A .wad with the same stem placed directly in SPRAYS_DIR always takes
    // priority over a converted version.  Check using the bare stem (without
    // size-mode suffix) so a hand-placed "graffiti.wad" overrides any
    // converted variant regardless of which size mode is active.
    fs::path bareName = imagePath.filename();
    bareName = bareName.replace_extension(".wad");
    fs::path wadInSprays = SPRAYS_DIR / bareName;
    if (fs::exists(wadInSprays))
        return wadInSprays;

    fs::create_directories(CONVERTED_DIR);

    // Each size mode gets its own cached .wad file (cleanStem_<mode>.wad) so
    // that switching modes does not overwrite the previously cached version.
    // If the source filename already carries a name flag (e.g. logo_small.jpg),
    // we strip that flag from the stem before appending the mode suffix to
    // avoid double-suffixes like "logo_small_small.wad".
    // Examples:
    //   "graffiti.png"   + mode "medium" → "graffiti_medium.wad"
    //   "logo_small.jpg" + mode "small"  → "logo_small.wad"
    std::string _cleanStem;
    extractNameFlag(imagePath.filename(), _cleanStem);   // sets _cleanStem
    // Include frame index in the cached filename when non-zero so that
    // different frame selections for the same source file don't collide.
    std::string cachedStem = _cleanStem + "_" + sizeMode;
    if (frameIndex != 0) cachedStem += "_f" + std::to_string(frameIndex);
    fs::path    wadName = fs::path(cachedStem).replace_extension(".wad");

    // When convert-autodeletion is enabled, scan CONVERTED_DIR for .wad files
    // whose source images no longer exist and delete them before we proceed.
    if (autoDeleteOrphans) {
        try {
            for (const auto& entry : fs::directory_iterator(CONVERTED_DIR)) {
                if (!entry.is_regular_file()) continue;
                if (getLowerExtension(entry.path()) != ".wad") continue;

                // The cached .wad filename has the form  stem_<mode>.wad.
                // Strip the known size-mode suffixes to recover the original
                // image stem before checking whether the source still exists.
                std::string wadStem = entry.path().stem().string();
                std::string sourceStem = wadStem;
                for (const std::string& sz : { "_large", "_medium", "_small" }) {
                    std::string wadStemLow = toLowerCopy(wadStem);
                    if (wadStemLow.size() > sz.size() &&
                        wadStemLow.substr(wadStemLow.size() - sz.size()) == sz)
                    {
                        sourceStem = wadStem.substr(0, wadStem.size() - sz.size());
                        break;
                    }
                }
                bool sourceGone = true;
                // Check all supported image extensions for the recovered source stem.
                for (const std::string& ext : { ".png", ".jpg", ".jpeg" }) {
                    if (fs::exists(SPRAYS_DIR / (sourceStem + ext))) {
                        sourceGone = false;
                        break;
                    }
                }
                if (sourceGone) {
                    // Remove the orphaned .wad and its hash store entry.
                    // The store key format is: <imageFilename>:<sizeMode>
                    // Reconstruct all possible keys for this cached wad file.
                    fs::remove(entry.path());
                    // Erase all possible (ext, size, frame) combinations.
                    // Frame 0 uses no suffix; frames 1+ use :1, :2 etc.
                    for (const std::string& ext : { ".png", ".jpg", ".jpeg", ".bmp", ".gif", ".tiff", ".tif", ".tga", ".wad" }) {
                        for (const std::string& sz : { "large", "medium", "small" }) {
                            for (int fr = 0; fr < 32; ++fr) {
                                hashStore.erase(sourceStem + ext + ":" + sz + ":" + std::to_string(fr));
                            }
                        }
                    }
                }
            }
        }
        catch (...) {
            // Non-fatal: if cleanup fails for any reason, continue with the
            // conversion so the primary function still succeeds.
        }
    }

    fs::path wadInConverted = CONVERTED_DIR / wadName;

    // Compute the current SHA-256 hash of the source image file.
    std::string currentHash = sha256::hashFile(imagePath);
    std::string storedValue = "";
    // The store key combines the source filename, size mode, and frame index
    // so that changing any of these produces a cache miss and forces a rebuild.
    std::string storeKey = imagePath.filename().string() + ":" + sizeMode + ":" + std::to_string(frameIndex);

    // Look up the previously stored value for this (file, size) combination.
    auto it = hashStore.find(storeKey);
    if (it != hashStore.end())
        storedValue = it->second;

    // The .wad is up to date when: it exists on disk AND the stored hash
    // matches the current hash of the source image for the same size mode.
    bool wadExists = fs::exists(wadInConverted);
    bool hashMatches = (!currentHash.empty() && currentHash == storedValue);

    if (wadExists && hashMatches) {
        // Source image is unchanged and size mode is the same; return the cached .wad.
        return wadInConverted;
    }

    // Either the .wad does not exist, the source image was modified, the
    // size mode changed, or the frame index changed.  Re-convert.
    // For WAD source files use the WAD frame extractor; for all other image
    // formats use the GDI+ loader with frame selection.
    LoadedImage image;
    if (getLowerExtension(imagePath) == ".wad")
        image = loadWadFrame(imagePath, frameIndex);
    else
        image = loadImageFile(imagePath, frameIndex);
    std::vector<std::uint8_t> wad = convertTextureToTempdecal(
        image.pixels, image.width, image.height,
        false, // bilinear interpolation
        sizeMode
    );

    // Write the new .wad to the converted directory.
    std::ofstream out(wadInConverted, std::ios::binary);
    if (!out) {
        throw std::runtime_error("Could not create converted WAD: " + wadInConverted.string());
    }
    out.write(reinterpret_cast<const char*>(wad.data()), static_cast<std::streamsize>(wad.size()));
    if (!out) {
        throw std::runtime_error("Could not write converted WAD: " + wadInConverted.string());
    }

    // Update the hash store so the next run knows this (file, size) combination
    // is current.  Stale entries for the same file under different size modes
    // are left in the store; they are harmless and will be overwritten if the
    // user switches back to that size mode.
    if (!currentHash.empty()) {
        hashStore[storeKey] = currentHash;
        saveHashStore(hashStore);
    }

    return wadInConverted;
}

// ---------------------------------------------------------------------------
// CONFIGURATION
// ---------------------------------------------------------------------------

// Holds all settings parsed from the INI file.  Defaults reflect the
// original behaviour before the configuration system was introduced.
struct Config {
    // Spray selection strategy: "sequential" (default), "random", or "static".
    std::string method = "sequential";
    // When true, the spray is copied to the folder corresponding to the -game
    // argument rather than always using "valve".
    bool        include_mods = true;
    // Used only when method=static; specifies the spray file to use.
    std::string sprayname = "";
    // Executable to launch after applying the spray (default: "hl.exe").
    std::string launch = "hl.exe";
    // Target spray size class: "large" (default), "medium", or "small".
    std::string size = "large";
    // When true, orphaned .wad files in CONVERTED_DIR are removed on each run.
    bool        convert_autodeletion = true;
    // When true, the program stays alive after launching the game and applies
    // a new spray every cycle_delay seconds until the game process exits.
    bool        cycle_on_runtime = false;
    // Interval in seconds between spray cycles when cycle_on_runtime is true.
    // Minimum clamped to 10 seconds to avoid hammering the filesystem.
    int         cycle_delay = 60;
};

// ---------------------------------------------------------------------------
// INI ENTRY DEFINITIONS
// ---------------------------------------------------------------------------
// Each setting is declared here as an IniEntry.  loadConfig() uses this table
// to parse values AND to append any missing entries to the INI file on every
// run, so users always get new settings without having to delete the file.
//
// Fields:
//   key          - the INI key name (case-insensitive on read).
//   defaultValue - written when the entry is missing from the file.
//   comment      - inline comment written on the same line as the key.
//   preamble     - optional lines written above the key (comments, blank
//                  lines).  Each element becomes one line in the file.

struct IniEntry {
    std::string              key;
    std::string              defaultValue;
    std::string              comment;
    std::vector<std::string> preamble;  // written before the key=value line
};

// Master list of all recognised settings, in the order they appear in the
// INI file.  The two cycle-* entries are separated from the rest by two
// blank lines and preceded by the experimental warning block.
static const std::vector<IniEntry> INI_ENTRIES = {
    {
        "method", "sequential",
        "random/sequential/static",
        {}
    },
    {
        "include-mods", "true",
        "true/false -> sets the spray on the modfolder based on -game parameter",
        {}
    },
    {
        "sprayname", "",
        "name.wad/name.png/name.jpg -> only needed when method=static",
        {}
    },
    {
        "launch", "hl.exe",
        "executable to launch after applying the spray",
        {}
    },
    {
        "size", "large",
        "large (default)/medium/small -> controls output spray resolution",
        {}
    },
    {
        "convert-autodeletion", "true",
        "true/false -> auto-delete converted wads when source image is gone",
        {}
    },
    // ---- Experimental cycle settings (separated by blank lines) ----
    {
        "cycle-on-runtime", "false",
        "true/false -> keep running and re-apply spray on a timer while the game is open",
        {
            "",
            "",
            "; [EXPERIMENTAL] servers cache sprays in custom.hpk so the change is not always",
            ";   visible immediately. retry/rejoin to the same server may not reflect the new",
            ";   spray. for best results:",
            ";   reconnect from the server browser so the server re-sends the cache from scratch."
        }
    },
    {
        "cycle-delay", "60",
        "seconds between spray cycles when cycle-on-runtime=true (minimum 10)",
        {}
    },
};

// Writes the INI line for a single entry: optional preamble lines, then the
// key=value line with its inline comment.
static void writeIniEntry(std::ofstream& f, const IniEntry& entry, const std::string& value) {
    for (const std::string& pre : entry.preamble)
        f << pre << "\n";
    // Pad the key=value portion to column 32 for alignment.
    std::string kv = entry.key + "=" + value;
    if (kv.size() < 32)
        kv += std::string(32 - kv.size(), ' ');
    f << kv << ";" << entry.comment << "\n";
}

// Parses the INI file and returns a Config struct, then appends any settings
// that are missing from the file so the file is always up to date.
// Existing user values are never modified.
Config loadConfig() {
    // ---- Step 1: read whatever is already in the file ----
    // Build a map of key → raw value string from the file on disk.
    std::unordered_map<std::string, std::string> found;
    bool fileExists = fs::exists(INI_PATH);

    if (fileExists) {
        std::ifstream file(INI_PATH);
        std::string   line;
        while (std::getline(file, line)) {
            line = trim(line);
            if (line.empty() || line[0] == ';') continue;
            std::size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = toLowerCopy(trim(line.substr(0, eq)));
            std::string value = trim(line.substr(eq + 1));
            // Strip inline comment.
            std::size_t sc = value.find(';');
            if (sc != std::string::npos)
                value = trim(value.substr(0, sc));
            found[key] = value;
        }
    }

    // ---- Step 2: append missing entries to the file ----
    // Open in append mode so existing content is untouched.  If the file does
    // not exist yet it is created from scratch.
    {
        // Count how many entries are missing before opening the file.
        std::size_t missingCount = 0;
        for (const IniEntry& entry : INI_ENTRIES)
            if (found.count(entry.key) == 0) ++missingCount;

        if (missingCount > 0) {
            // Before appending, check whether the existing file ends with a
            // newline.  If it does not (e.g. it was written by an older version
            // of the tool or edited manually without a trailing newline), the
            // first appended entry would be glued onto the last existing line.
            if (fileExists) {
                std::ifstream check(INI_PATH, std::ios::binary | std::ios::ate);
                if (check) {
                    // Cast to std::streamoff to avoid ambiguous operator- on streampos.
                    std::streamoff sz = static_cast<std::streamoff>(check.tellg());
                    if (sz > 0) {
                        check.seekg(sz - 1);
                        char last = 0;
                        check.read(&last, 1);
                        // If the file does not end with LF, we need to add one.
                        if (last != '\n') {
                            std::ofstream fix(INI_PATH, std::ios::app);
                            fix << "\n";
                        }
                    }
                }
            }
            std::ofstream f(INI_PATH, std::ios::app);
            for (const IniEntry& entry : INI_ENTRIES) {
                if (found.count(entry.key) == 0) {
                    // This entry is missing — write it using its default value.
                    writeIniEntry(f, entry, entry.defaultValue);
                }
            }
        }
    }

    // ---- Step 3: populate Config from the merged key→value map ----
    // For keys that were absent from the file we fall back to the default
    // declared in INI_ENTRIES so the Config always has a valid value.
    auto get = [&](const std::string& key) -> std::string {
        auto it = found.find(key);
        if (it != found.end()) return it->second;
        // Fall back to the declared default.
        for (const IniEntry& e : INI_ENTRIES)
            if (e.key == key) return e.defaultValue;
        return "";
    };

    Config cfg;
    cfg.method = toLowerCopy(get("method"));
    cfg.include_mods = (toLowerCopy(get("include-mods")) == "true");
    cfg.sprayname = get("sprayname");
    {
        std::string v = get("launch");
        cfg.launch = v.empty() ? "hl.exe" : v;
    }
    {
        std::string v = toLowerCopy(get("size"));
        cfg.size = v.empty() ? "large" : v;
    }
    cfg.convert_autodeletion = (toLowerCopy(get("convert-autodeletion")) == "true");
    cfg.cycle_on_runtime = (toLowerCopy(get("cycle-on-runtime")) == "true");
    {
        try {
            cfg.cycle_delay = std::max(10, std::stoi(get("cycle-delay")));
        }
        catch (...) {
            cfg.cycle_delay = 60;
        }
    }

    return cfg;
}

// ---------------------------------------------------------------------------
// SPRAY ENUMERATION
// ---------------------------------------------------------------------------

// Scans SPRAYS_DIR for files with supported extensions and returns their
// names sorted using the Windows natural-sort order so the sequential mode
// matches what the user sees in Explorer.
std::vector<std::string> getSprays() {
    std::vector<std::string> sprays;

    // Create the directory if it doesn't exist yet so the user gets a clear
    // place to put their spray files.
    if (!fs::exists(SPRAYS_DIR))
        fs::create_directories(SPRAYS_DIR);

    for (const auto& entry : fs::directory_iterator(SPRAYS_DIR)) {
        if (!entry.is_regular_file())
            continue;

        if (isSupportedSprayExtension(entry.path())) {
            sprays.push_back(entry.path().filename().string());
        }
    }

    std::sort(sprays.begin(), sprays.end(), windowsNaturalSort);
    return sprays;
}

// ---------------------------------------------------------------------------
// STATE FILE  (persistent last-spray / shuffle-deck storage)
// ---------------------------------------------------------------------------
// The state file stores two pieces of persistent data on separate lines:
//   Line 1: the filename of the last spray that was applied (all modes).
//   Line 2: space-separated shuffle deck for random mode (remaining sprays
//            in the current cycle that have not yet been shown).
// If line 2 is absent the shuffle deck is treated as empty, which triggers
// a new full shuffle on the next random-mode pick.

// Reads just the first line of the state file (the last spray name).
std::string getLastSpray() {
    if (!fs::exists(STATE_FILE)) return "";
    std::ifstream f(STATE_FILE);
    std::string   s;
    std::getline(f, s);
    return s;
}

// Reads the second line of the state file and parses it as a space-separated
// list of spray names representing the remaining shuffle deck.
std::vector<std::string> getShuffleDeck() {
    if (!fs::exists(STATE_FILE)) return {};
    std::ifstream     f(STATE_FILE);
    std::string       first;
    // Consume and discard the first line (last spray name).
    std::getline(f, first);
    std::string       deckLine;
    if (!std::getline(f, deckLine)) return {};
    deckLine = trim(deckLine);
    if (deckLine.empty()) return {};

    // Tokenise on spaces.
    std::vector<std::string> deck;
    std::istringstream       ss(deckLine);
    std::string              token;
    while (ss >> token)
        deck.push_back(token);
    return deck;
}

// Writes `lastSpray` as line 1 and `deck` as a space-separated list on
// line 2 to the state file, replacing any previous content.
void saveState(const std::string& lastSpray, const std::vector<std::string>& deck) {
    std::ofstream f(STATE_FILE);
    f << lastSpray << '\n';
    for (std::size_t i = 0; i < deck.size(); ++i) {
        if (i > 0) f << ' ';
        f << deck[i];
    }
    f << '\n';
}

// Convenience wrapper for callers that only need to update the last-spray
// field without touching the shuffle deck.
void saveLastSpray(const std::string& s) {
    // Preserve the existing deck (if any) so sequential/static modes do not
    // accidentally clear the random-mode rotation.
    std::vector<std::string> deck = getShuffleDeck();
    saveState(s, deck);
}

// ---------------------------------------------------------------------------
// CASE-INSENSITIVE SPRAY SEARCH
// ---------------------------------------------------------------------------

// Searches `sprays` for an entry whose name matches `name` ignoring case.
// Returns a pointer to the matching entry, or nullptr if none is found.
const std::string* findSprayName(const std::vector<std::string>& sprays, const std::string& name) {
    auto it = std::find_if(sprays.begin(), sprays.end(), [&](const std::string& spray) {
        return equalsIgnoreCase(spray, name);
        });
    return it == sprays.end() ? nullptr : &(*it);
}

// ---------------------------------------------------------------------------
// SPRAY SELECTION  (chooseSpray)
// ---------------------------------------------------------------------------
// All three selection modes (random, sequential, static) are implemented here.
//
// RANDOM mode  – guaranteed non-repeating full-cycle shuffle:
//   The tool maintains a "shuffle deck" in the state file (line 2).  On each
//   random pick we draw from the deck.  When the deck is empty (or does not
//   contain any current spray) we rebuild it as a fresh Fisher-Yates shuffle
//   of all available sprays, then draw the first entry.  This guarantees that
//   every spray is shown exactly once per cycle before any spray is repeated.
//
// SEQUENTIAL mode – step one position forward through the sorted list:
//   The last spray name is looked up in the current list; the next entry is
//   returned.  If the last spray is not found (deleted?) we wrap back to the
//   beginning.
//
// STATIC mode – always use the configured spray:
//   Returns cfg.sprayname if it exists in the list, otherwise falls back to
//   the last-used spray, and finally to the first entry.

std::string chooseSpray(const Config& cfg, const std::vector<std::string>& sprays, const std::string& activeMod) {
    if (sprays.empty())
        return "";

    // ---- Filter by _game- flag ----
    // Build a filtered list containing only sprays that are compatible with
    // the active mod.  A spray without a _game- flag is always compatible.
    // A spray with _game-<mod> is only compatible when <mod> matches activeMod.
    std::vector<std::string> eligible;
    for (const std::string& name : sprays) {
        SprayFlags f = parseNameFlags(fs::path(name));
        if (f.gameTarget.empty() || equalsIgnoreCase(f.gameTarget, activeMod))
            eligible.push_back(name);
    }

    // If no sprays are eligible for this mod, return empty — do not alter
    // tempdecal.wad and launch the game without changing the spray.
    if (eligible.empty())
        return "";

    // ---- _static flag: takes absolute priority over method= ----
    // If any eligible spray carries the _static flag, it is always used.
    // When multiple static sprays exist, the first in natural sort order wins.
    // A _static + _game- combination makes the spray static only for that mod.
    for (const std::string& name : eligible) {
        SprayFlags f = parseNameFlags(fs::path(name));
        if (f.isStatic) {
            saveLastSpray(name);
            return name;
        }
    }

    std::string last = getLastSpray();

    // ---- RANDOM mode (no-repeat shuffle deck) ----
    if (cfg.method == "random") {
        // Load the persisted shuffle deck.
        std::vector<std::string> deck = getShuffleDeck();

        // Remove any entries that are no longer present in the current spray
        // list (e.g. files were deleted since the last run).
        deck.erase(
            std::remove_if(deck.begin(), deck.end(), [&](const std::string& name) {
                return findSprayName(eligible, name) == nullptr;
                }),
            deck.end()
                    );

        // If the deck is exhausted (or was just cleared), build a new full
        // shuffle of all available sprays so the next cycle begins.
        if (deck.empty()) {
            deck = eligible; // copy the eligible filtered list

            // Fisher-Yates shuffle seeded with the current time combined with
            // a hash of the last spray name for additional entropy, ensuring
            // different shuffles even when launched in rapid succession.
            std::size_t seed = static_cast<std::size_t>(time(NULL));
            for (const char c : last)
                seed ^= static_cast<std::size_t>(c) + 0x9e3779b9u + (seed << 6) + (seed >> 2);

            std::mt19937 rng(static_cast<unsigned>(seed));
            for (std::size_t i = deck.size() - 1; i > 0; --i) {
                // Generate a uniform random index in [0, i].
                std::uniform_int_distribution<std::size_t> dist(0, i);
                std::swap(deck[i], deck[dist(rng)]);
            }

            // If the first entry of the new shuffle matches the spray that was
            // just applied, rotate the deck by one position to avoid an
            // immediate consecutive repeat at the cycle boundary.
            if (deck.size() > 1 && equalsIgnoreCase(deck.front(), last)) {
                std::rotate(deck.begin(), deck.begin() + 1, deck.end());
            }
        }

        // Draw the first entry from the front of the deck.
        std::string chosen = deck.front();
        deck.erase(deck.begin());

        // Persist the updated deck and the chosen spray name.
        saveState(chosen, deck);
        return chosen;
    }

    // ---- SEQUENTIAL mode ----
    if (cfg.method == "sequential") {
        // Locate the last spray in the current list.
        auto it = std::find_if(eligible.begin(), eligible.end(), [&](const std::string& spray) {
            return equalsIgnoreCase(spray, last);
            });
        // Advance to the next entry, wrapping to the beginning if we're at
        // the end or the last spray was not found.
        if (it != eligible.end() && ++it != eligible.end()) {
            saveLastSpray(*it);
            return *it;
        }
        // Wrap-around: start from the first eligible spray.
        saveLastSpray(eligible[0]);
        return eligible[0];
    }

    // ---- STATIC mode ----
    if (cfg.method == "static") {
        if (!cfg.sprayname.empty()) {
            // Return the configured spray if it is present.
            if (const std::string* match = findSprayName(eligible, cfg.sprayname)) {
                saveLastSpray(*match);
                return *match;
            }
        }
        // Fall back to whatever was used last, or the first spray in the list.
        if (!last.empty() && findSprayName(eligible, last)) return last;
        saveLastSpray(eligible[0]);
        return eligible[0];
    }

    // Fallback for unrecognised method values: use the first eligible spray.
    saveLastSpray(eligible[0]);
    return eligible[0];
}

// ---------------------------------------------------------------------------
// MOD DETECTION
// ---------------------------------------------------------------------------

// Scans the forwarded command-line arguments for a "-game" flag and returns
// the mod directory name that follows it.  Defaults to "valve" (the base
// Half-Life game directory) when no "-game" argument is present.
std::string detectMod(const std::vector<std::string>& args) {
    for (std::size_t i = 1; i + 1 < args.size(); i++) {
        if (args[i] == "-game")
            return args[i + 1];
    }
    return "valve";
}

// ---------------------------------------------------------------------------
// SPRAY FILE RESOLUTION
// ---------------------------------------------------------------------------

// Given a spray name (filename as returned by getSprays()), this function
// returns the path of the .wad file to copy as tempdecal.wad.  For .wad
// sprays the path is returned directly.  For image sprays the conversion
// pipeline is invoked (with hash-change detection) and the resulting path
// inside CONVERTED_DIR is returned.
fs::path resolveSprayFile(
    const std::string& sprayName,
    const std::string& sizeMode,
    bool autoDeleteOrphans,
    std::unordered_map<std::string, std::string>& hashStore
) {
    fs::path selected = SPRAYS_DIR / sprayName;
    if (!fs::exists(selected)) {
        throw std::runtime_error("Selected spray not found: " + sprayName);
    }

    // Parse all name flags from the filename.
    SprayFlags flags = parseNameFlags(selected.filename());

    // Determine effective size mode: flag overrides global INI setting.
    const std::string& effectiveSizeMode = flags.sizeMode.empty() ? sizeMode : flags.sizeMode;

    // Plain .wad with no _f flag and no size flag: return as-is (no conversion).
    // .wad with a _f flag (multi-texture selection): run through conversion pipeline.
    if (getLowerExtension(selected) == ".wad" && flags.frameIndex == 0 && flags.sizeMode.empty())
        return selected;

    if (!isImageSprayExtension(selected) && getLowerExtension(selected) != ".wad") {
        throw std::runtime_error("Unsupported spray format: " + selected.string());
    }

    // Convert (or return cached conversion) using the resolved size mode and frame.
    return convertImageSprayToWad(selected, effectiveSizeMode, flags.frameIndex, autoDeleteOrphans, hashStore);
}

// ---------------------------------------------------------------------------
// APPLY SPRAY
// ---------------------------------------------------------------------------

// Returns true when the two files are byte-for-byte identical.
// Strategy: compare file sizes first (cheap), then SHA-256 hashes only when
// sizes match.  This avoids hashing large files that differ in size.
bool filesAreIdentical(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    auto sizeA = fs::file_size(a, ec); if (ec) return false;
    auto sizeB = fs::file_size(b, ec); if (ec) return false;
    if (sizeA != sizeB) return false;
    // Same size: compare hashes to rule out hash collisions from coincidental
    // size matches (extremely unlikely for WAD files but correct to check).
    return sha256::hashFile(a) == sha256::hashFile(b);
}

// Copies the chosen spray .wad to <gameDir>/<mod>/tempdecal.wad.
// The copy is skipped entirely when the destination already contains the
// exact same file — this prevents redundant disk writes in cycle-on-runtime
// mode when only one spray is available, or in static mode when the spray
// has not changed since the last application.
// When a copy is necessary the destination is temporarily set to normal
// attributes before overwriting (GoldSrc marks tempdecal.wad read-only),
// then re-marked read-only afterwards so the engine does not clobber it.
void applySpray(const fs::path& sprayPath, const fs::path& gameDir, const std::string& mod) {
    fs::path dstDir = gameDir / mod;

    // Ensure the mod directory exists (important for newly installed mods).
    if (!fs::exists(dstDir))
        fs::create_directories(dstDir);

    fs::path dst = dstDir / "tempdecal.wad";

    // Skip the copy if the destination already contains this exact spray.
    // This is the common case in static mode and single-spray collections.
    if (fs::exists(dst) && filesAreIdentical(sprayPath, dst))
        return;

    // Clear the read-only flag so the copy can overwrite the destination.
    if (fs::exists(dst))
        SetFileAttributesA(dst.string().c_str(), FILE_ATTRIBUTE_NORMAL);

    // Copy the selected spray, overwriting any existing tempdecal.wad.
    fs::copy_file(sprayPath, dst, fs::copy_options::overwrite_existing);

    // Re-mark as read-only so GoldSrc does not replace it with the default
    // spray when the engine initialises.
    SetFileAttributesA(dst.string().c_str(), FILE_ATTRIBUTE_READONLY);
}

// ---------------------------------------------------------------------------
// COMMAND-LINE FORWARDING
// ---------------------------------------------------------------------------

// Extracts all command-line arguments that follow the first token (the exe
// path) from the raw Windows command line string.  These arguments are
// forwarded verbatim to the launched game executable so that steam:// and
// other protocol parameters are passed through transparently.
std::string getForwardedParams() {
    std::string fullCmd = GetCommandLineA();
    std::size_t start = 0;

    // Skip over the first token (quoted or unquoted executable path).
    if (!fullCmd.empty() && fullCmd[0] == '"') {
        std::size_t endQuote = fullCmd.find('"', 1);
        if (endQuote != std::string::npos)
            start = endQuote + 1;
    }
    else {
        std::size_t space = fullCmd.find(' ');
        if (space != std::string::npos)
            start = space;
    }

    // Skip any leading spaces after the exe token.
    while (start < fullCmd.size() && fullCmd[start] == ' ')
        start++;

    return (start < fullCmd.size()) ? fullCmd.substr(start) : "";
}

// ---------------------------------------------------------------------------
// GAME LAUNCHER
// ---------------------------------------------------------------------------

// Launches the configured game executable in its own directory, forwarding
// all arguments passed to this process.
// On success returns an open process HANDLE that the caller can use to monitor
// the game lifetime (e.g. WaitForSingleObject).  The caller is responsible for
// closing this handle with CloseHandle().
// Returns NULL on failure after showing an error dialog.
HANDLE runGame(const fs::path& exePath) {
    if (!fs::exists(exePath)) {
        showError(
            "Could not find the game executable:\n" + exePath.string() +
            "\nCheck the 'launch=' setting in spraysettings.ini."
        );
        return NULL;
    }

    // Assemble the full command line with forwarded parameters.
    std::string params = getForwardedParams();
    std::string exeStr = exePath.string();
    std::string cmd = "\"" + exeStr + "\"";

    if (!params.empty()) {
        cmd += " ";
        cmd += params;
    }

    // CreateProcess requires a mutable char buffer for the command line.
    std::vector<char> buffer(cmd.begin(), cmd.end());
    buffer.push_back('\0');

    STARTUPINFOA        si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    // Launch the game exe from its own directory so relative paths within the
    // game work correctly regardless of where hlgraff.exe was launched from.
    if (CreateProcessA(
        exeStr.c_str(),
        buffer.data(),
        NULL,
        NULL,
        FALSE,
        0,
        NULL,
        exePath.parent_path().string().c_str(),
        &si,
        &pi
    )) {
        // Close the thread handle immediately — we only need the process handle
        // to monitor the game lifetime.
        CloseHandle(pi.hThread);
        // Return the process handle to the caller for lifetime monitoring.
        return pi.hProcess;
    }

    showError("Could not launch the game. Windows error: " + std::to_string(GetLastError()));
    return NULL;
}

// ---------------------------------------------------------------------------
// ENTRY POINT  (no console window)
// ---------------------------------------------------------------------------

// WinMain is the standard entry point for Windows GUI applications; it is
// used here simply to suppress the console window that would appear with a
// regular main().
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    try {
        // ---- Parse the wide-character command line into UTF-8 strings ----
        // CommandLineToArgvW handles all quoting and escaping rules correctly.
        int    argc = 0;
        LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);

        std::vector<std::string> args;
        args.reserve(static_cast<std::size_t>(argc));

        for (int i = 0; i < argc; i++) {
            // Measure the required buffer size for the UTF-8 conversion.
            int len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
            std::vector<char> buffer(static_cast<std::size_t>(len));
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, buffer.data(), len, NULL, NULL);
            args.emplace_back(buffer.data());
        }

        LocalFree(wargv);

        // ---- Locate the game directory and the target executable ----
        fs::path gameDir = resolveGameDir();

        // ---- Load configuration ----
        Config cfg = loadConfig();

        // Build the path to the game executable using the `launch` setting.
        fs::path exePath = gameDir / cfg.launch;

        // ---- Load the persistent SHA-256 hash store ----
        // This is shared between the spray-resolution call below and any
        // auto-deletion pass run inside convertImageSprayToWad.
        auto hashStore = loadHashStore();

        // ---- Determine the target mod directory ----
        // When include-mods=false we always write to "valve" regardless of
        // any -game argument passed to this process.
        std::string mod = cfg.include_mods ? detectMod(args) : "valve";

        // ---- Enumerate sprays and choose one ----
        auto sprays = getSprays();
        std::string chosen = chooseSpray(cfg, sprays, mod);

        // ---- Resolve, convert (if needed), and apply the spray ----
        if (!chosen.empty()) {
            fs::path sprayPath = resolveSprayFile(
                chosen, cfg.size, cfg.convert_autodeletion, hashStore
            );
            applySpray(sprayPath, gameDir, mod);
        }

        // ---- Launch the game ----
        HANDLE hGame = runGame(exePath);
        if (hGame == NULL) {
            return 1;
        }

        // ---- Cycle-on-runtime loop ----
        // When cycle-on-runtime is disabled (the default) we close the game
        // handle immediately and exit — same behaviour as before.
        if (!cfg.cycle_on_runtime) {
            CloseHandle(hGame);
            ExitProcess(0);
        }

        // Cycle-on-runtime is active: stay alive and re-apply a new spray
        // every cycle_delay seconds for as long as the game process is running.
        //
        // Implementation notes:
        //  - We use WaitForSingleObject with a cycle_delay timeout instead of
        //    Sleep so that if the game exits mid-wait we detect it immediately
        //    rather than sleeping through to the next cycle.
        //  - The spray list and hash store are reloaded on every cycle so that
        //    new images dropped into the sprays folder during a session are
        //    picked up without restarting the tool.
        //  - applySpray skips the disk write if the destination is already
        //    identical, so cycles where the spray does not change are free.
        DWORD waitMs = static_cast<DWORD>(cfg.cycle_delay) * 1000UL;

        while (true) {
            // Wait for either the game to exit or the cycle timer to expire.
            DWORD result = WaitForSingleObject(hGame, waitMs);

            // WAIT_OBJECT_0 means the game process has exited — we are done.
            if (result == WAIT_OBJECT_0 || result == WAIT_FAILED)
                break;

            // Timer expired (WAIT_TIMEOUT): apply the next spray while the
            // game is still running.

            // Reload the spray list in case files were added or removed.
            auto cyclesprays = getSprays();
            if (cyclesprays.empty())
                continue;

            // Reload the hash store so any images modified during the session
            // are detected and their cached .wad is rebuilt.
            auto cycleHashStore = loadHashStore();

            // Choose the next spray using the same selection logic as on
            // startup — this advances the sequential/random state correctly.
            std::string cycleChosen = chooseSpray(cfg, cyclesprays, mod);
            if (cycleChosen.empty())
                continue;

            try {
                fs::path cycleSprayPath = resolveSprayFile(
                    cycleChosen, cfg.size, cfg.convert_autodeletion, cycleHashStore
                );
                applySpray(cycleSprayPath, gameDir, mod);
            }
            catch (const std::exception& ex) {
                // Non-fatal: log to a message box would be intrusive mid-game,
                // so we silently skip this cycle and try again on the next one.
                (void)ex;
            }
        }

        CloseHandle(hGame);
        ExitProcess(0);
    }
    catch (const std::exception& ex) {
        showError(ex.what());
        return 1;
    }
    catch (...) {
        showError("An unexpected error occurred.");
        return 1;
    }
}
