#pragma once

#include <array>
#include <cstdint>

namespace InventoryCatalog {
    struct Item { std::uint16_t Definition; const char* Name; const char* Model; };

    inline constexpr Item Knives[]{
        {0, "Default knife", ""},
        {500, "Bayonet", "weapons/models/knife/knife_bayonet/weapon_knife_bayonet.vmdl"},
        {503, "Classic Knife", "weapons/models/knife/knife_css/weapon_knife_css.vmdl"},
        {505, "Flip Knife", "weapons/models/knife/knife_flip/weapon_knife_flip.vmdl"},
        {506, "Gut Knife", "weapons/models/knife/knife_gut/weapon_knife_gut.vmdl"},
        {507, "Karambit", "weapons/models/knife/knife_karambit/weapon_knife_karambit.vmdl"},
        {508, "M9 Bayonet", "weapons/models/knife/knife_m9/weapon_knife_m9.vmdl"},
        {509, "Huntsman Knife", "weapons/models/knife/knife_tactical/weapon_knife_tactical.vmdl"},
        {512, "Falchion Knife", "weapons/models/knife/knife_falchion/weapon_knife_falchion.vmdl"},
        {514, "Bowie Knife", "weapons/models/knife/knife_bowie/weapon_knife_bowie.vmdl"},
        {515, "Butterfly Knife", "weapons/models/knife/knife_butterfly/weapon_knife_butterfly.vmdl"},
        {516, "Shadow Daggers", "weapons/models/knife/knife_push/weapon_knife_push.vmdl"},
        {517, "Paracord Knife", "weapons/models/knife/knife_cord/weapon_knife_cord.vmdl"},
        {518, "Survival Knife", "weapons/models/knife/knife_canis/weapon_knife_canis.vmdl"},
        {519, "Ursus Knife", "weapons/models/knife/knife_ursus/weapon_knife_ursus.vmdl"},
        {520, "Navaja Knife", "weapons/models/knife/knife_navaja/weapon_knife_navaja.vmdl"},
        {521, "Nomad Knife", "weapons/models/knife/knife_outdoor/weapon_knife_outdoor.vmdl"},
        {522, "Stiletto Knife", "weapons/models/knife/knife_stiletto/weapon_knife_stiletto.vmdl"},
        {523, "Talon Knife", "weapons/models/knife/knife_talon/weapon_knife_talon.vmdl"},
        {525, "Skeleton Knife", "weapons/models/knife/knife_skeleton/weapon_knife_skeleton.vmdl"},
        {526, "Kukri Knife", "weapons/models/knife/knife_kukri/weapon_knife_kukri.vmdl"}
    };

    inline constexpr Item Gloves[]{
        {0, "Default gloves", ""}, {5027, "Bloodhound Gloves", ""},
        {5030, "Sport Gloves", ""}, {5031, "Driver Gloves", ""},
        {5032, "Hand Wraps", ""}, {5033, "Moto Gloves", ""},
        {5034, "Specialist Gloves", ""}, {5035, "Hydra Gloves", ""},
        {4725, "Broken Fang Gloves", ""}
    };
}
