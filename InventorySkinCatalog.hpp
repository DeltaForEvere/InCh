#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace InventorySkinCatalog {
    struct Entry {
        std::uint16_t Definition{};
        int PaintKit{};
        const char* Finish{};
        const char* FullName{};
        std::uint8_t Rarity{};
        bool LegacyModel{};
        float MinWear{};
        float MaxWear{ 1.0f };
    };

    inline constexpr Entry Entries[]{
#include "InventorySkinCatalog.generated.inc"
    };

    inline const Entry* Find(std::uint16_t definition, int paintKit) noexcept {
        if (!definition || paintKit <= 0)
            return nullptr;
        for (const auto& entry : Entries) {
            if (entry.Definition == definition && entry.PaintKit == paintKit)
                return &entry;
        }
        return nullptr;
    }

    inline std::size_t Count(std::uint16_t definition) noexcept {
        std::size_t count{};
        for (const auto& entry : Entries)
            count += entry.Definition == definition;
        return count;
    }

    inline bool MatchesSearch(std::string_view text, const char* search) noexcept {
        if (!search || !*search)
            return true;
        const std::string_view needle{ search };
        if (needle.size() > text.size())
            return false;
        for (std::size_t start = 0; start + needle.size() <= text.size(); ++start) {
            bool equal = true;
            for (std::size_t index = 0; index < needle.size(); ++index) {
                const auto left = static_cast<unsigned char>(text[start + index]);
                const auto right = static_cast<unsigned char>(needle[index]);
                if (std::tolower(left) != std::tolower(right)) {
                    equal = false;
                    break;
                }
            }
            if (equal)
                return true;
        }
        return false;
    }
}
