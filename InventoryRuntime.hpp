#pragma once

#include <Windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include "DebugLog.hpp"
#include "DynamicSchema.hpp"
#include "Entities/Entities.hpp"
#include "Memory.hpp"

namespace InventoryRuntime {
    // CUtlVectorFixed ABI used by Source 2 networked loadout vectors.
    struct LoadoutVector {
        std::uint32_t Size{};
        std::uint32_t Padding{};
        uintptr_t Data{};
        std::uint64_t Reserved{};
    };

    struct RuntimeOffsets {
        std::ptrdiff_t LocalController{};
        std::ptrdiff_t InventoryServices{};
        std::ptrdiff_t NetworkableLoadout{};
        std::ptrdiff_t SlotItem{};
        std::ptrdiff_t SlotTeam{};
        std::ptrdiff_t SlotNumber{};
        std::ptrdiff_t DefinitionIndex{};
        std::ptrdiff_t ItemIdHigh{};
        std::ptrdiff_t ItemIdLow{};
        std::size_t SlotStride{};
        bool Loaded{};
    };

    inline RuntimeOffsets Offsets{};
    inline int LastCount{};
    inline int ValidItems{};
    inline std::uint16_t LastDefinition{};
    inline std::uint64_t LastItemId{};
    inline uintptr_t Manager{};       // Local controller in the schema-only path.
    inline uintptr_t LocalInventory{}; // CCSPlayerController_InventoryServices.
    inline bool SignatureFound{};     // Kept for compatibility with the UI.
    inline bool Ready{};

    inline std::size_t AlignUp(std::size_t value, std::size_t alignment) noexcept {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    inline bool ImportOffsets() noexcept {
        if (Offsets.Loaded)
            return true;
        if (!DynamicSchema::Ready())
            return false;

        const auto localController = DynamicSchema::ModuleOffset("client.dll", "dwLocalPlayerController");
        const auto inventoryServices = DynamicSchema::Field("CCSPlayerController", "m_pInventoryServices");
        const auto loadout = DynamicSchema::Field(
            "CCSPlayerController_InventoryServices", "m_vecNetworkableLoadout");
        const auto slotItem = DynamicSchema::Field(
            "CCSPlayerController_InventoryServices__NetworkedLoadoutSlot_t", "pItem");
        const auto slotTeam = DynamicSchema::Field(
            "CCSPlayerController_InventoryServices__NetworkedLoadoutSlot_t", "team");
        const auto slotNumber = DynamicSchema::Field(
            "CCSPlayerController_InventoryServices__NetworkedLoadoutSlot_t", "slot");
        const auto definition = DynamicSchema::Field("C_EconItemView", "m_iItemDefinitionIndex");
        const auto idHigh = DynamicSchema::Field("C_EconItemView", "m_iItemIDHigh");
        const auto idLow = DynamicSchema::Field("C_EconItemView", "m_iItemIDLow");
        if (!localController || !inventoryServices || !loadout || !slotItem ||
            !slotTeam || !slotNumber || !definition || !idHigh || !idLow)
            return false;

        RuntimeOffsets imported{};
        imported.LocalController = *localController;
        imported.InventoryServices = *inventoryServices;
        imported.NetworkableLoadout = *loadout;
        imported.SlotItem = *slotItem;
        imported.SlotTeam = *slotTeam;
        imported.SlotNumber = *slotNumber;
        imported.DefinitionIndex = *definition;
        imported.ItemIdHigh = *idHigh;
        imported.ItemIdLow = *idLow;
        const auto endOfFields = (std::max)({
            static_cast<std::size_t>(imported.SlotItem) + sizeof(uintptr_t),
            static_cast<std::size_t>(imported.SlotTeam) + sizeof(std::uint16_t),
            static_cast<std::size_t>(imported.SlotNumber) + sizeof(std::uint16_t) });
        imported.SlotStride = AlignUp(endOfFields, alignof(uintptr_t));
        if (imported.SlotStride < sizeof(uintptr_t) || imported.SlotStride > 128)
            return false;
        imported.Loaded = true;
        Offsets = imported;
        return true;
    }

    inline void Update() noexcept {
        LastCount = ValidItems = 0;
        LastDefinition = 0;
        LastItemId = 0;
        Manager = LocalInventory = 0;
        Ready = false;
        SignatureFound = ImportOffsets();
        if (!SignatureFound || !CEntities::Client)
            return;

        if (!Memory::Read(CEntities::Client + Offsets.LocalController, Manager) || !Manager ||
            !Memory::Read(Manager + Offsets.InventoryServices, LocalInventory) || !LocalInventory)
            return;

        LoadoutVector loadout{};
        if (!Memory::Read(LocalInventory + Offsets.NetworkableLoadout, loadout) ||
            loadout.Size > 256 || (loadout.Size && !loadout.Data))
            return;

        LastCount = static_cast<int>(loadout.Size);
        Ready = true;
        for (std::uint32_t index = 0; index < loadout.Size; ++index) {
            const uintptr_t slotAddress = loadout.Data + Offsets.SlotStride * index;
            uintptr_t item{};
            if (!Memory::Read(slotAddress + Offsets.SlotItem, item) || !item)
                continue;
            std::uint16_t definition{};
            std::uint32_t idHigh{}, idLow{};
            if (!Memory::Read(item + Offsets.DefinitionIndex, definition) || !definition ||
                !Memory::Read(item + Offsets.ItemIdHigh, idHigh) ||
                !Memory::Read(item + Offsets.ItemIdLow, idLow))
                continue;
            ++ValidItems;
            LastDefinition = definition;
            LastItemId = (static_cast<std::uint64_t>(idHigh) << 32) | idLow;
        }

        static ULONGLONG lastLog{};
        const auto now = GetTickCount64();
        if (now - lastLog >= 2000) {
            char message[288]{};
            std::snprintf(message, sizeof(message),
                "[Inventory] schema=%d ready=%d controller=%p services=%p count=%d valid=%d last(def=%u id=%llX)",
                SignatureFound, Ready, reinterpret_cast<void*>(Manager),
                reinterpret_cast<void*>(LocalInventory), LastCount, ValidItems,
                static_cast<unsigned>(LastDefinition),
                static_cast<unsigned long long>(LastItemId));
            DebugLog::Write(message);
            lastLog = now;
        }
    }
}
