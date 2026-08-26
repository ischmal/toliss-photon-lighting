#include "core/image_io.h"

#include <cstring>
#include <fstream>
#include <vector>

#include "core/fsutil.h"

// ⚠ STATIC. See the header — plugin.cpp compiles a non-static copy of the same
// implementation and both land in ToLissPhoton.xpl.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#include "third_party/stb/stb_image.h"

namespace fs = std::filesystem;

namespace photon {
namespace image {
namespace {

// --- checksums ---------------------------------------------------------------

std::uint32_t Crc32(const std::uint8_t* data, std::size_t n,
                    std::uint32_t crc = 0xFFFFFFFFu) {
    static std::uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        ready = true;
    }
    for (std::size_t i = 0; i < n; ++i)
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

std::uint32_t Adler32(const std::uint8_t* data, std::size_t n) {
    std::uint32_t a = 1, b = 0;
    // 5552 is the largest run that cannot overflow the 32-bit accumulators.
    const std::size_t kChunk = 5552;
    while (n > 0) {
        const std::size_t k = n < kChunk ? n : kChunk;
        for (std::size_t i = 0; i < k; ++i) {
            a += data[i];
            b += a;
        }
        a %= 65521;
        b %= 65521;
        data += k;
        n -= k;
    }
    return (b << 16) | a;
}

// --- DEFLATE (fixed Huffman + LZ77) -----------------------------------------

// The spec's length and distance tables. Kept as plain arrays in spec order so
// they can be read straight against RFC 1951 §3.2.5.
const std::uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10,  11,  13,
                                    15, 17, 19, 23, 27, 31, 35, 43,  51,  59,
                                    67, 83, 99, 115, 131, 163, 195, 227, 258};
const std::uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                    2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const std::uint16_t kDistBase[30] = {
    1,    2,    3,    4,    5,    7,     9,     13,    17,   25,
    33,   49,   65,   97,   129,  193,   257,   385,   513,  769,
    1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577};
const std::uint8_t kDistExtra[30] = {0, 0, 0,  0,  1,  1,  2,  2,  3,  3,
                                     4, 4, 5,  5,  6,  6,  7,  7,  8,  8,
                                     9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

class BitWriter {
public:
    explicit BitWriter(std::vector<std::uint8_t>& out) : out_(out) {}

    // DEFLATE packs bits into bytes starting at the least significant bit.
    void Bits(std::uint32_t value, int count) {
        bits_ |= static_cast<std::uint64_t>(value & ((1u << count) - 1)) << n_;
        n_ += count;
        while (n_ >= 8) {
            out_.push_back(static_cast<std::uint8_t>(bits_ & 0xFF));
            bits_ >>= 8;
            n_ -= 8;
        }
    }

    // ⚠ Huffman codes go in MOST significant bit first, which is the opposite
    // order to everything else in the stream, so they are reversed here rather
    // than at each call site. Getting this backwards produces a stream that
    // decodes to plausible garbage instead of failing.
    void Huff(std::uint32_t code, int count) {
        std::uint32_t r = 0;
        for (int i = 0; i < count; ++i) r |= ((code >> i) & 1u) << (count - 1 - i);
        Bits(r, count);
    }

    void Flush() {
        if (n_ > 0) {
            out_.push_back(static_cast<std::uint8_t>(bits_ & 0xFF));
            bits_ = 0;
            n_ = 0;
        }
    }

private:
    std::vector<std::uint8_t>& out_;
    std::uint64_t bits_ = 0;
    int n_ = 0;
};

void WriteFixedSymbol(BitWriter& bw, int sym) {
    // RFC 1951 §3.2.6's fixed literal/length code.
    if (sym < 144) {
        bw.Huff(0x30 + sym, 8);
    } else if (sym < 256) {
        bw.Huff(0x190 + (sym - 144), 9);
    } else if (sym < 280) {
        bw.Huff(sym - 256, 7);
    } else {
        bw.Huff(0xC0 + (sym - 280), 8);
    }
}

constexpr int kWindow = 32768;
constexpr int kMinMatch = 3;
constexpr int kMaxMatch = 258;
constexpr int kHashBits = 15;
constexpr int kMaxChain = 128;  // quality/speed knob; 128 is well past the knee

std::vector<std::uint8_t> Deflate(const std::vector<std::uint8_t>& src) {
    std::vector<std::uint8_t> out;
    out.reserve(src.size() / 3 + 64);
    BitWriter bw(out);
    bw.Bits(1, 1);  // BFINAL — one block for the whole stream
    bw.Bits(1, 2);  // BTYPE = 01, fixed Huffman

    const std::size_t n = src.size();
    std::vector<int> head(1 << kHashBits, -1);
    std::vector<int> prev(n > 0 ? n : 1, -1);

    auto hash3 = [&](std::size_t i) -> std::uint32_t {
        return ((static_cast<std::uint32_t>(src[i]) << 10) ^
                (static_cast<std::uint32_t>(src[i + 1]) << 5) ^
                static_cast<std::uint32_t>(src[i + 2])) &
               ((1u << kHashBits) - 1);
    };

    std::size_t i = 0;
    while (i < n) {
        int bestLen = 0;
        std::size_t bestDist = 0;
        if (i + kMinMatch <= n) {
            const std::uint32_t h = hash3(i);
            int cand = head[h];
            int chain = kMaxChain;
            const std::size_t maxLen = (n - i) < kMaxMatch ? (n - i) : kMaxMatch;
            while (cand >= 0 && chain-- > 0) {
                const std::size_t dist = i - static_cast<std::size_t>(cand);
                if (dist > kWindow) break;
                // Compare the byte one past the current best first: it is the
                // cheapest possible rejection of a candidate that cannot win.
                if (bestLen == 0 ||
                    src[static_cast<std::size_t>(cand) + bestLen] ==
                        src[i + bestLen]) {
                    std::size_t l = 0;
                    while (l < maxLen &&
                           src[static_cast<std::size_t>(cand) + l] == src[i + l])
                        ++l;
                    if (static_cast<int>(l) > bestLen) {
                        bestLen = static_cast<int>(l);
                        bestDist = dist;
                        if (bestLen >= static_cast<int>(maxLen)) break;
                    }
                }
                cand = prev[static_cast<std::size_t>(cand)];
            }
        }

        if (bestLen >= kMinMatch) {
            int lc = 28;
            while (lc > 0 && kLenBase[lc] > bestLen) --lc;
            WriteFixedSymbol(bw, 257 + lc);
            if (kLenExtra[lc])
                bw.Bits(static_cast<std::uint32_t>(bestLen - kLenBase[lc]),
                        kLenExtra[lc]);
            int dc = 29;
            while (dc > 0 && kDistBase[dc] > bestDist) --dc;
            bw.Huff(static_cast<std::uint32_t>(dc), 5);
            if (kDistExtra[dc])
                bw.Bits(
                    static_cast<std::uint32_t>(bestDist - kDistBase[dc]),
                    kDistExtra[dc]);
            // Every position inside the match still has to enter the chain, or
            // later matches lose the ability to point into it.
            for (int k = 0; k < bestLen; ++k) {
                if (i + kMinMatch <= n) {
                    const std::uint32_t h = hash3(i);
                    prev[i] = head[h];
                    head[h] = static_cast<int>(i);
                }
                ++i;
            }
        } else {
            WriteFixedSymbol(bw, src[i]);
            if (i + kMinMatch <= n) {
                const std::uint32_t h = hash3(i);
                prev[i] = head[h];
                head[h] = static_cast<int>(i);
            }
            ++i;
        }
    }

    WriteFixedSymbol(bw, 256);  // end of block
    bw.Flush();
    return out;
}

// --- PNG ---------------------------------------------------------------------

void PushBE32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    v.push_back(static_cast<std::uint8_t>(x >> 24));
    v.push_back(static_cast<std::uint8_t>(x >> 16));
    v.push_back(static_cast<std::uint8_t>(x >> 8));
    v.push_back(static_cast<std::uint8_t>(x));
}

void Chunk(std::vector<std::uint8_t>& out, const char type[5],
           const std::uint8_t* data, std::size_t n) {
    PushBE32(out, static_cast<std::uint32_t>(n));
    const std::size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    if (n) out.insert(out.end(), data, data + n);
    const std::uint32_t crc =
        Crc32(out.data() + start, out.size() - start) ^ 0xFFFFFFFFu;
    PushBE32(out, crc);
}

int PaethPredictor(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = p > a ? p - a : a - p;
    const int pb = p > b ? p - b : b - p;
    const int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

}  // namespace

std::vector<std::uint8_t> EncodePng(const lit::Image& img) {
    std::vector<std::uint8_t> out;
    if (!img.Valid()) return out;

    const int w = img.width, h = img.height;
    const int bpp = 4;
    const std::size_t stride = static_cast<std::size_t>(w) * bpp;

    // Filtered scanlines: one filter-type byte then `stride` bytes per row.
    std::vector<std::uint8_t> raw;
    raw.resize((stride + 1) * static_cast<std::size_t>(h));

    std::vector<std::uint8_t> line(stride), best(stride);
    const std::vector<std::uint8_t> zero(stride, 0);

    for (int y = 0; y < h; ++y) {
        const std::uint8_t* cur = img.pixels.data() + static_cast<std::size_t>(y) * stride;
        const std::uint8_t* up =
            y > 0 ? img.pixels.data() + static_cast<std::size_t>(y - 1) * stride
                  : zero.data();
        int bestFilter = 0;
        long bestScore = -1;
        for (int f = 0; f < 5; ++f) {
            long score = 0;
            for (std::size_t x = 0; x < stride; ++x) {
                const int a = x >= static_cast<std::size_t>(bpp)
                                  ? cur[x - bpp]
                                  : 0;
                const int b = up[x];
                const int c = (x >= static_cast<std::size_t>(bpp) && y > 0)
                                  ? up[x - bpp]
                                  : 0;
                int v = 0;
                switch (f) {
                    case 0: v = cur[x]; break;
                    case 1: v = cur[x] - a; break;
                    case 2: v = cur[x] - b; break;
                    case 3: v = cur[x] - ((a + b) >> 1); break;
                    default: v = cur[x] - PaethPredictor(a, b, c); break;
                }
                line[x] = static_cast<std::uint8_t>(v & 0xFF);
                // The standard heuristic: prefer the filter whose output is
                // closest to zero when read as signed bytes.
                const int s = static_cast<std::int8_t>(line[x]);
                score += s < 0 ? -s : s;
            }
            if (bestScore < 0 || score < bestScore) {
                bestScore = score;
                bestFilter = f;
                best = line;
            }
        }
        std::uint8_t* dst = raw.data() + static_cast<std::size_t>(y) * (stride + 1);
        dst[0] = static_cast<std::uint8_t>(bestFilter);
        std::memcpy(dst + 1, best.data(), stride);
    }

    // zlib wrapper: 0x78 0x01 is deflate/32K with a header checksum that
    // satisfies (CMF<<8 | FLG) % 31 == 0.
    std::vector<std::uint8_t> z;
    z.push_back(0x78);
    z.push_back(0x01);
    const std::vector<std::uint8_t> comp = Deflate(raw);
    z.insert(z.end(), comp.begin(), comp.end());
    PushBE32(z, Adler32(raw.data(), raw.size()));

    static const std::uint8_t kSig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    out.insert(out.end(), kSig, kSig + 8);

    std::uint8_t ihdr[13];
    ihdr[0] = static_cast<std::uint8_t>(w >> 24);
    ihdr[1] = static_cast<std::uint8_t>(w >> 16);
    ihdr[2] = static_cast<std::uint8_t>(w >> 8);
    ihdr[3] = static_cast<std::uint8_t>(w);
    ihdr[4] = static_cast<std::uint8_t>(h >> 24);
    ihdr[5] = static_cast<std::uint8_t>(h >> 16);
    ihdr[6] = static_cast<std::uint8_t>(h >> 8);
    ihdr[7] = static_cast<std::uint8_t>(h);
    ihdr[8] = 8;   // bit depth
    ihdr[9] = 6;   // color type: RGBA
    ihdr[10] = 0;  // compression
    ihdr[11] = 0;  // filter method
    ihdr[12] = 0;  // no interlace
    Chunk(out, "IHDR", ihdr, sizeof(ihdr));
    Chunk(out, "IDAT", z.data(), z.size());
    Chunk(out, "IEND", nullptr, 0);
    return out;
}

bool LoadPng(const fs::path& path, lit::Image& out, std::string& error) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) {
        error = "cannot stat " + fsutil::PathToUtf8(path) + ": " + ec.message();
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + fsutil::PathToUtf8(path);
        return false;
    }
    std::vector<char> buf(static_cast<std::size_t>(size));
    if (size && !in.read(buf.data(), static_cast<std::streamsize>(size))) {
        error = "cannot read " + fsutil::PathToUtf8(path);
        return false;
    }
    int w = 0, h = 0, comp = 0;
    stbi_uc* px = stbi_load_from_memory(
        reinterpret_cast<const stbi_uc*>(buf.data()),
        static_cast<int>(buf.size()), &w, &h, &comp, 4);
    if (!px) {
        const char* why = stbi_failure_reason();
        error = "cannot decode " + fsutil::PathToUtf8(path) + ": " + (why ? why : "unknown");
        return false;
    }
    out.width = w;
    out.height = h;
    out.pixels.assign(px, px + static_cast<std::size_t>(w) * h * 4);
    stbi_image_free(px);
    return true;
}

bool SavePng(const fs::path& path, const lit::Image& img, std::string& error) {
    if (!img.Valid()) {
        error = "cannot write an invalid image";
        return false;
    }
    const std::vector<std::uint8_t> bytes = EncodePng(img);
    if (bytes.empty()) {
        error = "encoder produced nothing";
        return false;
    }
    // Write beside the target and rename. A texture is the one file the aircraft
    // cannot do without, and a half-written one looks exactly like a corrupt
    // install.
    fs::path tmp = path;
    tmp += ".photon-tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) {
            error = "cannot open " + fsutil::PathToUtf8(tmp) + " for writing";
            return false;
        }
        o.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
        if (!o) {
            error = "write failed for " + fsutil::PathToUtf8(tmp);
            return false;
        }
    }
    std::error_code ec;
    fs::remove(path, ec);
    fs::rename(tmp, path, ec);
    if (ec) {
        error = "cannot move " + fsutil::PathToUtf8(tmp) + " into place: " + ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

}  // namespace image
}  // namespace photon
