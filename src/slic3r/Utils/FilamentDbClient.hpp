#pragma once

#include <string>
#include <vector>
#include <mutex>

#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

/// A single filament entry fetched from the Filament DB service.
struct FilamentDbEntry {
    std::string id;      // filament-db ObjectId (e.g. "abc123...")
    std::string name;    // Display name (e.g. "Prusament PLA Galaxy Black")
    std::string vendor;  // Vendor (e.g. "Prusament")
    std::string type;    // Filament type (e.g. "PLA", "PETG")
    std::string color;   // Hex color (e.g. "#FF0000")

    DynamicPrintConfig config; // Full OrcaSlicer-compatible config
};

/// Calibration overrides for a specific nozzle/bed-type combination.
struct FilamentDbCalibration {
    DynamicPrintConfig overrides;
};

/// HTTP client for the Filament DB REST API.
///
/// Fetches filament profiles and calibration data from a local Filament DB
/// service and converts them to OrcaSlicer DynamicPrintConfig objects.
///
/// Thread-safe: all public methods lock an internal mutex.
class FilamentDbClient {
public:
    FilamentDbClient() = default;

    /// Fetch all filaments from the DB. Results are cached until clear_cache().
    /// Returns true on success.
    bool fetch_filaments(const std::string& base_url);

    /// Get the cached filament entries (empty if not yet fetched).
    std::vector<FilamentDbEntry> get_filaments() const;

    /// Fetch calibration overrides for a specific filament + hardware combo.
    bool fetch_calibration(const std::string& base_url,
                           const std::string& filament_id_or_name,
                           double             nozzle_diameter,
                           const std::string& bed_type,
                           FilamentDbCalibration& out);

    /// Quick connection test (GET /api/setup).
    bool test_connection(const std::string& base_url);

    /// Clear the cached filament list.
    void clear_cache();

    /// True if a non-empty URL is configured.
    bool is_enabled() const;

    /// Set the base URL. Clears cache if URL changed.
    void set_url(const std::string& url);

    /// Get the configured base URL.
    std::string get_url() const;

    // --- Public for testing ---

    /// Parse a JSON response from GET /api/filaments/orcaslicer into entries.
    static bool parse_filaments_response(const std::string& json_body,
                                         std::vector<FilamentDbEntry>& out);

    /// Parse a JSON calibration response (format=orcaslicer) into overrides.
    static bool parse_calibration_response(const std::string& json_body,
                                           FilamentDbCalibration& out);

private:
    mutable std::mutex       m_mutex;
    std::vector<FilamentDbEntry> m_filaments;
    bool                     m_fetched{false};
    std::string              m_url;
};

} // namespace Slic3r
