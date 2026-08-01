# stb_image (vendored)

`stb_image.h` v2.30, from https://github.com/nothings/stb — public domain
(MIT alternative also offered; see the licence block at the end of the header).
Unmodified. To update, overwrite the file; there is nothing else here.

## Why it is here

The Panel FX compositor lets a layer be an **image** rather than a flat colour,
and X-Plane's SDK has no image loader — `XPLMLoadTexture` is gone from the XPLM4
headers, and the only texture entry points left (`XPLMGenerateTextureNumbers`,
`XPLMBindTexture2d`) hand out a texture *number* and expect you to have already
decoded the pixels.

## It is DEV-ONLY

The implementation is compiled inside `#if PHOTON_DEV` in `plugin.cpp`, so a
release `.xpl` contains none of it. Only PNG/JPEG/BMP/TGA are enabled
(`STBI_ONLY_*`), which keeps the dev build's growth small and drops the decoders
nothing here will ever hand a file.

`STBI_NO_STDIO` is deliberate: files are read through `std::ifstream` from a
`std::filesystem::path` and decoded with `stbi_load_from_memory`, so a path with
non-ASCII characters in it works the same as everywhere else in this plugin.
