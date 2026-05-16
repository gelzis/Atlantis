#ifdef WIN32
#include <memory.h> // Needed for memcpy on windows
#include "io.h"     // Needed for access() on windows
#define F_OK    0
#endif

#include <iostream>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string.h>

#include "game.h"
#include "gamedata.h"
#include "indenter.hpp"
#include "text_report_generator.hpp"
#include "quests.h"
#include "unit.h"
#include "rng.hpp"
#include "string_parser.hpp"
#include "strings_util.hpp"

#include "external/nlohmann/json.hpp"
using json = nlohmann::json;

namespace {

struct StatRecord {
    int min = 0;
    int max = 0;
    int range = 0;
    int occurance = 0;
    double mean = 0.0;
    int median = 0;
    int mode = 0;
    std::array<int, 10> percentile = {};
    double stdDev = 0.0;
};

const json *find_json_field(const json& value, std::initializer_list<const char *> keys) {
    if (!value.is_object()) return nullptr;

    for (const char *key : keys) {
        auto it = value.find(key);
        if (it != value.end()) return &(*it);
    }

    return nullptr;
}

std::optional<std::string> get_string_field(const json& value, std::initializer_list<const char *> keys) {
    if (const json *field = find_json_field(value, keys); field && field->is_string()) {
        return field->get<std::string>();
    }

    return std::nullopt;
}

bool parse_behind_flag(const json& flags) {
    if (!flags.is_object()) return false;

    auto it = flags.find("behind");
    return it != flags.end() && it->is_boolean() && it->get<bool>();
}

const json *get_units_array(const json& value) {
    if (!value.is_object()) return nullptr;

    auto units = value.find("units");
    if (units == value.end() || !units->is_array()) return nullptr;

    return &(*units);
}

const json *get_known_skills_array(const json& skills) {
    if (!skills.is_object()) return nullptr;

    auto known = skills.find("known");
    if (known == skills.end() || !known->is_array()) return nullptr;

    return &(*known);
}

bool side_has_units(const json& side) {
    if (const json *units = get_units_array(side); units && !units->empty()) {
        return true;
    }

    if (const json *structures = find_json_field(side, {"structures"})) {
        if (!structures->is_array()) return false;

        for (const auto& structure : *structures) {
            if (const json *units = get_units_array(structure); units && !units->empty()) {
                return true;
            }
        }
    }

    return false;
}

int resolve_item(const json& value) {
    auto text = get_string_field(value, {"tag", "name"});
    if (!text) throw std::runtime_error("Each simulated item must define a tag or name.");

    int item = lookup_item(*text);
    if (item != -1) return item;

    item = parse_enabled_item(parser::token(*text));
    if (item != -1) return item;

    throw std::runtime_error("Couldn't find item: " + *text);
}

int resolve_skill(const json& value) {
    auto text = get_string_field(value, {"tag", "name"});
    if (!text) throw std::runtime_error("Each simulated skill must define a tag or name.");

    int skill = lookup_skill(parser::token(*text));
    if (skill != -1) return skill;

    skill = parse_skill(parser::token(*text));
    if (skill != -1) return skill;

    throw std::runtime_error("Couldn't find skill: " + *text);
}

int resolve_object_type(const json& value) {
    if (!value.is_object()) {
        throw std::runtime_error("Each simulated structure must be a JSON object.");
    }

    auto text = get_string_field(value, {"type"});
    if (!text) throw std::runtime_error("Each simulated structure must define a type.");

    int object_type = lookup_object(*text);
    if (object_type != -1) return object_type;

    object_type = parse_object(parser::token(*text), false);
    if (object_type >= 0) return object_type;

    throw std::runtime_error("Couldn't find object: " + *text);
}

void validate_battle_document(const json& document) {
    if (!document.is_object()) {
        throw std::runtime_error("Battle simulation input must be a JSON object.");
    }

    for (const char *side_name : {"attackers", "defenders"}) {
        auto side = document.find(side_name);
        if (side == document.end() || !side->is_object()) {
            throw std::runtime_error(std::string("Please specify ") + side_name + " as JSON objects.");
        }

        if (!side_has_units(*side)) {
            throw std::runtime_error(
                std::string("Please specify ") + side_name
                + " units in report-style json using units[] or structures[].units[]!"
            );
        }
    }
}

void populate_unit_items(Unit *unit, const json& items) {
    if (!items.is_array()) {
        throw std::runtime_error("Each simulated unit must define items as an array.");
    }

    bool has_items = false;
    bool all_items_are_monsters = true;

    for (const auto& item : items) {
        if (!item.is_object()) {
            throw std::runtime_error("Each simulated item must be a JSON object.");
        }

        auto amount_it = item.find("amount");
        if (amount_it == item.end() || !amount_it->is_number_integer()) {
            throw std::runtime_error("Each simulated item must define an integer amount.");
        }

        int amount = amount_it->get<int>();
        if (amount <= 0) {
            throw std::runtime_error("Simulated item amounts must be positive.");
        }

        int item_num = resolve_item(item);
        unit->items.SetNum(item_num, amount);
        has_items = true;

        if (!(ItemDefs[item_num].type & IT_MONSTER)) {
            all_items_are_monsters = false;
        }
    }

    if (!has_items) {
        throw std::runtime_error("Each simulated unit must define at least one item.");
    }

    if (all_items_are_monsters) {
        unit->type = U_WMON;
    }
}

void populate_unit_skills(Unit *unit, const json& skills) {
    const json *known_skills = get_known_skills_array(skills);
    if (!known_skills) {
        throw std::runtime_error("Each simulated unit must define skills.known as an array.");
    }

    for (const auto& skill : *known_skills) {
        if (!skill.is_object()) {
            throw std::runtime_error("Each simulated skill must be a JSON object.");
        }

        auto level_it = skill.find("level");
        if (level_it == skill.end() || !level_it->is_number_integer()) {
            throw std::runtime_error("Each simulated skill must define an integer level.");
        }

        int skill_num = resolve_skill(skill);
        int level = level_it->get<int>();
        if (level < 0) {
            throw std::runtime_error("Simulated skill levels must be non-negative.");
        }

        unit->skills.SetDays(skill_num, GetDaysByLevel(level) * unit->GetMen());
    }
}

void populate_unit_readied(Unit *unit, const json& readied) {
    if (!readied.is_object()) {
        throw std::runtime_error("Each simulated unit must define readied as an object.");
    }

    if (const json *ready_item = find_json_field(readied, {"item"})) {
        unit->readyItem = resolve_item(*ready_item);
    }

    if (const json *weapons = find_json_field(readied, {"weapons"})) {
        if (!weapons->is_array()) {
            throw std::runtime_error("Each simulated unit must define readied.weapons as an array.");
        }
        if (weapons->size() > MAX_READY) {
            throw std::runtime_error("Simulated units may define at most MAX_READY ready weapons.");
        }

        int index = 0;
        for (const auto& weapon : *weapons) {
            int item_num = resolve_item(weapon);
            if (!(ItemDefs[item_num].type & IT_WEAPON)) {
                throw std::runtime_error("Each readied weapon must resolve to a weapon item.");
            }
            unit->readyWeapon[index++] = item_num;
        }
    }

    if (const json *armor = find_json_field(readied, {"armor"})) {
        if (!armor->is_array()) {
            throw std::runtime_error("Each simulated unit must define readied.armor as an array.");
        }
        if (armor->size() > MAX_READY) {
            throw std::runtime_error("Simulated units may define at most MAX_READY ready armor items.");
        }

        int index = 0;
        for (const auto& armor_item : *armor) {
            int item_num = resolve_item(armor_item);
            if (!(ItemDefs[item_num].type & IT_ARMOR)) {
                throw std::runtime_error("Each readied armor entry must resolve to an armor item.");
            }
            unit->readyArmor[index++] = item_num;
        }
    }
}

Object *create_simulation_structure(const json& structure_json, ARegion *region) {
    if (!structure_json.is_object()) {
        throw std::runtime_error("Battle simulation structures must be JSON objects.");
    }

    int object_type = resolve_object_type(structure_json);

    Object *structure = new Object(region);
    structure->type = object_type;
    structure->num = region->buildingseq++;
    structure->capacity = 20000;
    if (auto name = get_string_field(structure_json, {"name"})) {
        structure->set_name(*name);
    } else {
        structure->set_name(ObjectDefs[object_type].name);
    }
    region->objects.push_back(structure);
    return structure;
}

void build_simulation_units(
    const json& units,
    Faction *faction,
    ARegion *region,
    Object *target_object,
    int& next_unit_number,
    std::vector<std::unique_ptr<Location>>& owned_locations,
    std::list<Location *>& raw_locations,
    Unit *&leader
) {
    if (!units.is_array()) {
        throw std::runtime_error("Battle simulation units must be an array.");
    }

    for (const auto& unit_json : units) {
        if (!unit_json.is_object()) {
            throw std::runtime_error("Each simulated unit must be a JSON object.");
        }

        Unit *unit = new Unit(next_unit_number++, faction);
        bool moved_to_object = false;

        try {
            if (auto name = get_string_field(unit_json, {"name"})) {
                unit->set_name(*name);
            }

            if (auto description = get_string_field(unit_json, {"description"})) {
                unit->set_description(*description);
            }

            if (const json *flags = find_json_field(unit_json, {"flags"}); flags && parse_behind_flag(*flags)) {
                unit->SetFlag(FLAG_BEHIND, 1);
            }

            auto items_it = unit_json.find("items");
            if (items_it == unit_json.end()) {
                throw std::runtime_error("Each simulated unit must define items.");
            }
            populate_unit_items(unit, *items_it);

            if (const json *skills = find_json_field(unit_json, {"skills"})) {
                populate_unit_skills(unit, *skills);
            }

            if (const json *combat_spell = find_json_field(unit_json, {"combat_spell"})) {
                unit->type = U_MAGE;
                unit->combat = resolve_skill(*combat_spell);
            }

            if (const json *readied = find_json_field(unit_json, {"readied"})) {
                populate_unit_readied(unit, *readied);
            }

            if (unit->GetSoldiers() <= 0) {
                throw std::runtime_error("Simulated units must contain at least one soldier.");
            }

            unit->MoveUnit(target_object);
            moved_to_object = true;

            auto location = std::make_unique<Location>();
            location->unit = unit;
            location->obj = target_object;
            location->region = region;
            raw_locations.push_back(location.get());
            owned_locations.push_back(std::move(location));

            leader = unit;
        } catch (...) {
            if (!moved_to_object) {
                delete unit;
            }
            throw;
        }
    }
}

void build_simulation_side(
    const json& side,
    Faction *faction,
    ARegion *region,
    int& next_unit_number,
    std::vector<std::unique_ptr<Location>>& owned_locations,
    std::list<Location *>& raw_locations,
    Unit *&leader
) {
    bool built_any_units = false;

    if (const json *outdoor_units = get_units_array(side); outdoor_units && !outdoor_units->empty()) {
        build_simulation_units(
            *outdoor_units,
            faction,
            region,
            region->GetDummy(),
            next_unit_number,
            owned_locations,
            raw_locations,
            leader
        );
        built_any_units = true;
    }

    if (const json *structures = find_json_field(side, {"structures"})) {
        if (!structures->is_array()) {
            throw std::runtime_error("Battle simulation structures must be an array.");
        }

        for (const auto& structure_json : *structures) {
            const json *structure_units = get_units_array(structure_json);
            if (!structure_units) {
                throw std::runtime_error("Each simulated structure must define units as an array.");
            }
            if (structure_units->empty()) {
                continue;
            }

            Object *structure = create_simulation_structure(structure_json, region);
            build_simulation_units(
                *structure_units,
                faction,
                region,
                structure,
                next_unit_number,
                owned_locations,
                raw_locations,
                leader
            );
            built_any_units = true;
        }
    }

    if (!built_any_units || !leader) {
        throw std::runtime_error("Battle simulation side must contain at least one unit.");
    }
}

StatRecord calculate_stat(const std::vector<int>& values) {
    StatRecord rec;
    if (values.empty()) return rec;

    std::map<int, int> histogram;
    int sum = 0;
    bool first = true;

    for (int value : values) {
        sum += value;
        histogram[value]++;

        if (value > 0) rec.occurance++;

        if (first) {
            first = false;
            rec.min = value;
            rec.max = value;
        } else {
            rec.min = std::min(rec.min, value);
            rec.max = std::max(rec.max, value);
        }
    }

    rec.range = rec.max - rec.min;
    rec.mean = static_cast<double>(sum) / values.size();

    double variance_sum = 0.0;
    for (int value : values) {
        double delta = static_cast<double>(value) - rec.mean;
        variance_sum += delta * delta;
    }
    rec.stdDev = std::sqrt(variance_sum / values.size());

    std::vector<int> sorted_values(values);
    std::sort(sorted_values.begin(), sorted_values.end());
    rec.median = sorted_values[sorted_values.size() / 2];

    for (int i = 0; i < 10; i++) {
        int idx = static_cast<int>(((i + 1) * sorted_values.size()) / 10) - 1;
        rec.percentile[i] = sorted_values[std::max(idx, 0)];
    }

    int mode_count = 0;
    for (const auto& [value, count] : histogram) {
        if (count > mode_count) {
            mode_count = count;
            rec.mode = value;
        }
    }

    return rec;
}

json stat_to_json(const StatRecord& rec) {
    json result = {
        {"occurance", rec.occurance},
        {"min", rec.min},
        {"max", rec.max},
        {"range", rec.range},
        {"mean", rec.mean},
        {"median", rec.median},
        {"mode", rec.mode},
        {"stdDev", rec.stdDev},
    };

    result["percentile"] = json::array();
    for (int value : rec.percentile) {
        result["percentile"].push_back(value);
    }

    return result;
}

std::string get_item_category(int type) {
    if (type & IT_MAN) return "man";
    if (type & IT_MONSTER) return "monster";
    if (type & IT_WEAPON) return "weapon";
    if (type & IT_ARMOR) return "armour";
    if (type & IT_MOUNT) return "mount";
    if (type & IT_TOOL) return "tool";
    if (type & IT_ILLUSION) return "illusion";
    if (type & IT_UNDEAD) return "monster";
    if (type & IT_DEMON) return "monster";

    return "other";
}

} // namespace

