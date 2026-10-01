#include "roomeq/eqexport.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <clocale>
#include <sstream>

using namespace roomeq;

namespace
{
EqExport example()
{
    EqExport e;
    e.generator = "Adaptive Room EQ 1.2.3";
    e.created = "2026-10-01T12:00:00Z";
    e.source = "Mains (\"PA\", L/R)";
    e.sampleRate = 48000.0;
    e.correctionStatus = "applied";
    e.amount = 0.5;
    e.correction = { Band { BandKind::bell, 147.3, -2.1, 2.5 }, Band { BandKind::highShelf, 2930.0, -0.8, shelfQ } };
    VoicingBand hp { true, VoicingType::highPass24, 35.0, 0.0, 0.7 };
    VoicingBand bell { true, VoicingType::bell, 3200.0, 1.5, 1.0 };
    e.voicing = { hp, bell };
    e.outputGainDb = 0.4;
    e.delayMs = 12.5;
    e.polarityInverted = true;
    return e;
}

std::vector<std::string> lines (const std::string& s)
{
    std::vector<std::string> out;
    std::istringstream in (s);
    for (std::string line; std::getline (in, line);)
        out.push_back (line);
    return out;
}
} // namespace

TEST_CASE ("JSON export: versioned, every stage, numbers with a dot")
{
    const auto json = eqExportJson (example());
    CHECK (json.find ("\"format\": \"adaptive-room-eq.eq\"") != std::string::npos);
    CHECK (json.find ("\"version\": 1") != std::string::npos);
    CHECK (json.find ("\"source\": \"Mains (\\\"PA\\\", L/R)\"") != std::string::npos);   // escaped
    CHECK (json.find ("{\"type\": \"bell\", \"frequency_hz\": 147.30, \"gain_db\": -2.10, \"q\": 2.500, \"bandwidth_octaves\": 0.573}")
           != std::string::npos);
    CHECK (json.find ("{\"type\": \"high_shelf\", \"frequency_hz\": 2930.00, \"gain_db\": -0.80, \"q\": 0.707}") != std::string::npos);
    CHECK (json.find ("{\"type\": \"high_pass\", \"frequency_hz\": 35.00, \"slope_db_per_octave\": 24}") != std::string::npos);
    CHECK (json.find ("\"amount\": 0.500") != std::string::npos);
    CHECK (json.find ("\"output_gain_db\": 0.40") != std::string::npos);
    CHECK (json.find ("\"delay_ms\": 12.500") != std::string::npos);
    CHECK (json.find ("\"polarity_inverted\": true") != std::string::npos);

    // A host in a comma-decimal locale doesn't change the file.
    const auto* previous = std::setlocale (LC_NUMERIC, nullptr);
    const std::string saved = previous != nullptr ? previous : "C";
    if (std::setlocale (LC_NUMERIC, "de_DE.UTF-8") != nullptr)
    {
        CHECK (eqExportJson (example()) == json);
        std::setlocale (LC_NUMERIC, saved.c_str());
    }
}

TEST_CASE ("CSV export: a header, one row per filter or setting")
{
    const auto rows = lines (eqExportCsv (example()));
    std::vector<std::string> data;
    for (const auto& r : rows)
        if (r.empty() || r.front() != '#')
            data.push_back (r);
    REQUIRE (data.size() == 1 + 2 + 2 + 3);
    CHECK (data[0] == "section,index,type,frequency_hz,gain_db,q,bandwidth_octaves,slope_db_per_octave,value");
    CHECK (data[1] == "correction,1,bell,147.30,-2.10,2.500,0.573,,");
    CHECK (data[2] == "correction,2,high_shelf,2930.00,-0.80,0.707,,,");
    CHECK (data[3] == "voicing,1,high_pass,35.00,,,,24,");
    CHECK (data[4] == "voicing,2,bell,3200.00,1.50,1.000,1.388,,");
    CHECK (data[5] == "output,1,gain,,0.40,,,,");
    CHECK (data[6] == "zone,1,delay_ms,,,,,,12.500");
    CHECK (data[7] == "zone,2,polarity_inverted,,,,,,1");
    CHECK (rows[0] == "# Adaptive Room EQ export: adaptive-room-eq.eq version 1");
    for (const auto& r : data)
        CHECK (std::count (r.begin(), r.end(), ',') == 8);
}

TEST_CASE ("text export: for typing into a console")
{
    const auto text = eqExportText (example());
    CHECK (text.find ("Correction (applied, Amount 50%):\n  1  Bell        147 Hz     -2.1 dB   Q 2.50 (0.57 oct)\n"
                      "  2  High shelf  2.93 kHz   -0.8 dB   Q 0.71\n")
           != std::string::npos);
    CHECK (text.find ("  1  High-pass   35.0 Hz    24 dB/oct\n") != std::string::npos);
    CHECK (text.find ("Output gain: +0.4 dB\nDelay: 12.50 ms, polarity inverted\n") != std::string::npos);

    auto none = example();
    none.correctionStatus = "none";
    none.correction.clear();
    none.voicing.clear();
    none.voicingOn = false;
    const auto empty = eqExportText (none);
    CHECK (empty.find ("Correction (none): no filters\n") != std::string::npos);
    CHECK (empty.find ("Voicing (switched off): no bands on\n") != std::string::npos);
    CHECK (eqExportJson (none).find ("\"filters\": []") != std::string::npos);
}
