// Dev tool: decodes every subsound of one FFX FSB5 bank (read from the player's FFX_Data.vbf) to
// 16-bit mono .raw files in an output folder, plus index.tsv (index, name, rate, samples).
// Used only to choose sound effects for the audio sheet; nothing it writes is shipped.
// usage: sfxdump <FFX_Data.vbf> <archive path of bank> <outdir>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include "miniz.h"

struct E { uint32_t block; uint64_t size, offset; };
static FILE* f;
static std::unordered_map<std::string, E> files;
static std::vector<uint16_t> blocks;

static bool Open(const char* p) {
    f = fopen(p, "rb"); if (!f) return false;
    uint8_t h[16]; fread(h, 1, 16, f);
    uint32_t hl; uint64_t n; memcpy(&hl, h + 4, 4); memcpy(&n, h + 8, 8);
    std::vector<uint8_t> hdr(hl - 16); fread(hdr.data(), 1, hdr.size(), f);
    size_t eo = 16 * n, o = eo + 32 * n; uint32_t nts; memcpy(&nts, &hdr[o], 4);
    const char* names = (const char*)&hdr[o + 4]; o += nts;
    blocks.resize((hdr.size() - o) / 2); memcpy(blocks.data(), &hdr[o], blocks.size() * 2);
    for (uint64_t i = 0; i < n; ++i) { E e; uint64_t no; const uint8_t* p2 = &hdr[eo + 32 * i];
        memcpy(&e.block, p2, 4); memcpy(&e.size, p2 + 8, 8); memcpy(&e.offset, p2 + 16, 8); memcpy(&no, p2 + 24, 8);
        files[names + no] = e; }
    return true;
}
static bool Read(const std::string& name, std::vector<uint8_t>& out) {
    auto it = files.find(name); if (it == files.end()) return false;
    E e = it->second; out.clear(); _fseeki64(f, e.offset, SEEK_SET);
    std::vector<uint8_t> c, r(0x10000);
    for (uint64_t b = 0; b * 0x10000 < e.size; ++b) {
        uint32_t usz = (uint32_t)std::min<uint64_t>(0x10000, e.size - b * 0x10000), csz = blocks[e.block + b] ? blocks[e.block + b] : 0x10000;
        c.resize(csz); fread(c.data(), 1, csz, f);
        if (csz == usz) out.insert(out.end(), c.begin(), c.end());
        else { size_t n = tinfl_decompress_mem_to_mem(r.data(), r.size(), c.data(), csz, TINFL_FLAG_PARSE_ZLIB_HEADER); out.insert(out.end(), r.begin(), r.begin() + n); }
    }
    return true;
}
static const int ST[89] = {7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767};
static const int IX[16] = {-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8};
static const int RT[16] = {4000,8000,11000,11025,16000,22050,24000,32000,44100,48000,96000,48000,48000,48000,48000,48000};

int main(int argc, char** argv) {
    if (argc < 4 || !Open(argv[1])) { printf("usage\n"); return 1; }
    std::vector<uint8_t> d; if (!Read(argv[2], d)) { printf("bank not found\n"); return 1; }
    uint32_t ver, n, shs, nts, ds; memcpy(&ver, &d[4], 4); memcpy(&n, &d[8], 4); memcpy(&shs, &d[12], 4); memcpy(&nts, &d[16], 4); memcpy(&ds, &d[20], 4);
    uint32_t hs = ver == 1 ? 0x3C : 0x40; size_t o = hs;
    struct S { int rate, ch; uint64_t off, ns; }; std::vector<S> ss;
    for (uint32_t i = 0; i < n; ++i) { uint64_t q; memcpy(&q, &d[o], 8); o += 8; bool x = q & 1;
        ss.push_back({RT[(q >> 1) & 15], (int)((q >> 5) & 1) + 1, ((q >> 6) & 0x0FFFFFFF) * 16, q >> 34});
        while (x) { uint32_t c; memcpy(&c, &d[o], 4); o += 4; x = c & 1; o += (c >> 1) & 0xFFFFFF; } }
    std::string out = argv[3]; CreateDirectoryA(out.c_str(), nullptr);
    FILE* idx = fopen((out + "\\index.tsv").c_str(), "w");
    size_t base = hs + shs + nts;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t no; memcpy(&no, &d[hs + shs + 4 * i], 4); const char* nm = (const char*)&d[hs + shs + no];
        size_t s = base + ss[i].off, e = base + (i + 1 < n ? ss[i + 1].off : ds);
        int ch = ss[i].ch; size_t bs = 0x24 * ch; std::vector<int16_t> pcm;
        for (size_t b = s; b + bs <= e && pcm.size() < ss[i].ns; b += bs) {
            int pr[2], ix[2]; int16_t buf[2][64]; int cn[2] = {0, 0};
            for (int c = 0; c < ch; ++c) { int16_t p; memcpy(&p, &d[b + 4 * c], 2); pr[c] = p; ix[c] = std::min<int>(d[b + 4 * c + 2], 88); buf[c][cn[c]++] = p; }
            for (size_t q = b + 4 * ch; q < b + bs;) for (int c = 0; c < ch; ++c) for (int k = 0; k < 4; ++k, ++q) for (int nb : {d[q] & 15, d[q] >> 4}) {
                int st = ST[ix[c]], df = st >> 3; if (nb & 4) df += st; if (nb & 2) df += st >> 1; if (nb & 1) df += st >> 2;
                pr[c] = std::clamp((nb & 8) ? pr[c] - df : pr[c] + df, -32768, 32767); ix[c] = std::clamp(ix[c] + IX[nb], 0, 88);
                if (cn[c] < 64) buf[c][cn[c]++] = (int16_t)pr[c]; }
            for (int k = 0; k < 64; ++k) pcm.push_back(ch > 1 ? (int16_t)((buf[0][k] + buf[1][k]) / 2) : buf[0][k]);
        }
        pcm.resize(std::min<size_t>(pcm.size(), ss[i].ns));
        FILE* r = fopen((out + "\\" + std::to_string(i) + ".raw").c_str(), "wb"); fwrite(pcm.data(), 2, pcm.size(), r); fclose(r);
        fprintf(idx, "%u\t%s\t%d\t%zu\n", i, nm, ss[i].rate, pcm.size());
    }
    fclose(idx); printf("%u sounds\n", n); return 0;
}