Game::Game()
{
    gameStatus = GAME_STATUS_UNINIT;
    ppUnits = 0;
    maxppunits = 0;
    events = new Events();
    rulesetSpecificData = json::object();

    if (Globals->FACTION_ACTIVITY == FactionActivityRules::DEFAULT)
    {
        FactionTypes->push_back(F_WAR);
        FactionTypes->push_back(F_TRADE);
        FactionTypes->push_back(F_MAGIC);
    }
    else
    {
        FactionTypes->push_back(F_MARTIAL);
        FactionTypes->push_back(F_MAGIC);
    }
}

Game::~Game()
{
    delete[] ppUnits;
    delete events;
    ppUnits = 0;
    maxppunits = 0;
    // Return the global array to it's original state. (needed for unit tests)
    FactionTypes->clear();
}

int Game::TurnNumber()
{
    return (year-1)*12 + month + 1;
}

// ALT, 25-Jul-2000
// Default work order procedure
void Game::DefaultWorkOrder()
{
    for(const auto r : regions) {
        if (r->type == R_NEXUS) continue;
        for(const auto o : r->objects) {
            for(const auto u : o->units) {
                if (u->monthorders || u->faction->is_npc || (Globals->TAX_PILLAGE_MONTH_LONG && u->taxing != TAX_NONE))
                    continue;
                if (u->GetFlag(FLAG_AUTOTAX) && (Globals->TAX_PILLAGE_MONTH_LONG && u->Taxers(1))) {
                    u->taxing = TAX_AUTO;
                } else if (Globals->DEFAULT_WORK_ORDER) {
                    ProcessWorkOrder(u, 1, 0);
                }
            }
        }
    }
}

std::string Game::GetXtraMap(ARegion *reg, int type)
{
    int i;

    if (!reg) return " ";

    switch (type) {
        case 0:
            return reg->IsStartingCity() ? "!" : (reg->HasShaft() ? "*" : " ");
        case 1:
            i = reg->CountWMons();
            return (i ? std::to_string(i) : " ");
        case 2:
            for(const auto o : reg->objects) {
                if (!(ObjectDefs[o->type].flags & ObjectType::CANENTER)) {
                    return (o->units.front() ? "*" : ".");
                }
            }
            return " ";
        case 3:
            if (reg->gate) return "*";
            return " ";
    }
    return " ";
}

void Game::WriteSurfaceMap(std::ostream& f, ARegionArray *pArr, int type)
{
    ARegion *reg;
    int yy = 0;
    int xx = 0;

    f << "Map (" << xx*32 << "," << yy*16 << ")\n";
    for (int y=0; y < pArr->y; y+=2) {
        std::string temp;
        int x;
        for (x=0; x< pArr->x; x+=2) {
            reg = pArr->GetRegion(x+xx*32,y+yy*16);
            temp += GetRChar(reg);
            temp += GetXtraMap(reg,type);
            temp += "  ";
        }
        f << temp << "\n";
        temp = "  ";
        for (x=1; x< pArr->x; x+=2) {
            reg = pArr->GetRegion(x+xx*32,y+yy*16+1);
            temp += GetRChar(reg);
            temp += GetXtraMap(reg,type);
            temp += "  ";
        }
        f << temp << "\n";
    }
    f << "\n";
}

void Game::WriteUnderworldMap(std::ostream& f, ARegionArray *pArr, int type)
{
    ARegion *reg, *reg2;
    int xx = 0;
    int yy = 0;
    f << "Map (" << xx*32 << "," << yy*16 << ")\n";
    for (int y=0; y< pArr->y; y+=2) {
        std::string temp = " ";
        std::string temp2;
        int x;
        for (x=0; x< pArr->x; x+=2) {
            reg = pArr->GetRegion(x+xx*32,y+yy*16);
            reg2 = pArr->GetRegion(x+xx*32+1,y+yy*16+1);
            temp += GetRChar(reg);
            temp += GetXtraMap(reg,type);
            if (reg2 && reg2->neighbors[D_NORTH]) temp += "|";
            else temp += " ";

            temp += " ";
            if (reg && reg->neighbors[D_SOUTHWEST]) temp2 += "/";
            else temp2 += " ";

            temp2 += " ";
            if (reg && reg->neighbors[D_SOUTHEAST]) temp2 += "\\";
            else temp2 += " ";

            temp2 += " ";
        }
        f << temp << "\n" << temp2 << "\n";

        temp = " ";
        temp2 = "  ";
        for (x=1; x< pArr->x; x+=2) {
            reg = pArr->GetRegion(x+xx*32,y+yy*16+1);
            reg2 = pArr->GetRegion(x+xx*32-1,y+yy*16);

            if (reg2 && reg2->neighbors[D_SOUTH]) temp += "|";
            else temp += " ";

            temp += " ";
            temp += GetRChar(reg);
            temp += GetXtraMap(reg,type);

            if (reg && reg->neighbors[D_SOUTHWEST]) temp2 += "/";
            else temp2 += " ";

            temp2 += " ";
            if (reg && reg->neighbors[D_SOUTHEAST]) temp2 += "\\";
            else temp2 += " ";

            temp2 += " ";
        }
        f << temp << "\n" << temp2 << "\n";
    }
    f << "\n";
}

