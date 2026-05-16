#include "../external/boost/ut.hpp"
#include "../external/nlohmann/json.hpp"

#include "../game.h"
#include "../gamedata.h"
#include "../rng.hpp"
#include "testhelper.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>

using json = nlohmann::json;
namespace ut = boost::ut;

namespace {

json make_non_report_style_battle_document()
{
    return json{
        {"attackers", {
            {"units", json::array({
                {
                    {"name", "Legacy Attacker"},
                    {"flags", json::array({"behind"})},
                    {"items", json::array({
                        {{"abbr", ItemDefs[I_LEADERS].abr}, {"amount", 10}},
                        {{"abbr", ItemDefs[I_SWORD].abr}, {"amount", 10}}
                    })},
                    {"skills", json::array({
                        {{"abbr", SkillDefs[S_COMBAT].abbr}, {"level", 2}}
                    })},
                    {"combatSpell", SkillDefs[S_FIRE].abbr}
                }
            })}
        }},
        {"defenders", {
            {"structure", {{"type", ObjectDefs[O_TOWER].name}}},
            {"units", json::array({
                {
                    {"name", "Legacy Defender"},
                    {"items", json::array({
                        {{"abbr", ItemDefs[I_LEADERS].abr}, {"amount", 8}},
                        {{"abbr", ItemDefs[I_SWORD].abr}, {"amount", 8}}
                    })},
                    {"skills", json::array({
                        {{"abbr", SkillDefs[S_COMBAT].abbr}, {"level", 1}}
                    })}
                }
            })}
        }}
    };
}

json make_report_style_battle_document()
{
    return json{
        {"attackers", {
            {"units", json::array({
                {
                    {"name", "Normalized Attacker"},
                    {"flags", {{"behind", true}}},
                    {"items", json::array({
                        {{"tag", ItemDefs[I_LEADERS].abr}, {"amount", 6}},
                        {{"tag", ItemDefs[I_SWORD].abr}, {"amount", 6}}
                    })},
                    {"skills", {
                        {"known", json::array({
                            {{"tag", SkillDefs[S_COMBAT].abbr}, {"level", 2}, {"skill_days", 60}}
                        })}
                    }},
                    {"combat_spell", {{"name", SkillDefs[S_FIRE].name}, {"tag", SkillDefs[S_FIRE].abbr}}},
                    {"readied", {
                        {"weapons", json::array({
                            {{"name", ItemDefs[I_SWORD].name}, {"tag", ItemDefs[I_SWORD].abr}}
                        })}
                    }}
                }
            })}
        }},
        {"defenders", {
            {"structures", json::array({
                {
                    {"name", "Tower"},
                    {"type", ObjectDefs[O_TOWER].name},
                    {"units", json::array({
                        {
                            {"name", "Report Defender"},
                            {"items", json::array({
                                {{"name", ItemDefs[I_LEADERS].name}, {"tag", ItemDefs[I_LEADERS].abr}, {"amount", 6}},
                                {{"name", ItemDefs[I_SWORD].name}, {"tag", ItemDefs[I_SWORD].abr}, {"amount", 6}}
                            })},
                            {"skills", {
                                {"known", json::array({
                                    {{"name", SkillDefs[S_COMBAT].name}, {"tag", SkillDefs[S_COMBAT].abbr}, {"level", 1}, {"skill_days", 30}}
                                })}
                            }}
                        }
                    })}
                }
            })}
        }}
    };
}

struct WorkingDirectoryGuard {
    explicit WorkingDirectoryGuard(const std::filesystem::path& path)
        : original(std::filesystem::current_path())
    {
        std::filesystem::current_path(path);
    }

    ~WorkingDirectoryGuard()
    {
        std::error_code ignored;
        std::filesystem::current_path(original, ignored);
    }

    std::filesystem::path original;
};

} // namespace

