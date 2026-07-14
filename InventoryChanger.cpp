#include "InventoryChanger.hpp"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include "DebugLog.hpp"
#include "DynamicSchema.hpp"
#include "Entities/Entities.hpp"
#include "InventoryCatalog.hpp"
#include "InventorySkinCatalog.hpp"
#include "Memory.hpp"

namespace {
    constexpr int kTeamT = 2;
    constexpr int kTeamCT = 3;
    constexpr int kIncrementalEvent = 4;
    constexpr int kLoadoutSlotCount = 57;
    constexpr std::size_t kMaxInjectedItems = 128;

    // CEconItem is not exported through Schema. These offsets are verified by
    // the CreateSharedObjectSubclassEconItem factory before the backend is armed.
    constexpr std::ptrdiff_t kEconItemId = 0x10;
    constexpr std::ptrdiff_t kEconOriginalId = 0x18;
    constexpr std::ptrdiff_t kEconAccountId = 0x28;
    constexpr std::ptrdiff_t kEconInventory = 0x2C;
    constexpr std::ptrdiff_t kEconDefinition = 0x30;
    constexpr std::ptrdiff_t kInventoryOwner = 0x10;
    constexpr int kEconItemTypeId = 1;

    struct SOID {
        std::uint64_t Id{};
        std::uint32_t Type{};
        std::uint32_t Padding{};
    };
    static_assert(sizeof(SOID) == 16);

    struct PatternByte {
        std::uint8_t Value{};
        bool Wildcard{};
    };

    struct TextSection {
        std::uint8_t* Begin{};
        std::size_t Size{};
    };

    struct RuntimeFunctions {
        uintptr_t Manager{};
        std::ptrdiff_t LocalInventoryOffset{};
        std::ptrdiff_t SharedObjectCacheOffset{};
        uintptr_t CreateEconItem{};
        uintptr_t CreateBaseTypeCache{};
        uintptr_t GetEconItemSystem{};
        uintptr_t GetAttributeDefinition{};
        uintptr_t SetDynamicAttribute{};
        uintptr_t SetAttributeValueByName{};
        uintptr_t EquipItemInLoadout{};
        uintptr_t GetItemInLoadout{};
        uintptr_t SetModel{};
        uintptr_t SetMeshGroupMask{};
        uintptr_t UpdateSubclass{};
        uintptr_t UpdateSkin{};
        uintptr_t UpdateCompositeMaterial{};
        uintptr_t UpdateCompositeMaterialSet{};
        std::ptrdiff_t CompositeMaterialOffset{};
        uintptr_t GetBodyGroupCount{};
        uintptr_t GetBodyGroupName{};
        uintptr_t SetBodyGroup{};
        uintptr_t UpdateBodyGroupChoice{};
        uintptr_t FindHudElement{};
        uintptr_t ClearHudWeaponIcon{};
        bool Loaded{};
    } g_Runtime;

    struct ViewOffsets {
        std::ptrdiff_t LocalPawn{};
        std::ptrdiff_t Team{};
        std::ptrdiff_t AttributeManager{};
        std::ptrdiff_t Item{};
        std::ptrdiff_t Definition{};
        std::ptrdiff_t ItemId{};
        std::ptrdiff_t ItemIdHigh{};
        std::ptrdiff_t ItemIdLow{};
        std::ptrdiff_t AccountId{};
        std::ptrdiff_t Initialized{};
        std::ptrdiff_t DisallowSoc{};
        std::ptrdiff_t RestoreCustomMaterial{};
        std::ptrdiff_t EntityQuality{};
        std::ptrdiff_t ItemDescription{};
        std::ptrdiff_t CustomNameOverride{};
        std::ptrdiff_t FallbackPaint{};
        std::ptrdiff_t FallbackSeed{};
        std::ptrdiff_t FallbackWear{};
        std::ptrdiff_t FallbackStatTrak{};
        std::ptrdiff_t EconGloves{};
        std::ptrdiff_t ReapplyGloves{};
        std::ptrdiff_t LastSpawnTime{};
        std::ptrdiff_t EntityList{};
        std::ptrdiff_t HudModelArms{};
        std::ptrdiff_t SceneNode{};
        std::ptrdiff_t SceneChild{};
        std::ptrdiff_t SceneSibling{};
        std::ptrdiff_t SceneOwner{};
        std::ptrdiff_t OwnerEntity{};
        std::ptrdiff_t SubclassId{};
        std::ptrdiff_t ModelState{};
        std::ptrdiff_t MeshGroupMask{};
        bool Loaded{};
    } g_ViewOffsets;

    struct AppliedKnifeModel {
        uintptr_t Weapon{};
        uintptr_t HudWeapon{};
        std::uint16_t Definition{};
        bool WorldReady{};
    } g_AppliedKnifeModel;
    std::uint64_t g_LastAppliedKnifeItemId{};



    struct AppliedGloveState {
        uintptr_t Pawn{};
        std::uint64_t ItemId{};
        float SpawnTime{};
        std::uint8_t UpdateFrames{};
    } g_AppliedGloveState;

    struct AppliedWeaponSkin {
        uintptr_t Weapon{};
        int PaintKit{};
        int Seed{};
        float Wear{};
        int StatTrak{ -1 };
    };
    std::array<AppliedWeaponSkin, 8> g_WeaponSkinCache{};

    AppliedWeaponSkin& GetOrCreateWeaponSkinEntry(uintptr_t weapon) noexcept {
        for (auto& e : g_WeaponSkinCache)
            if (e.Weapon == weapon) return e;
        for (auto& e : g_WeaponSkinCache)
            if (!e.Weapon) { e.Weapon = weapon; return e; }
        g_WeaponSkinCache[0] = {};
        g_WeaponSkinCache[0].Weapon = weapon;
        return g_WeaponSkinCache[0];
    }

    struct OwnedItem {
        InventoryChanger::ItemRecord Public;
        uintptr_t Object{};
    };

    std::mutex g_Mutex;
    std::vector<OwnedItem> g_Items;
    std::optional<InventoryChanger::ItemRequest> g_Pending;
    std::string g_Status{ "Not initialized" };
    const char* g_LastAttributeFailure{ "not attempted" };
    std::atomic_uint64_t g_IdCounter{ 1 };

    std::vector<PatternByte> ParsePattern(std::string_view text) {
        std::vector<PatternByte> result;
        std::istringstream stream{ std::string(text) };
        std::string token;
        while (stream >> token) {
            if (token == "?" || token == "??") {
                result.push_back({ 0, true });
                continue;
            }
            unsigned value{};
            std::istringstream converter(token);
            converter >> std::hex >> value;
            if (!converter || value > 0xFF)
                return {};
            result.push_back({ static_cast<std::uint8_t>(value), false });
        }
        return result;
    }

    std::optional<TextSection> GetTextSection(HMODULE module) noexcept {
        if (!module)
            return std::nullopt;
        const auto base = reinterpret_cast<std::uint8_t*>(module);
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return std::nullopt;
        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return std::nullopt;
        const auto section = IMAGE_FIRST_SECTION(nt);
        for (unsigned index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
            if (std::memcmp(section[index].Name, ".text", 5) == 0) {
                if (section[index].VirtualAddress >= nt->OptionalHeader.SizeOfImage)
                    return std::nullopt;
                const auto requestedSize = static_cast<std::size_t>(
                    (std::max)(section[index].Misc.VirtualSize, section[index].SizeOfRawData));
                const auto remainingImage = static_cast<std::size_t>(
                    nt->OptionalHeader.SizeOfImage - section[index].VirtualAddress);
                const auto size = (std::min)(requestedSize, remainingImage);
                if (!size)
                    return std::nullopt;
                return TextSection{ base + section[index].VirtualAddress, size };
            }
        }
        return std::nullopt;
    }

    uintptr_t FindUnique(const TextSection& text, std::string_view patternText) {
        const auto pattern = ParsePattern(patternText);
        if (pattern.empty() || pattern.size() > text.Size)
            return 0;
        uintptr_t result{};
        unsigned matches{};
        for (std::size_t offset = 0; offset + pattern.size() <= text.Size; ++offset) {
            bool match = true;
            for (std::size_t index = 0; index < pattern.size(); ++index) {
                if (!pattern[index].Wildcard &&
                    text.Begin[offset + index] != pattern[index].Value) {
                    match = false;
                    break;
                }
            }
            if (!match)
                continue;
            result = reinterpret_cast<uintptr_t>(text.Begin + offset);
            if (++matches > 1)
                return 0;
        }
        return matches == 1 ? result : 0;
    }

    uintptr_t FindFirst(const TextSection& text, std::string_view patternText) {
        const auto pattern = ParsePattern(patternText);
        if (pattern.empty() || pattern.size() > text.Size)
            return 0;
        for (std::size_t offset = 0; offset + pattern.size() <= text.Size; ++offset) {
            bool match = true;
            for (std::size_t index = 0; index < pattern.size(); ++index) {
                if (!pattern[index].Wildcard &&
                    text.Begin[offset + index] != pattern[index].Value) {
                    match = false;
                    break;
                }
            }
            if (match)
                return reinterpret_cast<uintptr_t>(text.Begin + offset);
        }
        return 0;
    }

    uintptr_t ResolveRelativeTarget(uintptr_t instruction, std::size_t displacementOffset,
        std::size_t instructionSize) noexcept {
        std::int32_t displacement{};
        if (!instruction || !Memory::Read(
            instruction + displacementOffset, displacement))
            return 0;
        return instruction + instructionSize + displacement;
    }

