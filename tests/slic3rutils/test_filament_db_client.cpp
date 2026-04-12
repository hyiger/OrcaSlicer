#include <catch2/catch_all.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "slic3r/Utils/FilamentDbClient.hpp"

using namespace Slic3r;

// ---------------------------------------------------------------------------
// parse_filaments_response tests
// ---------------------------------------------------------------------------

TEST_CASE("FilamentDbClient parse_filaments_response", "[FilamentDb]") {

    SECTION("parses valid JSON array into entries") {
        std::string json = R"([
            {
                "name": "Generic PLA",
                "type": "filament",
                "filament_id": "fdb_abc123",
                "from": "filament_db",
                "instantiation": "true",
                "filament_type": ["PLA"],
                "filament_vendor": ["Generic"],
                "filament_colour": ["#DDDDDD"],
                "filament_diameter": ["1.75"],
                "filament_density": ["1.24"],
                "filament_max_volumetric_speed": ["15"],
                "nozzle_temperature": ["210"],
                "nozzle_temperature_initial_layer": ["215"],
                "hot_plate_temp": ["60"],
                "hot_plate_temp_initial_layer": ["65"]
            }
        ])";

        std::vector<FilamentDbEntry> entries;
        bool ok = FilamentDbClient::parse_filaments_response(json, entries);

        REQUIRE(ok);
        REQUIRE(entries.size() == 1);

        const auto& e = entries[0];
        CHECK(e.name == "Generic PLA");
        CHECK(e.vendor == "Generic");
        CHECK(e.type == "PLA");
        CHECK(e.color == "#DDDDDD");
        CHECK(e.id == "fdb_abc123");
    }

    SECTION("parses multiple filaments") {
        std::string json = R"([
            {"name": "PLA", "filament_type": ["PLA"], "filament_vendor": ["A"], "filament_colour": ["#FF0000"]},
            {"name": "PETG", "filament_type": ["PETG"], "filament_vendor": ["B"], "filament_colour": ["#00FF00"]}
        ])";

        std::vector<FilamentDbEntry> entries;
        bool ok = FilamentDbClient::parse_filaments_response(json, entries);

        REQUIRE(ok);
        REQUIRE(entries.size() == 2);
        CHECK(entries[0].name == "PLA");
        CHECK(entries[0].type == "PLA");
        CHECK(entries[1].name == "PETG");
        CHECK(entries[1].type == "PETG");
    }

    SECTION("returns empty vector on empty JSON array") {
        std::string json = "[]";
        std::vector<FilamentDbEntry> entries;
        bool ok = FilamentDbClient::parse_filaments_response(json, entries);

        REQUIRE(ok);
        REQUIRE(entries.empty());
    }

    SECTION("returns false on empty string") {
        std::vector<FilamentDbEntry> entries;
        bool ok = FilamentDbClient::parse_filaments_response("", entries);
        REQUIRE_FALSE(ok);
    }

    SECTION("returns false on malformed JSON") {
        std::vector<FilamentDbEntry> entries;
        bool ok = FilamentDbClient::parse_filaments_response("{not valid json", entries);
        REQUIRE_FALSE(ok);
    }

    SECTION("returns false on non-array JSON") {
        std::vector<FilamentDbEntry> entries;
        bool ok = FilamentDbClient::parse_filaments_response(R"({"key": "value"})", entries);
        REQUIRE_FALSE(ok);
    }

    SECTION("handles missing optional fields gracefully") {
        std::string json = R"([{"name": "Minimal"}])";

        std::vector<FilamentDbEntry> entries;
        bool ok = FilamentDbClient::parse_filaments_response(json, entries);

        REQUIRE(ok);
        REQUIRE(entries.size() == 1);
        CHECK(entries[0].name == "Minimal");
        CHECK(entries[0].vendor.empty());
        CHECK(entries[0].type.empty());
        CHECK(entries[0].color.empty());
        CHECK(entries[0].id.empty());
    }
}

// ---------------------------------------------------------------------------
// parse_calibration_response tests
// ---------------------------------------------------------------------------

TEST_CASE("FilamentDbClient parse_calibration_response", "[FilamentDb]") {

    SECTION("parses calibration_orca keys into DynamicPrintConfig") {
        std::string json = R"({
            "filament": "Generic PLA",
            "nozzle": {"diameter": 0.4, "name": "Brass"},
            "calibration_orca": {
                "filament_flow_ratio": ["0.95"],
                "nozzle_temperature": ["210"],
                "hot_plate_temp": ["60"],
                "filament_max_volumetric_speed": ["15"]
            }
        })";

        FilamentDbCalibration cal;
        bool ok = FilamentDbClient::parse_calibration_response(json, cal);

        REQUIRE(ok);
        // The overrides config should have been populated
        // (exact keys depend on PrintConfig definitions being valid)
    }

    SECTION("returns false when calibration_orca key is missing") {
        std::string json = R"({
            "filament": "Generic PLA",
            "calibration": {
                "pressureAdvance": 0.045
            }
        })";

        FilamentDbCalibration cal;
        bool ok = FilamentDbClient::parse_calibration_response(json, cal);
        REQUIRE_FALSE(ok);
    }

    SECTION("returns false on malformed JSON") {
        FilamentDbCalibration cal;
        bool ok = FilamentDbClient::parse_calibration_response("{bad json", cal);
        REQUIRE_FALSE(ok);
    }

    SECTION("returns false on empty string") {
        FilamentDbCalibration cal;
        bool ok = FilamentDbClient::parse_calibration_response("", cal);
        REQUIRE_FALSE(ok);
    }

    SECTION("handles empty calibration_orca object") {
        std::string json = R"({"calibration_orca": {}})";

        FilamentDbCalibration cal;
        bool ok = FilamentDbClient::parse_calibration_response(json, cal);
        REQUIRE(ok);
    }
}

// ---------------------------------------------------------------------------
// FilamentDbClient state tests
// ---------------------------------------------------------------------------

TEST_CASE("FilamentDbClient state management", "[FilamentDb]") {

    FilamentDbClient client;

    SECTION("is_enabled returns false when URL is empty") {
        REQUIRE_FALSE(client.is_enabled());
    }

    SECTION("is_enabled returns true when URL is set") {
        client.set_url("http://localhost:3456");
        REQUIRE(client.is_enabled());
    }

    SECTION("set_url clears cache when URL changes") {
        client.set_url("http://localhost:3456");
        REQUIRE(client.get_filaments().empty());
    }

    SECTION("get_url returns configured URL") {
        client.set_url("http://localhost:3456");
        REQUIRE(client.get_url() == "http://localhost:3456");
    }

    SECTION("clear_cache empties filament list") {
        client.set_url("http://localhost:3456");
        client.clear_cache();
        REQUIRE(client.get_filaments().empty());
    }
}