ut::suite<"Battle Simulator"> battle_simulator_suite = []
{
    using namespace ut;

    "Report style battle simulator input produces an aggregated summary"_test = []
    {
        UnitTestHelper helper;
        helper.initialize_game();
        helper.setup_turn();
        helper.enable(UnitTestHelper::ITEM, I_SWORD, true);
        helper.enable(UnitTestHelper::SKILL, S_FIRE, true);
        helper.enable(UnitTestHelper::OBJECT, O_TOWER, true);

        rng::seed_random(0xdeadbeef);

        json summary = helper.game_object().summarize_battle_simulation(make_report_style_battle_document(), 3);

        int outcomes = summary["wins"].get<int>() + summary["loses"].get<int>() + summary["draws"].get<int>();

        expect(outcomes == 3_i);
        expect(summary["attackerLooses"]["percentile"].size() == 10_ul);
        expect(summary["defenderLooses"]["percentile"].size() == 10_ul);
        expect(summary.contains("spoils"));
        expect(summary["spoils"].is_array());
        expect(
            summary.contains("victoryBattleText")
            || summary.contains("lossBattleText")
            || summary.contains("drawBattleText")
        );
    };

    "Non report style battle simulator input is rejected"_test = []
    {
        UnitTestHelper helper;
        helper.initialize_game();
        helper.setup_turn();
        helper.enable(UnitTestHelper::ITEM, I_SWORD, true);
        helper.enable(UnitTestHelper::SKILL, S_FIRE, true);
        helper.enable(UnitTestHelper::OBJECT, O_TOWER, true);

        bool threw = false;
        try {
            (void) helper.game_object().simulate_battle(make_non_report_style_battle_document());
        } catch (const std::runtime_error&) {
            threw = true;
        }

        expect(threw);
    };

    "Report style battle simulator input returns battle details"_test = []
    {
        UnitTestHelper helper;
        helper.initialize_game();
        helper.setup_turn();
        helper.enable(UnitTestHelper::ITEM, I_SWORD, true);
        helper.enable(UnitTestHelper::SKILL, S_FIRE, true);
        helper.enable(UnitTestHelper::OBJECT, O_TOWER, true);

        rng::seed_random(0x12345678);

        SimulationResult result = helper.game_object().simulate_battle(make_report_style_battle_document());

        expect(result.result != BATTLE_IMPOSSIBLE);
        expect(result.details != nullptr);
        expect(!result.battleText.empty());
        if (result.details) {
            expect(result.details->attackerLost >= 0);
            expect(result.details->defenderLost >= 0);
        }

        delete result.details;
    };

    "Export data writes items skills and objects json files"_test = []
    {
        UnitTestHelper helper;
        helper.initialize_game();
        helper.setup_turn();
        helper.enable(UnitTestHelper::OBJECT, O_TOWER, true);

        auto temp_dir = std::filesystem::temp_directory_path() /
            ("atlantis-export-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(temp_dir);

        json items;
        json skills;
        json objects;

        {
            WorkingDirectoryGuard guard(temp_dir);
            expect(helper.game_object().export_game_data() == 1_i);

            std::ifstream items_file("items.json");
            std::ifstream skills_file("skills.json");
            std::ifstream objects_file("objects.json");

            expect(items_file.is_open());
            expect(skills_file.is_open());
            expect(objects_file.is_open());

            if (items_file.is_open()) items_file >> items;
            if (skills_file.is_open()) skills_file >> skills;
            if (objects_file.is_open()) objects_file >> objects;
        }

        expect(items.is_array());
        expect(skills.is_array());
        expect(objects.is_array());
        expect(!items.empty());
        expect(!skills.empty());
        expect(!objects.empty());
        expect(std::any_of(items.begin(), items.end(), [](const json& item) { return item.contains("category"); }));
        expect(std::any_of(skills.begin(), skills.end(), [](const json& skill) {
            return skill.contains("abbr") && skill.contains("name");
        }));

        std::error_code ignored;
        std::filesystem::remove_all(temp_dir, ignored);
    };
};
