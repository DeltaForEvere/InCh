#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace InventoryChanger {
    enum class ItemKind : std::uint8_t {
        Generic,
        Weapon,
        Knife,
        Glove
    };

    struct ItemRequest {
        ItemKind Kind{ ItemKind::Generic };
        std::uint16_t DefinitionIndex{};
        int PaintKit{};
        int Seed{};
        float Wear{ 0.01f };
        int StatTrak{ -1 };
        int Team{}; // 0 = both, 2 = T, 3 = CT.
        // -1 asks the inventory backend to find the current slot by definition.
        int LoadoutSlot{ -1 };
    };

    struct ItemRecord {
        std::uint64_t ItemId{};
        std::uint16_t DefinitionIndex{};
        int PaintKit{};
        int Seed{};
        float Wear{};
        int StatTrak{};
        int Team{};
        int LoadoutSlot{};
        ItemKind Kind{ ItemKind::Generic };
        bool EquippedT{};
        bool EquippedCT{};
        bool ViewConfirmed{};
    };

    bool Initialize() noexcept;
    bool Ready() noexcept;
    std::string Status();

    bool QueueCreate(const ItemRequest& request);
    bool Remove(std::uint64_t itemId) noexcept;
    std::vector<ItemRecord> Snapshot();

    // Runs queued SOCache work and keeps equipped glove views synchronized.
    void Update() noexcept;
    // Applies the currently equipped local knife item to an already spawned knife.
    bool ApplyKnifeOverride(uintptr_t pawn, uintptr_t weapon) noexcept;
    // Synchronizes a created local weapon item with the matching spawned weapon.
    bool ApplyWeaponOverride(uintptr_t pawn, uintptr_t weapon) noexcept;
    // Rebuilds the active weapon's material using the same safe Econ path.
    bool ApplyWeaponPaintOverride(uintptr_t pawn, uintptr_t weapon, int paintKit,
        int seed, float wear, int statTrak, bool legacyModel) noexcept;
    void Shutdown() noexcept;
}