    bool IsExecutable(uintptr_t address) noexcept {
        if (!Memory::IsCanonicalUserAddress(address))
            return false;
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) ||
            info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            return false;
        const DWORD protection = info.Protect & 0xFF;
        return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
            protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
    }

    uintptr_t VtableFunction(uintptr_t object, std::size_t index) noexcept {
        uintptr_t vtable{}, function{};
        if (!Memory::Read(object, vtable) || !vtable ||
            !Memory::Read(vtable + index * sizeof(uintptr_t), function) ||
            !IsExecutable(function))
            return 0;
        return function;
    }

    uintptr_t SafeCreate(uintptr_t function) noexcept {
        __try {
            return reinterpret_cast<uintptr_t(__cdecl*)()>(function)();
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    uintptr_t SafeGetSystem(uintptr_t function) noexcept {
        __try {
            return reinterpret_cast<uintptr_t(__fastcall*)()>(function)();
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    uintptr_t SafeCreateBaseTypeCache(uintptr_t function, uintptr_t cache) noexcept {
        __try {
            return reinterpret_cast<uintptr_t(__fastcall*)(uintptr_t, int)>(function)(
                cache, kEconItemTypeId);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    bool SafeAddObject(uintptr_t function, uintptr_t cache, uintptr_t object) noexcept {
        __try {
            return reinterpret_cast<bool(__fastcall*)(uintptr_t, uintptr_t)>(function)(
                cache, object);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool SafeRemoveObject(uintptr_t function, uintptr_t cache, uintptr_t object) noexcept {
        __try {
            return reinterpret_cast<uintptr_t(__fastcall*)(uintptr_t, uintptr_t)>(function)(
                cache, object) != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    uintptr_t SafeGetAttributeDefinition(uintptr_t schema, uintptr_t function, int index) noexcept {
        __try {
            return reinterpret_cast<uintptr_t(__fastcall*)(uintptr_t, int)>(function)(schema, index);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    bool SafeSetAttribute(uintptr_t function, uintptr_t item,
        uintptr_t definition, const void* value) noexcept {
        __try {
            reinterpret_cast<void(__fastcall*)(uintptr_t, uintptr_t, const void*)>(function)(
                item, definition, value);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool SafeSetViewAttribute(uintptr_t function, uintptr_t view,
        const char* name, float value) noexcept {
        if (!function || !view || !name || !*name)
            return false;
        __try {
            reinterpret_cast<void(__fastcall*)(uintptr_t, const char*, float)>(function)(
                view, name, value);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool ApplyViewPaintAttributes(uintptr_t view, int paint,
        float wear, int seed) noexcept {
        if (paint <= 0)
            return true;
        return SafeSetViewAttribute(g_Runtime.SetAttributeValueByName, view,
                   "set item texture prefab", static_cast<float>(paint)) &&
            SafeSetViewAttribute(g_Runtime.SetAttributeValueByName, view,
                "set item texture wear", wear) &&
            SafeSetViewAttribute(g_Runtime.SetAttributeValueByName, view,
                "set item texture seed", static_cast<float>(seed));
    }

    bool SafeEquip(uintptr_t function, uintptr_t manager, int team,
        int slot, std::uint64_t itemId) noexcept {
        __try {
            return reinterpret_cast<bool(__fastcall*)(uintptr_t, int, int, std::uint64_t)>(function)(
                manager, team, slot, itemId);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool SafeSetModel(uintptr_t function, uintptr_t entity, const char* model) noexcept {
        if (!function || !entity || !model || !*model)
            return false;
        __try {
            reinterpret_cast<void(__fastcall*)(uintptr_t, const char*)>(function)(entity, model);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool SafeSetMeshGroup(uintptr_t function, uintptr_t sceneNode,
        std::uint64_t mask) noexcept {
        if (!function || !sceneNode)
            return false;
        __try {
            reinterpret_cast<void(__fastcall*)(uintptr_t, std::uint64_t)>(function)(
                sceneNode, mask);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool SafeUpdateSubclass(uintptr_t function, uintptr_t weapon) noexcept {
        if (!function || !weapon)
            return false;
        __try {
            reinterpret_cast<void(__fastcall*)(uintptr_t)>(function)(weapon);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool SafeUpdateSkin(uintptr_t function, uintptr_t weapon) noexcept {
        if (!function || !weapon)
            return false;
        __try {
            reinterpret_cast<void(__fastcall*)(uintptr_t, bool)>(function)(weapon, true);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool SafeUpdateCompositeMaterial(uintptr_t function,
        uintptr_t owner) noexcept {
        if (!function || !owner)
            return false;
        __try {
            reinterpret_cast<void(__fastcall*)(uintptr_t, bool)>(function)(owner, true);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool SafeUpdateCompositeMaterialSet(uintptr_t function,
        uintptr_t weapon) noexcept {
        if (!function || !weapon)
            return false;
        __try {
            reinterpret_cast<void(__fastcall*)(uintptr_t, bool)>(function)(weapon, true);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    int SafeGetBodyGroupCount(uintptr_t function, uintptr_t pawn) noexcept {
        if (!function || !pawn)
            return 0;
        __try {
            return reinterpret_cast<int(__fastcall*)(uintptr_t, int)>(function)(pawn, 0);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    uintptr_t SafeGetBodyGroupName(uintptr_t function, uintptr_t pawn) noexcept {
        if (!function || !pawn)
            return 0;
        __try {
            return reinterpret_cast<uintptr_t(__fastcall*)(uintptr_t, int)>(function)(pawn, 0);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    bool SafeRefreshGloveBodyGroup(uintptr_t pawn) noexcept {
        if (!pawn || !g_Runtime.GetBodyGroupCount || !g_Runtime.GetBodyGroupName ||
            !g_Runtime.SetBodyGroup || !g_Runtime.UpdateBodyGroupChoice)
            return false;
        const int count = SafeGetBodyGroupCount(g_Runtime.GetBodyGroupCount, pawn);
        const uintptr_t groupName = SafeGetBodyGroupName(
            g_Runtime.GetBodyGroupName, pawn);
        if (count <= 0 || !Memory::IsCanonicalUserAddress(groupName, 1))
            return false;
        __try {
            reinterpret_cast<void(__fastcall*)(uintptr_t, const char*, int)>(
                g_Runtime.SetBodyGroup)(pawn,
                    reinterpret_cast<const char*>(groupName), count - 1);
            reinterpret_cast<void(__fastcall*)(uintptr_t)>(
                g_Runtime.UpdateBodyGroupChoice)(pawn);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    uintptr_t SafeFindHudElement(uintptr_t function, const char* name) noexcept {
        if (!function || !name || !*name)
            return 0;
        __try {
            return reinterpret_cast<uintptr_t(__fastcall*)(const char*)>(function)(name);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    bool SafeClearHudWeaponIcon(uintptr_t function, uintptr_t selection) noexcept {
        if (!function || !selection)
            return false;
        __try {
            reinterpret_cast<void(__fastcall*)(uintptr_t, int, std::int64_t)>(function)(
                selection, 0, 0);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    std::uint32_t StringTokenHash(std::string_view text) noexcept {
        constexpr std::uint32_t seed = 0x31415926;
        constexpr std::uint32_t multiplier = 0x5BD1E995;
        std::uint32_t hash = seed ^ static_cast<std::uint32_t>(text.size());
        std::size_t offset{};
        while (text.size() - offset >= sizeof(std::uint32_t)) {
            std::uint32_t value{};
            std::array<unsigned char, sizeof(value)> lowered{};
            for (std::size_t index = 0; index < lowered.size(); ++index) {
                const auto character = static_cast<unsigned char>(text[offset + index]);
                lowered[index] = character >= 'A' && character <= 'Z'
                    ? static_cast<unsigned char>(character + ('a' - 'A')) : character;
            }
            std::memcpy(&value, lowered.data(), sizeof(value));
            value *= multiplier;
            value ^= value >> 24;
            value *= multiplier;
            hash *= multiplier;
            hash ^= value;
            offset += sizeof(value);
        }
        const auto lower = [](char value) noexcept {
            auto character = static_cast<unsigned char>(value);
            return character >= 'A' && character <= 'Z'
                ? static_cast<unsigned char>(character + ('a' - 'A')) : character;
        };
        const auto remaining = text.size() - offset;
        if (remaining >= 3)
            hash ^= static_cast<std::uint32_t>(lower(text[offset + 2])) << 16;
        if (remaining >= 2)
            hash ^= static_cast<std::uint32_t>(lower(text[offset + 1])) << 8;
        if (remaining >= 1) {
            hash ^= lower(text[offset]);
            hash *= multiplier;
        }
        hash ^= hash >> 13;
        hash *= multiplier;
        hash ^= hash >> 15;
        return hash;
    }

    uintptr_t SafeGetItemInLoadout(uintptr_t function, uintptr_t inventory,
        int team, int slot) noexcept {
        __try {
            return reinterpret_cast<uintptr_t(__fastcall*)(uintptr_t, int, int)>(function)(
                inventory, team, slot);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    bool SafeNotify(uintptr_t function, uintptr_t inventory, SOID owner,
        uintptr_t item, int event) noexcept {
        __try {
            reinterpret_cast<void(__fastcall*)(uintptr_t, SOID, uintptr_t, int)>(function)(
                inventory, owner, item, event);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool SafeDestroy(uintptr_t function, uintptr_t item) noexcept {
        __try {
            reinterpret_cast<void(__fastcall*)(uintptr_t, bool)>(function)(item, true);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool LoadViewOffsets() noexcept {
        if (g_ViewOffsets.Loaded)
            return true;
        const auto localPawn = DynamicSchema::ModuleOffset("client.dll", "dwLocalPlayerPawn");
        const auto team = DynamicSchema::Field("C_BaseEntity", "m_iTeamNum");
        const auto attributeManager = DynamicSchema::Field("C_EconEntity", "m_AttributeManager");
        const auto item = DynamicSchema::Field("C_AttributeContainer", "m_Item");
        const auto definition = DynamicSchema::Field("C_EconItemView", "m_iItemDefinitionIndex");
        const auto itemId = DynamicSchema::Field("C_EconItemView", "m_iItemID");
        const auto itemIdHigh = DynamicSchema::Field("C_EconItemView", "m_iItemIDHigh");
        const auto itemIdLow = DynamicSchema::Field("C_EconItemView", "m_iItemIDLow");
        const auto accountId = DynamicSchema::Field("C_EconItemView", "m_iAccountID");
        const auto initialized = DynamicSchema::Field("C_EconItemView", "m_bInitialized");
        const auto disallowSoc = DynamicSchema::Field("C_EconItemView", "m_bDisallowSOC");
        const auto restoreCustomMaterial = DynamicSchema::Field(
            "C_EconItemView", "m_bRestoreCustomMaterialAfterPrecache");
        const auto entityQuality = DynamicSchema::Field(
            "C_EconItemView", "m_iEntityQuality");
        const auto attributeList = DynamicSchema::Field("C_EconItemView", "m_AttributeList");
        const auto customNameOverride = DynamicSchema::Field(
            "C_EconItemView", "m_szCustomNameOverride");
        const auto fallbackPaint = DynamicSchema::Field("C_EconEntity", "m_nFallbackPaintKit");
        const auto fallbackSeed = DynamicSchema::Field("C_EconEntity", "m_nFallbackSeed");
        const auto fallbackWear = DynamicSchema::Field("C_EconEntity", "m_flFallbackWear");
        const auto fallbackStatTrak = DynamicSchema::Field("C_EconEntity", "m_nFallbackStatTrak");
        const auto econGloves = DynamicSchema::Field("C_CSPlayerPawn", "m_EconGloves");
        const auto reapplyGloves = DynamicSchema::Field("C_CSPlayerPawn", "m_bNeedToReApplyGloves");
        const auto lastSpawnTime = DynamicSchema::Field(
            "C_CSPlayerPawnBase", "m_flLastSpawnTimeIndex");
        const auto entityList = DynamicSchema::ModuleOffset("client.dll", "dwEntityList");
        const auto hudModelArms = DynamicSchema::Field("C_CSPlayerPawn", "m_hHudModelArms");
        const auto sceneNode = DynamicSchema::Field("C_BaseEntity", "m_pGameSceneNode");
        const auto sceneChild = DynamicSchema::Field("CGameSceneNode", "m_pChild");
        const auto sceneSibling = DynamicSchema::Field("CGameSceneNode", "m_pNextSibling");
        const auto sceneOwner = DynamicSchema::Field("CGameSceneNode", "m_pOwner");
        const auto ownerEntity = DynamicSchema::Field("C_BaseEntity", "m_hOwnerEntity");
        const auto subclassId = DynamicSchema::Field("C_BaseEntity", "m_nSubclassID");
        const auto modelState = DynamicSchema::Field("CSkeletonInstance", "m_modelState");
        const auto meshGroupMask = DynamicSchema::Field("CModelState", "m_MeshGroupMask");
        if (!localPawn || !team || !attributeManager || !item || !definition ||
            !itemId || !itemIdHigh || !itemIdLow || !accountId || !initialized ||
            !disallowSoc || !restoreCustomMaterial || !entityQuality ||
            !attributeList || !customNameOverride ||
            *attributeList < static_cast<std::ptrdiff_t>(sizeof(uintptr_t)) ||
            !fallbackPaint || !fallbackSeed || !fallbackWear ||
            !fallbackStatTrak || !econGloves || !reapplyGloves || !lastSpawnTime || !entityList ||
            !hudModelArms || !sceneNode || !sceneChild || !sceneSibling ||
            !sceneOwner || !ownerEntity || !subclassId || !modelState || !meshGroupMask)
            return false;
        g_ViewOffsets = { *localPawn, *team, *attributeManager, *item, *definition,
            *itemId, *itemIdHigh, *itemIdLow, *accountId, *initialized, *disallowSoc,
            *restoreCustomMaterial, *entityQuality, *attributeList -
                static_cast<std::ptrdiff_t>(sizeof(uintptr_t)),
            *customNameOverride,
            *fallbackPaint, *fallbackSeed, *fallbackWear, *fallbackStatTrak,
            *econGloves, *reapplyGloves, *lastSpawnTime, *entityList, *hudModelArms, *sceneNode,
            *sceneChild, *sceneSibling, *sceneOwner, *ownerEntity, *subclassId, *modelState,
            *meshGroupMask, true };
        return true;
    }

    uintptr_t ResolveEntityHandle(std::uint32_t handle) noexcept {
        if (!CEntities::Client || !g_ViewOffsets.Loaded || !handle || handle == 0xFFFFFFFF)
            return 0;
        uintptr_t list{}, chunk{}, entity{};
        if (!Memory::Read(CEntities::Client + g_ViewOffsets.EntityList, list) || !list)
            return 0;
        const auto index = handle & 0x7FFF;
        if (!index ||
            !Memory::Read(list + 0x10 + sizeof(uintptr_t) * (index >> 9), chunk) || !chunk ||
            !Memory::Read(chunk + 0x70 * (index & 0x1FF), entity) || !entity)
            return 0;
        uintptr_t vtable{};
        return Memory::Read(entity, vtable) && vtable ? entity : 0;
    }

    uintptr_t ResolveHudWeapon(uintptr_t pawn, uintptr_t weapon) noexcept {
        std::uint32_t armsHandle{};
        uintptr_t arms{}, armsNode{}, child{};
        if (!pawn || !weapon ||
            !Memory::Read(pawn + g_ViewOffsets.HudModelArms, armsHandle) ||
            !(arms = ResolveEntityHandle(armsHandle)) ||
            !Memory::Read(arms + g_ViewOffsets.SceneNode, armsNode) || !armsNode ||
            !Memory::Read(armsNode + g_ViewOffsets.SceneChild, child))
            return 0;

        // Scene-node lists are game-owned. Bound traversal so corrupt/stale links
        // cannot turn a cosmetic update into an infinite loop.
        for (unsigned index = 0; child && index < 64; ++index) {
            uintptr_t owner{};
            std::uint32_t ownerHandle{};
            if (Memory::Read(child + g_ViewOffsets.SceneOwner, owner) && owner &&
                Memory::Read(owner + g_ViewOffsets.OwnerEntity, ownerHandle) &&
                ResolveEntityHandle(ownerHandle) == weapon)
                return owner;
            uintptr_t sibling{};
            if (!Memory::Read(child + g_ViewOffsets.SceneSibling, sibling) || sibling == child)
                break;
            child = sibling;
        }
        return 0;
    }

    bool InvalidateHudDescription(uintptr_t view) noexcept {
        if (!view || !g_ViewOffsets.ItemDescription)
            return false;
        constexpr uintptr_t emptyDescription{};
        const bool descriptionCleared = Memory::Write(
            view + g_ViewOffsets.ItemDescription, emptyDescription);
        const uintptr_t element = SafeFindHudElement(
            g_Runtime.FindHudElement, "HudWeaponSelection");
        // FindHudElement returns the embedded Panorama element. The selection
        // object begins 0x98 bytes earlier in the current client ABI.
        if (element > 0x98 && g_Runtime.ClearHudWeaponIcon)
            SafeClearHudWeaponIcon(g_Runtime.ClearHudWeaponIcon, element - 0x98);
        return descriptionCleared;
    }

    bool SetMeshGroup(uintptr_t entity, std::uint64_t mask) noexcept {
        uintptr_t sceneNode{};
        if (!entity || !Memory::Read(
            entity + g_ViewOffsets.SceneNode, sceneNode) || !sceneNode)
            return false;
        if (SafeSetMeshGroup(g_Runtime.SetMeshGroupMask, sceneNode, mask))
            return true;
        return Memory::Write(sceneNode + g_ViewOffsets.ModelState +
            g_ViewOffsets.MeshGroupMask, mask);
    }

    const InventoryCatalog::Item* FindKnifeCatalogItem(std::uint16_t definition) noexcept {
        for (const auto& item : InventoryCatalog::Knives) {
            if (item.Definition == definition)
                return &item;
        }
        return nullptr;
    }

    bool WriteViewDisplayName(uintptr_t view, std::uint16_t definition,
        int paintKit) noexcept {
        if (!view || !g_ViewOffsets.CustomNameOverride)
            return false;
        std::array<char, 161> displayName{};
        const auto skin = InventorySkinCatalog::Find(definition, paintKit);
        const char* source = skin ? skin->FullName : nullptr;
        if (!source) {
            const auto knife = FindKnifeCatalogItem(definition);
            source = knife ? knife->Name : nullptr;
        }
        if (source) {
            const auto length = (std::min)(std::strlen(source), displayName.size() - 1);
            std::memcpy(displayName.data(), source, length);
        }
        return Memory::Write(
            view + g_ViewOffsets.CustomNameOverride, displayName);
    }

    bool ApplyKnifeModel(uintptr_t pawn, uintptr_t weapon,
        std::uint16_t definition, int paintKit, bool definitionChanged) noexcept {
        const auto catalogItem = FindKnifeCatalogItem(definition);
        if (!catalogItem || !catalogItem->Model || !*catalogItem->Model)
            return false;

        const auto skin = InventorySkinCatalog::Find(definition, paintKit);
        const std::uint64_t meshGroupMask = skin && skin->LegacyModel ? 2 : 1;
        const bool newWorldModel = definitionChanged ||
            g_AppliedKnifeModel.Weapon != weapon ||
            g_AppliedKnifeModel.Definition != definition ||
            !g_AppliedKnifeModel.WorldReady;
        bool worldReady = g_AppliedKnifeModel.WorldReady && !newWorldModel;
        if (newWorldModel) {
            worldReady = SafeSetModel(g_Runtime.SetModel, weapon, catalogItem->Model);
            if (worldReady)
                SetMeshGroup(weapon, meshGroupMask);
        }

        const uintptr_t hudWeapon = ResolveHudWeapon(pawn, weapon);
        if (hudWeapon && (newWorldModel || g_AppliedKnifeModel.HudWeapon != hudWeapon)) {
            if (SafeSetModel(g_Runtime.SetModel, hudWeapon, catalogItem->Model))
                SetMeshGroup(hudWeapon, meshGroupMask);
        }

        g_AppliedKnifeModel = { weapon, hudWeapon, definition, worldReady };
        return worldReady;
    }

    bool ResolveInventory(uintptr_t& manager, uintptr_t& inventory, SOID& owner) noexcept {
        manager = g_Runtime.Manager;
        inventory = 0;
        owner = {};
        uintptr_t vtable{};
        if (!manager || !Memory::Read(manager, vtable) || !vtable ||
            !Memory::Read(manager + g_Runtime.LocalInventoryOffset, inventory) || !inventory ||
            !Memory::Read(inventory, vtable) || !vtable ||
            !Memory::Read(inventory + kInventoryOwner, owner) || !owner.Id)
            return false;
        return true;
    }

    uintptr_t ResolveItemTypeCache(uintptr_t inventory) noexcept {
        uintptr_t sharedObjectCache{};
        if (!inventory || !Memory::Read(
            inventory + g_Runtime.SharedObjectCacheOffset, sharedObjectCache) ||
            !sharedObjectCache)
            return 0;
        const auto typeCache = SafeCreateBaseTypeCache(
            g_Runtime.CreateBaseTypeCache, sharedObjectCache);
        if (!typeCache || !VtableFunction(typeCache, 1) || !VtableFunction(typeCache, 3))
            return 0;
        return typeCache;
    }

    bool SetDynamicAttribute(uintptr_t item, int index, const void* value) noexcept {
        const uintptr_t system = SafeGetSystem(g_Runtime.GetEconItemSystem);
        uintptr_t schema{};
        if (!system) {
            g_LastAttributeFailure = "CEconItemSystem getter returned null";
            return false;
        }
        if (!Memory::Read(system + sizeof(uintptr_t), schema) || !schema) {
            g_LastAttributeFailure = "CEconItemSchema pointer is unavailable";
            return false;
        }
        const uintptr_t definition = SafeGetAttributeDefinition(
            schema, g_Runtime.GetAttributeDefinition, index);
        if (!definition) {
            g_LastAttributeFailure = "attribute definition lookup failed";
            return false;
        }
        if (!SafeSetAttribute(g_Runtime.SetDynamicAttribute, item, definition, value)) {
            g_LastAttributeFailure = "CEconItem attribute setter raised an exception";
            return false;
        }
        g_LastAttributeFailure = "none";
        return true;
    }

    bool ApplyAttributes(uintptr_t item, const InventoryChanger::ItemRequest& request) noexcept {
        if (request.PaintKit > 0) {
            const float paint = static_cast<float>(request.PaintKit);
            const float seed = static_cast<float>((std::clamp)(request.Seed, 0, 1000));
            const float wear = (std::clamp)(request.Wear, 0.0001f, 1.0f);
            if (!SetDynamicAttribute(item, 6, &paint) ||
                !SetDynamicAttribute(item, 7, &seed) ||
                !SetDynamicAttribute(item, 8, &wear))
                return false;
        }
        if (request.StatTrak >= 0) {
            const int score = request.StatTrak;
            const int type = 0;
            if (!SetDynamicAttribute(item, 80, &score) ||
                !SetDynamicAttribute(item, 81, &type))
                return false;
        }
        return true;
    }

    std::uint64_t MakeItemId() noexcept {
        // Keep below 0xF000...: the game reserves that range for default item IDs.
        return 0x0E00000000000000ULL | (g_IdCounter.fetch_add(1) & 0x0000FFFFFFFFFFFFULL);
    }

    bool IsKnifeDefinition(std::uint16_t definition) noexcept {
        return definition == 42 || definition == 59 ||
            (definition >= 500 && definition <= 526);
    }

    bool WriteItemView(uintptr_t view, const OwnedItem& item, std::uint32_t account) noexcept {
        if (!view || !g_ViewOffsets.Loaded)
            return false;
        const auto id = item.Public.ItemId;
        const auto high = static_cast<std::uint32_t>(id >> 32);
        const auto low = static_cast<std::uint32_t>(id);
        constexpr bool initialized = true;
        constexpr bool allowSoc = false;
        constexpr bool restoreCustomMaterial = true;
        return Memory::Write(view + g_ViewOffsets.Definition, item.Public.DefinitionIndex) &&
            Memory::Write(view + g_ViewOffsets.ItemId, id) &&
            Memory::Write(view + g_ViewOffsets.ItemIdHigh, high) &&
            Memory::Write(view + g_ViewOffsets.ItemIdLow, low) &&
            Memory::Write(view + g_ViewOffsets.AccountId, account) &&
            Memory::Write(view + g_ViewOffsets.Initialized, initialized) &&
            Memory::Write(view + g_ViewOffsets.DisallowSoc, allowSoc) &&
            Memory::Write(view + g_ViewOffsets.RestoreCustomMaterial, restoreCustomMaterial);
    }

    std::optional<OwnedItem> FindEquipped(
        InventoryChanger::ItemKind kind, int team, int slot) {
        std::scoped_lock lock(g_Mutex);
        for (auto iterator = g_Items.rbegin(); iterator != g_Items.rend(); ++iterator) {
            if (iterator->Public.Kind != kind || iterator->Public.LoadoutSlot != slot)
                continue;
            if ((team == kTeamT && iterator->Public.EquippedT) ||
                (team == kTeamCT && iterator->Public.EquippedCT))
                return *iterator;
        }
        return std::nullopt;
    }

    // Ищет оснащённый предмет для definition+team.
    // Если equipped не найден — возвращает любой предмет для этого оружия как fallback.
    // Это предотвращает серый скин из-за race condition с ConfirmViews (250мс).
    std::optional<OwnedItem> FindWeaponItem(std::uint16_t definition, int team) {
        std::scoped_lock lock(g_Mutex);
        std::optional<OwnedItem> fallback;
        for (auto iterator = g_Items.rbegin(); iterator != g_Items.rend(); ++iterator) {
            if (iterator->Public.Kind != InventoryChanger::ItemKind::Weapon ||
                iterator->Public.DefinitionIndex != definition)
                continue;
            if ((team == kTeamT && iterator->Public.EquippedT) ||
                (team == kTeamCT && iterator->Public.EquippedCT))
                return *iterator;  // Предпочитаем явно экипированный
            if (!fallback)
                fallback = *iterator;  // Fallback: любой предмет этого оружия
        }
        return fallback;
    }

    bool HasWeaponItemForDefinition(std::uint16_t definition) noexcept {
        std::scoped_lock lock(g_Mutex);
        for (const auto& item : g_Items) {
            if (item.Public.Kind == InventoryChanger::ItemKind::Weapon &&
                item.Public.DefinitionIndex == definition)
                return true;
        }
        return false;
    }

    int FindLoadoutSlotByDefinition(uintptr_t inventory, int team,
        std::uint16_t definition) noexcept {
        if (!inventory || (team != kTeamT && team != kTeamCT))
            return -1;
        for (int slot = 0; slot < kLoadoutSlotCount; ++slot) {
            const uintptr_t view = SafeGetItemInLoadout(
                g_Runtime.GetItemInLoadout, inventory, team, slot);
            std::uint16_t currentDefinition{};
            if (view && Memory::Read(
                view + g_ViewOffsets.Definition, currentDefinition) &&
                currentDefinition == definition)
                return slot;
        }
        return -1;
    }

    bool CreatePendingItem(const InventoryChanger::ItemRequest& request) noexcept {
        uintptr_t manager{}, inventory{};
        SOID owner{};
        if (!ResolveInventory(manager, inventory, owner)) {
            std::scoped_lock lock(g_Mutex);
            g_Status = "Local CCSPlayerInventory is not ready";
            return false;
        }

        const uintptr_t object = SafeCreate(g_Runtime.CreateEconItem);
        if (!object || !VtableFunction(object, 1)) {
            std::scoped_lock lock(g_Mutex);
            g_Status = "CEconItem factory validation failed";
            return false;
        }

        const auto itemId = MakeItemId();
        const auto account = static_cast<std::uint32_t>(owner.Id);
        const auto inventoryPosition = 0x40000000u |
            static_cast<std::uint32_t>(itemId & 0x00FFFFFFu);
        const bool initialized =
            Memory::Write(object + kEconItemId, itemId) &&
            Memory::Write(object + kEconOriginalId, itemId) &&
            Memory::Write(object + kEconAccountId, account) &&
            Memory::Write(object + kEconInventory, inventoryPosition) &&
            Memory::Write(object + kEconDefinition, request.DefinitionIndex) &&
            ApplyAttributes(object, request);
        if (!initialized) {
            SafeDestroy(VtableFunction(object, 1), object);
            std::scoped_lock lock(g_Mutex);
            g_Status = std::string("Failed to initialize CEconItem attributes: ") +
                g_LastAttributeFailure;
            return false;
        }

        const uintptr_t typeCache = ResolveItemTypeCache(inventory);
        const uintptr_t addObject = VtableFunction(typeCache, 1);
        if (!typeCache || !addObject || !SafeAddObject(addObject, typeCache, object)) {
            SafeDestroy(VtableFunction(object, 1), object);
            std::scoped_lock lock(g_Mutex);
            g_Status = "Econ shared-object type cache rejected the item";
            return false;
        }

        const uintptr_t soCreated = VtableFunction(inventory, 0);
        if (!soCreated || !SafeNotify(soCreated, inventory, owner, object, kIncrementalEvent)) {
            SafeRemoveObject(VtableFunction(typeCache, 3), typeCache, object);
            SafeDestroy(VtableFunction(object, 1), object);
            std::scoped_lock lock(g_Mutex);
            g_Status = "CCSPlayerInventory::SOCreated failed";
            return false;
        }

        const auto resolveSlot = [&](int team) noexcept {
            return request.LoadoutSlot >= 0 ? request.LoadoutSlot :
                FindLoadoutSlotByDefinition(
                    inventory, team, request.DefinitionIndex);
        };
        const int slotT = (request.Team == 0 || request.Team == kTeamT)
            ? resolveSlot(kTeamT) : -1;
        const int slotCT = (request.Team == 0 || request.Team == kTeamCT)
            ? resolveSlot(kTeamCT) : -1;
        const bool equippedT = slotT >= 0 && SafeEquip(
            g_Runtime.EquipItemInLoadout, manager, kTeamT, slotT, itemId);
        const bool equippedCT = slotCT >= 0 && SafeEquip(
            g_Runtime.EquipItemInLoadout, manager, kTeamCT, slotCT, itemId);
        if (equippedT || equippedCT) {
            const auto soUpdated = VtableFunction(inventory, 1);
            if (soUpdated)
                SafeNotify(soUpdated, inventory, owner, object, kIncrementalEvent);
        }

        OwnedItem created{};
        created.Object = object;
        const int resolvedSlot = slotT >= 0 ? slotT : slotCT;
        created.Public = { itemId, request.DefinitionIndex, request.PaintKit,
            request.Seed, (std::clamp)(request.Wear, 0.0001f, 1.0f), request.StatTrak,
            request.Team, resolvedSlot, request.Kind, equippedT, equippedCT, false };
        {
            std::scoped_lock lock(g_Mutex);
            g_Items.push_back(created);
            if (equippedT || equippedCT)
                g_Status = "Item created in CCSPlayerInventory and equipped";
            else if (request.LoadoutSlot < 0)
                g_Status = "Item created; matching loadout slot was not found (runtime preview remains active)";
            else
                g_Status = "Item created in CCSPlayerInventory; equip was rejected";
        }
        DebugLog::Write("[InventoryChanger] local CEconItem created");
        return true;
    }

    void ConfirmViews(uintptr_t inventory) noexcept {
        std::scoped_lock lock(g_Mutex);
        
        for (auto& item : g_Items) {
            item.Public.EquippedT = false;
            item.Public.EquippedCT = false;
        }

        uintptr_t cachedViewT[64]{};
        uintptr_t cachedViewCT[64]{};
        bool fetchedT[64]{};
        bool fetchedCT[64]{};

        for (auto& item : g_Items) {
            if (item.Public.LoadoutSlot < 0 || item.Public.LoadoutSlot >= 64) continue;
            
            const int slot = item.Public.LoadoutSlot;

            if (!fetchedT[slot]) {
                cachedViewT[slot] = SafeGetItemInLoadout(g_Runtime.GetItemInLoadout, inventory, kTeamT, slot);
                fetchedT[slot] = true;
            }
            if (!fetchedCT[slot]) {
                cachedViewCT[slot] = SafeGetItemInLoadout(g_Runtime.GetItemInLoadout, inventory, kTeamCT, slot);
                fetchedCT[slot] = true;
            }

            if (cachedViewT[slot]) {
                std::uint64_t currentIdT{};
                if (Memory::Read(cachedViewT[slot] + g_ViewOffsets.ItemId, currentIdT) && currentIdT == item.Public.ItemId) {
                    item.Public.EquippedT = true;
                }
            }

            if (cachedViewCT[slot]) {
                std::uint64_t currentIdCT{};
                if (Memory::Read(cachedViewCT[slot] + g_ViewOffsets.ItemId, currentIdCT) && currentIdCT == item.Public.ItemId) {
                    item.Public.EquippedCT = true;
                }
            }

            item.Public.ViewConfirmed = item.Public.EquippedT || item.Public.EquippedCT;
        }
    }

    void ApplyGloveOverride() noexcept {
        if (!CEntities::Client || !g_ViewOffsets.Loaded)
            return;
        uintptr_t pawn{};
        if (!Memory::Read(CEntities::Client + g_ViewOffsets.LocalPawn, pawn) || !pawn)
            return;
        std::uint8_t team{};
        if (!Memory::Read(pawn + g_ViewOffsets.Team, team))
            return;
        const auto glove = FindEquipped(InventoryChanger::ItemKind::Glove, team, 41);
        if (!glove)
            return;
        const uintptr_t view = pawn + g_ViewOffsets.EconGloves;
        float spawnTime{};
        std::uint64_t currentItemId{};
        std::uint16_t currentDefinition{};
        if (!Memory::Read(pawn + g_ViewOffsets.LastSpawnTime, spawnTime) ||
            !Memory::Read(view + g_ViewOffsets.ItemId, currentItemId) ||
            !Memory::Read(view + g_ViewOffsets.Definition, currentDefinition))
            return;
        const bool needsInitialization =
            g_AppliedGloveState.Pawn != pawn ||
            g_AppliedGloveState.ItemId != glove->Public.ItemId ||
            g_AppliedGloveState.SpawnTime != spawnTime ||
            currentItemId != glove->Public.ItemId ||
            currentDefinition != glove->Public.DefinitionIndex;
        if (needsInitialization) {
            uintptr_t manager{}, inventory{};
            SOID owner{};
            if (!ResolveInventory(manager, inventory, owner))
                return;
            constexpr bool notInitialized = false;
            constexpr int unusualQuality = 3;
            if (!Memory::Write(view + g_ViewOffsets.Initialized, notInitialized) ||
                !WriteItemView(view, *glove, static_cast<std::uint32_t>(owner.Id)) ||
                !Memory::Write(view + g_ViewOffsets.EntityQuality, unusualQuality) ||
                !WriteViewDisplayName(view, glove->Public.DefinitionIndex,
                    glove->Public.PaintKit) ||
                !ApplyViewPaintAttributes(view, glove->Public.PaintKit,
                    (std::clamp)(glove->Public.Wear, 0.0001f, 1.0f),
                    (std::clamp)(glove->Public.Seed, 0, 1000))) {
                std::scoped_lock lock(g_Mutex);
                g_Status = "Failed to initialize m_EconGloves item view";
                return;
            }
            g_AppliedGloveState = { pawn, glove->Public.ItemId, spawnTime, 3 };
            DebugLog::Write(
                "[InventoryChanger] glove item view initialized; bodygroup refresh queued");
        }

        if (g_AppliedGloveState.UpdateFrames > 0) {
            constexpr bool initialized = true;
            constexpr bool reapply = true;
            const bool refreshed =
                Memory::Write(view + g_ViewOffsets.Initialized, initialized) &&
                SafeRefreshGloveBodyGroup(pawn) &&
                Memory::Write(pawn + g_ViewOffsets.ReapplyGloves, reapply);
            --g_AppliedGloveState.UpdateFrames;
            if (refreshed && g_AppliedGloveState.UpdateFrames == 0) {
                std::scoped_lock lock(g_Mutex);
                g_Status = "Glove item and pawn bodygroup applied";
                DebugLog::Write(
                    "[InventoryChanger] glove bodygroup refresh completed");
            }
            else if (!refreshed && g_AppliedGloveState.UpdateFrames == 0) {
                std::scoped_lock lock(g_Mutex);
                g_Status = "Glove item view is ready, but pawn bodygroup refresh failed";
                DebugLog::Write(
                    "[InventoryChanger] glove bodygroup refresh failed after three frames");
            }
        }
    }
}

bool InventoryChanger::Initialize() noexcept {
    if (g_Runtime.Loaded)
        return true;
    if (!DynamicSchema::Ready() || !LoadViewOffsets()) {
        std::scoped_lock lock(g_Mutex);
        g_Status = "Required Schema fields are missing";
        DebugLog::Write(
            "[InventoryChanger] initialization stopped before ABI scan: schema/view offsets unavailable");
        return false;
    }
    const auto module = GetModuleHandleA("client.dll");
    const auto text = GetTextSection(module);
    if (!text) {
        std::scoped_lock lock(g_Mutex);
        g_Status = "client.dll .text is unavailable";
        return false;
    }

    const uintptr_t managerGetter = FindUnique(*text,
        "48 8D 05 ? ? ? ? C3 CC CC CC CC CC CC CC CC 0F B6 81 6B");
    const uintptr_t inventoryAccessor = FindUnique(*text,
        "48 8B 81 ? ? ? ? C3 CC CC CC CC CC CC CC CC 45 33 C0 3B 91 10 02 00 00");
    const uintptr_t sharedCacheAccessor = FindUnique(*text,
        "48 8B 56 ? 48 85 D2 74 ? 48 8B 52 08 EB ? "
        "48 C7 C2 FF FF FF FF 48 8D 0D");
    const uintptr_t createTypeCacheCall = FindUnique(*text,
        "E8 ? ? ? ? 41 8B D5 49 8B CD");
    RuntimeFunctions found{};
    found.CreateEconItem = FindUnique(*text,
        "48 83 EC 28 B9 48 00 00 00 E8 ? ? ? ? 48 85 C0 74 ? "
        "48 8D 0D ? ? ? ? C7 40 32 00 00 FF 00 48 89 08 "
        "48 8D 0D ? ? ? ? 48 89 48 08 33 C9 48 89 48 20 "
        "48 89 48 10 48 89 48 18 48 89 48 38 48 89 48 28 "
        "66 89 48 30 88 48 40");
    // The short form is intentional: the branch displacement distinguishes the
    // real Econ singleton from the many generated singleton constructors nearby.
    found.GetEconItemSystem = FindUnique(*text,
        "48 83 EC 28 48 8B 05 ? ? ? ? 48 85 C0 0F 85 81 00 00 00 "
        "48 89 5C 24 30 B9 10 00 00 00 48 89 7C 24 20");
    const uintptr_t attributeDefinitionCall = FindUnique(*text,
        "E8 ? ? ? ? 48 85 C0 74 ? E8 ? ? ? ? 0F B7 14 3B "
        "48 8B C8 E8 ? ? ? ? 0F B6 48");
    found.GetAttributeDefinition = ResolveRelativeTarget(
        attributeDefinitionCall, 1, 5);
    const uintptr_t attributeThunk = FindUnique(*text,
        "E9 ? ? ? ? CC CC CC CC CC CC CC CC CC CC CC CC CC CC CC "
        "49 8B C0 48 8B CA 48 8B D0");
    std::int32_t attributeDisplacement{};
    if (attributeThunk)
        std::memcpy(&attributeDisplacement,
            reinterpret_cast<const void*>(attributeThunk + 1), sizeof(attributeDisplacement));
    found.SetDynamicAttribute = attributeThunk
        ? attributeThunk + 5 + attributeDisplacement : 0;
    found.EquipItemInLoadout = FindUnique(*text,
        "48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 89 54 24 ? 57 41 54 41 55 41 56 41 57 48 83 EC ? 0F B7 FA");
    found.GetItemInLoadout = FindUnique(*text,
        "40 55 48 83 EC ? 49 63 E8 83 FD 38 0F 87 ? ? ? ? 83 FA 03");
    const uintptr_t setViewAttributeCall = FindUnique(*text,
        "E8 ? ? ? ? 66 41 0F 6E D4");
    found.SetAttributeValueByName = ResolveRelativeTarget(
        setViewAttributeCall, 1, 5);
    found.SetModel = FindUnique(*text,
        "40 53 48 83 EC ? 48 8B D9 4C 8B C2 48 8B 0D ? ? ? ? "
        "48 8D 54 24 40");
    found.SetMeshGroupMask = FindUnique(*text,
        "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ? "
        "48 8D 99 ? ? ? ? 48 8B 71");
    found.UpdateSubclass = FindUnique(*text,
        "4C 8B DC 53 48 81 EC ? ? ? ? 48 8B 41");
    // Current client.dll emits two semantically identical UpdateSkin thunks. Both call
    // the same material refresh pipeline, so the first full-pattern match is
    // deterministic and avoids weakening the signature itself.
    found.UpdateSkin = FindFirst(*text,
        "48 89 5C 24 08 57 48 83 EC 20 8B DA 48 8B F9 "
        "E8 ? ? ? ? F6 C3 01 74 0A 33 D2 48 8B CF E8 ? ? ? ? "
        "48 8D 8F ? ? ? ?");
    const uintptr_t updateCompositeMaterialCall = FindUnique(*text,
        "E8 ? ? ? ? 48 8D 8B ? ? ? ? 48 89 BC 24");
    found.UpdateCompositeMaterial = ResolveRelativeTarget(
        updateCompositeMaterialCall, 1, 5);
    found.UpdateCompositeMaterialSet = FindUnique(*text,
        "40 55 53 41 57 48 8D AC 24 00 FE ? ?");
    // LEA encodes this member displacement as a signed 32-bit immediate.
    // Reading directly into ptrdiff_t would consume the following four opcode
    // bytes too and turn the valid 0x1990 offset into a bogus 64-bit value.
    std::int32_t compositeMaterialOffset{};
    if (found.UpdateSkin &&
        Memory::Read(found.UpdateSkin + 38, compositeMaterialOffset))
        found.CompositeMaterialOffset = compositeMaterialOffset;

    const uintptr_t bodyGroupSequence = FindUnique(*text,
        "33 D2 48 8B CF E8 ? ? ? ? 33 D2 48 8B CF 8D 58 FF "
        "E8 ? ? ? ? 48 8B D0 44 8B C3 48 8B CF E8 ? ? ? ? EB 0C");
    found.GetBodyGroupCount = ResolveRelativeTarget(bodyGroupSequence + 5, 1, 5);
    found.GetBodyGroupName = ResolveRelativeTarget(bodyGroupSequence + 18, 1, 5);
    found.SetBodyGroup = ResolveRelativeTarget(bodyGroupSequence + 32, 1, 5);
    const uintptr_t updateBodyGroupChoiceCall = FindUnique(*text,
        "E8 ? ? ? ? 48 8B 9C 24 ? ? ? ? 4C 8B B4 24 ? ? ? ? 48 83 C4");
    found.UpdateBodyGroupChoice = ResolveRelativeTarget(
        updateBodyGroupChoiceCall, 1, 5);
    found.FindHudElement = FindUnique(*text,
        "40 53 48 83 EC 20 48 8B 05 ? ? ? ? 48 8B D9 48 85 C0 74 ? "
        "48 89 5C 24 ? 48 8D 88 58 02 00 00");
    const uintptr_t clearHudIconCall = FindUnique(*text,
        "E8 ? ? ? ? 8B F8 C6 84 24");
    found.ClearHudWeaponIcon = ResolveRelativeTarget(clearHudIconCall, 1, 5);

    std::int32_t displacement{};
    std::int32_t inventoryOffset{};
    std::int32_t createTypeCacheDisplacement{};
    if (managerGetter)
        std::memcpy(&displacement, reinterpret_cast<const void*>(managerGetter + 3), sizeof(displacement));
    if (inventoryAccessor)
        std::memcpy(&inventoryOffset, reinterpret_cast<const void*>(inventoryAccessor + 3), sizeof(inventoryOffset));
    if (createTypeCacheCall)
        std::memcpy(&createTypeCacheDisplacement,
            reinterpret_cast<const void*>(createTypeCacheCall + 1),
            sizeof(createTypeCacheDisplacement));
    found.Manager = managerGetter ? managerGetter + 7 + displacement : 0;
    found.LocalInventoryOffset = inventoryOffset;
    found.SharedObjectCacheOffset = sharedCacheAccessor
        ? *reinterpret_cast<const std::uint8_t*>(sharedCacheAccessor + 3) : 0;
    found.CreateBaseTypeCache = createTypeCacheCall
        ? createTypeCacheCall + 5 + createTypeCacheDisplacement : 0;

    if (!found.Manager || inventoryOffset < 0x1000 || inventoryOffset > 0x100000 ||
        found.SharedObjectCacheOffset < 0x20 || found.SharedObjectCacheOffset > 0x200 ||
        !found.CreateEconItem || !found.CreateBaseTypeCache || !found.GetEconItemSystem ||
        !found.GetAttributeDefinition || !found.SetDynamicAttribute ||
        !found.SetAttributeValueByName || !found.EquipItemInLoadout ||
        !found.GetItemInLoadout || !found.SetModel || !found.SetMeshGroupMask ||
        !found.UpdateSubclass || !found.UpdateSkin ||
        !found.UpdateCompositeMaterial || !found.UpdateCompositeMaterialSet ||
        found.CompositeMaterialOffset < 0x1000 ||
        found.CompositeMaterialOffset > 0x3000 ||
        !found.GetBodyGroupCount || !found.GetBodyGroupName ||
        !found.SetBodyGroup || !found.UpdateBodyGroupChoice) {
        std::ostringstream diagnostics;
        diagnostics << "[InventoryChanger] missing ABI:";
        const auto missing = [&](bool unavailable, const char* name) {
            if (unavailable)
                diagnostics << ' ' << name;
        };
        missing(!found.Manager, "manager");
        missing(inventoryOffset < 0x1000 || inventoryOffset > 0x100000,
            "inventoryOffset");
        missing(found.SharedObjectCacheOffset < 0x20 ||
            found.SharedObjectCacheOffset > 0x200, "sharedCacheOffset");
        missing(!found.CreateEconItem, "createItem");
        missing(!found.CreateBaseTypeCache, "createTypeCache");
        missing(!found.GetEconItemSystem, "itemSystem");
        missing(!found.GetAttributeDefinition, "attributeDefinition");
        missing(!found.SetDynamicAttribute, "dynamicAttribute");
        missing(!found.SetAttributeValueByName, "viewAttribute");
        missing(!found.EquipItemInLoadout, "equip");
        missing(!found.GetItemInLoadout, "getLoadout");
        missing(!found.SetModel, "setModel");
        missing(!found.SetMeshGroupMask, "meshGroup");
        missing(!found.UpdateSubclass, "subclass");
        missing(!found.UpdateSkin, "skin");
        missing(!found.UpdateCompositeMaterial, "composite");
        missing(!found.UpdateCompositeMaterialSet, "compositeSet");
        missing(found.CompositeMaterialOffset < 0x1000 ||
            found.CompositeMaterialOffset > 0x3000, "compositeOffset");
        missing(!found.GetBodyGroupCount, "bodyGroupCount");
        missing(!found.GetBodyGroupName, "bodyGroupName");
        missing(!found.SetBodyGroup, "setBodyGroup");
        missing(!found.UpdateBodyGroupChoice, "bodyGroupChoice");
        const auto diagnosticText = diagnostics.str();
        DebugLog::Write(diagnosticText.c_str());
        std::scoped_lock lock(g_Mutex);
        g_Status = "Inventory ABI signatures are missing or ambiguous";
        return false;
    }
    found.Loaded = true;
    g_Runtime = found;
    {
        std::scoped_lock lock(g_Mutex);
        g_Status = "CCSPlayerInventory backend ready";
    }
    DebugLog::Write("[InventoryChanger] runtime signatures validated");
    {
        std::ostringstream message;
        message << "[InventoryChanger] composite owner offset=0x" << std::hex
            << found.CompositeMaterialOffset
            << ", glove bodygroup pipeline ready";
        const auto text = message.str();
        DebugLog::Write(text.c_str());
    }
    return true;
}

bool InventoryChanger::Ready() noexcept {
    return g_Runtime.Loaded;
}

std::string InventoryChanger::Status() {
    std::scoped_lock lock(g_Mutex);
    return g_Status;
}

bool InventoryChanger::QueueCreate(const ItemRequest& request) {
    if (!Ready() || !request.DefinitionIndex || request.Team < 0 ||
        (request.Team != 0 && request.Team != kTeamT && request.Team != kTeamCT) ||
        request.LoadoutSlot < -1 || request.LoadoutSlot >= kLoadoutSlotCount ||
        (request.LoadoutSlot < 0 && request.Kind != ItemKind::Weapon) ||
        !std::isfinite(request.Wear) || request.Wear < 0.0f || request.Wear > 1.0f)
        return false;
    std::scoped_lock lock(g_Mutex);
    if (g_Pending || g_Items.size() >= kMaxInjectedItems)
        return false;
    g_Pending = request;
    g_Status = "Item creation queued";
    return true;
}

bool InventoryChanger::Remove(std::uint64_t itemId) noexcept {
    uintptr_t manager{}, inventory{};
    SOID owner{};
    if (!ResolveInventory(manager, inventory, owner))
        return false;
    OwnedItem removed{};
    {
        std::scoped_lock lock(g_Mutex);
        const auto found = std::find_if(g_Items.begin(), g_Items.end(),
            [itemId](const OwnedItem& item) { return item.Public.ItemId == itemId; });
        if (found == g_Items.end())
            return false;
        removed = *found;
    }
    const uintptr_t typeCache = ResolveItemTypeCache(inventory);
    const uintptr_t removeObject = VtableFunction(typeCache, 3);
    if (!typeCache || !removeObject) {
        std::scoped_lock lock(g_Mutex);
        g_Status = "Econ shared-object type cache is unavailable";
        return false;
    }
    const auto destroyed = VtableFunction(inventory, 2);
    const bool notified = destroyed && SafeNotify(
        destroyed, inventory, owner, removed.Object, kIncrementalEvent);
    if (!notified) {
        std::scoped_lock lock(g_Mutex);
        g_Status = "CCSPlayerInventory::SODestroyed failed";
        return false;
    }
    if (!SafeRemoveObject(removeObject, typeCache, removed.Object)) {
        std::scoped_lock lock(g_Mutex);
        g_Status = "Failed to remove item from Econ shared-object type cache";
        return false;
    }
    const auto destructor = VtableFunction(removed.Object, 1);
    if (destructor)
        SafeDestroy(destructor, removed.Object);
    {
        std::scoped_lock lock(g_Mutex);
        const auto found = std::find_if(g_Items.begin(), g_Items.end(),
            [itemId](const OwnedItem& item) { return item.Public.ItemId == itemId; });
        if (found != g_Items.end())
            g_Items.erase(found);
        g_Status = "Local item removed";
    }
    return true;
}

std::vector<InventoryChanger::ItemRecord> InventoryChanger::Snapshot() {
    std::scoped_lock lock(g_Mutex);
    std::vector<ItemRecord> result;
    result.reserve(g_Items.size());
    for (const auto& item : g_Items)
        result.push_back(item.Public);
    return result;
}

void InventoryChanger::Update() noexcept {
    // Initialization is attempted once by the startup thread. A failed pattern
    // scan cannot recover while the same client image remains loaded, and
    // rescanning it from Present would reduce the game to a few FPS.
    if (!Ready())
        return;
    std::optional<ItemRequest> pending;
    {
        std::scoped_lock lock(g_Mutex);
        if (g_Pending) {
            pending = g_Pending;
            g_Pending.reset();
        }
    }
    bool itemCreated{};
    if (pending)
        itemCreated = CreatePendingItem(*pending);

    // Confirming a newly inserted item crosses several game interfaces. It is
    // useful immediately after creation, but doing it on every render frame is
    // unnecessarily expensive while the loadout view is still being rebuilt.
    static ULONGLONG lastViewConfirmation{};
    const ULONGLONG now = GetTickCount64();
    if (itemCreated || now - lastViewConfirmation >= 250) {
        uintptr_t manager{}, inventory{};
        SOID owner{};
        if (ResolveInventory(manager, inventory, owner))
            ConfirmViews(inventory);
        lastViewConfirmation = now;
    }
    ApplyGloveOverride();
}

bool InventoryChanger::ApplyKnifeOverride(uintptr_t pawn, uintptr_t weapon) noexcept {
    if (!Ready() || !g_ViewOffsets.Loaded || !pawn || !weapon)
        return false;
    std::uint8_t team{};
    if (!Memory::Read(pawn + g_ViewOffsets.Team, team))
        return false;
    const auto knife = FindEquipped(ItemKind::Knife, team, 0);
    if (!knife)
        return false;
    const uintptr_t view = weapon + g_ViewOffsets.AttributeManager + g_ViewOffsets.Item;
    std::uint16_t currentDefinition{};
    if (!Memory::Read(view + g_ViewOffsets.Definition, currentDefinition) ||
        !IsKnifeDefinition(currentDefinition))
        return false;

    const int paint = knife->Public.PaintKit;
    const int seed = knife->Public.Seed;
    const float wear = knife->Public.Wear;
    const int statTrak = knife->Public.StatTrak;
    const auto definitionText = std::to_string(knife->Public.DefinitionIndex);
    const std::uint32_t wantedSubclass = StringTokenHash(definitionText);
    std::uint32_t currentSubclass{};
    std::uint64_t currentItemId{};
    int currentPaint{};
    if (!wantedSubclass ||
        !Memory::Read(weapon + g_ViewOffsets.SubclassId, currentSubclass) ||
        !Memory::Read(view + g_ViewOffsets.ItemId, currentItemId) ||
        !Memory::Read(weapon + g_ViewOffsets.FallbackPaint, currentPaint))
        return false;

    const bool definitionChanged = currentDefinition != knife->Public.DefinitionIndex;
    const bool subclassChanged = currentSubclass != wantedSubclass;
    const bool renderTargetChanged =
        g_AppliedKnifeModel.Weapon != weapon ||
        g_AppliedKnifeModel.Definition != knife->Public.DefinitionIndex ||
        g_LastAppliedKnifeItemId != knife->Public.ItemId;
    const bool viewChanged = currentItemId != knife->Public.ItemId ||
        currentPaint != paint;
    const bool refreshSkin = definitionChanged || subclassChanged ||
        renderTargetChanged || viewChanged;
    if (!refreshSkin)
        return ApplyKnifeModel(
            pawn, weapon, knife->Public.DefinitionIndex, paint, false);

    uintptr_t manager{}, inventory{};
    SOID owner{};
    if (!ResolveInventory(manager, inventory, owner) ||
        !WriteItemView(view, *knife, static_cast<std::uint32_t>(owner.Id)))
        return false;
    const bool attributesWritten = WriteViewDisplayName(
            view, knife->Public.DefinitionIndex, paint) &&
        Memory::Write(weapon + g_ViewOffsets.FallbackPaint, paint) &&
        Memory::Write(weapon + g_ViewOffsets.FallbackSeed, seed) &&
        Memory::Write(weapon + g_ViewOffsets.FallbackWear, wear) &&
        Memory::Write(weapon + g_ViewOffsets.FallbackStatTrak, statTrak);
    if (!attributesWritten)
        return false;
    bool viewAttributesWritten = ApplyViewPaintAttributes(view, paint, wear, seed);
    bool modelApplied = ApplyKnifeModel(pawn, weapon,
        knife->Public.DefinitionIndex, paint, refreshSkin);
    bool subclassApplied = true;
    if (subclassChanged) {
        subclassApplied = Memory::Write(
            weapon + g_ViewOffsets.SubclassId, wantedSubclass) &&
            SafeUpdateSubclass(g_Runtime.UpdateSubclass, weapon);
        if (!subclassApplied)
            Memory::Write(weapon + g_ViewOffsets.SubclassId, currentSubclass);
        else {
            // UpdateSubclass may rebuild the render state; restore the selected
            // model/mesh on the same frame and resolve the HUD child again.
            modelApplied = ApplyKnifeModel(
                pawn, weapon, knife->Public.DefinitionIndex, paint, true);
            viewAttributesWritten = viewAttributesWritten &&
                ApplyViewPaintAttributes(view, paint, wear, seed);
        }
    }
    if (!subclassApplied) {
        std::scoped_lock lock(g_Mutex);
        g_Status = "Failed to update knife subclass";
        return false;
    }
    bool skinApplied = true;
    if (modelApplied && refreshSkin) {
        skinApplied = SafeUpdateSkin(g_Runtime.UpdateSkin, weapon);
        if (skinApplied) {
            g_LastAppliedKnifeItemId = knife->Public.ItemId;
            InvalidateHudDescription(view);
        }
    }
    if (!skinApplied) {
        std::scoped_lock lock(g_Mutex);
        g_Status = "Failed to rebuild knife composite material";
        return false;
    }
    if (modelApplied && subclassApplied && skinApplied && refreshSkin) {
        std::scoped_lock lock(g_Mutex);
        g_Status = "Knife item, model, skin and HUD view applied";
        std::ostringstream message;
        message << "[InventoryChanger] knife subclass 0x" << std::hex
            << currentSubclass << " -> 0x" << wantedSubclass
            << ", model=" << FindKnifeCatalogItem(knife->Public.DefinitionIndex)->Model
            << ", skin and HUD refresh applied";
        const auto text = message.str();
        DebugLog::Write(text.c_str());
    }
    return modelApplied && subclassApplied && skinApplied && viewAttributesWritten;
}

bool InventoryChanger::ApplyWeaponOverride(uintptr_t pawn, uintptr_t weapon) noexcept {
    static uintptr_t s_lastWeapon = 0;
    const bool weaponSwitched = (weapon != s_lastWeapon);
    s_lastWeapon = weapon;

    if (!Ready() || !g_ViewOffsets.Loaded || !pawn || !weapon)
        return false;
    std::uint8_t team{};
    if (!Memory::Read(pawn + g_ViewOffsets.Team, team))
        return false;

    const uintptr_t view = weapon + g_ViewOffsets.AttributeManager + g_ViewOffsets.Item;
    std::uint16_t definition{};
    if (!Memory::Read(view + g_ViewOffsets.Definition, definition) ||
        IsKnifeDefinition(definition))
        return false;
    
    const auto item = FindWeaponItem(definition, team);
    if (!item) {
        return false; 
    }

    const int paintKit = (std::max)(0, item->Public.PaintKit);
    const int seed = (std::clamp)(item->Public.Seed, 0, 1000);
    const float wear = (std::clamp)(item->Public.Wear, 0.0001f, 1.0f);
    const int statTrak = item->Public.StatTrak;

    auto& cached = GetOrCreateWeaponSkinEntry(weapon);
    const bool attributesChanged = (cached.PaintKit != paintKit || cached.Seed != seed || cached.Wear != wear || cached.StatTrak != statTrak);

    if (weaponSwitched || attributesChanged) {
        const auto catalog = InventorySkinCatalog::Find(definition, paintKit);
        const std::uint64_t meshGroupMask = catalog && catalog->LegacyModel ? 2 : 1;

        if (attributesChanged) {
            cached.PaintKit = paintKit;
            cached.Seed = seed;
            cached.Wear = wear;
            cached.StatTrak = statTrak;

            const auto definitionText = std::to_string(definition);
            const std::uint32_t wantedSubclass = StringTokenHash(definitionText);
            std::uint32_t currentSubclass = 0;
            if (Memory::Read(weapon + g_ViewOffsets.SubclassId, currentSubclass) && wantedSubclass && currentSubclass != wantedSubclass) {
                if (Memory::Write(weapon + g_ViewOffsets.SubclassId, wantedSubclass)) {
                    SafeUpdateSubclass(g_Runtime.UpdateSubclass, weapon);
                }
            }

            // Медленный поиск имени для UI - только при изменении настроек скина
            WriteViewDisplayName(view, definition, paintKit);
        }

        // WriteItemView быстрый, нужен каждый раз при смене, т.к. CS2 может стереть AccountId/ItemId
        uintptr_t mgr{}, inv{};
        SOID own{};
        if (ResolveInventory(mgr, inv, own)) {
            WriteItemView(view, *item, static_cast<std::uint32_t>(own.Id));
        }
        
        Memory::Write(view + g_ViewOffsets.ItemIdHigh, 0xFFFFFFFFu);
        Memory::Write(view + g_ViewOffsets.ItemIdLow, 0xFFFFFFFFu);

        Memory::Write(weapon + g_ViewOffsets.FallbackPaint, paintKit);
        Memory::Write(weapon + g_ViewOffsets.FallbackSeed, seed);
        Memory::Write(weapon + g_ViewOffsets.FallbackWear, wear);
        Memory::Write(weapon + g_ViewOffsets.FallbackStatTrak, statTrak);
        
        ApplyViewPaintAttributes(view, paintKit, wear, seed);
        
        SetMeshGroup(weapon, meshGroupMask);
        const uintptr_t refreshedHudWeapon = ResolveHudWeapon(pawn, weapon);
        if (refreshedHudWeapon)
            SetMeshGroup(refreshedHudWeapon, meshGroupMask);

        // Движок мог уничтожить CompositeMaterial пока оружие было в кобуре, 
        // обязательно обновляем его при каждом свапе на пушку!
        const uintptr_t compositeOwner = weapon + static_cast<uintptr_t>(g_Runtime.CompositeMaterialOffset);
        SafeUpdateCompositeMaterial(g_Runtime.UpdateCompositeMaterial, compositeOwner);
        SafeUpdateCompositeMaterialSet(g_Runtime.UpdateCompositeMaterialSet, weapon);
        SafeUpdateSkin(g_Runtime.UpdateSkin, weapon);
        
        if (attributesChanged) {
            InvalidateHudDescription(view);
            std::ostringstream message;
            message << "[InventoryChanger] weapon def=" << definition << " paint=" << paintKit << " applied";
            DebugLog::Write(message.str().c_str());
        }
    }

    // ЛЕГКИЙ ПУТЬ (каждый кадр). Только быстрые записи в память.
    Memory::Write(view + g_ViewOffsets.ItemIdHigh, 0xFFFFFFFFu);
    Memory::Write(view + g_ViewOffsets.ItemIdLow, 0xFFFFFFFFu);
    Memory::Write(weapon + g_ViewOffsets.FallbackPaint, paintKit);
    Memory::Write(weapon + g_ViewOffsets.FallbackSeed, seed);
    Memory::Write(weapon + g_ViewOffsets.FallbackWear, wear);
    Memory::Write(weapon + g_ViewOffsets.FallbackStatTrak, statTrak);
    
    return true;
}


bool InventoryChanger::ApplyWeaponPaintOverride(uintptr_t pawn, uintptr_t weapon,
    int paintKit, int seed, float wear, int statTrak, bool legacyModel) noexcept {
    if (!Ready() || !g_ViewOffsets.Loaded || !pawn || !weapon || paintKit <= 0 ||
        !std::isfinite(wear))
        return false;
    paintKit = (std::max)(0, paintKit);
    seed = (std::clamp)(seed, 0, 1000);
    wear = (std::clamp)(wear, 0.0001f, 1.0f);
    const uintptr_t view = weapon + g_ViewOffsets.AttributeManager + g_ViewOffsets.Item;
    std::uint16_t definition{};
    if (!Memory::Read(view + g_ViewOffsets.Definition, definition))
        return false;
    const bool attributesWritten = WriteViewDisplayName(view, definition, paintKit) &&
        Memory::Write(weapon + g_ViewOffsets.FallbackPaint, paintKit) &&
        Memory::Write(weapon + g_ViewOffsets.FallbackSeed, seed) &&
        Memory::Write(weapon + g_ViewOffsets.FallbackWear, wear) &&
        Memory::Write(weapon + g_ViewOffsets.FallbackStatTrak, statTrak) &&
        ApplyViewPaintAttributes(view, paintKit, wear, seed);
    if (!attributesWritten)
        return false;

    const std::uint64_t meshGroupMask = legacyModel ? 2 : 1;
    SetMeshGroup(weapon, meshGroupMask);
    const uintptr_t hudWeapon = ResolveHudWeapon(pawn, weapon);
    if (hudWeapon)
        SetMeshGroup(hudWeapon, meshGroupMask);
    if (!SafeUpdateSkin(g_Runtime.UpdateSkin, weapon))
        return false;
    InvalidateHudDescription(view);
    return true;
}

void InventoryChanger::Shutdown() noexcept {
    std::vector<std::uint64_t> ids;
    {
        std::scoped_lock lock(g_Mutex);
        ids.reserve(g_Items.size());
        for (const auto& item : g_Items)
            ids.push_back(item.Public.ItemId);
    }
    for (const auto id : ids)
        Remove(id);
    g_AppliedKnifeModel = {};
    g_LastAppliedKnifeItemId = 0;
    g_WeaponSkinCache = {};
    g_AppliedGloveState = {};
    std::scoped_lock lock(g_Mutex);
    g_Pending.reset();
    g_Status = "Stopped";
}