int Game::view_map(const std::string& typestr,const std::string& mapfile)
{
    int type = 0;
    if (typestr == "wmon") type = 1;
    if (typestr == "lair") type = 2;
    if (typestr == "gate") type = 3;
    if (typestr == "cities") type = 4;
    if (typestr == "hex") type = 5;

    std::ofstream f(mapfile, std::ios::out|std::ios::ate);
    if (!f.is_open()) return(0);

    switch (type) {
        case 0:
            f << "Geographical Map\n";
            break;
        case 1:
            f << "Wandering Monster Map\n";
            break;
        case 2:
            f << "Lair Map\n";
            break;
        case 3:
            f << "Gate Map\n";
            break;
        case 4:
            f << "Cities Map\n";
            break;
    }

    // Cities map is a bit special since it is really just a list of all the cities in that region
    if (type == 4) {
        for(const auto reg : regions) {
            // Ignore anything that isn't the surface
            if (reg->level->levelType != ARegionArray::LEVEL_SURFACE) continue;
            // Ignore anything with no city
            if (!reg->town || (reg->town->TownType() != TOWN_CITY)) continue;

            f << "(" << reg->xloc << "," << reg->yloc << "): " << reg->town->name << "\n";
        }
        return(1);
    }

    if (type == 5) {
        json worldmap;

        std::vector<ARegion *> start_regions;
        for (auto i = 0; i < regions.numLevels; i++) {
            ARegionArray *pArr = regions.pRegionArrays[i];
            if (pArr->levelType == ARegionArray::LEVEL_NEXUS) {
                if (!Globals->START_CITIES_EXIST) {
                    ARegion *nexus = pArr->GetRegion(0, 0);
                    for(const auto o : nexus->objects) {
                        if (o->inner != -1) {
                            start_regions.push_back(regions.GetRegion(o->inner));
                        }
                    }
                }
                continue;
            };
            std::string label = (pArr->strName.empty() ? "surface" : (pArr->strName + std::to_string(i-1)));
            worldmap[label] = json::array();

            for (int y = 0; y < pArr->y; y++) {
                for (int x = 0; x < pArr->x; x++) {
                    ARegion *reg = pArr->GetRegion(x, y);
                    if (!reg) continue;
                    json data = reg->basic_region_data();
                    json hexout = {
                        { "x", x }, { "y", y }, { "z", i },
                        { "terrain", data["terrain"] },
                        { "yew", reg->produces_item(I_YEW) },
                        { "mithril", reg->produces_item(I_MITHRIL) },
                        { "admantium", reg->produces_item(I_ADMANTIUM) },
                        { "floater", reg->produces_item(I_FLOATER) },
                        { "wing", reg->produces_item(I_WHORSE) },
                        { "gate", reg->gate != 0 },
                        { "exits", json::array() },
                        { "shaft", reg->HasShaft() },
                        { "starting",
                          (start_regions.end() != std::find(start_regions.begin(), start_regions.end(), reg)) ||
                          reg->IsStartingCity()
                        }
                    };
                    // Fill in the exits
                    for (auto d = 0; d < NDIRS; d++) {
                        hexout["exits"].push_back(reg->neighbors[d] != nullptr);
                    }
                    if (reg->town) {
                        hexout["town"] = data["settlement"]["size"];
                    }
                    worldmap[label].push_back(hexout);
                }
            }
        }
        f << worldmap.dump(2);
        return(1);
    }

    int i;
    for (i = 0; i < regions.numLevels; i++) {
        f << '\n';
        ARegionArray *pArr = regions.pRegionArrays[i];
        switch(pArr->levelType) {
            case ARegionArray::LEVEL_NEXUS:
                f << "Level " << i << ": Nexus\n";
                break;
            case ARegionArray::LEVEL_SURFACE:
                f << "Level " << i << ": Surface\n";
                WriteSurfaceMap(f, pArr, type);
                break;
            case ARegionArray::LEVEL_UNDERWORLD:
                f << "Level " << i << ": Underworld\n";
                WriteUnderworldMap(f, pArr, type);
                break;
            case ARegionArray::LEVEL_UNDERDEEP:
                f << "Level " << i << ": Underdeep\n";
                WriteUnderworldMap(f, pArr, type);
                break;
        }
    }
    return(1);
}

int Game::NewGame()
{
    factionseq = 1;
    guardfaction = 0;
    monfaction = 0;
    unitseq = 1;
    SetupUnitNums();
    shipseq = 100;
    year = 1;
    month = -1;
    gameStatus = GAME_STATUS_NEW;

    //
    // Seed the random number generator with a different value each time.
    //
    // init_random_seed() is a function reference that can be overwritten by the test suites to control the random seed
    // so that tests are repeatable.
    init_random_seed();

    CreateWorld();
    CreateNPCFactions();

    if (Globals->CITY_MONSTERS_EXIST)
        CreateCityMons();
    if (Globals->WANDERING_MONSTERS_EXIST)
        CreateWMons();
    if (Globals->LAIR_MONSTERS_EXIST)
        CreateLMons();

    if (Globals->LAIR_MONSTERS_EXIST)
        CreateVMons();

    /*
    if (Globals->PLAYER_ECONOMY) {
        Equilibrate();
    }
    */



    return(1);
}

int Game::OpenGame()
{
    //
    // The order here must match the order in SaveGame
    //
    std::ifstream f("game.in", std::ios::in);
    if (!f.is_open()) return(0);
    //
    // Read in Globals
    std::string str;
    std::getline(f >> std::ws, str);
    if (f.eof()) return(0);

    if (str.empty()) return(0);

    if (str != "atlantis_game") return(0);

    ATL_VER eVersion;
    f >> eVersion;
    logger::write("Saved Game Engine Version: " + ATL_VER_STRING(eVersion));
    if (
        ATL_VER_MAJOR(eVersion) != ATL_VER_MAJOR(CURRENT_ATL_VER) ||
        ATL_VER_MINOR(eVersion) != ATL_VER_MINOR(CURRENT_ATL_VER)
    ) {
        logger::write("Incompatible Engine versions!");
        return(0);
    }
    if (ATL_VER_PATCH(eVersion) > ATL_VER_PATCH(CURRENT_ATL_VER)) {
        logger::write("This game was created with a more recent Atlantis Engine!");
        return(0);
    }

    std::string gameName;
    f >> std::ws;
    std::getline(f, gameName);
    if (f.eof()) return(0);

    if (!(gameName == Globals->RULESET_NAME)) {
        logger::write("Incompatible rule-set!");
        return(0);
    }

    ATL_VER gVersion;
    f >> gVersion;
    logger::write("Saved Rule-Set Version: " + ATL_VER_STRING(gVersion));

    if (ATL_VER_MAJOR(gVersion) < ATL_VER_MAJOR(Globals->RULESET_VERSION)) {
        logger::write("Upgrading to " + ATL_VER_STRING(MAKE_ATL_VER(ATL_VER_MAJOR(Globals->RULESET_VERSION), 0, 0)));
        if (!upgrade_major_version(gVersion)) {
            logger::write("Unable to upgrade!  Aborting!");
            return(0);
        }
        gVersion = MAKE_ATL_VER(ATL_VER_MAJOR(Globals->RULESET_VERSION), 0, 0);
    }
    if (ATL_VER_MINOR(gVersion) < ATL_VER_MINOR(Globals->RULESET_VERSION)) {
        logger::write("Upgrading to " + ATL_VER_STRING(
            MAKE_ATL_VER(ATL_VER_MAJOR(Globals->RULESET_VERSION), ATL_VER_MINOR(Globals->RULESET_VERSION), 0)
        ));
        if (!upgrade_minor_version(gVersion)) {
            logger::write("Unable to upgrade!  Aborting!");
            return(0);
        }
        gVersion = MAKE_ATL_VER(ATL_VER_MAJOR(gVersion), ATL_VER_MINOR(Globals->RULESET_VERSION), 0);
    }
    if (ATL_VER_PATCH(gVersion) < ATL_VER_PATCH(Globals->RULESET_VERSION)) {
        logger::write("Upgrading to " + ATL_VER_STRING(Globals->RULESET_VERSION));
        if (!upgrade_patch_level(gVersion)) {
            logger::write("Unable to upgrade!  Aborting!");
            return(0);
        }
        gVersion = MAKE_ATL_VER(ATL_VER_MAJOR(gVersion), ATL_VER_MINOR(gVersion), ATL_VER_PATCH(Globals->RULESET_VERSION));
    }

    f >> year;
    f >> month;

    int seed;
    f >> seed;
    rng::seed_random(seed);

    f >> factionseq;
    f >> unitseq;
    f >> shipseq;
    f >> guardfaction;
    f >> monfaction;

    //
    // Read in the Factions
    //
    int i;
    f >> i;

    for (int j = 0; j < i; j++) {
        Faction *temp = new Faction();
        temp->Readin(f);
        factions.push_back(temp);
    }

    //
    // Read in the ARegions
    //
    i = regions.ReadRegions(f, factions);
    if (!i) return 0;

    // read in quests
    if (!quests.read_quests(f))
        return 0;

    SetupUnitNums();

    return(1);
}

int Game::SaveGame()
{
    std::ofstream f("game.out", std::ios::out|std::ios::ate);
    if (!f.is_open()) return(0);

    //
    // Write out Globals
    //
    f << "atlantis_game\n";
    f << CURRENT_ATL_VER << "\n";
    f << Globals->RULESET_NAME << "\n";
    f << Globals->RULESET_VERSION << "\n";

    f << year << "\n";
    f << month << "\n";
    f << rng::get_random(10000) << "\n";
    f << factionseq << "\n";
    f << unitseq << "\n";
    f << shipseq << "\n";
    f << guardfaction << "\n";
    f << monfaction << "\n";
    //
    // Write out the Factions
    //
    f << factions.size() << "\n";
    for(const auto fac : factions) {
        fac->Writeout(f);
    }

    //
    // Write out the ARegions
    //
    regions.WriteRegions(f);

    // Write out quests
    quests.write_quests(f);

    return(1);
}

void Game::DummyGame()
{
    //
    // No need to set anything up; we're just syntax checking some orders.
    //
}

#define PLAYERS_FIRST_LINE "AtlantisPlayerStatus"

int Game::WritePlayers()
{
    std::ofstream f("players.out", std::ios::out|std::ios::ate);
    if (!f.is_open()) return(0);

    f << PLAYERS_FIRST_LINE << "\n";
    f << "Version: " << CURRENT_ATL_VER << "\n";
    f << "TurnNumber: " << TurnNumber() << "\n";
    if (gameStatus == GAME_STATUS_UNINIT)
        return(0);

    if (gameStatus == GAME_STATUS_NEW)
        f << "GameStatus: New\n\n";
    else if (gameStatus == GAME_STATUS_RUNNING)
        f << "GameStatus: Running\n\n";
    else if (gameStatus == GAME_STATUS_FINISHED)
        f << "GameStatus: Finished\n\n";

    for(const auto fac : factions) {
        fac->WriteFacInfo(f);
    }

    return(1);
}

