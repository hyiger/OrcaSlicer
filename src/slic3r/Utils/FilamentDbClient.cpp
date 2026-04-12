#include "FilamentDbClient.hpp"
#include "Http.hpp"

#include "nlohmann/json.hpp"

#include <boost/log/trivial.hpp>

using json = nlohmann::json;

namespace Slic3r {

// ---------------------------------------------------------------------------
// URL helpers
// ---------------------------------------------------------------------------

static std::string strip_trailing_slash(const std::string& url)
{
    if (!url.empty() && url.back() == '/')
        return url.substr(0, url.size() - 1);
    return url;
}

// ---------------------------------------------------------------------------
// JSON → DynamicPrintConfig helpers
// ---------------------------------------------------------------------------

/// Apply a single OrcaSlicer key-value pair to a DynamicPrintConfig.
/// Values from the API are string arrays like ["210"]. We take the first
/// element and use set_deserialize_strict() which handles type conversion.
static void apply_json_value(DynamicPrintConfig& config,
                             const std::string& key,
                             const json& value)
{
    // Skip metadata keys that are not print config options
    static const std::set<std::string> skip_keys = {
        "name", "type", "filament_id", "from", "instantiation"
    };
    if (skip_keys.count(key))
        return;

    // The print config definition tells us if this key is valid
    const ConfigDef* def = config.def();
    if (!def || !def->has(key))
        return;

    try {
        std::string str_value;
        if (value.is_array() && !value.empty()) {
            str_value = value[0].is_string() ? value[0].get<std::string>()
                                              : value[0].dump();
        } else if (value.is_string()) {
            str_value = value.get<std::string>();
        } else {
            str_value = value.dump();
        }

        config.set_deserialize_strict(key, str_value);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(debug) << "FilamentDbClient: skipping key '"
                                 << key << "': " << e.what();
    }
}

// ---------------------------------------------------------------------------
// parse_filaments_response
// ---------------------------------------------------------------------------

bool FilamentDbClient::parse_filaments_response(
    const std::string& json_body,
    std::vector<FilamentDbEntry>& out)
{
    if (json_body.empty())
        return false;

    try {
        json root = json::parse(json_body);
        if (!root.is_array())
            return false;

        out.clear();
        out.reserve(root.size());

        for (const auto& item : root) {
            FilamentDbEntry entry;

            // Extract metadata
            entry.name   = item.value("name", "");
            entry.vendor = "";
            entry.type   = "";
            entry.color  = "";
            entry.id     = "";

            if (item.contains("filament_id")) {
                const auto& fid = item["filament_id"];
                entry.id = fid.is_string() ? fid.get<std::string>() : "";
            }

            // Extract vendor/type/color from array-valued slicer keys
            if (item.contains("filament_vendor") && item["filament_vendor"].is_array()
                && !item["filament_vendor"].empty()) {
                entry.vendor = item["filament_vendor"][0].get<std::string>();
            }
            if (item.contains("filament_type") && item["filament_type"].is_array()
                && !item["filament_type"].empty()) {
                entry.type = item["filament_type"][0].get<std::string>();
            }
            if (item.contains("filament_colour") && item["filament_colour"].is_array()
                && !item["filament_colour"].empty()) {
                entry.color = item["filament_colour"][0].get<std::string>();
            }

            // Build DynamicPrintConfig from all key-value pairs
            for (const auto& [key, value] : item.items()) {
                apply_json_value(entry.config, key, value);
            }

            out.push_back(std::move(entry));
        }

        return true;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "FilamentDbClient: failed to parse filaments JSON: "
                                 << e.what();
        return false;
    }
}

// ---------------------------------------------------------------------------
// parse_calibration_response
// ---------------------------------------------------------------------------

bool FilamentDbClient::parse_calibration_response(
    const std::string& json_body,
    FilamentDbCalibration& out)
{
    if (json_body.empty())
        return false;

    try {
        json root = json::parse(json_body);

        if (!root.contains("calibration_orca"))
            return false;

        const json& cal = root["calibration_orca"];
        if (!cal.is_object())
            return false;

        out.overrides = DynamicPrintConfig();
        for (const auto& [key, value] : cal.items()) {
            apply_json_value(out.overrides, key, value);
        }

        return true;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "FilamentDbClient: failed to parse calibration JSON: "
                                 << e.what();
        return false;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool FilamentDbClient::fetch_filaments(const std::string& base_url)
{
    std::string url = strip_trailing_slash(base_url) + "/api/filaments/orcaslicer";

    std::string body;
    bool        success = false;

    Http::get(url)
        .timeout_connect(5)
        .timeout_max(30)
        .on_complete([&](std::string resp_body, unsigned http_status) {
            if (http_status >= 200 && http_status < 300) {
                body    = std::move(resp_body);
                success = true;
            } else {
                BOOST_LOG_TRIVIAL(warning) << "FilamentDbClient: fetch returned HTTP "
                                           << http_status;
            }
        })
        .on_error([&](std::string /*body*/, std::string error, unsigned /*status*/) {
            BOOST_LOG_TRIVIAL(warning) << "FilamentDbClient: fetch error: " << error;
        })
        .perform_sync();

    if (!success)
        return false;

    std::vector<FilamentDbEntry> entries;
    if (!parse_filaments_response(body, entries))
        return false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_filaments       = std::move(entries);
        m_fetched         = true;
        m_last_fetch_time = std::chrono::steady_clock::now();
    }

    BOOST_LOG_TRIVIAL(info) << "FilamentDbClient: loaded " << m_filaments.size()
                            << " filaments from " << base_url;
    return true;
}

std::vector<FilamentDbEntry> FilamentDbClient::get_filaments() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_filaments;
}

bool FilamentDbClient::fetch_calibration(
    const std::string& base_url,
    const std::string& filament_id_or_name,
    double             nozzle_diameter,
    const std::string& bed_type,
    FilamentDbCalibration& out)
{
    // Build URL: /api/filaments/{id}/calibration?nozzle_diameter=X&format=orcaslicer
    std::string url = strip_trailing_slash(base_url)
        + "/api/filaments/" + Http::url_encode(filament_id_or_name)
        + "/calibration?nozzle_diameter=" + std::to_string(nozzle_diameter)
        + "&format=orcaslicer";

    if (!bed_type.empty())
        url += "&bed_type=" + Http::url_encode(bed_type);

    std::string body;
    bool        success = false;

    Http::get(url)
        .timeout_connect(5)
        .timeout_max(15)
        .on_complete([&](std::string resp_body, unsigned http_status) {
            if (http_status >= 200 && http_status < 300) {
                body    = std::move(resp_body);
                success = true;
            }
        })
        .on_error([&](std::string /*body*/, std::string error, unsigned /*status*/) {
            BOOST_LOG_TRIVIAL(debug) << "FilamentDbClient: calibration fetch error: " << error;
        })
        .perform_sync();

    if (!success)
        return false;

    return parse_calibration_response(body, out);
}

bool FilamentDbClient::test_connection(const std::string& base_url)
{
    std::string url = strip_trailing_slash(base_url) + "/api/setup";
    bool        ok  = false;

    Http::get(url)
        .timeout_connect(3)
        .timeout_max(5)
        .on_complete([&](std::string /*body*/, unsigned http_status) {
            ok = (http_status >= 200 && http_status < 300);
        })
        .on_error([](std::string, std::string, unsigned) {})
        .perform_sync();

    return ok;
}

void FilamentDbClient::clear_cache()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_filaments.clear();
    m_fetched = false;
}

