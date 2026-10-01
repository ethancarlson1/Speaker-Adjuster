#include "roomeq/targetfile.h"

#include <doctest/doctest.h>

using namespace roomeq;

TEST_CASE ("targets: CSV and JSON written, CSV read back exactly")
{
    // Values that aren't exact in binary must still come back exactly (the file's decimals, correctly rounded).
    TargetCurve t { "House \"live\"", { { 20.0, 4.0 }, { 80.0, 3.4 }, { 125.0, 1.9 }, { 180.0, 0.6 }, { 2000.0, 0.0 }, { 16000.0, -3.3 } } };
    const auto csv = targetToCsv (t);
    CHECK (csv.rfind ("# Adaptive Room EQ target: adaptive-room-eq.target version 1\n# name: House \"live\"\nfrequency_hz,gain_db\n20.0,4.00\n80.0,3.40\n", 0) == 0);
    std::string error;
    const auto back = targetFromCsv (csv, "file", error);
    REQUIRE (back.has_value());
    CHECK (*back == t);
    const auto json = targetToJson (t);
    CHECK (json.find ("\"format\": \"adaptive-room-eq.target\"") != std::string::npos);
    CHECK (json.find ("\"name\": \"House \\\"live\\\"\"") != std::string::npos);
    CHECK (json.find ("\"points\": [[20.0, 4.00], [80.0, 3.40], [125.0, 1.90], [180.0, 0.60], [2000.0, 0.00], [16000.0, -3.30]]") != std::string::npos);
}

TEST_CASE ("targets: other tools' text files read too")
{
    std::string error;
    // Whitespace columns with a header and an extra phase column (a measurement-tool export).
    const auto spaces = targetFromCsv ("* Freq(Hz) SPL(dB) Phase\nFreq SPL Phase\n 20  3.5 0\n100\t\t 1  0\n1000 0 0\n", "house curve", error);
    REQUIRE (spaces.has_value());
    CHECK (spaces->name == "house curve");
    CHECK (spaces->points == std::vector<std::pair<double, double>> { { 20.0, 3.5 }, { 100.0, 1.0 }, { 1000.0, 0.0 } });
    // Semicolons with decimal commas, unsorted, a repeated frequency (the last wins).
    const auto european = targetFromCsv ("Frequenz;Pegel\n1000;0\n31,5;+2,5\n1000;-0,5\n", "x", error);
    REQUIRE (european.has_value());
    CHECK (european->points == std::vector<std::pair<double, double>> { { 31.5, 2.5 }, { 1000.0, -0.5 } });
    // Text among the points, or nothing at all, says why.
    CHECK_FALSE (targetFromCsv ("20,1\noops\n40,2\n", "x", error).has_value());
    CHECK (error == "Couldn't read line 2: \"oops\" (expected frequency and gain)");
    CHECK_FALSE (targetFromCsv ("# only comments\n", "x", error).has_value());
    CHECK (error.rfind ("No points found", 0) == 0);
}