bool Game::ReadPlayers()
{
    std::ifstream f("players.in", std::ios::in);
    if (!f.is_open()) return(0);

    parser::string_parser parser;

    //
    // Default: failure.
    //
    bool return_code = false;

    do {
        //
        // The first line of the file should match.
        //
        f >> parser;
        if (parser.get_token() != PLAYERS_FIRST_LINE) break;

        //
        // Get the file version number.
        //
        f >> parser;
        if (parser.get_token() != "Version:") break;

        auto token = parser.get_token();
        if (!token) break;

        int nVer = token.get_number().value_or(0);
        if (
            ATL_VER_MAJOR(nVer) != ATL_VER_MAJOR(CURRENT_ATL_VER) ||
            ATL_VER_MINOR(nVer) != ATL_VER_MINOR(CURRENT_ATL_VER) ||
            ATL_VER_PATCH(nVer) > ATL_VER_PATCH(CURRENT_ATL_VER)
        ) {
            logger::write("The players.in file is not compatible with this version of Atlantis.");
            break;
        }

        //
        // Ignore the turn number line.
        //
        f >> parser;

        //
        // Next, the game status.
        //
        f >> parser;
        if (parser.get_token() != "GameStatus:") break;

        token = parser.get_token();
        if (!token) break;

        if (token == "New") gameStatus = GAME_STATUS_NEW;
        else if (token == "Running") gameStatus = GAME_STATUS_RUNNING;
        else if (token == "Finished") gameStatus = GAME_STATUS_FINISHED;
        else break; // invalid game status

        //
        // Now, we should have a list of factions.
        //
        f >> parser;
        Faction *fac = nullptr;

        bool lastWasNew = false;

        //
        // OK, set our return code to success; we'll set it to fail below
        // if necessary.
        //
        return_code = true;

        while(!f.eof()) {
            auto token = parser.get_token();
            if (!token) {
                f >> parser;
                continue;
            }

            if (token == "Faction:") {
                //
                // Get the new faction
                //
                token = parser.get_token();
                if (!token) {
                    return_code = false;
                    break;
                }

                if (token == "new") {
                    std::string save = parser.str();
                    int noleader = 0;
                    int x, y, z;
                    ARegion *reg = nullptr;

                    /* Check for the noleader flag */
                    token = parser.get_token();
                    if (token == "noleader") {
                        noleader = 1;
                        token = parser.get_token();
                        /* Initialize reg to something useful */
                        reg = regions.GetRegion(0, 0, 0);
                    }

                    x = token.get_number().value_or(-1);
                    y = parser.get_token().get_number().value_or(-1);
                    z = parser.get_token().get_number().value_or(-1);
                    if (x != -1 && y != -1 && z != -1) {
                        reg = regions.GetRegion(x, y, z);
                        if (reg == nullptr) logger::write("Bad faction line: " + save);
                    }

                    fac = AddFaction(noleader, reg);
                    if (!fac) {
                        logger::write("Failed to add a new faction!");
                        return_code = false;
                        break;
                    }

                    lastWasNew = true;
                } else {
                    int nFacNum = token.get_number().value_or(0);
                    lastWasNew = false;
                    fac = GetFaction(factions, nFacNum);
                    if (!fac) continue;

                    fac->startturn = TurnNumber();
                }
            } else if (fac) {
                return_code = ReadPlayersLine(token, parser, fac, lastWasNew);
                if (!return_code) break;
            }

            f >> parser;
        }
    } while(false);

    return(return_code);
}

Unit *Game::parse_gm_unit(std::string tag, Faction *fac)
{
    if (tag.empty()) return nullptr;

    if (tag[0] == 'g' && tag[1] == 'm') {
        auto gma = parser::token(tag.substr(2)).get_number();
        for(const auto reg : regions) {
            for(const auto obj : reg->objects) {
                for(const auto u : obj->units) {
                    if (u->faction->num == fac->num && u->gm_alias == gma) {
                        return u;
                    }
                }
            }
        }
    } else {
        auto v = parser::token(tag).get_number();
        if (!v) return nullptr;
        int id = v.value();
        if (static_cast<unsigned int>(id) >= maxppunits) return nullptr;
        return GetUnit(id);
    }
    return nullptr;
}

bool Game::ReadPlayersLine(parser::token& token, parser::string_parser& parser, Faction *fac, bool new_player)
{
    if (token == "Name:") {
        std::string name = parser.str();
        if (!name.empty()) {
            if (new_player) name += " (" + std::to_string(fac->num) + ")";
            fac->set_name(name, false);
        }
        return true;
    }

    if (token == "RewardTimes") {
        fac->TimesReward();
        return true;
    }

    if (token == "Email:") {
        std::string str = parser.get_token().get_string();
        if (!str.empty()) {
            fac->address = str;
        }
        return true;
    }

    if (token == "Password:") {
        std::string newpass = parser.get_token().get_string();
        if (newpass.empty()) newpass = "none";
        fac->password = newpass;
        return true;
    }

    if (token == "Battle:") {
        fac->battleLogFormat = 0;
        return true;
    }

    if (token == "Template:") {
        token = parser.get_token();
        int temp = parse_template_type(token);
        fac->temformat = temp == -1 ? TEMPLATE_LONG : temp;
        return true;
    }

    if (token == "Reward:") {
        int amt = parser.get_token().get_number().value_or(0);
        fac->event("Reward of " + std::to_string(amt) + " silver.", "reward");
        fac->unclaimed += amt;
        return true;
    }

    if (token == "SendTimes:") {
        // get the token, but otherwise ignore it
        fac->times = parser.get_token().get_number().value_or(0);
        return true;
    }

    if (token == "LastOrders:") {
        // Read this line and correctly set the lastorders for this
        // faction if the game itself isn't maintaining them.
        if (Globals->LASTORDERS_MAINTAINED_BY_SCRIPTS)
            fac->lastorders = parser.get_token().get_number().value_or(0);
        return true;
    }

    if (token == "FirstTurn:") {
        fac->startturn = parser.get_token().get_number().value_or(0);
        return true;
    }

    if (token == "Loc:") {
        auto x = parser.get_token().get_number();
        auto y = parser.get_token().get_number();
        auto z = parser.get_token().get_number();

        ARegion *reg = nullptr;
        if (x && y && z) {
            reg = regions.GetRegion(x.value(), y.value(), z.value());
        }
        if (!reg) {
            std::string str = "Invalid Loc: ";
            str += x ? std::to_string(x.value()) + ", " : "missing x-coord, ";
            str += y ? std::to_string(y.value()) + ", " : "missing y-coord, ";
            str += z ? std::to_string(z.value()) : "missing z-coord";
            str += " in faction " + std::to_string(fac->num);
            logger::write(str);
        }
        fac->pReg = reg;
        return true;
    }

    if (token == "NewUnit:") {
        // Creates a new unit in the location specified by a Loc: line
        // with a gm_alias of whatever is after the NewUnit: tag.
        if (!fac->pReg) {
            logger::write("NewUnit is not valid without a Loc: for faction "+ std::to_string(fac->num));
            return true;
        }
        int val = parser.get_token().get_number().value_or(0);
        if (!val) {
            logger::write("NewUnit: must be followed by an alias in faction " + std::to_string(fac->num));
            return true;
        }
        Unit *u = GetNewUnit(fac);
        u->gm_alias = val;
        u->MoveUnit(fac->pReg->GetDummy());
        u->event("Is given to your faction.", "gm_gift");
        return true;
    }

    if (token == "Item:") {
        std::string alias = parser.get_token().get_string();
        if (alias.empty()) {
            logger::write("Item: needs to specify a unit in faction " + std::to_string(fac->num));
            return true;
        }
        Unit *u = parse_gm_unit(alias, fac);
        if (!u) {
            logger::write("Item: needs to specify a unit in faction " + std::to_string(fac->num));
            return true;
        }
        if (u->faction->num != fac->num) {
            logger::write("Item: unit "+ std::to_string(u->num) + " doesn't belong to faction " +
                std::to_string(fac->num));
            return true;
        }

        auto val = parser.get_token().get_number();
        if (!val) {
            logger::write("Must specify a number of items to give for Item: in faction " + std::to_string(fac->num));
            return true;
        }

        token = parser.get_token();
        int it = parse_all_items(token);
        if (it == -1) {
            logger::write("Must specify a valid item to give for Item: in faction " + std::to_string(fac->num));
            return true;
        }

        int has = u->items.GetNum(it);
        u->items.SetNum(it, has + val.value_or(0));
        if (!u->gm_alias) {
            u->event("Is given " + item_string(it, val.value_or(0)) + " by the gods.", "gm_gift");
        }
        u->faction->DiscoverItem(it, 0, 1);
        return true;
    }

    if (token == "Skill:") {
        std::string alias = parser.get_token().get_string();
        if (alias.empty()) {
            logger::write("Skill: needs to specify a unit in faction " + std::to_string(fac->num));
            return true;
        }
        Unit *u = parse_gm_unit(alias, fac);
        if (!u) {
            logger::write("Skill: needs to specify a unit in faction " + std::to_string(fac->num));
            return true;
        }
        if (u->faction->num != fac->num) {
            logger::write("Item: unit "+ std::to_string(u->num) + " doesn't belong to faction " +
                std::to_string(fac->num));
            return true;
        }

        token = parser.get_token();
        int sk = parse_skill(token);
        if (sk == -1) {
            logger::write("Must specify a valid skill for Skill: in faction " + std::to_string(fac->num));
            return true;
        }

        int days = parser.get_token().get_number().value_or(0) * u->GetMen();
        if (!days) {
            logger::write("Must specify a days for Skill: in faction " + std::to_string(fac->num));
            return true;
        }

        int odays = u->skills.GetDays(sk);
        u->skills.SetDays(sk, odays + days);
        u->AdjustSkills();
        int lvl = u->GetRealSkill(sk);
        if (lvl > fac->skills.GetDays(sk)) {
            fac->skills.SetDays(sk, lvl);
            fac->shows.push_back({ .skill = sk, .level = lvl });
        }
        if (!u->gm_alias) {
            u->event("Is taught " + std::to_string(days) + " days of " + SkillStrs(sk) + " by the gods.", "gm_gift");
        }
        /* This is NOT quite the same, but the gods are more powerful than mere mortals */
        int mage = (SkillDefs[sk].flags & SkillType::MAGIC);
        int app = (SkillDefs[sk].flags & SkillType::APPRENTICE);
        if (mage) u->type = U_MAGE;
        if (app && u->type == U_NORMAL) u->type = U_APPRENTICE;
        return true;
    }

    if (token == "Order:") {
        std::string alias = parser.get_token().get_string();
        if (alias == "quit") {
            fac->quit = QUIT_BY_GM;
            return true;
        }
        //Not quit so, handle it as a unit order
        Unit *u = parse_gm_unit(alias, fac);
        if (!u) {
            logger::write("Order: needs to specify a unit in faction " + std::to_string(fac->num));
            return true;
        }
        if (u->faction->num != fac->num) {
            logger::write("Order: unit "+ std::to_string(u->num) + " doesn't belong to faction " +
                std::to_string(fac->num));
            return true;
        }

        std::string saved = parser.str();
        bool repeating = parser.get_at();
        token = parser.get_token();
        if (!token) {
            logger::write("Order: must provide unit order for faction "+ std::to_string(fac->num));
            return true;
        }
        int o = Parse1Order(token);
        if (o == -1 || o == O_ATLANTIS || o == O_END || o == O_UNIT || o == O_FORM || o == O_ENDFORM) {
            logger::write("Order: invalid order given for faction "+ std::to_string(fac->num));
            return true;
        }
        if (repeating) {
            u->oldorders.push_back(saved);
        }
        ProcessOrder(o, u, parser, nullptr, repeating);
        return true;
    }

    std::string temp = parser.original();
    if (temp.empty()) return true;

    fac->extra_player_data.push_back(temp);
    return true;
}

