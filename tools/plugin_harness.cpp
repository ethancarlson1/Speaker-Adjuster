// Headless end-to-end check of the plugin, plus a UI screenshot.
//
// Instantiates the processor, runs processBlock against a simulated room (the
// mic hears the speaker output through filters, reflections, a small reverb
// and noise), measures several positions, a noisy one and a music capture,
// checks grades / delays / state round-trip, then renders the editor to PNG.
//
//   AdaptiveRoomEQ_Harness [--out snapshot.png]
//
// Exits non-zero if any check fails.

#include "plugin/PluginEditor.h"
#include "plugin/PluginProcessor.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <iostream>
#include <random>

namespace
{
constexpr double fs = 48000.0;
constexpr int blockSize = 512;
int failures = 0;

void check (bool ok, const juce::String& what)
{
    std::cout << (ok ? "  ok    " : "  FAIL  ") << what << "\n";
    if (! ok)
        ++failures;
}

struct Biquad
{
    double b0, b1, b2, a1, a2, s1 = 0.0, s2 = 0.0;

    static Biquad peaking (double f0, double gainDb, double q)
    {
        const auto a = std::pow (10.0, gainDb / 40.0), w0 = juce::MathConstants<double>::twoPi * f0 / fs;
        const auto alpha = std::sin (w0) / (2.0 * q), c = std::cos (w0), a0 = 1.0 + alpha / a;
        return { (1.0 + alpha * a) / a0, -2.0 * c / a0, (1.0 - alpha * a) / a0, -2.0 * c / a0, (1.0 - alpha / a) / a0 };
    }

    static Biquad lowpass (double f0, double q)
    {
        const auto w0 = juce::MathConstants<double>::twoPi * f0 / fs, alpha = std::sin (w0) / (2.0 * q), c = std::cos (w0);
        const auto a0 = 1.0 + alpha;
        return { (1.0 - c) / 2.0 / a0, (1.0 - c) / a0, (1.0 - c) / 2.0 / a0, -2.0 * c / a0, (1.0 - alpha) / a0 };
    }

    static Biquad highpass (double f0, double q)
    {
        const auto w0 = juce::MathConstants<double>::twoPi * f0 / fs, alpha = std::sin (w0) / (2.0 * q), c = std::cos (w0);
        const auto a0 = 1.0 + alpha;
        return { (1.0 + c) / 2.0 / a0, -(1.0 + c) / a0, (1.0 + c) / 2.0 / a0, -2.0 * c / a0, (1.0 - alpha) / a0 };
    }

    double process (double x)
    {
        const auto y = b0 * x + s1;
        s1 = b1 * x - a1 * y + s2;
        s2 = b2 * x - a2 * y;
        return y;
    }
};

// A streaming "room" for one mic position: PA filters, direct sound plus a
// few reflections, a Schroeder reverb tail, noise, and a fixed latency.
class SimulatedRoom
{
public:
    SimulatedRoom (double distance, unsigned seed, double noiseLevel, double rumbleLevel)
        : rng (seed), noise (noiseLevel), rumble (rumbleLevel)
    {
        std::uniform_int_distribution<int> tap (200, 2400);
        std::uniform_real_distribution<double> gain (-0.5, 0.5);
        direct = 1.0 / distance;
        delay = static_cast<int> (distance / 343.0 * fs) + 288;   // flight time + 6 ms latency
        for (int i = 0; i < 6; ++i)
            reflections.push_back ({ delay + tap (rng), gain (rng) / distance });
        line.assign (static_cast<std::size_t> (delay + 2600), 0.0);
        const int combLengths[] = { 1557, 1617, 1491, 1422 };
        for (auto len : combLengths)
            combs.push_back ({ std::vector<double> (static_cast<std::size_t> (len + static_cast<int> (seed % 50)), 0.0), 0 });
        allpasses = { { std::vector<double> (225, 0.0), 0 }, { std::vector<double> (556, 0.0), 0 } };
    }

