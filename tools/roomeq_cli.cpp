// Command-line front end to the C++ analysis core.
//
// Used to cross-check the port against the Python prototype
// (prototype/tests/test_cpp_port.py) and to analyse recordings offline.
// Recordings are raw little-endian float64 files.
//
//   roomeq_cli sweep   --fs 48000 --duration 5 [--level -12] --out sweep.f64
//   roomeq_cli loudness-plan --fs 48000 --ref 95
//   roomeq_cli track --fs 48000 --in output.f64 [--mic mic.f64] [--speed 5] [--block 512]
//   roomeq_cli transfer-bands --fs 48000 --out output.f64 --mic mic.f64
//   roomeq_cli show-track --fs 48000 --ref-out a.f64 --ref-mic b.f64 --out c.f64 --mic d.f64
//   roomeq_cli analyze --fs 48000 --duration 5 [--preroll 0.25] [--tail 2] [--level -12] [--smoothing 6]
//                      --position P1=a.f64,b.f64 [--position ...]
//                      [--program P2=reference.f64:mic.f64] [--exclude P1] [--band-lo 250 --band-hi 4000]
//                      [--target flat|house|speech [--max-cut 12] [--max-boost 3] [--range-lo 20] [--range-hi 20000]]
//
// `analyze` prints JSON: every capture's grade plus the session summary
// (level-aligned position curves, average, usable range, target level) and,
// with --target, the fitted correction. --band-lo/--band-hi set the reference
// band (grading, level alignment, usable range, target placement): 40-100 for a sub.

#include "roomeq/averaging.h"
#include "roomeq/capture.h"
#include "roomeq/correction.h"
#include "roomeq/loudness.h"
#include "roomeq/showtrack.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
std::vector<double> readF64 (const std::string& path)
{
    std::ifstream in (path, std::ios::binary | std::ios::ate);
    if (! in)
        throw std::runtime_error ("cannot open " + path);
    const auto bytes = static_cast<std::size_t> (in.tellg());
    std::vector<double> data (bytes / sizeof (double));
    in.seekg (0);
    in.read (reinterpret_cast<char*> (data.data()), static_cast<std::streamsize> (data.size() * sizeof (double)));
    return data;
}

void writeF64 (const std::string& path, const std::vector<double>& data)
{
    std::ofstream out (path, std::ios::binary);
    out.write (reinterpret_cast<const char*> (data.data()), static_cast<std::streamsize> (data.size() * sizeof (double)));
}

std::vector<std::string> split (const std::string& s, char sep)
{
    std::vector<std::string> parts;
    std::stringstream ss (s);
    std::string item;
    while (std::getline (ss, item, sep))
        parts.push_back (item);
    return parts;
}

// ---- minimal JSON writer --------------------------------------------------
std::string num (double v)
{
    if (! std::isfinite (v))
        return "null";
    char buf[40];
    std::snprintf (buf, sizeof (buf), "%.17g", v);
    return buf;
}