int Game::do_orders_check(const std::string& strOrders, const std::string& strCheck)
{
    std::ifstream ordersFile(strOrders, std::ios::in);
    if (!ordersFile.is_open()) {
        logger::write("No such orders file!");
        return(0);
    }

    std::ofstream checkFile(strCheck, std::ios::out|std::ios::ate);
    if (!checkFile.is_open()) {
        logger::write("Couldn't open the orders check file!");
        return(0);
    }

    orders_check check(checkFile);
    ParseOrders(0, ordersFile, &check);

    return(1);
}

int Game::RunGame()
{
    logger::write("Setting Up Turn...");
    PreProcessTurn();

    logger::write("Reading the Gamemaster File...");
    if (!ReadPlayers()) return(0);

    if (gameStatus == GAME_STATUS_FINISHED) {
        logger::write("This game is finished!");
        return(0);
    }
    gameStatus = GAME_STATUS_RUNNING;

    logger::write("Reading the Orders File...");
    ReadOrders();

    if (Globals->MAX_INACTIVE_TURNS != -1) {
        logger::write("QUITting Inactive Factions...");
        RemoveInactiveFactions();
    }

    logger::write("Running the Turn...");
    RunOrders();

    if (Globals->WORLD_EVENTS) {
        logger::write("Writing world events...");
        WriteWorldEvents();
    }

    logger::write("Writing the Report File...");
    WriteReport();
    logger::write("");

    battles.clear();

    EmptyHell();

    logger::write("Writing Playerinfo File...");
    WritePlayers();

    logger::write("Removing Dead Factions...");
    DeleteDeadFactions();

    logger::write("done");

    return(1);
}

void Game::RecordFact(FactBase* fact) {
    this->events->AddFact(fact);
}

void Game::WriteWorldEvents() {
    std::string text = this->events->Write(Globals->RULESET_NAME, MonthNames[this->month], this->year);
    if (text.empty() || text.length() == 0) return;

    this->write_times_article(text);
}

void Game::PreProcessTurn()
{
    month++;
    if (month>11) {
        month = 0;
        year++;
    }
    SetupUnitNums();
    for(const auto f : factions) f->DefaultOrders();

    for(const auto reg : regions) {
        if (Globals->WEATHER_EXISTS)
            reg->SetWeather(regions.GetWeather(reg, month));
        if (Globals->GATES_NOT_PERENNIAL)
            reg->SetGateStatus(month);
        reg->DefaultOrders();
    }
}

void Game::ClearOrders(Faction *f)
{
    for(const auto r : regions) {
        for(const auto o : r->objects) {
            for(const auto u : o->units) {
                if (u->faction == f) {
                    u->ClearOrders();
                }
            }
        }
    }
}

void Game::ReadOrders()
{
    for(const auto fac : factions) {
        if (!fac->is_npc) {
            std::string str = "orders.";
            str += std::to_string(fac->num);

            std::ifstream file(str, std::ios::in);
            if(file.is_open()) {
                ParseOrders(fac->num, file, nullptr);
                file.close();
            }
            DefaultWorkOrder();
        }
    }
}

void Game::MakeFactionReportLists()
{
    std::vector<Faction *> facs(factionseq, nullptr);

    for(const auto reg : regions) {
        // clear the temporary
        std::fill(facs.begin(), facs.end(), nullptr);

        for(const auto far : reg->farsees) {
            facs[far->faction->num] = far->faction;
        }
        for(const auto pass : reg->passers) {
            facs[pass->faction->num] = pass->faction;
        }
        for(const auto obj : reg->objects) {
            for(const auto unit : obj->units) {
                facs[unit->faction->num] = unit->faction;
            }
        }

        for(const auto& fac: facs) {
            if (fac) fac->present_regions.push_back(reg);
        }
    }
}

void Game::WriteReport()
{
    MakeFactionReportLists();
    CountAllSpecialists();

    size_t ** citems = nullptr;

    if (Globals->FACTION_STATISTICS) {
        citems = new size_t * [factionseq];
        for (int i = 0; i < factionseq; i++)
        {
            citems [i] = new size_t [NITEMS];
            for (int j = 0; j < NITEMS; j++)
            {
                citems [i][j] = 0;
            }
        }
        CountItems(citems);
    }

    for(const auto fac : factions) {
        std::string report_file = "report." + std::to_string(fac->num);
        std::string template_file = "template." + std::to_string(fac->num);

        if (!fac->is_npc || fac->gets_gm_report(this)) {
            // Generate the report in JSON format and then write it to whatever formats we want
            json json_report;
            bool show_region_depth = Globals->EASIER_UNDERWORLD
                && (Globals->UNDERWORLD_LEVELS + Globals->UNDERDEEP_LEVELS > 1);
            fac->build_json_report(json_report, this, citems);

            if (Globals->REPORT_FORMAT & GameDefs::REPORT_FORMAT_JSON) {
                std::ofstream jsonf(report_file + ".json", std::ios::out | std::ios::ate);
                if (jsonf.is_open()) {
                    jsonf << json_report.dump(2);
                }
            }

            if (Globals->REPORT_FORMAT & GameDefs::REPORT_FORMAT_TEXT) {
                TextReportGenerator text_report;
                std::ofstream f(report_file, std::ios::out | std::ios::ate);
                if (f.is_open()) {
                    text_report.output(f, json_report, show_region_depth);
                }
                if (!fac->is_npc && fac->temformat != TEMPLATE_OFF) {
                    // even factions which get a gm report do not get a template.
                    std::ofstream f(template_file, std::ios::out | std::ios::ate);
                    if (f.is_open()) {
                        text_report.output_template(f, json_report, fac->temformat, show_region_depth);
                    }
                }
            }
        }
        logger::dot();
    }

    // stop leaking memory
    if (Globals->FACTION_STATISTICS) {
        for (auto i = 0; i < factionseq; i++) delete [] citems [i];
        delete [] citems;
    }
}

void Game::DeleteDeadFactions()
{
    for(auto it = factions.begin(); it != factions.end();) {
        auto fac = *it;
        if (!fac->is_npc && !fac->exists) {
            it = factions.erase(it);
            for(const auto fac2 : factions) fac2->remove_attitude(fac->num);
            delete fac;
            continue;
        }
        ++it;
    }
}

Faction *Game::AddFaction(int noleader, ARegion *pStart)
{
    //
    // set up faction
    //
    Faction *temp = new Faction(factionseq);
    temp->set_address("NoAddress");
    temp->lastorders = TurnNumber();
    temp->startturn = TurnNumber();
    temp->pStartLoc = pStart;
    temp->pReg = pStart;
    temp->noStartLeader = noleader;

    if (!SetupFaction(temp)) {
        delete temp;
        return 0;
    }
    factions.push_back(temp);
    factionseq++;
    return temp;
}

void Game::ViewFactions()
{
    for(const auto f : factions) f->View();
}

void Game::SetupUnitSeq()
{
    int max = 0;
    for(const auto r : regions) {
        for(const auto o : r->objects) {
            for(const auto u : o->units) {
                if (u && u->num > max) max = u->num;
            }
        }
    }
    unitseq = max+1;
}

void Game::SetupUnitNums()
{
    if (ppUnits) delete[] ppUnits;

    SetupUnitSeq();

    maxppunits = unitseq+10000;

    ppUnits = new Unit *[maxppunits];

    unsigned int i;
    for (i = 0; i < maxppunits ; i++) ppUnits[i] = 0;

    for(const auto r : regions) {
        for(const auto o : r->objects) {
            for(const auto u : o->units) {
                i = u->num;
                if ((i > 0) && (i < maxppunits)) {
                    if (!ppUnits[i])
                        ppUnits[u->num] = u;
                    else {
                        logger::write("Error: Unit number " + std::to_string(i) + " multiply defined.");
                        if ((unitseq > 0) && (unitseq < maxppunits)) {
                            u->num = unitseq;
                            ppUnits[unitseq++] = u;
                        }
                    }
                } else {
                    logger::write("Error: Unit number " + std::to_string(i) + " out of range.");
                    if ((unitseq > 0) && (unitseq < maxppunits)) {
                        u->num = unitseq;
                        ppUnits[unitseq++] = u;
                    }
                }
            }
        }
    }
}

Unit *Game::GetNewUnit(Faction *fac, int an)
{
    unsigned int i;
    for (i = 1; i < unitseq; i++) {
        if (!ppUnits[i]) {
            Unit *pUnit = new Unit(i, fac, an);
            ppUnits[i] = pUnit;
            return(pUnit);
        }
    }

    Unit *pUnit = new Unit(unitseq, fac, an);
    ppUnits[unitseq] = pUnit;
    unitseq++;
    if (unitseq >= maxppunits) {
        Unit **temp = new Unit*[maxppunits+10000];
        memcpy(temp, ppUnits, maxppunits*sizeof(Unit *));
        maxppunits += 10000;
        delete[] ppUnits;
        ppUnits = temp;
    }

    return(pUnit);
}

Unit *Game::GetUnit(int num)
{
    if (num < 0 || (unsigned int)num >= maxppunits) return NULL;
    return(ppUnits[num]);
}


void Game::CountAllSpecialists()
{
    for(const auto f : factions) {
        f->nummages = 0;
        f->numqms = 0;
        f->numtacts = 0;
        f->numapprentices = 0;
    }
    for(const auto r : regions) {
        for(const auto o : r->objects) {
            for(const auto u : o->units) {
                if (u->type == U_MAGE) u->faction->nummages++;
                if (u->GetSkill(S_QUARTERMASTER)) u->faction->numqms++;
                if (u->GetSkill(S_TACTICS) == 5) u->faction->numtacts++;
                if (u->type == U_APPRENTICE) u->faction->numapprentices++;
            }
        }
    }
}

