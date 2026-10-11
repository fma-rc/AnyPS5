// Runs the repository test core/libs/tests/GuestFontWritingLine.cpp against the PS4 11.00 libSceFont.sprx.
// This file defines the seven sceFontWritingLine exports the test calls and forwards each one to the function
// of the same NID in the mapped PS4 module, printing the call and what the module returned. Lines starting
// with "harness:" are decisions of this file, not module output; they are the places where the repository
// implementation deliberately does not follow the PS4 module (see README.md).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_set>

#include "prx/libSceFont/include/FontTypes.hpp"
#include "trace.hpp"

namespace {

std::uint8_t* base;

void MapModule(const char* path) {
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

void* OracleMalloc(void*, std::uint32_t size) { return std::malloc(size); }
void OracleFree(void*, void* p) { std::free(p); }

struct OracleLibrary {
    std::uint16_t magic = 0x0F00;
    std::uint8_t pad[14] = {};
    void* heap = nullptr;
    void** functions = nullptr;
};

template <typename T>
T Fn(std::uint64_t vaddr) {
    if (!base) {
        const char* path = std::getenv("PS4_LIBSCEFONT");
        if (!path) { std::fprintf(stderr, "set PS4_LIBSCEFONT to the PS4 11.00 libSceFont.sprx\n"); std::exit(2); }
        MapModule(path);
    }
    return reinterpret_cast<T>(base + vaddr);
}

void* functions[2] = {reinterpret_cast<void*>(&OracleMalloc), reinterpret_cast<void*>(&OracleFree)};
OracleLibrary library{0x0F00, {}, nullptr, functions};
std::unordered_set<void*> live;
std::map<void*, int> lineNames;
std::map<void*, int> ordererNames;

std::string LineName(void* line) {
    if (!line) return "null";
    auto it = lineNames.find(line);
    if (it == lineNames.end()) return "not-a-line";
    return "L" + std::to_string(it->second);
}

std::string OrdererName(void* orderer) {
    if (!orderer) return "null";
    auto it = ordererNames.find(orderer);
    if (it == ordererNames.end()) it = ordererNames.emplace(orderer, static_cast<int>(ordererNames.size()) + 1).first;
    return "O" + std::to_string(it->second);
}

std::string MetricsText(const FontWritingMetrics& m) {
    char text[200];
    std::snprintf(text, sizeof text, "{advance %.9g %.9g, top %.9g, bottom %.9g, left %.9g, right %.9g}", m.advanceX, m.advanceY, m.Extent.top,
                  m.Extent.bottom, m.Extent.left, m.Extent.right);
    return text;
}

[[noreturn]] void Throw(const char* why) {
    std::printf("  harness: throws (%s)\n", why);
    throw std::runtime_error(why);
}

void RequireLive(void* line) {
    if (line && !live.contains(line)) Throw("handle is not a live line; module not called");
}

void ThrowOnError(int rc) {
    if (rc != 0) Throw("module returned an error; the repository implementation throws where the PS4 module returns one");
}

}

void TraceLine(int line) {
    std::printf("test line %d\n", line);
}

extern "C" {

int APS5_VABI sceFontCreateWritingLine(const FontMemory*, std::int32_t writingForm, const void* detail, void** pWritingLine) {
    std::int32_t form = writingForm;
    const void* passedDetail = detail;
    std::uint16_t detailId = 0;
    if (detail) std::memcpy(&detailId, detail, 2);
    if (writingForm == 0x10) {
        std::printf("  harness: form 0x10 is passed to the module as 0x12\n");
        form = 0x12;
    }
    if (detail && detailId == 0x0FD5) {
        std::printf("  harness: detail with id 0x0FD5 is passed to the module as null\n");
        passedDetail = nullptr;
    }
    void* created = nullptr;
    const int rc = Fn<int (*)(void*, std::int32_t, const void*, void**)>(0x19830)(&library, form, passedDetail, pWritingLine ? &created : nullptr);
    if (created) {
        live.insert(created);
        lineNames[created] = static_cast<int>(lineNames.size()) + 1;
    }
    std::printf("  Create(form 0x%x, detail %s, out %s) -> 0x%x %s\n", form, passedDetail ? "non-null" : "null", pWritingLine ? "non-null" : "null",
                static_cast<unsigned>(rc), created ? LineName(created).c_str() : "");
    ThrowOnError(rc);
    *pWritingLine = created;
    return rc;
}

int APS5_VABI sceFontDestroyWritingLine(void** pWritingLine) {
    if (!pWritingLine || !*pWritingLine) {
        std::printf("  harness: Destroy of a null pointer or null line returns 0; module not called\n");
        return 0;
    }
    RequireLive(*pWritingLine);
    void* line = *pWritingLine;
    const std::string name = LineName(line);
    const int rc = Fn<int (*)(void**)>(0x1a2f0)(pWritingLine);
    std::printf("  Destroy(%s) -> 0x%x, pointer afterwards %s\n", name.c_str(), static_cast<unsigned>(rc), *pWritingLine ? "unchanged" : "null");
    live.erase(line);
    ThrowOnError(rc);
    return rc;
}

int APS5_VABI sceFontWritingLineClear(void* writingLine) {
    RequireLive(writingLine);
    const int rc = Fn<int (*)(void*)>(0x1a1a0)(writingLine);
    std::printf("  Clear(%s) -> 0x%x\n", LineName(writingLine).c_str(), static_cast<unsigned>(rc));
    ThrowOnError(rc);
    return rc;
}

int APS5_VABI sceFontWritingLineWritesOrder(void* writingLine, std::uint64_t attribute, const FontWritingMetrics* metrics, void* orderer) {
    RequireLive(writingLine);
    if (attribute == 0) {
        std::printf("  harness: attribute 0 is passed to the module as 0xfa\n");
        attribute = 0xFA;
    }
    const int rc = Fn<int (*)(void*, std::uint64_t, const FontWritingMetrics*, void*)>(0x198f0)(writingLine, attribute, metrics, orderer);
    std::printf("  WritesOrder(%s, attribute 0x%llx, metrics %s, orderer %s) -> 0x%x\n", LineName(writingLine).c_str(),
                static_cast<unsigned long long>(attribute), metrics ? MetricsText(*metrics).c_str() : "null", OrdererName(orderer).c_str(),
                static_cast<unsigned>(rc));
    ThrowOnError(rc);
    return rc;
}

const FontWritingLineStep* APS5_VABI sceFontWritingLineRefersRenderStep(void* writingLine) {
    RequireLive(writingLine);
    if (writingLine) {
        const auto* current = reinterpret_cast<const FontWritingLineStep*>(static_cast<std::uint8_t*>(writingLine) + 0x28);
        if (current->Adjusting.x != 0.0f || current->Adjusting.y != 0.0f)
            std::printf("  (the caller wrote Adjusting %.9g %.9g into the step it was given)\n", current->Adjusting.x, current->Adjusting.y);
    }
    const FontWritingLineStep* step = Fn<const FontWritingLineStep* (*)(void*)>(0x19f40)(writingLine);
    if (!step) {
        std::printf("  RefersRenderStep(%s) -> null\n", LineName(writingLine).c_str());
        return nullptr;
    }
    std::printf("  RefersRenderStep(%s) -> x %.9g, y %.9g, advanceX %.9g, advanceY %.9g, spacingProgress %.9g, orderer %s, Adjusting %.9g %.9g, Metrics %s\n",
                LineName(writingLine).c_str(), step->x, step->y, step->advanceX, step->advanceY, step->spacingProgress,
                OrdererName(step->writingOrderer).c_str(), step->Adjusting.x, step->Adjusting.y, MetricsText(step->Metrics).c_str());
    return step;
}

int APS5_VABI sceFontWritingLineGetRenderMetrics(void* writingLine, FontWritingMetrics* metrics) {
    RequireLive(writingLine);
    const int rc = Fn<int (*)(void*, FontWritingMetrics*)>(0x1a2a0)(writingLine, metrics);
    std::printf("  GetRenderMetrics(%s, out %s) -> 0x%x %s\n", LineName(writingLine).c_str(), metrics ? "non-null" : "null", static_cast<unsigned>(rc),
                metrics ? MetricsText(*metrics).c_str() : "");
    ThrowOnError(rc);
    return rc;
}

int APS5_VABI sceFontWritingLineGetOrderingSpace(void* writingLine, float* p1, float* p2, float* p3, float* p4) {
    RequireLive(writingLine);
    if (writingLine && p2 && !p4) Throw("second output set with a null fourth output: the module writes through the null pointer; module not called");
    const int rc = Fn<int (*)(void*, float*, float*, float*, float*)>(0x19e40)(writingLine, p1, p2, p3, p4);
    std::string outputs;
    float* pointers[4] = {p1, p2, p3, p4};
    for (int i = 0; i < 4; ++i) {
        char text[64];
        if (pointers[i]) std::snprintf(text, sizeof text, " out%d %.9g", i + 1, *pointers[i]);
        else std::snprintf(text, sizeof text, " out%d null", i + 1);
        outputs += text;
    }
    std::printf("  GetOrderingSpace(%s) -> 0x%x%s\n", LineName(writingLine).c_str(), static_cast<unsigned>(rc), outputs.c_str());
    ThrowOnError(rc);
    return rc;
}

}
