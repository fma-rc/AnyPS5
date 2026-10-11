// Differential check: core/libs/prx/libSceFont/src/WritingLine.cpp against the PS4 11.00 libSceFont.sprx.
// Build and run: see build.sh. The PS4 module is mapped at a fixed base, its memset PLT slot is pointed at
// the host memset, and the writing-line exports are called directly (same SysV ABI).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <random>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

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

static bool SameBytes(const void* a, const void* b, std::size_t n) { return std::memcmp(a, b, n) == 0; }

static bool SameStep(const FontWritingLineStep* a, const FontWritingLineStep* b) {
    if (!a || !b) return a == b;
    return SameBytes(&a->x, &b->x, 4) && SameBytes(&a->y, &b->y, 4) && SameBytes(&a->advanceX, &b->advanceX, 4) &&
           SameBytes(&a->advanceY, &b->advanceY, 4) && SameBytes(&a->spacingProgress, &b->spacingProgress, 4) &&
           a->writingOrderer == b->writingOrderer && SameBytes(&a->Adjusting, &b->Adjusting, 8) &&
           SameBytes(&a->Metrics, &b->Metrics, sizeof(FontWritingMetrics));
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    MapModule(argv[1]);
    const int sequences = argc > 2 ? std::atoi(argv[2]) : 20000;
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
    std::mt19937 rng(12345);
    static int orderers[64];
    long ops = 0;
    long rejected = 0;
    for (int s = 0; s < sequences; ++s) {
        const std::int32_t form = (rng() & 1) ? 0x11 : 0x12;
        const unsigned maxLevel = 1 + rng() % 6;
        void* ours = nullptr;
        void* theirs = nullptr;
        const int rc1 = sceFontCreateWritingLine(&memory, form, nullptr, &ours);
        const int rc2 = create(&library, form, nullptr, &theirs);
        if (rc1 != rc2 || !ours || !theirs) { std::printf("create mismatch %x %x\n", rc1, rc2); return 1; }
        const int length = 1 + rng() % 40;
        for (int i = 0; i < length; ++i, ++ops) {
            const unsigned op = rng() % 16;
            if (op < 8) {
                std::uint64_t attribute = 0xFA | (std::uint64_t{rng() % maxLevel} << 8);
                if (rng() % 4 == 0) attribute |= std::uint64_t{1} << 30;
                if (rng() % 4 == 0) attribute |= std::uint64_t{1} << 31;
                if (rng() % 8 == 0) attribute |= std::uint64_t{rng()} << 32;
                if (rng() % 16 == 0) attribute |= 0x10000u << (rng() % 14);
                if (rng() % 40 == 0) attribute = (attribute & ~0xFFull) | (rng() & 0xFF);
                if (rng() % 40 == 0) attribute = (attribute & ~0xFF00ull) | (std::uint64_t{0x7D + rng() % 6} << 8);
                if (rng() % 40 == 0) attribute = 0;
                FontWritingMetrics m{};
                auto value = [&] { return static_cast<float>(static_cast<int>(rng() % 81) - 40) * 0.25f; };
                m.advanceX = value();
                m.advanceY = rng() % 4 == 0 ? value() : 0.0f;
                m.Extent = {value(), value(), value(), value()};
                if (rng() % 8 == 0) m.Extent.right = m.Extent.left;
                if (rng() % 8 == 0) m.Extent.bottom = m.Extent.top;
                const FontWritingMetrics* pm = rng() % 16 == 0 ? nullptr : &m;
                void* orderer = rng() % 32 == 0 ? nullptr : &orderers[rng() % 64];
                const int b = writes(theirs, attribute == 0 ? 0xFA : attribute, pm, orderer);
                int a = 0;
                bool threw = false;
                try { a = sceFontWritingLineWritesOrder(ours, attribute, pm, orderer); } catch (const std::runtime_error&) { threw = true; }
                if (threw) ++rejected;
                if (threw ? b != static_cast<int>(0x80460002) : (a != 0 || b != 0)) { std::printf("seq %d op %d writes rc %x vs %x attr %llx\n", s, i, a, b, (unsigned long long)attribute); return 1; }
            } else if (op < 13) {
                const FontWritingLineStep* a = sceFontWritingLineRefersRenderStep(ours);
                const FontWritingLineStep* b = refers(theirs);
                if (!SameStep(a, b)) {
                    std::printf("seq %d op %d step mismatch form %x: ours %p theirs %p\n", s, i, form, (const void*)a, (const void*)b);
                    if (a && b) std::printf(" x %g/%g y %g/%g adv %g/%g prog %g/%g ord %p/%p\n", a->x, b->x, a->y, b->y, a->advanceX, b->advanceX, a->spacingProgress, b->spacingProgress, a->writingOrderer, b->writingOrderer);
                    return 1;
                }
            } else if (op == 13) {
                FontWritingMetrics a{1, 1, {1, 1, 1, 1}}, b{1, 1, {1, 1, 1, 1}};
                const int ra = sceFontWritingLineGetRenderMetrics(ours, &a);
                const int rb = render(theirs, &b);
                if (ra != rb || !SameBytes(&a, &b, sizeof a)) { std::printf("seq %d op %d render metrics mismatch: adv %g/%g ext %g %g %g %g / %g %g %g %g\n", s, i, a.advanceX, b.advanceX, a.Extent.top, a.Extent.bottom, a.Extent.left, a.Extent.right, b.Extent.top, b.Extent.bottom, b.Extent.left, b.Extent.right); return 1; }
            } else if (op == 14) {
                float a[4] = {9, 9, 9, 9}, b[4] = {9, 9, 9, 9};
                const bool partial = rng() % 4 == 0;
                const int ra = sceFontWritingLineGetOrderingSpace(ours, partial ? nullptr : &a[0], &a[1], partial ? nullptr : &a[2], &a[3]);
                const int rb = space(theirs, partial ? nullptr : &b[0], &b[1], partial ? nullptr : &b[2], &b[3]);
                if (ra != rb || !SameBytes(a, b, sizeof a)) { std::printf("seq %d op %d space mismatch form %x: %g %g %g %g / %g %g %g %g\n", s, i, form, a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]); return 1; }
            } else {
                const int ra = sceFontWritingLineClear(ours);
                const int rb = clear(theirs);
                if (ra != rb) { std::printf("seq %d op %d clear rc %x vs %x\n", s, i, ra, rb); return 1; }
            }
        }
        sceFontDestroyWritingLine(&ours);
        destroy(&theirs);
    }
    FontWritingMetrics m{1, 1, {1, 1, 1, 1}};
    float f = 9;
    auto throws = [](auto call) {
        try { call(); } catch (const std::runtime_error&) { return true; }
        return false;
    };
    const int invalid = static_cast<int>(0x80460002);
    const bool nulls = throws([&] { sceFontWritingLineWritesOrder(nullptr, 0xFA, &m, &m); }) && writes(nullptr, 0xFA, &m, &m) == invalid &&
                       throws([&] { sceFontWritingLineClear(nullptr); }) && clear(nullptr) == invalid &&
                       throws([&] { sceFontWritingLineGetRenderMetrics(nullptr, &m); }) && render(nullptr, &m) == invalid &&
                       throws([&] { sceFontWritingLineGetOrderingSpace(nullptr, &f, &f, &f, &f); }) && space(nullptr, &f, &f, &f, &f) == invalid &&
                       sceFontWritingLineRefersRenderStep(nullptr) == refers(nullptr);
    std::printf("%d sequences, %ld operations (%ld where the PS4 module returned 0x80460002 and ours threw), null handles (ours throws, PS4 returns 0x80460002) %s: all match\n", sequences, ops, rejected, nulls ? "as expected" : "DIFFER");
    return nulls ? 0 : 1;
}