// LLS
void Game::UnitFactionMap()
{
    unsigned int i;
    Unit *u;

    logger::write("Opening units.txt");
    std::ofstream f("units.txt", std::ios::out|std::ios::ate);
    if (!f.is_open()) {
        logger::write("Couldn't open file!");
        return;
    }
    logger::write("Writing " + std::to_string(unitseq) + " units");
    for (i = 1; i < unitseq; i++) {
        u = GetUnit(i);
        if (!u) {
            logger::write("doesn't exist");
        } else {
            logger::write(std::to_string(i) + ":" + std::to_string(u->faction->num));
            f << i << ":" << u->faction->num << std::endl;
        }
    }
}

//The following function added by Creative PBM February 2000
void Game::RemoveInactiveFactions()
{
    if (Globals->MAX_INACTIVE_TURNS == -1) return;

    int cturn;
    cturn = TurnNumber();
    for(const auto fac : factions) {
        if ((cturn - fac->lastorders) >= Globals->MAX_INACTIVE_TURNS && !fac->is_npc) {
            fac->quit = QUIT_BY_GM;
        }
    }
}

int Game::CountMages(Faction *pFac)
{
    int i = 0;
    for(const auto r : regions) {
        for(const auto o : r->objects) {
            for(const auto u :o->units) {
                if (u->faction == pFac && u->type == U_MAGE) i++;
            }
        }
    }
    return(i);
}

int Game::CountQuarterMasters(Faction *pFac)
{
    int i = 0;
    for(const auto r : regions) {
        for(const auto o : r->objects) {
            for(const auto u : o->units) {
                if (u->faction == pFac && u->GetSkill(S_QUARTERMASTER)) i++;
            }
        }
    }
    return i;
}

int Game::CountTacticians(Faction *pFac)
{
    int i = 0;
    for(const auto r : regions) {
        for(const auto o : r->objects) {
            for(const auto u : o->units) {
                if (u->faction == pFac && u->GetSkill(S_TACTICS) == 5) i++;
            }
        }
    }
    return i;
}

int Game::CountApprentices(Faction *pFac)
{
    int i = 0;
    for(const auto r : regions) {
        for(const auto o : r->objects) {
            for(const auto u : o->units) {
                if (u->faction == pFac && u->type == U_APPRENTICE) i++;
            }
        }
    }
    return i;
}

int Game::AllowedMages(Faction *pFac)
{
    int points = pFac->type[F_MAGIC];

    if (points < 0) points = 0;
    if (points > allowedMagesSize - 1) points = allowedMagesSize - 1;

    return allowedMages[points];
}

int Game::AllowedQuarterMasters(Faction *pFac)
{
    int points = std::max(pFac->type[F_TRADE], pFac->type[F_MARTIAL]);

    if (points < 0) points = 0;
    if (points > allowedQuartermastersSize - 1)
        points = allowedQuartermastersSize - 1;

    return allowedQuartermasters[points];
}

int Game::AllowedTacticians(Faction *pFac)
{
    int points = std::max(pFac->type[F_WAR], pFac->type[F_MARTIAL]);

    if (points < 0) points = 0;
    if (points > allowedTacticiansSize - 1)
        points = allowedTacticiansSize - 1;

    return allowedTacticians[points];
}

int Game::AllowedApprentices(Faction *pFac)
{
    int points = pFac->type[F_MAGIC];

    if (points < 0) points = 0;
    if (points > allowedApprenticesSize - 1)
        points = allowedApprenticesSize - 1;

    return allowedApprentices[points];
}

int Game::AllowedTaxes(Faction *pFac)
{
    int points = std::max(pFac->type[F_WAR], pFac->type[F_MARTIAL]);

    if (points < 0) points = 0;
    if (points > allowedTaxesSize - 1) points = allowedTaxesSize - 1;

    return allowedTaxes[points];
}

int Game::AllowedTrades(Faction *pFac)
{
    int points = std::max(pFac->type[F_TRADE], pFac->type[F_MARTIAL]);

    if (points < 0) points = 0;
    if (points > allowedTradesSize - 1) points = allowedTradesSize - 1;

    return allowedTrades[points];
}

int Game::AllowedMartial(Faction *pFac)
{
    int points = pFac->type[F_MARTIAL];

    if (points < 0) points = 0;
    if (points > allowedMartialSize - 1) points = allowedMartialSize - 1;

    return allowedMartial[points];
}

bool Game::upgrade_major_version(int)
{
    return false;
}

bool Game::upgrade_minor_version(int)
{
    return true;
}

bool Game::upgrade_patch_level(int)
{
    return true;
}

void Game::MidProcessUnitExtra(ARegion *r, Unit *u)
{
    if (Globals->CHECK_MONSTER_CONTROL_MID_TURN) MonsterCheck(r, u);
}

void Game::PostProcessUnitExtra(ARegion *r, Unit *u)
{
    if (!Globals->CHECK_MONSTER_CONTROL_MID_TURN) MonsterCheck(r, u);
}

void Game::MonsterCheck(ARegion *r, Unit *u)
{
    std::string tmp;
    int skill;
    int linked = 0;
    std::map< int, int > chances;

    if (u->type != U_WMON) {

        for(auto it = u->items.begin(); it != u->items.end();) {
            Item *i = *it;
            // Since items can be removed below, we will increment the iterator here
            it++;

            if (!i->num) continue;
            if (!ItemDefs[i->type].escape) continue;

            // Okay, check flat loss.
            if (ItemDefs[i->type].escape & ItemType::LOSS_CHANCE) {
                int losses = rng::calculate_losses(i->num, ItemDefs[i->type].esc_val);
                // LOSS_CHANCE and HAS_SKILL together mean the
                // decay rate only applies if you don't have
                // the required skill (this might get used if
                // you made illusions GIVEable, for example).
                if (ItemDefs[i->type].escape & ItemType::HAS_SKILL) {
                    skill = lookup_skill(ItemDefs[i->type].esc_skill);
                    if (u->GetSkill(skill) >= ItemDefs[i->type].esc_val) losses = 0;
                }
                if (losses) {
                    std::string temp = item_string(i->type, losses) + strings::plural(losses, " decay", " decays") +
                        " into nothingness.";
                    u->event(temp, "decay");
                    u->items.SetNum(i->type,i->num - losses);
                }
            } else if (ItemDefs[i->type].escape & ItemType::HAS_SKILL) {
                skill = lookup_skill(ItemDefs[i->type].esc_skill);
                if (u->GetSkill(skill) < ItemDefs[i->type].esc_val) {
                    if (Globals->WANDERING_MONSTERS_EXIST) {
                        Faction *mfac = GetFaction(factions, monfaction);
                        Unit *mon = GetNewUnit(mfac, 0);
                        auto monster = find_monster(ItemDefs[i->type].abr, (ItemDefs[i->type].type & IT_ILLUSION))->get();
                        mon->MakeWMon(monster.name.c_str(), i->type, i->num);
                        mon->MoveUnit(r->GetDummy());
                        // This will be zero unless these are set. (0 means
                        // full spoils)
                        mon->free = Globals->MONSTER_NO_SPOILS + Globals->MONSTER_SPOILS_RECOVERY;
                    }
                    u->event("Loses control of " + item_string(i->type, i->num) + ".", "escape");
                    u->items.SetNum(i->type, 0);
                }
            } else {
                // ESC_LEV_*
                skill = lookup_skill(ItemDefs[i->type].esc_skill);
                int level = u->GetSkill(skill);
                int chance;

                if (!level) chance = 10000;
                else {
                    int top = (ItemDefs[i->type].escape & ItemType::ESC_NUM_SQUARE) ? i->num * i->num : i->num;
                    int bottom = 0;
                    if (ItemDefs[i->type].escape & ItemType::ESC_LEV_LINEAR) bottom = level;
                    else if (ItemDefs[i->type].escape & ItemType::ESC_LEV_SQUARE) bottom = level * level;
                    else if (ItemDefs[i->type].escape & ItemType::ESC_LEV_CUBE) bottom = level * level * level;
                    else if (ItemDefs[i->type].escape & ItemType::ESC_LEV_QUAD) bottom = level * level * level * level;
                    else bottom = 1;
                    bottom = bottom * ItemDefs[i->type].esc_val;
                    chance = (top * 10000)/bottom;
                }

                if (ItemDefs[i->type].escape & ItemType::LOSE_LINKED) {
                    if (chance > chances[ItemDefs[i->type].type]) chances[ItemDefs[i->type].type] = chance;
                    linked = 1;
                } else if (chance > rng::get_random(10000)) {
                    if (Globals->WANDERING_MONSTERS_EXIST) {
                        Faction *mfac = GetFaction(factions, monfaction);
                        Unit *mon = GetNewUnit(mfac, 0);
                        auto monster = find_monster(ItemDefs[i->type].abr, (ItemDefs[i->type].type & IT_ILLUSION))->get();
                        mon->MakeWMon(monster.name.c_str(), i->type, i->num);
                        mon->MoveUnit(r->GetDummy());
                        // This will be zero unless these are set. (0 means full spoils)
                        mon->free = Globals->MONSTER_NO_SPOILS + Globals->MONSTER_SPOILS_RECOVERY;
                    }
                    u->event("Loses control of " + item_string(i->type, i->num) + ".", "escape");
                    u->items.SetNum(i->type, 0);
                }
            }
        }

        if (linked) {
            for (auto i = chances.begin(); i != chances.end(); i++) {
                // walk the chances list and for each chance, see if
                // escape happens and if escape happens then walk all items
                // and everything that is that type, get rid of it.
                if (i->second < rng::get_random(10000)) continue;
                for(auto iter = u->items.begin(); iter != u->items.end();) {
                    Item *it = *iter;
                    // Since items can be removed below, we will increment the iterator here
                    iter++;
                    if (ItemDefs[it->type].type == i->first) {
                        if (Globals->WANDERING_MONSTERS_EXIST) {
                            Faction *mfac = GetFaction(factions, monfaction);
                            Unit *mon = GetNewUnit(mfac, 0);
                            auto monster = find_monster(ItemDefs[it->type].abr, (ItemDefs[it->type].type & IT_ILLUSION))->get();
                            mon->MakeWMon(monster.name.c_str(), it->type, it->num);
                            mon->MoveUnit(r->GetDummy());
                            // This will be zero unless these are set. (0 means full spoils)
                            mon->free = Globals->MONSTER_NO_SPOILS + Globals->MONSTER_SPOILS_RECOVERY;
                        }
                        u->event("Loses control of " + item_string(it->type, it->num) + ".", "escape");
                        u->items.SetNum(it->type, 0);
                    }
                }
            }
        }

    }
}