std::string str (const std::string& s)
{
    std::string out = "\"";
    for (auto c : s)
    {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    return out + "\"";
}

template <typename T, typename F>
std::string list (const std::vector<T>& items, F&& f)
{
    std::string out = "[";
    for (std::size_t i = 0; i < items.size(); ++i)
        out += (i > 0 ? "," : "") + f (items[i]);
    return out + "]";
}

std::string numbers (const std::vector<double>& v) { return list (v, num); }

std::string captureJson (const roomeq::Capture& c)
{
    const auto band = [] (const roomeq::BandResult& b)
    {
        const auto g = b.grade();
        return std::string ("{") + "\"name\":" + str (b.name) + ",\"level_db\":" + num (b.levelDb)
               + ",\"snr_db\":" + num (b.snrDb)
               + ",\"spread_db\":" + (b.spreadDb ? num (*b.spreadDb) : "null")
               + ",\"excess_spread_db\":" + (b.excessSpreadDb ? num (*b.excessSpreadDb) : "null")
               + ",\"out_of_range\":" + (b.outOfRange ? "true" : "false")
               + ",\"grade\":" + (g ? str (roomeq::gradeLabel (*g)) : "null") + "}";
    };
    return std::string ("{") + "\"name\":" + str (c.name) + ",\"kind\":" + str (c.kind)
           + ",\"excluded\":" + (c.excluded ? "true" : "false")
           + ",\"delays_ms\":" + numbers (c.delaysMs)
           + ",\"drift_ppm\":" + (std::isnan (c.driftPpm) ? std::string ("null") : num (c.driftPpm))
           + ",\"overall\":" + str (roomeq::gradeLabel (c.grade.overall))
           + ",\"reasons\":" + list (c.grade.reasons, str)
           + ",\"notes\":" + list (c.grade.notes, str)
           + ",\"bands\":" + list (c.grade.bands, band) + "}";
}

std::string summaryJson (const roomeq::SessionSummary& s)
{
    return std::string ("{") + "\"offsets_db\":" + numbers (s.offsetsDb) + ",\"n_good\":" + std::to_string (s.nGood)
           + ",\"smoothing_fraction\":" + std::to_string (s.policy.smoothingFraction)
           + ",\"strength\":" + num (s.policy.strength)
           + ",\"max_correction_db\":" + (s.policy.maxCorrectionDb ? num (*s.policy.maxCorrectionDb) : "null")
           + ",\"grid\":" + numbers (s.grid) + ",\"average_db\":" + numbers (s.averageDb)
           + ",\"position_db\":" + list (s.positionDb, numbers)
           + ",\"usable\":[" + num (s.usable.first) + "," + num (s.usable.second) + "]"
           + ",\"target_db\":" + num (s.targetDb) + "}";
}

std::string bandJson (const roomeq::Band& b)
{
    return std::string ("{\"kind\":") + str (roomeq::bandKindName (b.kind)) + ",\"freq\":" + num (b.freq)
           + ",\"gain_db\":" + num (b.gainDb) + ",\"q\":" + num (b.q) + "}";
}

std::string correctionJson (const roomeq::CorrectionResult& r)
{
    std::vector<double> nulls;
    for (auto v : r.nullMask)
        nulls.push_back (v ? 1.0 : 0.0);
    return std::string ("{") + "\"grid\":" + numbers (r.grid) + ",\"average_db\":" + numbers (r.averageDb)
           + ",\"target_db\":" + numbers (r.targetDb) + ",\"desired_db\":" + numbers (r.desiredDb)
           + ",\"null_mask\":" + numbers (nulls)
           + ",\"fit_range\":[" + num (r.fitRange.first) + "," + num (r.fitRange.second) + "]"
           + ",\"strength\":" + num (r.strength) + ",\"fitted\":" + list (r.fitted, bandJson)
           + ",\"bands\":" + list (r.bands, bandJson) + ",\"correction_db\":" + numbers (r.correctionDb)
           + ",\"rms_error_db\":" + num (r.rmsErrorDb) + "}";
}

struct Args
{
    std::string command;
    std::map<std::string, std::string> values;
    std::vector<std::string> positions, programs;
    std::set<std::string> excluded;

    double get (const std::string& key, double fallback) const
    {
        const auto it = values.find (key);
        return it == values.end() ? fallback : std::stod (it->second);
    }
};

Args parse (int argc, char** argv)
{
    if (argc < 2)
        throw std::runtime_error ("usage: roomeq_cli <sweep|analyze> [options]  (see tools/roomeq_cli.cpp)");
    Args a;
    a.command = argv[1];
    for (int i = 2; i < argc; ++i)
    {
        const std::string key = argv[i];
        if (key.rfind ("--", 0) != 0 || i + 1 >= argc)
            throw std::runtime_error ("bad argument: " + key);
        const std::string value = argv[++i];
        if (key == "--position")
            a.positions.push_back (value);
        else if (key == "--program")
            a.programs.push_back (value);
        else if (key == "--exclude")
            a.excluded.insert (value);
        else
            a.values[key.substr (2)] = value;
    }
    return a;
}

roomeq::SweepConfig sweepConfig (const Args& a)
{
    roomeq::SweepConfig cfg;
    cfg.fs = a.get ("fs", cfg.fs);
    cfg.duration = a.get ("duration", cfg.duration);
    cfg.levelDbfs = a.get ("level", cfg.levelDbfs);
    cfg.preroll = a.get ("preroll", cfg.preroll);
    cfg.tail = a.get ("tail", cfg.tail);
    return cfg;
}

std::pair<std::string, std::string> nameAndSpec (const std::string& arg)
{
    const auto eq = arg.find ('=');
    if (eq == std::string::npos)
        throw std::runtime_error ("expected NAME=files: " + arg);
    return { arg.substr (0, eq), arg.substr (eq + 1) };
}

int run (int argc, char** argv)
{
    const auto args = parse (argc, argv);
    const auto cfg = sweepConfig (args);

    if (args.command == "sweep")
    {
        writeF64 (args.values.at ("out"), roomeq::generateSweep (cfg));
        return 0;
    }
    if (args.command == "loudness-plan")
    {
        const auto plan = roomeq::planShelves (args.get ("ref", 95.0), cfg.fs);
        std::cout << "{\"low_freq\":" << num (plan.lowFreq) << ",\"low_q\":" << num (plan.lowQ)
                  << ",\"high_freq\":" << num (plan.highFreq) << ",\"high_q\":" << num (plan.highQ)
                  << ",\"low_gain\":" << numbers ({ plan.lowGain.begin(), plan.lowGain.end() })
                  << ",\"high_gain\":" << numbers ({ plan.highGain.begin(), plan.highGain.end() }) << "}\n";
        return 0;
    }
    if (args.command == "track")
    {
        // The tracker (and a mic tracker following it), block by block as the
        // plugin runs them; one entry per completed window.
        const auto toFloat = [] (const std::vector<double>& v) { return std::vector<float> (v.begin(), v.end()); };
        const auto out = toFloat (readF64 (args.values.at ("in")));
        const auto hasMic = args.values.count ("mic") > 0;
        const auto mic = hasMic ? toFloat (readF64 (args.values.at ("mic"))) : std::vector<float> {};
        roomeq::LoudnessConfig lcfg;
        lcfg.speedS = args.get ("speed", lcfg.speedS);
        roomeq::LevelTracker outTracker, micTracker;
        outTracker.prepare (cfg.fs, lcfg);
        micTracker.prepare (cfg.fs, lcfg);
        roomeq::Deadband deadband;
        const auto block = static_cast<std::size_t> (args.get ("block", 512));
        std::vector<double> estimates, held, micEstimates;
        const auto nan = std::numeric_limits<double>::quiet_NaN();
        for (std::size_t i = 0; i + block <= out.size(); i += block)
        {
            const auto windows = outTracker.process (out.data() + i, static_cast<int> (block));
            if (hasMic)
                micTracker.process (mic.data() + i, static_cast<int> (block), &outTracker);
            if (windows > 0)
            {
                estimates.push_back (outTracker.hasEstimate() ? outTracker.estimate() : nan);
                held.push_back (outTracker.hasEstimate() ? deadband.update (outTracker.estimate(), lcfg) : nan);
                micEstimates.push_back (micTracker.hasEstimate() ? micTracker.estimate() : nan);
            }
        }
        std::cout << "{\"estimates\":" << numbers (estimates) << ",\"held\":" << numbers (held)
                  << ",\"mic\":" << numbers (micEstimates) << "}\n";
        return 0;
    }
    if (args.command == "show-track")
    {
        // Reference from the soundcheck recording, then 10 s blocks of the show.
        const auto reference = roomeq::transferBandsDb (readF64 (args.values.at ("ref-out")), readF64 (args.values.at ("ref-mic")), cfg.fs);
        const auto out = readF64 (args.values.at ("out"));
        const auto mic = readF64 (args.values.at ("mic"));
        roomeq::DeltaTracker tracker (reference);
        const auto block = static_cast<std::size_t> (10.0 * cfg.fs);
        std::vector<std::string> states;
        for (std::size_t s = 0; s + block <= std::min (out.size(), mic.size()); s += block)
        {
            const std::vector<double> o (out.begin() + static_cast<long> (s), out.begin() + static_cast<long> (s + block));
            const std::vector<double> m (mic.begin() + static_cast<long> (s), mic.begin() + static_cast<long> (s + block));
            const auto& st = tracker.addBlock (roomeq::transferBandsDb (o, m, cfg.fs));
            std::vector<double> flags (st.flags.begin(), st.flags.end());
            states.push_back (std::string ("{") + "\"delta_db\":" + numbers (st.deltaDb) + ",\"level_db\":" + num (st.levelDb)
                              + ",\"flags\":" + numbers (flags) + ",\"level_flag\":" + std::to_string (st.levelFlag)
                              + ",\"severity\":" + std::to_string (st.severity) + ",\"message\":" + str (st.message)
                              + ",\"details\":" + list (st.details, str) + "}");
        }
        std::cout << "{\"reference\":" << numbers (reference)
                  << ",\"states\":" << list (states, [] (const std::string& x) { return x; }) << "}\n";
        return 0;
    }
    if (args.command == "transfer-bands")
    {
        std::cout << "{\"bands\":"
                  << numbers (roomeq::transferBandsDb (readF64 (args.values.at ("out")), readF64 (args.values.at ("mic")), cfg.fs))
                  << "}\n";
        return 0;
    }
    if (args.command != "analyze")
        throw std::runtime_error ("unknown command: " + args.command);

    const double bandLo = args.get ("band-lo", 250.0), bandHi = args.get ("band-hi", 4000.0);
    roomeq::AnalysisConfig acfg;
    acfg.grading.passbandLo = bandLo;
    acfg.grading.passbandHi = bandHi;

    std::vector<std::shared_ptr<const roomeq::Capture>> captures;
    for (const auto& p : args.positions)
    {
        const auto [name, files] = nameAndSpec (p);
        std::vector<std::vector<double>> recordings;
        for (const auto& f : split (files, ','))
            recordings.push_back (readF64 (f));
        auto c = roomeq::analyzeSweepCapture (name, recordings, cfg, acfg);
        c.excluded = args.excluded.count (name) > 0;
        captures.push_back (std::make_shared<roomeq::Capture> (std::move (c)));
    }
    for (const auto& p : args.programs)
    {
        const auto [name, files] = nameAndSpec (p);
        const auto parts = split (files, ':');
        if (parts.size() != 2)
            throw std::runtime_error ("expected NAME=reference.f64:mic.f64: " + p);
        auto c = roomeq::analyzeProgramCapture (name, readF64 (parts[0]), readF64 (parts[1]), cfg.fs, acfg);
        c.excluded = args.excluded.count (name) > 0;
        captures.push_back (std::make_shared<roomeq::Capture> (std::move (c)));
    }

    const auto fraction = static_cast<int> (args.get ("smoothing", 6));
    const auto summary = roomeq::summarizeSession (captures, fraction, roomeq::logFreqGrid (20.0, 20000.0, 48), bandLo, bandHi);
    std::string correction = "null";
    if (summary && args.values.count ("target") > 0)
    {
        const auto& name = args.values.at ("target");
        const roomeq::TargetCurve* target = nullptr;
        for (const auto& t : roomeq::targetPresets())
            if (t.name == name || (name.size() == t.name.size()
                                   && std::equal (name.begin(), name.end(), t.name.begin(),
                                                  [] (char a, char b) { return std::tolower (a) == std::tolower (b); })))
                target = &t;
        if (target == nullptr)
            throw std::runtime_error ("unknown target: " + name);
        roomeq::CorrectionConfig ccfg;
        ccfg.maxCutDb = args.get ("max-cut", ccfg.maxCutDb);
        ccfg.maxBoostDb = args.get ("max-boost", ccfg.maxBoostDb);
        ccfg.rangeLoHz = args.get ("range-lo", ccfg.rangeLoHz);
        ccfg.rangeHiHz = args.get ("range-hi", ccfg.rangeHiHz);
        ccfg.refBandLoHz = bandLo;
        ccfg.refBandHiHz = bandHi;
        correction = correctionJson (roomeq::designCorrection (captures, *summary, *target, cfg.fs, ccfg));
    }
    std::cout << "{\"captures\":" << list (captures, [] (const auto& c) { return captureJson (*c); })
              << ",\"summary\":" << (summary ? summaryJson (*summary) : "null")
              << ",\"correction\":" << correction << "}\n";
    return 0;
}
} // namespace

int main (int argc, char** argv)
{
    try
    {
        return run (argc, argv);
    }
    catch (const std::exception& e)
    {
        std::cerr << "roomeq_cli: " << e.what() << "\n";
        return 1;
    }
}
