#pragma once

// Target curves as files to share between sessions, machines and tools.
//
// - JSON: {"format": "adaptive-room-eq.target", "version": 1, "name": ..., "points": [[Hz, dB], ...]}
//   (files without format and version, as saved before, read the same).
// - CSV: "frequency_hz,gain_db" rows after optional #-comment lines; "# name: ..." names it.
//
// Reading CSV is forgiving, so other tools' target and house-curve text files
// load too: comma, semicolon, tab or space between the columns; a decimal
// comma with semicolons or tabs between them; header and comment lines
// (#, *, //) skipped; extra columns ignored; points sorted, and repeated
// frequencies keep the last.

#include "roomeq/targets.h"

#include <optional>
#include <string>

namespace roomeq
{
inline constexpr const char* targetFileFormat = "adaptive-room-eq.target";
inline constexpr int targetFileVersion = 1;

std::string targetToJson (const TargetCurve& t);
std::string targetToCsv (const TargetCurve& t);

// The curve, or why not (in `error`). `fallbackName` names it when the file doesn't.
std::optional<TargetCurve> targetFromCsv (const std::string& text, const std::string& fallbackName, std::string& error);
} // namespace roomeq