void Game::CheckUnitMaintenance(int consume)
{
    CheckUnitMaintenanceItem(I_FOOD, Globals->UPKEEP_FOOD_VALUE, consume);
    CheckUnitMaintenanceItem(I_GRAIN, Globals->UPKEEP_FOOD_VALUE, consume);
    CheckUnitMaintenanceItem(I_LIVESTOCK, Globals->UPKEEP_FOOD_VALUE, consume);
    CheckUnitMaintenanceItem(I_FISH, Globals->UPKEEP_FOOD_VALUE, consume);
}

void Game::CheckFactionMaintenance(int con)
{
    CheckFactionMaintenanceItem(I_FOOD, Globals->UPKEEP_FOOD_VALUE, con);
    CheckFactionMaintenanceItem(I_GRAIN, Globals->UPKEEP_FOOD_VALUE, con);
    CheckFactionMaintenanceItem(I_LIVESTOCK, Globals->UPKEEP_FOOD_VALUE, con);
    CheckFactionMaintenanceItem(I_FISH, Globals->UPKEEP_FOOD_VALUE, con);
}

void Game::CheckAllyMaintenance()
{
    CheckAllyMaintenanceItem(I_FOOD, Globals->UPKEEP_FOOD_VALUE);
    CheckAllyMaintenanceItem(I_GRAIN, Globals->UPKEEP_FOOD_VALUE);
    CheckAllyMaintenanceItem(I_LIVESTOCK, Globals->UPKEEP_FOOD_VALUE);
    CheckAllyMaintenanceItem(I_FISH, Globals->UPKEEP_FOOD_VALUE);
}

void Game::CheckUnitHunger()
{
    CheckUnitHungerItem(I_FOOD, Globals->UPKEEP_FOOD_VALUE);
    CheckUnitHungerItem(I_GRAIN, Globals->UPKEEP_FOOD_VALUE);
    CheckUnitHungerItem(I_LIVESTOCK, Globals->UPKEEP_FOOD_VALUE);
    CheckUnitHungerItem(I_FISH, Globals->UPKEEP_FOOD_VALUE);
}

void Game::CheckFactionHunger()
{
    CheckFactionHungerItem(I_FOOD, Globals->UPKEEP_FOOD_VALUE);
    CheckFactionHungerItem(I_GRAIN, Globals->UPKEEP_FOOD_VALUE);
    CheckFactionHungerItem(I_LIVESTOCK, Globals->UPKEEP_FOOD_VALUE);
    CheckFactionHungerItem(I_FISH, Globals->UPKEEP_FOOD_VALUE);
}

void Game::CheckAllyHunger()
{
    CheckAllyHungerItem(I_FOOD, Globals->UPKEEP_FOOD_VALUE);
    CheckAllyHungerItem(I_GRAIN, Globals->UPKEEP_FOOD_VALUE);
    CheckAllyHungerItem(I_LIVESTOCK, Globals->UPKEEP_FOOD_VALUE);
    CheckAllyHungerItem(I_FISH, Globals->UPKEEP_FOOD_VALUE);
}

char Game::GetRChar(ARegion *r)
{
    int t;

    if (!r)
        return ' ';
    t = r->type;
    if (t < 0 || t > R_NUM) return '?';
    char c = TerrainDefs[r->type].marker;
    if (r->town) {
        c = (c - 'a') + 'A';
    }
    return c;
}

void Game::CreateNPCFactions()
{
    Faction *f;
    if (Globals->CITY_MONSTERS_EXIST) {
        f = new Faction(factionseq++);
        guardfaction = f->num;
        f->set_name("The Guardsmen");
        f->is_npc = true;
        f->lastorders = 0;
        factions.push_back(f);
    } else guardfaction = 0;
    // Only create the monster faction if wandering monsters or lair
    // monsters exist.
    if (Globals->LAIR_MONSTERS_EXIST || Globals->WANDERING_MONSTERS_EXIST) {
        f = new Faction(factionseq++);
        monfaction = f->num;
        f->set_name("Creatures");
        f->is_npc = true;
        f->lastorders = 0;
        factions.push_back(f);
    } else monfaction = 0;
}

void Game::CreateCityMon(ARegion *region, int percent, int needmage)
{
    int skilllevel;
    int AC = 0;
    int IV = 0;
    int num;
    if (region->type == R_NEXUS || region->IsStartingCity()) {
        skilllevel = TOWN_CITY + 1;
        if (Globals->SAFE_START_CITIES || (region->type == R_NEXUS))
            IV = 1;
        AC = 1;
        num = Globals->AMT_START_CITY_GUARDS;
    } else {
        skilllevel = region->town->TownType() + 1;
        num = Globals->CITY_GUARD * skilllevel;
    }
    num = num * percent / 100;
    Faction *fac = GetFaction(factions, guardfaction);
    Unit *u = GetNewUnit(fac);
    Unit *u2;

    if ((Globals->LEADERS_EXIST) || (region->type == R_NEXUS)) {
        /* standard Leader-type guards */
        u->SetMen(I_LEADERS,num);
        u->items.SetNum(I_SWORD,num);
        if (IV) u->items.SetNum(I_AMULETOFI,num);
        u->SetMoney(num * Globals->GUARD_MONEY);
        u->SetSkill(S_COMBAT,skilllevel);
        u->set_name("City Guard");
        u->type = U_GUARD;
        u->guard = GUARD_GUARD;
        u->reveal = REVEAL_FACTION;
    } else {
        /* non-leader guards */
        int n = 3 * num / 4;
        int plate = 0;
        if ((AC) && (Globals->START_CITY_GUARDS_PLATE)) plate = 1;
        u = MakeManUnit(fac, region->race, n, skilllevel, 1, plate, 0);
        if (IV) u->items.SetNum(I_AMULETOFI,num);
        u->SetMoney(num * Globals->GUARD_MONEY / 2);
        u->set_name("City Guard");
        u->type = U_GUARD;
        u->guard = GUARD_GUARD;
        u->reveal = REVEAL_FACTION;
        u2 = MakeManUnit(fac, region->race, n, skilllevel, 1, plate, 1);
        if (IV) u2->items.SetNum(I_AMULETOFI,num);
        u2->SetMoney(num * Globals->GUARD_MONEY / 2);
        u2->set_name("City Guard");
        u2->type = U_GUARD;
        u2->guard = GUARD_GUARD;
        u2->reveal = REVEAL_FACTION;
    }

    if (AC) {
        if (Globals->START_CITY_GUARDS_PLATE) {
            if (Globals->LEADERS_EXIST) u->items.SetNum(I_PLATEARMOR, num);
        }
        u->SetSkill(S_OBSERVATION,10);
        if (Globals->START_CITY_TACTICS)
            u->SetSkill(S_TACTICS, Globals->START_CITY_TACTICS);
    } else {
        u->SetSkill(S_OBSERVATION, skilllevel);
    }
    u->SetFlag(FLAG_HOLDING,1);
    u->MoveUnit(region->GetDummy());
    if ((!Globals->LEADERS_EXIST) && (region->type != R_NEXUS)) {
        u2->SetFlag(FLAG_HOLDING,1);
        u2->MoveUnit(region->GetDummy());
    }

    if (AC && Globals->START_CITY_MAGES && needmage) {
        u = GetNewUnit(fac);
        u->set_name("City Mage");
        u->type = U_GUARDMAGE;
        u->reveal = REVEAL_FACTION;
        u->SetMen(I_LEADERS,1);
        if (IV) u->items.SetNum(I_AMULETOFI,1);
        u->SetMoney(Globals->GUARD_MONEY);
        u->SetSkill(S_FORCE,Globals->START_CITY_MAGES);
        u->SetSkill(S_FIRE,Globals->START_CITY_MAGES);
        if (Globals->START_CITY_TACTICS) u->SetSkill(S_TACTICS, Globals->START_CITY_TACTICS);
        u->combat = S_FIRE;
        u->SetFlag(FLAG_BEHIND, 1);
        u->SetFlag(FLAG_HOLDING, 1);
        u->MoveUnit(region->GetDummy());
    }
}

void Game::AdjustCityMons(ARegion *r)
{
    int needguard = 1;
    int needmage = 1;
    for(const auto o : r->objects) {
        for(const auto u : o->units) {
            if (u->type == U_GUARD || u->type == U_GUARDMAGE) {
                AdjustCityMon(r, u);
                /* Don't create new city guards if we have some */
                needguard = 0;
                if (u->type == U_GUARDMAGE)
                    needmage = 0;
            }
            if (u->guard == GUARD_GUARD) needguard = 0;
        }
    }

    if (needguard && (rng::get_random(100) < Globals->GUARD_REGEN)) {
        CreateCityMon(r, 10, needmage);
    }
}

