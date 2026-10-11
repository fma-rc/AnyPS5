#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "prx/libSceFont/include/FontTypes.hpp"

extern "C" {
int APS5_VABI sceFontCreateWritingLine(const FontMemory*, std::int32_t, const void*, void**);
int APS5_VABI sceFontDestroyWritingLine(void**);
int APS5_VABI sceFontWritingLineClear(void*);
int APS5_VABI sceFontWritingLineWritesOrder(void*, std::uint64_t, const FontWritingMetrics*, void*);
const FontWritingLineStep* APS5_VABI sceFontWritingLineRefersRenderStep(void*);
int APS5_VABI sceFontWritingLineGetRenderMetrics(void*, FontWritingMetrics*);
int APS5_VABI sceFontWritingLineGetOrderingSpace(void*, float*, float*, float*, float*);
}

static std::uint8_t* base;

static void MapModule(const char* path) {
    const int fd = open(path, O_RDONLY);
    struct stat st {};
    if (fd < 0 || fstat(fd, &st) != 0) { std::perror(path); std::exit(2); }
    auto* file = static_cast<std::uint8_t*>(mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0));
    base = static_cast<std::uint8_t*>(mmap(nullptr, 0x100000, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    std::uint64_t phoff;
    std::uint16_t phentsize, phnum;
    std::memcpy(&phoff, file + 0x20, 8);
    std::memcpy(&phentsize, file + 0x36, 2);
    std::memcpy(&phnum, file + 0x38, 2);
    for (int i = 0; i < phnum; ++i) {
        const std::uint8_t* ph = file + phoff + i * phentsize;
        std::uint32_t type;
        std::uint64_t off, va, filesz;
        std::memcpy(&type, ph, 4);
        std::memcpy(&off, ph + 8, 8);
        std::memcpy(&va, ph + 16, 8);
        std::memcpy(&filesz, ph + 32, 8);
        if (type == 1 || type == 0x61000010) std::memcpy(base + va, file + off, filesz);
    }
    const std::uint8_t* plt = base + 0x158;
    if (plt[0] != 0xFF || plt[1] != 0x25) { std::fprintf(stderr, "unexpected PLT stub at 0x158\n"); std::exit(2); }
    std::int32_t disp;
    std::memcpy(&disp, plt + 2, 4);
    void* target = reinterpret_cast<void*>(&std::memset);
    std::memcpy(base + 0x158 + 6 + disp, &target, 8);
}

static void* OracleMalloc(void*, std::uint32_t size) { return std::malloc(size); }
static void OracleFree(void*, void* p) { std::free(p); }

struct OracleLibrary {
    std::uint16_t magic = 0x0F00;
    std::uint8_t pad[14] = {};
    void* heap = nullptr;
    void** functions = nullptr;
};

template <typename T>
static T Fn(std::uint64_t vaddr) { return reinterpret_cast<T>(base + vaddr); }

static long nanBitDiffs = 0;

static bool SameF(float a, float b) {
    if (std::memcmp(&a, &b, 4) == 0) return true;
    if (std::isnan(a) && std::isnan(b)) { ++nanBitDiffs; return true; }
    return false;
}

static bool SameM(const FontWritingMetrics& a, const FontWritingMetrics& b) {
    return SameF(a.advanceX, b.advanceX) && SameF(a.advanceY, b.advanceY) && SameF(a.Extent.top, b.Extent.top) &&
           SameF(a.Extent.bottom, b.Extent.bottom) && SameF(a.Extent.left, b.Extent.left) && SameF(a.Extent.right, b.Extent.right);
}

static bool SameStep(const FontWritingLineStep* a, const FontWritingLineStep* b) {
    if (!a || !b) return a == b;
    return SameF(a->x, b->x) && SameF(a->y, b->y) && SameF(a->advanceX, b->advanceX) && SameF(a->advanceY, b->advanceY) &&
           SameF(a->spacingProgress, b->spacingProgress) && a->writingOrderer == b->writingOrderer &&
           SameF(a->Adjusting.x, b->Adjusting.x) && SameF(a->Adjusting.y, b->Adjusting.y) && SameM(a->Metrics, b->Metrics);
}

static std::string StepText(const FontWritingLineStep* s) {
    if (!s) return "null";
    char buffer[400];
    std::snprintf(buffer, sizeof buffer, "x %a y %a adv %a %a prog %a ord %p adj %a %a m %a %a [%a %a %a %a]", s->x, s->y, s->advanceX, s->advanceY,
                  s->spacingProgress, s->writingOrderer, s->Adjusting.x, s->Adjusting.y, s->Metrics.advanceX, s->Metrics.advanceY,
                  s->Metrics.Extent.top, s->Metrics.Extent.bottom, s->Metrics.Extent.left, s->Metrics.Extent.right);
    return buffer;
}

int main(int argc, char** argv) {
    if (argc < 8) {
        std::fprintf(stderr, "usage: module sequences maxLevel floatMode(0 grid,1 inexact,2 specials) writeStep(0/1) maxLength seed [levelWalk]\n");
        return 2;
    }
    MapModule(argv[1]);
    const int sequences = std::atoi(argv[2]);
    const unsigned levelLimit = std::atoi(argv[3]);
    const int floatMode = std::atoi(argv[4]);
    const bool writeStep = std::atoi(argv[5]) != 0;
    const int maxLength = std::atoi(argv[6]);
    const unsigned seed = std::atoi(argv[7]);
    const bool levelWalk = argc > 8 && std::atoi(argv[8]) != 0;
    const auto create = Fn<int (*)(void*, std::int32_t, const void*, void**)>(0x19830);
    const auto writes = Fn<int (*)(void*, std::uint64_t, const FontWritingMetrics*, void*)>(0x198f0);
    const auto space = Fn<int (*)(void*, float*, float*, float*, float*)>(0x19e40);
    const auto refers = Fn<const FontWritingLineStep* (*)(void*)>(0x19f40);
    const auto clear = Fn<int (*)(void*)>(0x1a1a0);
    const auto render = Fn<int (*)(void*, FontWritingMetrics*)>(0x1a2a0);
    const auto destroy = Fn<int (*)(void**)>(0x1a2f0);

    void* functions[2] = {reinterpret_cast<void*>(&OracleMalloc), reinterpret_cast<void*>(&OracleFree)};
    OracleLibrary library;
    library.functions = functions;
    FontMemory memory{};
    std::mt19937 rng(seed);
    static int orderers[64];
    long ops = 0;
    long staleDiffs = 0;
    long maxLevelSeen = 0;
    long bigDrops = 0;
    long rejected = 0;
    long zeroAttributes = 0;
    std::string firstStale;

    auto grid = [&] { return static_cast<float>(static_cast<int>(rng() % 81) - 40) * 0.25f; };
    auto value = [&]() -> float {
        if (floatMode == 0) return grid();
        if (floatMode == 1) {
            const float mantissa = static_cast<float>(static_cast<std::int32_t>(rng())) / 2147483648.0f;
            return std::ldexp(mantissa, static_cast<int>(rng() % 20) - 10);
        }
        switch (rng() % 14) {
            case 0: return std::numeric_limits<float>::quiet_NaN();
            case 1: return std::numeric_limits<float>::infinity();
            case 2: return -std::numeric_limits<float>::infinity();
            case 3: return -0.0f;
            case 4: return 0.0f;
            case 5: return std::numeric_limits<float>::denorm_min();
            case 6: return std::numeric_limits<float>::max();
            default: return grid();
        }
    };

    for (int s = 0; s < sequences; ++s) {
        const std::int32_t form = (rng() & 1) ? 0x11 : 0x12;
        const unsigned maxLevel = 1 + rng() % levelLimit;
        void* ours = nullptr;
        void* theirs = nullptr;
        const int rc1 = sceFontCreateWritingLine(&memory, form, nullptr, &ours);
        const int rc2 = create(&library, form, nullptr, &theirs);
        if (rc1 != rc2 || !ours || !theirs) { std::printf("create mismatch %x %x\n", rc1, rc2); return 1; }
        std::vector<std::string> trace;
        auto fail = [&](const std::string& what) {
            std::printf("MISMATCH seq %d form %x: %s\n", s, form, what.c_str());
            for (const std::string& line : trace) std::printf("  %s\n", line.c_str());
            return 1;
        };
        const FontWritingLineStep* keptOurs = nullptr;
        const FontWritingLineStep* keptTheirs = nullptr;
        unsigned level = 0;
        unsigned lastWritten = 0;
        const int length = 1 + rng() % maxLength;
        for (int i = 0; i < length; ++i, ++ops) {
            const unsigned op = rng() % 16;
            char text[600];
            try {
                if (op < 8) {
                    if (levelWalk) {
                        const unsigned kind = rng() % 6;
                        if (kind == 0) level = rng() % maxLevel;
                        else if (kind == 1) level = level + 1 + rng() % 3;
                        else if (kind == 2) level = level > 3 ? level - 1 - rng() % 3 : 0;
                        else if (kind == 3) level = level / 2;
                        if (level >= maxLevel) level = maxLevel - 1;
                    } else {
                        level = rng() % maxLevel;
                    }
                    std::uint64_t attribute = 0xFA | (std::uint64_t{level} << 8);
                    if (rng() % 4 == 0) attribute |= std::uint64_t{1} << 30;
                    if (rng() % 4 == 0) attribute |= std::uint64_t{1} << 31;
                    if (rng() % 8 == 0) attribute |= std::uint64_t{rng()} << 32;
                    if (rng() % 16 == 0) attribute |= 0x10000u << (rng() % 14);
                    if (rng() % 40 == 0) attribute = (attribute & ~0xFFull) | (rng() & 0xFF);
                    if (rng() % 40 == 0) attribute = (attribute & ~0xFF00ull) | (std::uint64_t{0x7D + rng() % 6} << 8);
                    if (rng() % 40 == 0) attribute = 0;
                    FontWritingMetrics m{};
                    m.advanceX = value();
                    m.advanceY = rng() % 4 == 0 ? value() : 0.0f;
                    m.Extent = {value(), value(), value(), value()};
                    if (rng() % 8 == 0) m.Extent.right = m.Extent.left;
                    if (rng() % 8 == 0) m.Extent.bottom = m.Extent.top;
                    const FontWritingMetrics* pm = rng() % 16 == 0 ? nullptr : &m;
                    void* orderer = rng() % 32 == 0 ? nullptr : &orderers[rng() % 64];
                    std::snprintf(text, sizeof text, "write attr %llx metrics %s{%a,%a,{%a,%a,%a,%a}} orderer %d", (unsigned long long)attribute,
                                  pm ? "" : "NULL ", m.advanceX, m.advanceY, m.Extent.top, m.Extent.bottom, m.Extent.left, m.Extent.right,
                                  orderer ? static_cast<int>(static_cast<int*>(orderer) - orderers) : -1);
                    trace.push_back(text);
                    const int b = writes(theirs, attribute == 0 ? 0xFA : attribute, pm, orderer);
                    int a = 0;
                    bool threw = false;
                    try { a = sceFontWritingLineWritesOrder(ours, attribute, pm, orderer); } catch (const std::runtime_error&) { threw = true; }
                    if (threw ? b != static_cast<int>(0x80460002) : (a != 0 || b != 0)) return fail("writes rc");
                    if (threw) ++rejected;
                    if (attribute == 0) ++zeroAttributes;
                    if (!threw) {
                        const unsigned written = (attribute >> 8) & 0xFF;
                        if (written > maxLevelSeen) maxLevelSeen = written;
                        if (written + 2 < lastWritten) ++bigDrops;
                        lastWritten = written;
                    }
                } else if (op < 13) {
                    trace.push_back("step");
                    const FontWritingLineStep* a = sceFontWritingLineRefersRenderStep(ours);
                    const FontWritingLineStep* b = refers(theirs);
                    if (!SameStep(a, b)) return fail("step: ours " + StepText(a) + " / theirs " + StepText(b));
                    if (a) {
                        keptOurs = a;
                        keptTheirs = b;
                        if (writeStep && rng() % 2 == 0) {
                            auto* wa = const_cast<FontWritingLineStep*>(a);
                            auto* wb = const_cast<FontWritingLineStep*>(b);
                            const float ax = value(), ay = value();
                            wa->Adjusting = {ax, ay};
                            wb->Adjusting = {ax, ay};
                            std::snprintf(text, sizeof text, "  guest writes Adjusting %a %a", ax, ay);
                            trace.push_back(text);
                            if (!std::getenv("NOADV") && rng() % 4 == 0) {
                                const float vx = value(), vy = value();
                                wa->advanceX = vx; wa->advanceY = vy;
                                wb->advanceX = vx; wb->advanceY = vy;
                                std::snprintf(text, sizeof text, "  guest writes step advance %a %a", vx, vy);
                                trace.push_back(text);
                            }
                        }
                    } else if (keptOurs && !SameStep(keptOurs, keptTheirs)) {
                        if (staleDiffs++ == 0) firstStale = "seq " + std::to_string(s) + " ours " + StepText(keptOurs) + " / theirs " + StepText(keptTheirs);
                    }
                } else if (op == 13) {
                    FontWritingMetrics a{1, 1, {1, 1, 1, 1}}, b{1, 1, {1, 1, 1, 1}};
                    const bool nullOut = rng() % 8 == 0;
                    trace.push_back(nullOut ? "render metrics (null out)" : "render metrics");
                    int ra = 0;
                    bool threw = false;
                    try { ra = sceFontWritingLineGetRenderMetrics(ours, nullOut ? nullptr : &a); } catch (const std::runtime_error&) { threw = true; }
                    const int rb = render(theirs, nullOut ? nullptr : &b);
                    if (threw) ++rejected;
                    if (threw ? rb != static_cast<int>(0x80460002) : (ra != rb || !SameM(a, b))) {
                        std::snprintf(text, sizeof text, "render metrics rc %x/%x: %a %a [%a %a %a %a] / %a %a [%a %a %a %a]", ra, rb, a.advanceX, a.advanceY,
                                      a.Extent.top, a.Extent.bottom, a.Extent.left, a.Extent.right, b.advanceX, b.advanceY, b.Extent.top, b.Extent.bottom,
                                      b.Extent.left, b.Extent.right);
                        return fail(text);
                    }
                } else if (op == 14) {
                    float a[4] = {9, 9, 9, 9}, b[4] = {9, 9, 9, 9};
                    unsigned mask = rng() % 16;
                    if ((mask & 2) && !(mask & 8)) mask |= 8;
                    std::snprintf(text, sizeof text, "space mask %x", mask);
                    trace.push_back(text);
                    const int ra = sceFontWritingLineGetOrderingSpace(ours, mask & 1 ? &a[0] : nullptr, mask & 2 ? &a[1] : nullptr, mask & 4 ? &a[2] : nullptr, mask & 8 ? &a[3] : nullptr);
                    const int rb = space(theirs, mask & 1 ? &b[0] : nullptr, mask & 2 ? &b[1] : nullptr, mask & 4 ? &b[2] : nullptr, mask & 8 ? &b[3] : nullptr);
                    if (ra != rb || !SameF(a[0], b[0]) || !SameF(a[1], b[1]) || !SameF(a[2], b[2]) || !SameF(a[3], b[3])) {
                        std::snprintf(text, sizeof text, "space rc %x/%x: %a %a %a %a / %a %a %a %a", ra, rb, a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
                        return fail(text);
                    }
                } else {
                    trace.push_back("clear");
                    const int ra = sceFontWritingLineClear(ours);
                    const int rb = clear(theirs);
                    if (ra != rb) return fail("clear rc");
                }
            } catch (const std::exception& e) {
                return fail(std::string("ours threw: ") + e.what());
            }
        }
        sceFontDestroyWritingLine(&ours);
        destroy(&theirs);
    }
    std::printf("%d sequences, %ld operations: all match (NaN sign/payload-only differences %ld, stale-step differences after a null return %ld, highest level written %ld, drops of 3+ levels %ld)\n",
                sequences, ops, nanBitDiffs, staleDiffs, maxLevelSeen, bigDrops);
    std::printf("calls where the PS4 module returned 0x80460002 and ours threw: %ld; writes with attribute 0 (ours) against 0xFA (PS4): %ld\n", rejected, zeroAttributes);
    if (staleDiffs) std::printf("first stale difference: %s\n", firstStale.c_str());
    return 0;
}