    float process (float speaker)
    {
        auto x = hp.process (speaker);
        x = dip.process (bump.process (x));
        line[writePos] = x;
        const auto at = [&] (int d) { return line[(writePos + line.size() - static_cast<std::size_t> (d)) % line.size()]; };
        auto y = direct * at (delay);
        for (const auto& [d, g] : reflections)
            y += g * at (d);

        // Diffuse tail (RT60 ~ 0.7 s), fed by the direct sound.
        auto wet = 0.0;
        for (auto& c : combs)
        {
            auto& v = c.buffer[c.pos];
            wet += v;
            v = at (delay) * 0.12 + v * 0.84;
            c.pos = (c.pos + 1) % c.buffer.size();
        }
        for (auto& a : allpasses)
        {
            const auto delayed = a.buffer[a.pos];
            const auto input = wet + 0.5 * delayed;
            a.buffer[a.pos] = input;
            wet = delayed - 0.5 * input;
            a.pos = (a.pos + 1) % a.buffer.size();
        }
        y += wet * 0.35;
        writePos = (writePos + 1) % line.size();

        const auto low = rumbleFilter2.process (rumbleFilter1.process (dist (rng)));   // below ~70 Hz
        return static_cast<float> (y + noise * dist (rng) + rumble * low);
    }

private:
    struct Delay
    {
        std::vector<double> buffer;
        std::size_t pos;
    };

    std::mt19937 rng;
    std::normal_distribution<double> dist { 0.0, 1.0 };
    double noise, rumble;
    Biquad rumbleFilter1 = Biquad::lowpass (70.0, 0.7071), rumbleFilter2 = Biquad::lowpass (70.0, 0.7071);
    double direct = 1.0;
    int delay = 0;
    std::vector<std::pair<int, double>> reflections;
    std::vector<double> line;
    std::size_t writePos = 0;
    std::vector<Delay> combs, allpasses;
    Biquad hp = Biquad::highpass (55.0, 0.7071), bump = Biquad::peaking (150.0, 3.0, 0.9),
           dip = Biquad::peaking (2800.0, -4.0, 1.8);
};

void pump (AdaptiveRoomEQProcessor& p)
{
    p.getEngine().update();
    juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
}

// Runs audio blocks until the current measurement is recorded and analysed.
void runMeasurement (AdaptiveRoomEQProcessor& p, SimulatedRoom& room, const std::vector<float>* program = nullptr)
{
    juce::AudioBuffer<float> buffer (3, blockSize);   // main L/R in-place + mic sidechain
    juce::MidiBuffer midi;
    std::vector<float> speaker (blockSize, 0.0f);
    std::size_t programPos = 0;
    const auto channel = p.getSweepSettings().channel;

    for (int block = 0; block < 100000 && p.getEngine().getActivity() != MeasurementEngine::Activity::idle; ++block)
    {
        for (int i = 0; i < blockSize; ++i)
        {
            const auto in = program != nullptr && programPos < program->size() ? (*program)[programPos++] : 0.0f;
            buffer.setSample (0, i, in);
            buffer.setSample (1, i, in);
            buffer.setSample (2, i, room.process (speaker[static_cast<std::size_t> (i)]));   // hears last block
        }
        p.processBlock (buffer, midi);
        for (int i = 0; i < blockSize; ++i)
            speaker[static_cast<std::size_t> (i)] = buffer.getSample (program != nullptr ? 0 : channel, i);
        if (block % 64 == 0)
            pump (p);
    }
    for (int i = 0; i < 400 && p.getEngine().getActivity() != MeasurementEngine::Activity::idle; ++i)
        pump (p);
    for (int i = 0; i < 20; ++i)   // let the averaged display catch up
        pump (p);
}

std::vector<float> musicLikeProgram (double seconds)
{
    std::mt19937 rng (42);
    std::normal_distribution<double> dist (0.0, 1.0);
    std::vector<float> x (static_cast<std::size_t> (seconds * fs));
    Biquad tilt = Biquad::peaking (200.0, 8.0, 0.5);
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        const auto beat = std::fmod (static_cast<double> (i) / fs, 0.5);
        const auto env = 0.4 + 0.6 * std::exp (-beat / 0.15);
        x[i] = static_cast<float> (0.05 * env * tilt.process (dist (rng)));
    }
    return x;
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juce;
    juce::String outPath = "plugin_snapshot.png";
    for (int i = 1; i + 1 < argc; ++i)
        if (juce::String (argv[i]) == "--out")
            outPath = argv[i + 1];

    AdaptiveRoomEQProcessor proc;
    proc.enableAllBuses();
    proc.setRateAndBufferSizeDetails (fs, blockSize);
    proc.prepareToPlay (fs, blockSize);
    check (proc.isMicConnected(), "mic sidechain bus is enabled");

