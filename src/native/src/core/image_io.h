// PNG in and out for the texture pipeline.
//
// Reading is `stb_image`, which the plugin already vendors for the FX overlay
// images. ⚠ THIS COPY IS `STB_IMAGE_STATIC` AND MUST STAY THAT WAY: plugin.cpp
// compiles its OWN non-static `STB_IMAGE_IMPLEMENTATION`, and photoncore links
// into the same `.xpl`, so two external copies of `stbi_load_from_memory` would
// be a duplicate-symbol link failure on every platform at once.
//
// Writing is ours, because there is no `stb_image_write.h` in `third_party/` and
// nothing else in the tree encodes a PNG. It is a real DEFLATE — fixed-Huffman
// LZ77 with a hash chain, plus per-scanline filter selection — rather than the
// stored-block shortcut, which on a 4096x4096 RGBA atlas would emit a 64 MB file
// where ToLiss's own is 8.
//
// Both sides are UTF-8-path safe: files move through std::filesystem::path and
// std::fstream, never through stb's stdio helpers, so an aircraft under a folder
// with non-ASCII characters behaves like any other.
#pragma once

#include <filesystem>
#include <string>

#include "core/lit_recolor.h"

namespace photon {
namespace image {

// Decode a PNG (or anything else stb reads) to 8-bit RGBA. `error` always
// carries a loggable reason on false.
bool LoadPng(const std::filesystem::path& path, lit::Image& out,
             std::string& error);

// Encode 8-bit RGBA as a PNG. Writes through a temporary and renames, so an
// interrupted write cannot leave the aircraft holding half a texture.
bool SavePng(const std::filesystem::path& path, const lit::Image& img,
             std::string& error);

// The encoder alone, for callers that already hold the bytes (the tests, and
// anything that wants to size an image before committing to a path).
std::vector<std::uint8_t> EncodePng(const lit::Image& img);

}  // namespace image
}  // namespace photon
