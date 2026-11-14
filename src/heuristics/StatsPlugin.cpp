// observex_stats.dll: reading the game's bars and numbers (heuristics/StatsReader) behind the plain C entry the app
// loads (heuristics/StatsAbi, engine/StatsLibrary). Nothing here throws out of the DLL.
#include "heuristics/StatsAbi.h"
#include "heuristics/StatsReader.h"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <span>
#include <string>

namespace {

void* create(const char* folder, char* error, std::uint32_t errorSize) {
    try {
        auto* reader = new observex::StatsReader();
        std::string why;
        const std::filesystem::path path(std::u8string(reinterpret_cast<const char8_t*>(folder)));
        if (reader->load(path, &why)) {
            why.clear();
        }
        if (error != nullptr && errorSize > 0) {
            std::snprintf(error, errorSize, "%s", why.c_str());
        }
        return reader;
    } catch (...) {
        return nullptr;
    }
}

void destroy(void* reader) { delete static_cast<observex::StatsReader*>(reader); }

int areas(void* reader, const observex::UiPiece* pieces, int count, int width, int height, observex::StatsArea* out,
          int max) {
    try {
        return static_cast<observex::StatsReader*>(reader)->areas(
            {pieces, static_cast<std::size_t>(count)}, width, height, out, max);
    } catch (...) {
        return -1;
    }
}

bool read(void* reader, const observex::UiPiece* pieces, int count, const observex::ColourPart* parts, int partCount,
          observex::GameStats* out) {
    try {
        static_cast<observex::StatsReader*>(reader)->read({pieces, static_cast<std::size_t>(count)},
                                                          {parts, static_cast<std::size_t>(partCount)}, *out);
        return true;
    } catch (...) {
        return false;
    }
}

bool setSkills(void* reader, const observex::SkillPicture* skills, int count) {
    try {
        static_cast<observex::StatsReader*>(reader)->setSkills({skills, static_cast<std::size_t>(count)});
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

extern "C" __declspec(dllexport) const ObservexStatsApi* observexStats() {
    static const ObservexStatsApi api{kObservexStatsAbi,
                                      static_cast<std::uint32_t>(sizeof(observex::UiPiece)),
                                      static_cast<std::uint32_t>(sizeof(observex::StatsArea)),
                                      static_cast<std::uint32_t>(sizeof(observex::ColourPart)),
                                      static_cast<std::uint32_t>(sizeof(observex::GameStats)),
                                      static_cast<std::uint32_t>(sizeof(observex::SkillPicture)),
                                      create,
                                      destroy,
                                      areas,
                                      read,
                                      setSkills};
    return &api;
}