bool FilamentDbClient::is_enabled() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return !m_url.empty();
}

void FilamentDbClient::set_url(const std::string& url)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_url != url) {
        m_url = url;
        m_filaments.clear();
        m_fetched = false;
    }
}

std::string FilamentDbClient::get_url() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_url;
}

bool FilamentDbClient::is_stale(int seconds) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_fetched)
        return true;
    auto elapsed = std::chrono::steady_clock::now() - m_last_fetch_time;
    return std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= seconds;
}

bool FilamentDbClient::is_db_filament(const std::string& preset_name) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& e : m_filaments) {
        if (e.name == preset_name)
            return true;
    }
    return false;
}

std::string FilamentDbClient::bed_type_to_db_name(BedType bt)
{
    switch (bt) {
    case btPC:        return "Cool Plate";
    case btPCT:       return "Textured Cool Plate";
    case btEP:        return "Engineering Plate";
    case btPEI:       return "Hot Plate";
    case btPTE:       return "Textured PEI Plate";
    case btSuperTack: return "SuperTack Plate";
    default:          return "";
    }
}

bool FilamentDbClient::sync_to_db(const std::string& base_url,
                                   const std::string& filament_name,
                                   const DynamicPrintConfig& config)
{
    // Build JSON body with OrcaSlicer config keys
    json j;
    // Map structured fields back to DB format
    auto set_temp = [&](const char* orca_key) -> int {
        auto* opt = config.option<ConfigOptionInts>(orca_key);
        return (opt && !opt->values.empty()) ? opt->values[0] : 0;
    };
    auto set_float = [&](const char* orca_key) -> double {
        auto* opt = config.option<ConfigOptionFloats>(orca_key);
        return (opt && !opt->values.empty()) ? opt->values[0] : 0.0;
    };
    auto set_string = [&](const char* orca_key) -> std::string {
        auto* opt = config.option<ConfigOptionStrings>(orca_key);
        return (opt && !opt->values.empty()) ? opt->values[0] : "";
    };

    j["name"] = filament_name;
    j["type"] = set_string("filament_type");
    j["vendor"] = set_string("filament_vendor");
    j["color"] = set_string("filament_colour");
    j["density"] = set_float("filament_density");
    j["cost"] = set_float("filament_cost");
    j["diameter"] = set_float("filament_diameter");
    j["maxVolumetricSpeed"] = set_float("filament_max_volumetric_speed");

    // Temperatures
    json temps;
    temps["nozzle"] = set_temp("nozzle_temperature");
    temps["nozzleFirstLayer"] = set_temp("nozzle_temperature_initial_layer");
    temps["bed"] = set_temp("hot_plate_temp");
    temps["bedFirstLayer"] = set_temp("hot_plate_temp_initial_layer");
    int range_low = set_temp("nozzle_temperature_range_low");
    int range_high = set_temp("nozzle_temperature_range_high");
    if (range_low > 0) temps["nozzleRangeMin"] = range_low;
    if (range_high > 0) temps["nozzleRangeMax"] = range_high;
    j["temperatures"] = temps;

    std::string url = strip_trailing_slash(base_url) + "/api/filaments/"
                      + Http::url_encode(filament_name);
    std::string json_body = j.dump();
    bool success = false;

    auto http = Http::put(url);
    http.header("Content-Type", "application/json")
        .set_post_body(json_body)
        .timeout_max(15)
        .on_complete([&](std::string /*body*/, unsigned http_status) {
            success = (http_status >= 200 && http_status < 300);
        })
        .on_error([&](std::string /*body*/, std::string error, unsigned /*status*/) {
            BOOST_LOG_TRIVIAL(warning) << "FilamentDbClient: sync error: " << error;
        })
        .perform_sync();

    if (success)
        BOOST_LOG_TRIVIAL(info) << "FilamentDbClient: synced '" << filament_name << "' to DB";
    return success;
}

// ---------------------------------------------------------------------------
// Global singleton
// ---------------------------------------------------------------------------

FilamentDbClient& get_filament_db_client()
{
    static FilamentDbClient s_client;
    return s_client;
}

} // namespace Slic3r