    std::cout << "Measuring positions\n";
    const struct { double distance; unsigned seed; double noise; double rumble; } positions[] = {
        { 5.3, 1, 3e-4, 0.0 }, { 8.3, 2, 3e-4, 0.0 }, { 11.8, 3, 3e-4, 0.0 }, { 14.1, 4, 3e-4, 0.0 },
        { 9.0, 5, 3e-4, 2.0 },   // truck idling outside: rumble below 70 Hz at about -19 dBFS
    };
    for (const auto& pos : positions)
    {
        SimulatedRoom room (pos.distance, pos.seed, pos.noise, pos.rumble);
        check (proc.startSweep().wasOk(), "start sweep at " + juce::String (pos.distance, 1) + " m");
        runMeasurement (proc, room);
    }

    std::cout << "Measuring from program material\n";
    {
        SimulatedRoom room (6.0, 9, 3e-4, 0.0);
        const auto program = musicLikeProgram (AdaptiveRoomEQProcessor::programSeconds + 1.0);
        check (proc.startProgram().wasOk(), "start program capture");
        runMeasurement (proc, room, &program);
    }

    auto entries = proc.getEngine().getEntries();
    check (entries.size() == 6, "six captures (" + juce::String (static_cast<int> (entries.size())) + ")");
    for (const auto& e : entries)
    {
        const auto& c = *e.capture;
        std::cout << "    " << c.name << " [" << c.kind << "] " << roomeq::gradeLabel (c.grade.overall)
                  << "  delay " << (c.delaysMs.empty() ? 0.0 : c.delaysMs.front()) << " ms";
        for (const auto& r : c.grade.reasons)
            std::cout << "  | " << r;
        std::cout << "\n";
    }
    if (entries.size() == 6)
    {
        for (int i = 0; i < 4; ++i)
        {
            const auto expected = 1000.0 * (positions[i].distance / 343.0 * fs + 288 + blockSize) / fs;
            check (entries[static_cast<std::size_t> (i)].capture->grade.overall == roomeq::Grade::pass,
                   juce::String (entries[static_cast<std::size_t> (i)].capture->name) + " graded pass");
            check (std::abs (entries[static_cast<std::size_t> (i)].capture->delaysMs.front() - expected) < 0.5,
                   "loop delay within 0.5 ms of the simulated " + juce::String (expected, 2) + " ms");
        }
        check (entries[4].capture->grade.overall == roomeq::Grade::redo, "rumble capture graded redo");
        check (entries[5].capture->kind == "program", "music capture analysed with the dual-FFT");
        proc.getEngine().setExcluded (entries[4].id, true);
        for (int i = 0; i < 40; ++i)
            pump (proc);
    }

    const auto display = proc.getEngine().getDisplay();
    check (display != nullptr, "averaged display available");
    if (display != nullptr)
        check (display->summary.nGood >= 4 && ! display->summary.policy.maxCorrectionDb.has_value(),
               "enough good positions for full strength (" + juce::String (display->summary.nGood) + ")");

    std::cout << "State round trip\n";
    juce::MemoryBlock state;
    proc.getStateInformation (state);
    {
        AdaptiveRoomEQProcessor restored;
        restored.setStateInformation (state.getData(), static_cast<int> (state.getSize()));
        for (int i = 0; i < 40; ++i)
            pump (restored);
        const auto again = restored.getEngine().getEntries();
        check (again.size() == entries.size(), "captures restored");
        auto same = again.size() == entries.size();
        for (std::size_t i = 0; same && i < again.size(); ++i)
            same = again[i].capture->name == entries[i].capture->name
                   && again[i].capture->grade.reasons == entries[i].capture->grade.reasons
                   && again[i].capture->excluded == (i == 4);
        check (same, "names, reasons and excluded flags restored");
        check (restored.getEngine().getDisplay() != nullptr, "average recomputed after restore");
        std::cout << "    state size " << state.getSize() / 1024 << " KB\n";
    }

    std::cout << "Rendering the editor\n";
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
        editor->setSize (1160, 760);
        for (int i = 0; i < 20; ++i)
            pump (proc);
        const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.5f);
        juce::File file (juce::File::getCurrentWorkingDirectory().getChildFile (outPath));
        file.deleteFile();
        juce::FileOutputStream stream (file);
        check (stream.openedOk() && juce::PNGImageFormat().writeImageToStream (image, stream), "wrote " + file.getFullPathName());
    }

    std::cout << (failures == 0 ? "All checks passed\n" : juce::String (failures) + " check(s) failed\n");
    return failures == 0 ? 0 : 1;
}
