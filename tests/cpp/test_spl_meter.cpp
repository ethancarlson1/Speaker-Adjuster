#include "plugin/SampleFifo.h"
#include "plugin/SplMeter.h"

#include <doctest/doctest.h>

#include <cmath>
#include <vector>

namespace
{
constexpr double fs = 48000.0;

// Runs `seconds` of a sine (amplitude from its rms level in dBFS) through the meter
// in 480-sample blocks, collecting every second like the plugin's timer.
void play (SplMeter& m, double seconds, double hz, double rmsDbfs, double& phase)
{
    const auto amp = std::sqrt (2.0) * std::pow (10.0, rmsDbfs / 20.0);
    std::vector<float> block (480);
    const auto blocks = static_cast<int> (seconds * fs / 480.0);
    for (int b = 0; b < blocks; ++b)
    {
        for (auto& v : block)
        {
            v = static_cast<float> (amp * std::sin (phase));
            phase += 2.0 * M_PI * hz / fs;
        }
        m.process (block.data(), static_cast<int> (block.size()));
        if (b % 100 == 99)
            m.collect();
    }
    m.collect();
}
} // namespace

TEST_CASE ("SPL meter: A and C weighting of a steady tone; fast level and max")
{
    SplMeter m;
    m.prepare (fs);
    double phase = 0.0;
    play (m, 5.0, 1000.0, -20.0, phase);                    // 0 dB for both at 1 kHz
    CHECK (m.fastDb() == doctest::Approx (-20.0).epsilon (0.01));
    CHECK (m.laeqDb() == doctest::Approx (-20.0).epsilon (0.005));
    CHECK (m.lceqDb() == doctest::Approx (-20.0).epsilon (0.005));
    CHECK (m.secondsHeard() == 5);
    CHECK (m.maxDb() == doctest::Approx (-20.0).epsilon (0.01));

    SplMeter low;
    low.prepare (fs);
    phase = 0.0;
    play (low, 5.0, 100.0, -20.0, phase);                   // A: -19.1 dB at 100 Hz, C: -0.3 dB
    CHECK (low.laeqDb() == doctest::Approx (-39.1).epsilon (0.02));
    CHECK (low.lceqDb() == doctest::Approx (-20.3).epsilon (0.02));
}

TEST_CASE ("SPL meter: Leq is the energy average of the last 15 minutes; reset; a dead mic doesn't count")
{
    SplMeter m;
    m.prepare (fs);
    double phase = 0.0;
    play (m, 10.0, 1000.0, -20.0, phase);
    play (m, 10.0, 1000.0, -30.0, phase);
    CHECK (m.laeqDb() == doctest::Approx (10.0 * std::log10 ((0.01 + 0.001) / 2.0)).epsilon (0.01));   // -22.6
    CHECK (m.maxDb() == doctest::Approx (-20.0).epsilon (0.01));
    CHECK (m.fastDb() == doctest::Approx (-30.0).epsilon (0.01));

    // Nothing from the mic: those seconds aren't silence, they're not heard.
    std::vector<float> zeros (480, 0.0f);
    for (int b = 0; b < 1000; ++b)
        m.process (zeros.data(), 480);
    m.collect();
    CHECK (m.secondsHeard() <= 21);                        // at most the filters' ring-out, not 10 s of "silence"
    CHECK (m.laeqDb() == doctest::Approx (-22.6).epsilon (0.01));

    m.reset();
    CHECK (m.secondsHeard() == 0);
    CHECK (m.maxDb() == SplMeter::noneDb);
    play (m, 3.0, 1000.0, -40.0, phase);
    CHECK (m.laeqDb() == doctest::Approx (-40.0).epsilon (0.01));
    CHECK (m.maxDb() == doctest::Approx (-40.0).epsilon (0.01));
}

TEST_CASE ("SPL meter: only the last 15 minutes count")
{
    SplMeter m;
    m.prepare (fs);
    double phase = 0.0;
    play (m, 5.0, 1000.0, -10.0, phase);                    // loud, then 15 minutes quieter
    play (m, SplMeter::leqSeconds, 1000.0, -30.0, phase);
    CHECK (m.secondsHeard() == SplMeter::leqSeconds);
    CHECK (m.laeqDb() == doctest::Approx (-30.0).epsilon (0.005));
    CHECK (m.maxDb() == doctest::Approx (-10.0).epsilon (0.01));   // the max stays until a reset
}

TEST_CASE ("sample FIFO: the reader gets what's new, and skips ahead when it falls behind")
{
    SampleFifo fifo (1024);
    std::vector<float> in (300), out (2000);
    for (std::size_t i = 0; i < in.size(); ++i)
        in[i] = static_cast<float> (i);
    fifo.push (in.data(), 300);
    CHECK (fifo.pull (out.data(), 100) == 100);
    CHECK (out[0] == 0.0f);
    CHECK (out[99] == 99.0f);
    CHECK (fifo.pull (out.data(), 2000) == 200);           // the rest
    CHECK (out[199] == 299.0f);
    CHECK (fifo.pull (out.data(), 2000) == 0);             // nothing new

    for (int k = 0; k < 10; ++k)                            // 3000 more: far more than half the ring
        fifo.push (in.data(), 300);
    CHECK (fifo.pull (out.data(), 2000) == 512);           // the newest half
    CHECK (out[511] == 299.0f);
    CHECK (fifo.totalPushed() == 3300);
}