void Game::AdjustCityMon(ARegion *r, Unit *u)
{
    int towntype;
    int AC = 0;
    int men;
    int IV = 0;
    int mantype;
    int maxmen;
    int weapon = -1;
    int maxweapon = 0;
    int armor = -1;
    int maxarmor = 0;
    for (int i=0; i<NITEMS; i++) {
        int num = u->items.GetNum(i);
        if (num == 0) continue;
        if (ItemDefs[i].type & IT_MAN) mantype = i;
        if ((ItemDefs[i].type & IT_WEAPON)
            && (num > maxweapon)) {
            weapon = i;
            maxweapon = num;
        }
        if ((ItemDefs[i].type & IT_ARMOR)
            && (num > maxarmor)) {
            armor = i;
            maxarmor = num;
        }
    }
    int skill = S_COMBAT;

    if (weapon != -1) {
        auto weapon_def = find_weapon(ItemDefs[weapon].abr)->get();
        auto pS = FindSkill(weapon_def.baseSkill)->get();
        if (pS == FindSkill("XBOW")->get()) skill = S_CROSSBOW;
        if (pS == FindSkill("LBOW")->get()) skill = S_LONGBOW;
    }

    int sl = u->GetRealSkill(skill);

    if (r->type == R_NEXUS || r->IsStartingCity()) {
        towntype = TOWN_CITY;
        AC = 1;
        if (Globals->SAFE_START_CITIES || (r->type == R_NEXUS))
            IV = 1;
        if (u->type == U_GUARDMAGE) {
            men = 1;
        } else {
            maxmen = Globals->AMT_START_CITY_GUARDS;
            if ((!Globals->LEADERS_EXIST) && (r->type != R_NEXUS))
                maxmen = 3 * maxmen / 4;
            men = u->GetMen() + (Globals->AMT_START_CITY_GUARDS/10);
            if (men > maxmen)
                men = maxmen;
        }
    } else {
        towntype = r->town->TownType();
        maxmen = Globals->CITY_GUARD * (towntype+1);
        if (!Globals->LEADERS_EXIST) maxmen = 3 * maxmen / 4;
        men = u->GetMen() + (maxmen/10);
        if (men > maxmen)
            men = maxmen;
    }

    u->SetMen(mantype,men);
    if (IV) u->items.SetNum(I_AMULETOFI,men);

    if (u->type == U_GUARDMAGE) {
        if (Globals->START_CITY_TACTICS)
            u->SetSkill(S_TACTICS, Globals->START_CITY_TACTICS);
        u->SetSkill(S_FORCE, Globals->START_CITY_MAGES);
        u->SetSkill(S_FIRE, Globals->START_CITY_MAGES);
        u->combat = S_FIRE;
        u->SetFlag(FLAG_BEHIND, 1);
        u->SetMoney(Globals->GUARD_MONEY);
    } else {
        int money = men * (Globals->GUARD_MONEY * men / maxmen);
        u->SetMoney(money);
        u->SetSkill(skill, sl);
        if (AC) {
            u->SetSkill(S_OBSERVATION,10);
            if (Globals->START_CITY_TACTICS)
                u->SetSkill(S_TACTICS, Globals->START_CITY_TACTICS);
            if (Globals->START_CITY_GUARDS_PLATE)
                u->items.SetNum(armor,men);
        } else {
            u->SetSkill(S_OBSERVATION,towntype + 1);
        }
        if (weapon!= -1) {
            u->items.SetNum(weapon,men);
        }
    }
}

void Game::Equilibrate()
{
    logger::write("Initialising the economy");
    for (int a=0; a<25; a++) {
        logger::dot();
        ProcessMigration();
        for(const auto r : regions) {
            r->PostTurn();
        }
    }
    logger::write("");
}

void Game::write_times_article(std::string article)
{
    std::string fname;

    do { fname = "times." + std::to_string(rng::get_random(10000)); } while (std::filesystem::exists(fname));
    std::ofstream f(fname, std::ios::out | std::ios::trunc);
    if (f.is_open()) {
        f << indent::wrap(78,70,0) << article << '\n';
    }
}

SimulationResult Game::simulate_battle(const json& document)
{
    validate_battle_document(document);

    ARegionList battle_regions;
    battle_regions.create_battleground_world();

    ARegion *battle_region = battle_regions.GetRegion(0, 0, 0);

    Faction attacker_faction(3);
    attacker_faction.set_name("Attacker");

    Faction defender_faction(4);
    defender_faction.set_name("Defender");

    int next_unit_number = 1;
    std::vector<std::unique_ptr<Location>> attacker_owned_locations;
    std::vector<std::unique_ptr<Location>> defender_owned_locations;
    std::list<Location *> attackers;
    std::list<Location *> defenders;
    Unit *attacker = nullptr;
    Unit *defender = nullptr;

    build_simulation_side(
        document.at("attackers"),
        &attacker_faction,
        battle_region,
        next_unit_number,
        attacker_owned_locations,
        attackers,
        attacker
    );
    build_simulation_side(
        document.at("defenders"),
        &defender_faction,
        battle_region,
        next_unit_number,
        defender_owned_locations,
        defenders,
        defender
    );

    Battle battle_simulation;
    battle_simulation.WriteSides(battle_region, attacker, defender, attackers, defenders, 0);

    Events simulation_events;
    std::unique_ptr<BattleDetails> details = std::make_unique<BattleDetails>();

    SimulationResult result;
    result.result = battle_simulation.Run(
        &simulation_events,
        battle_region,
        attacker,
        attackers,
        defender,
        defenders,
        0,
        details.get()
    );

    for (const auto& line : battle_simulation.text) {
        result.battleText += line;
        result.battleText += "\n";
    }

    result.details = details.release();
    return result;
}

json Game::summarize_battle_simulation(const json& document, int rounds)
{
    validate_battle_document(document);

    if (rounds <= 0) {
        throw std::runtime_error("Battle simulation rounds must be positive.");
    }

    int wins = 0;
    int loses = 0;
    int draws = 0;

    std::vector<int> attacker_losses;
    std::vector<int> defender_losses;
    std::map<int, std::vector<int>> spoils;

    std::string victory_battle_text;
    std::string loss_battle_text;
    std::string draw_battle_text;

    for (int round = 0; round < rounds; round++) {
        SimulationResult simulation = simulate_battle(document);
        std::unique_ptr<BattleDetails> details(simulation.details);

        if (!details) {
            throw std::runtime_error("Battle simulation did not produce statistics.");
        }

        attacker_losses.push_back(details->attackerLost);
        defender_losses.push_back(details->defenderLost);

        for (auto& [item, values] : spoils) {
            values.push_back(0);
        }

        for (const auto& [item, amount] : details->spoils) {
            auto [it, inserted] = spoils.try_emplace(item, round + 1, 0);
            it->second.back() = amount;
            (void) inserted;
        }

        if (simulation.result == BATTLE_WON) {
            wins++;
            if (victory_battle_text.empty()) {
                victory_battle_text = simulation.battleText;
            }
        } else if (simulation.result == BATTLE_LOST) {
            loses++;
            if (loss_battle_text.empty()) {
                loss_battle_text = simulation.battleText;
            }
        } else if (simulation.result == BATTLE_DRAW) {
            draws++;
            if (draw_battle_text.empty()) {
                draw_battle_text = simulation.battleText;
            }
        } else {
            throw std::runtime_error("Battle simulation could not resolve a winner.");
        }
    }

    json summary = {
        {"wins", wins},
        {"loses", loses},
        {"draws", draws},
        {"winRatio", (static_cast<double>(wins) / rounds) * 100.0},
        {"attackerLooses", stat_to_json(calculate_stat(attacker_losses))},
        {"defenderLooses", stat_to_json(calculate_stat(defender_losses))},
    };

    if (!victory_battle_text.empty()) summary["victoryBattleText"] = victory_battle_text;
    if (!loss_battle_text.empty()) summary["lossBattleText"] = loss_battle_text;
    if (!draw_battle_text.empty()) summary["drawBattleText"] = draw_battle_text;

    summary["spoils"] = json::array();
    for (const auto& [item, values] : spoils) {
        json spoil_stats = stat_to_json(calculate_stat(values));
        spoil_stats["item"] = ItemDefs[item].names;
        summary["spoils"].push_back(spoil_stats);
    }

    return summary;
}

int Game::run_battle_simulation(const std::string& input_json_filename, int rounds)
{
    std::ifstream json_file(input_json_filename);
    if (!json_file.is_open()) {
        std::cout << "No such file found " << input_json_filename << "!" << std::endl;
        return 0;
    }

    try {
        json document;
        json_file >> document;

        rng::seed_random();
        std::cout << summarize_battle_simulation(document, rounds).dump();
    } catch (const std::exception& ex) {
        std::cout << ex.what() << std::endl;
        return 0;
    }

    return 1;
}

int Game::export_game_data()
{
    auto write_json_file = [](const std::string& filename, const json& document) {
        std::ofstream output(filename, std::ios::out | std::ios::trunc);
        if (!output.is_open()) {
            return false;
        }

        output << document.dump();
        return true;
    };

    json items = json::array();
    for (int i = 0; i < NITEMS; i++) {
        if (ItemDefs[i].flags & ItemType::DISABLED) continue;

        if (!find_battle_item(ItemDefs[i].abr)
            && !find_armor(ItemDefs[i].abr)
            && !find_weapon(ItemDefs[i].abr)
            && !find_mount(ItemDefs[i].abr)
            && !find_monster(ItemDefs[i].abr, 0)
            && !find_race(ItemDefs[i].abr)
            && ItemDefs[i].abr != "HPOT"
            && ItemDefs[i].abr != "HERB"
            && ItemDefs[i].abr != "STAH"
            && !(ItemDefs[i].flags & ItemType::MANPRODUCE)) {
            continue;
        }

        if (ItemDefs[i].type & IT_ILLUSION) {
            items.push_back({
                {"abbr", "i" + ItemDefs[i].abr},
                {"name", "illusory " + ItemDefs[i].name},
                {"category", "illusion"},
            });
        } else {
            items.push_back({
                {"abbr", ItemDefs[i].abr},
                {"name", ItemDefs[i].name},
                {"category", get_item_category(ItemDefs[i].type)},
            });
        }
    }

    json skills = json::array();
    for (int i = 0; i < NSKILLS; i++) {
        if (SkillDefs[i].flags & SkillType::DISABLED) continue;

        if (!(SkillDefs[i].flags & SkillType::COMBAT)
            && !(SkillDefs[i].flags & SkillType::BATTLEREP)
            && SkillDefs[i].abbr != "MHEA"
            && SkillDefs[i].abbr != "HEAL") {
            continue;
        }

        json skill = {
            {"abbr", SkillDefs[i].abbr},
            {"name", SkillDefs[i].name},
        };

        if (SkillDefs[i].flags & SkillType::COMBAT) {
            skill["combatSpell"] = true;
        }

        skills.push_back(skill);
    }

    json objects = json::array();
    for (int i = 0; i < NOBJECTS; i++) {
        if (ObjectDefs[i].flags & ObjectType::DISABLED) continue;
        if (!ObjectDefs[i].protect) continue;

        objects.push_back(ObjectDefs[i].name);
    }

    if (!write_json_file("items.json", items)) return 0;
    if (!write_json_file("skills.json", skills)) return 0;
    if (!write_json_file("objects.json", objects)) return 0;

    return 1;
}


void Game::CountItems(size_t ** citems)
{
    for(const auto fac : factions) {
        if (!fac->is_npc) {
            int i = fac->num - 1; // faction numbers are 1-based
            for (int j = 0; j < NITEMS; j++){
                citems[i][j] = CountItem (fac, j);
            }
        }
    }
}

int Game::CountItem (Faction *fac, int item)
{
    if (ItemDefs[item].type & IT_SHIP) return 0;

    size_t all = 0;
    for (const auto& r : fac->present_regions) {
        for(const auto obj : r->objects) {
            for(const auto unit : obj->units) {
                if (unit->faction == fac) all += unit->items.GetNum(item);
            }
        }
    }
    return all;
}
