#include "roomeq/showtrack.h"

#include <doctest/doctest.h>

#include <cmath>
#include <random>

using namespace roomeq;

namespace
{
const auto n = showBandNames().size();

struct Blocks
{
    std::mt19937 rng;
    std::vector<double> ref;

    explicit Blocks (unsigned seed) : rng (seed)
    {
        std::normal_distribution<double> d (-20.0, 3.0);
        for (std::size_t i = 0; i < n; ++i)
            ref.push_back (d (rng));
    }

    std::vector<double> next (const std::vector<double>& change = {})
    {
        std::normal_distribution<double> jitter (0.0, 0.5);
        auto b = ref;
        for (std::size_t i = 0; i < n; ++i)
            b[i] += jitter (rng) + (change.empty() ? 0.0 : change[i]);
        return b;
    }
};

int flagged (const ShowState& s)
{
    int count = 0;
    for (auto f : s.flags)
        count += f != 0 ? 1 : 0;
    return count;
}
} // namespace

TEST_CASE ("show tracking: a steady room stays quiet; a sustained change is flagged, then clears")
{
    Blocks blocks (1);
    DeltaTracker t (blocks.ref);
    for (int i = 0; i < 12; ++i)
        t.addBlock (blocks.next());
    CHECK (t.state().severity == 0);
    CHECK (t.state().message.empty());
    CHECK (flagged (t.state()) == 0);
    CHECK (std::abs (t.state().levelDb) < 0.3);

    std::vector<double> bump (n, 0.0);
    bump[3] = bump[4] = bump[5] = 4.0;   // 125-200 Hz up 4 dB
    for (int i = 0; i < 12; ++i)
    {
        t.addBlock (blocks.next (bump));
        if (i < 7)
            CHECK (flagged (t.state()) == 0);   // 8 of 12 blocks: the 2-minute average is still under 3 dB
    }
    const auto& s = t.state();
    CHECK (s.flags[3] == 1);
    CHECK (s.flags[4] == 1);
    CHECK (s.flags[5] == 1);
    CHECK (flagged (s) == 3);
    CHECK (s.severity == 1);
    CHECK (s.message.rfind ("Since soundcheck: +", 0) == 0);
    CHECK (s.message.find ("at 125\xe2\x80\x93" "200 Hz") != std::string::npos);

    int cleared = -1;
    for (int i = 0; i < 12; ++i)
    {
        t.addBlock (blocks.next());
        if (cleared < 0 && flagged (t.state()) == 0)
            cleared = i;
    }
    CHECK (cleared >= 4);
    CHECK (cleared <= 7);
}

TEST_CASE ("show tracking: a level change is not a tonal change; big changes are severe; unheard bands don't count")
{
    Blocks level (2);
    DeltaTracker t (level.ref);
    for (int i = 0; i < 12; ++i)
        t.addBlock (level.next (std::vector<double> (n, 4.5)));
    CHECK (t.state().levelFlag == 1);
    CHECK (flagged (t.state()) == 0);
    CHECK (t.state().levelDb == doctest::Approx (4.5).epsilon (0.07));
    CHECK (t.state().message.find ("dB louder after the plugin") != std::string::npos);

    Blocks treble (3);
    DeltaTracker u (treble.ref);
    std::vector<double> cut (n, 0.0);
    for (std::size_t i = 18; i < n; ++i)
        cut[i] = -7.0;   // 4k-8k down 7 dB
    for (int i = 0; i < 12; ++i)
    {
        auto b = treble.next (cut);
        b[0] = b[1] = b[2] = std::nan ("");   // 63-100 Hz never heard clearly
        u.addBlock (b);
    }
    const auto& s = u.state();
    CHECK (std::isnan (s.deltaDb[0]));
    CHECK (std::isnan (s.deltaDb[2]));
    for (std::size_t i = 18; i < n; ++i)
        CHECK (s.flags[i] == -1);
    CHECK (s.severity == 2);
    CHECK (s.message.find ("at 4\xe2\x80\x93" "8 kHz") != std::string::npos);

    CHECK_FALSE (referenceIsUsable (std::vector<double> (n, std::nan (""))));
    CHECK (referenceIsUsable (std::vector<double> (n, 0.0)));
}
