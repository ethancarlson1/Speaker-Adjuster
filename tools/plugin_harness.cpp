// Headless end-to-end check of the plugin, plus UI screenshots.
//
// Instantiates the processor, runs processBlock against a simulated room (the
// mic hears the speaker output through filters, reflections, a small reverb
// and noise), measures several positions, a noisy one, a music capture and a
// pink-noise capture, checks grades and delays; then fits, applies and
// verifies a correction (the speakers must get exactly the predicted EQ, and
// re-measuring through it must land near the target), checks undo / compare
// and the state round trip, and renders each editor tab to PNG.
//
//   AdaptiveRoomEQ_Harness [--out snapshot.png]   (also writes snapshot-correct.png, -voicing.png, -standalone.png)
//
// Exits non-zero if any check fails.

#include "plugin/PluginEditor.h"
#include "plugin/PluginProcessor.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <complex>
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

void setParam (AdaptiveRoomEQProcessor& p, const juce::String& id, float value)
{
    auto* param = p.getParameters().getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 (value));
}

// The main path's response (program in, speakers out) to an impulse, after
// letting any EQ glide settle.
std::vector<double> impulseThrough (AdaptiveRoomEQProcessor& p, int length = 1 << 15)
{
    juce::AudioBuffer<float> buffer (3, blockSize);
    juce::MidiBuffer midi;
    for (int b = 0; b < 100; ++b)
    {
        buffer.clear();
        p.processBlock (buffer, midi);
    }
    std::vector<double> out;
    for (int pos = 0; pos < length; pos += blockSize)
    {
        buffer.clear();
        if (pos == 0)
        {
            buffer.setSample (0, 0, 1.0f);
            buffer.setSample (1, 0, 1.0f);
        }
        p.processBlock (buffer, midi);
        for (int i = 0; i < blockSize; ++i)
            out.push_back (buffer.getSample (0, i));
    }
    return out;
}

double dtftDb (const std::vector<double>& x, double f)
{
    std::complex<double> acc {}, z { 1.0, 0.0 };
    const auto step = std::polar (1.0, -juce::MathConstants<double>::twoPi * f / fs);
    for (auto v : x)
    {
        acc += v * z;
        z *= step;
    }
    return 20.0 * std::log10 (std::abs (acc));
}

double worstMismatchDb (const std::vector<double>& ir, const std::vector<roomeq::Band>& expected)
{
    double worst = 0.0;
    for (double f : { 30.0, 60.0, 120.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0 })
        worst = std::max (worst, std::abs (dtftDb (ir, f) - roomeq::responseDb (expected, { f }, fs).front()));
    return worst;
}

// RMS of (curve - target) over the fit range, outside nulls, on the display grid.
double rmsFromTarget (const MeasurementEngine::Display& d, const std::vector<double>& curve)
{
    const auto& r = *d.proposal;
    const auto& g = d.summary.grid;
    double sumSq = 0.0;
    int n = 0;
    for (std::size_t i = 0; i < g.size(); ++i)
    {
        if (g[i] < r.fitRange.first || g[i] > r.fitRange.second || ! std::isfinite (curve[i]))
            continue;
        std::size_t k = 0;
        for (std::size_t j = 1; j < r.grid.size(); ++j)
            if (std::abs (std::log (r.grid[j] / g[i])) < std::abs (std::log (r.grid[k] / g[i])))
                k = j;
        if (r.nullMask[k])
            continue;
        sumSq += (curve[i] - d.targetDb[i]) * (curve[i] - d.targetDb[i]);
        ++n;
    }
    return n > 0 ? std::sqrt (sumSq / n) : 1e9;
}

std::shared_ptr<const MeasurementEngine::Display> waitForDisplay (AdaptiveRoomEQProcessor& p,
                                                                  const std::function<bool (const MeasurementEngine::Display&)>& ready)
{
    for (int i = 0; i < 2000; ++i)
    {
        pump (p);
        if (auto d = p.getEngine().getDisplay(); d != nullptr && p.getEngine().isDisplayCurrent() && ready (*d))
            return d;
    }
    return p.getEngine().getDisplay();
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

void writeSnapshot (juce::Component& c, const juce::String& path)
{
    const auto image = c.createComponentSnapshot (c.getLocalBounds(), true, 1.5f);
    juce::File file (juce::File::getCurrentWorkingDirectory().getChildFile (path));
    file.deleteFile();
    juce::FileOutputStream stream (file);
    check (stream.openedOk() && juce::PNGImageFormat().writeImageToStream (image, stream), "wrote " + file.getFullPathName());
}

// The standalone app: mono mic in, stereo out, and the mic shares channel 0
// with the first output (as JUCE's AudioProcessorPlayer lays the buffer out).
void standaloneChecks (const juce::String& snapshotPath)
{
    std::cout << "Standalone app mode\n";
    // Same steps as JUCE's createPluginFilterOfType for the standalone wrapper.
    juce::PluginHostType::jucePlugInClientCurrentWrapperType = juce::AudioProcessor::wrapperType_Standalone;
    juce::AudioProcessor::setTypeOfNextNewPlugin (juce::AudioProcessor::wrapperType_Standalone);
    auto proc = std::make_unique<AdaptiveRoomEQProcessor>();
    juce::AudioProcessor::setTypeOfNextNewPlugin (juce::AudioProcessor::wrapperType_Undefined);
    juce::PluginHostType::jucePlugInClientCurrentWrapperType = juce::AudioProcessor::wrapperType_Undefined;
    check (proc->isStandalone(), "processor knows it's the standalone app");
    check (proc->getTotalNumInputChannels() == 1 && proc->getTotalNumOutputChannels() == 2, "mono mic input, stereo output");
    proc->setRateAndBufferSizeDetails (fs, blockSize);
    proc->prepareToPlay (fs, blockSize);
    check (proc->startProgram().failed(), "music capture refused (no program passes through the app)");

    const double distance = 7.0;
    SimulatedRoom room (distance, 11, 3e-4, 0.0);
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    std::vector<float> speaker (blockSize, 0.0f);
    check (proc->startSweep().wasOk(), "start sweep");
    for (int block = 0; block < 100000 && proc->getEngine().getActivity() != MeasurementEngine::Activity::idle; ++block)
    {
        for (int i = 0; i < blockSize; ++i)
        {
            buffer.setSample (0, i, room.process (speaker[static_cast<std::size_t> (i)]));   // mic, in place
            buffer.setSample (1, i, 0.0f);
        }
        proc->processBlock (buffer, midi);
        for (int i = 0; i < blockSize; ++i)
            speaker[static_cast<std::size_t> (i)] = buffer.getSample (0, i);   // output 1 feeds the speaker
        if (block % 64 == 0)
            pump (*proc);
    }
    for (int i = 0; i < 400 && proc->getEngine().getActivity() != MeasurementEngine::Activity::idle; ++i)
        pump (*proc);
    const auto entries = proc->getEngine().getEntries();
    check (entries.size() == 1 && entries[0].capture->grade.overall == roomeq::Grade::pass, "sweep capture graded pass");
    if (! entries.empty())
    {
        const auto expected = 1000.0 * (distance / 343.0 * fs + 288 + blockSize) / fs;
        check (std::abs (entries[0].capture->delaysMs.front() - expected) < 0.5,
               "loop delay within 0.5 ms of the simulated " + juce::String (expected, 2) + " ms");
    }

    // Verify through a (quick-mode) correction: the app plays the sweep through the EQ.
    for (int i = 0; i < 400 && ! (proc->getEngine().getDisplay() && proc->getEngine().getDisplay()->proposal
                                 && proc->getEngine().isDisplayCurrent()); ++i)
        pump (*proc);
    proc->getEngine().applyProposal();
    check (! proc->getEngine().getApplied().empty(), "standalone: quick-mode correction applied");
    {
        SimulatedRoom again (distance, 12, 3e-4, 0.0);
        std::fill (speaker.begin(), speaker.end(), 0.0f);
        check (proc->startVerify().wasOk(), "standalone: start verify");
        for (int block = 0; block < 100000 && proc->getEngine().getActivity() != MeasurementEngine::Activity::idle; ++block)
        {
            for (int i = 0; i < blockSize; ++i)
            {
                buffer.setSample (0, i, again.process (speaker[static_cast<std::size_t> (i)]));
                buffer.setSample (1, i, 0.0f);
            }
            proc->processBlock (buffer, midi);
            for (int i = 0; i < blockSize; ++i)
                speaker[static_cast<std::size_t> (i)] = buffer.getSample (0, i);
            if (block % 64 == 0)
                pump (*proc);
        }
        for (int i = 0; i < 400 && ! proc->getEngine().isDisplayCurrent(); ++i)
            pump (*proc);
        const auto all = proc->getEngine().getEntries();
        check (all.size() == 2 && all.back().verify && all.back().capture->grade.overall == roomeq::Grade::pass,
               "standalone: verify capture graded pass");
        const auto d = proc->getEngine().getDisplay();
        check (d != nullptr && d->verifiedCount == 1, "standalone: verified curve shown");
    }

    // Idle: a loud mic must never reach the speakers.
    for (int i = 0; i < blockSize; ++i)
        buffer.setSample (0, i, 0.5f * std::sin (0.05f * static_cast<float> (i)));
    proc->processBlock (buffer, midi);
    check (buffer.getMagnitude (0, blockSize) <= 0.0f, "mic is not passed through to the outputs");

    // Render this editor with pink noise selected so both layouts get a snapshot.
    auto* signal = proc->getParameters().getParameter ("measureSignal");
    signal->setValueNotifyingHost (signal->convertTo0to1 (1.0f));
    for (int i = 0; i < 20; ++i)
        pump (*proc);
    std::unique_ptr<juce::AudioProcessorEditor> editor (proc->createEditor());
    editor->setSize (1160, 760);
    for (int i = 0; i < 20; ++i)
        pump (*proc);
    writeSnapshot (*editor, snapshotPath);
}

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

    std::cout << "Measuring with pink noise\n";
    const double noiseDistance = 7.2;
    {
        auto* signal = proc.getParameters().getParameter ("measureSignal");
        signal->setValueNotifyingHost (signal->convertTo0to1 (1.0f));   // "Pink noise"
        check (proc.isNoiseSelected() && proc.getNoiseSeconds() > 19.0, "pink noise selected, 20 s");
        SimulatedRoom room (noiseDistance, 11, 3e-4, 0.0);
        check (proc.startMeasurement().wasOk(), "start pink noise capture");
        runMeasurement (proc, room);
        signal->setValueNotifyingHost (signal->convertTo0to1 (0.0f));
    }

    auto entries = proc.getEngine().getEntries();
    check (entries.size() == 7, "seven captures (" + juce::String (static_cast<int> (entries.size())) + ")");
    for (const auto& e : entries)
    {
        const auto& c = *e.capture;
        std::cout << "    " << c.name << " [" << c.kind << "] " << roomeq::gradeLabel (c.grade.overall)
                  << "  delay " << (c.delaysMs.empty() ? 0.0 : c.delaysMs.front()) << " ms";
        for (const auto& r : c.grade.reasons)
            std::cout << "  | " << r;
        std::cout << "\n";
    }
    if (entries.size() == 7)
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
        check (entries[6].capture->kind == "noise" && entries[6].capture->grade.overall == roomeq::Grade::pass,
               "pink noise capture graded pass");
        const auto expected = 1000.0 * (noiseDistance / 343.0 * fs + 288 + blockSize) / fs;
        check (std::abs (entries[6].capture->delaysMs.front() - expected) < 0.5,
               "pink noise delay within 0.5 ms of the simulated " + juce::String (expected, 2) + " ms");
        proc.getEngine().setExcluded (entries[4].id, true);
        for (int i = 0; i < 40; ++i)
            pump (proc);
    }

    const auto display = proc.getEngine().getDisplay();
    check (display != nullptr, "averaged display available");
    if (display != nullptr)
        check (display->summary.nGood >= 4 && ! display->summary.policy.maxCorrectionDb.has_value(),
               "enough good positions for full strength (" + juce::String (display->summary.nGood) + ")");

    std::cout << "Correction\n";
    auto& engine = proc.getEngine();
    auto d = waitForDisplay (proc, [] (const MeasurementEngine::Display& x) { return x.proposal.has_value(); });
    check (d != nullptr && d->proposal.has_value(), "proposed correction fitted in the background");
    std::vector<roomeq::Band> proposed;
    if (d != nullptr && d->proposal)
    {
        const auto& r = *d->proposal;
        proposed = r.bands;
        const auto [lo, hi] = std::minmax_element (r.correctionDb.begin(), r.correctionDb.end());
        check (! r.bands.empty() && r.bands.size() <= 10, juce::String (static_cast<int> (r.bands.size())) + " bands");
        check (*hi <= 3.1 && *lo >= -12.1, "within +3 / -12 dB (" + juce::String (*lo, 1) + " to " + juce::String (*hi, 1) + ")");
        check (r.rmsErrorDb < 1.5, "predicted " + juce::String (r.rmsErrorDb, 2) + " dB RMS from the target");
        check (juce::exactlyEqual (d->fitFs, 48000.0), "fitted at the plugin's sample rate");
        std::cout << "    fit range " << r.fitRange.first << " - " << r.fitRange.second << " Hz\n";
        for (const auto& b : r.bands)
            std::cout << "    " << roomeq::bandKindName (b.kind) << " " << b.freq << " Hz " << b.gainDb << " dB Q " << b.q << "\n";
    }

    check (worstMismatchDb (impulseThrough (proc), {}) < 0.001, "audio passes unchanged before Apply");
    check (engine.canApply(), "Apply available");
    engine.applyProposal();
    check (! engine.canApply() && engine.getApplied() == proposed && engine.getAppliedId() > 0, "proposal applied");

    // Band 3 as a +4 dB bell at 1 kHz; the speakers must get correction + voicing exactly as the graph predicts.
    setParam (proc, "v3Type", 0.0f);
    setParam (proc, "v3Freq", 1000.0f);
    setParam (proc, "v3Gain", 4.0f);
    setParam (proc, "v3Q", 1.4f);
    setParam (proc, "v3On", 1.0f);
    auto expected = engine.getApplied();
    for (const auto& b : proc.getVoicingSections())
        expected.push_back (b);
    auto worst = worstMismatchDb (impulseThrough (proc), expected);
    check (worst < 0.02, "speakers get correction + voicing as predicted (worst " + juce::String (worst, 3) + " dB)");
    setParam (proc, "correctionAmount", 50.0f);
    expected.clear();
    for (const auto& b : engine.getApplied())
        expected.push_back (b.scaled (0.5));
    for (const auto& b : proc.getVoicingSections())
        expected.push_back (b);
    worst = worstMismatchDb (impulseThrough (proc), expected);
    check (worst < 0.02, "50% amount halves the correction (worst " + juce::String (worst, 3) + " dB)");
    setParam (proc, "correctionAmount", 100.0f);
    setParam (proc, "correctionOn", 0.0f);
    setParam (proc, "voicingOn", 0.0f);
    check (worstMismatchDb (impulseThrough (proc), {}) < 0.001, "both stages bypassed: unchanged");
    setParam (proc, "correctionOn", 1.0f);
    setParam (proc, "voicingOn", 1.0f);
    setParam (proc, "v3On", 0.0f);

    std::cout << "Verify\n";
    for (int i = 0; i < 3; ++i)
    {
        SimulatedRoom room (positions[i].distance, positions[i].seed + 20, positions[i].noise, 0.0);
        check (proc.startVerify().wasOk(), "start verify at " + juce::String (positions[i].distance, 1) + " m");
        runMeasurement (proc, room);
    }
    entries = engine.getEntries();
    const auto verifyCount = std::count_if (entries.begin(), entries.end(), [] (const auto& e) { return e.verify; });
    check (verifyCount == 3 && entries.back().capture->name == "V3", "three verify captures, V1-V3");
    d = waitForDisplay (proc, [] (const MeasurementEngine::Display& x) { return x.verifiedCount == 3; });
    check (d != nullptr && d->proposal && d->verifiedCount == 3, "verified average from the three verify captures");
    if (d != nullptr && d->proposal && ! d->verifiedDb.empty())
    {
        const auto before = rmsFromTarget (*d, d->summary.averageDb);
        const auto after = rmsFromTarget (*d, d->verifiedDb);
        check (after < 1.5 && after < 0.6 * before, "re-measured through the correction: " + juce::String (before, 2) + " -> "
                                                        + juce::String (after, 2) + " dB RMS from target");
        check (d->proposal->bands == engine.getApplied(), "verify captures don't change the proposal");
    }

    std::cout << "Compare and undo\n";
    engine.setComparingPrevious (true);
    check (engine.isComparingPrevious() && engine.getPlaying().empty(), "hear previous plays the previous (none)");
    check (worstMismatchDb (impulseThrough (proc), {}) < 0.001, "and the speakers get it");
    engine.setComparingPrevious (false);
    check (engine.getPlaying() == proposed, "back to the applied correction");
    engine.undoApply();
    check (engine.getApplied().empty() && engine.getPrevious() == proposed, "undo swaps back to the previous");
    d = waitForDisplay (proc, [] (const MeasurementEngine::Display& x) { return x.verifiedCount == 0; });
    check (d != nullptr && d->verifiedDb.empty(), "verify captures belong to the correction they measured");
    engine.undoApply();
    check (engine.getApplied() == proposed, "undo again restores it");
    d = waitForDisplay (proc, [] (const MeasurementEngine::Display& x) { return x.verifiedCount == 3; });
    check (d != nullptr && d->verifiedCount == 3, "and its verify captures count again");

    std::cout << "Targets\n";
    proc.setCustomTarget ({ "Tilt", { { 100.0, 2.0 }, { 10000.0, -4.0 } } });
    setParam (proc, "target", static_cast<float> (AdaptiveRoomEQProcessor::targetCustom));
    d = waitForDisplay (proc, [&] (const MeasurementEngine::Display& x) { return x.proposal && x.proposal->bands != proposed; });
    check (d != nullptr && d->proposal && d->proposal->bands != proposed, "custom target refits the proposal");
    check (engine.canApply(), "and it can be applied");
    setParam (proc, "target", 0.0f);
    d = waitForDisplay (proc, [&] (const MeasurementEngine::Display& x) { return x.proposal && x.proposal->bands == proposed; });
    check (d != nullptr && d->proposal && d->proposal->bands == proposed, "back to flat: the same proposal (deterministic)");

    // Leave a little voicing on for the screenshots.
    setParam (proc, "v2On", 1.0f);
    setParam (proc, "v2Gain", 2.0f);
    setParam (proc, "v6On", 1.0f);
    setParam (proc, "v6Gain", -1.5f);
    for (int i = 0; i < 20; ++i)
        pump (proc);
    entries = engine.getEntries();

    std::cout << "State round trip\n";
    juce::MemoryBlock state;
    proc.getStateInformation (state);
    {
        AdaptiveRoomEQProcessor restored;
        const auto started = juce::Time::getMillisecondCounterHiRes();
        restored.setStateInformation (state.getData(), static_cast<int> (state.getSize()));
        waitForDisplay (restored, [] (const MeasurementEngine::Display& x) { return x.proposal.has_value(); });
        std::cout << "    summary and fit after restore: " << juce::roundToInt (juce::Time::getMillisecondCounterHiRes() - started)
                  << " ms\n";
        const auto again = restored.getEngine().getEntries();
        check (again.size() == entries.size(), "captures restored");
        auto same = again.size() == entries.size();
        for (std::size_t i = 0; same && i < again.size(); ++i)
            same = again[i].capture->name == entries[i].capture->name
                   && again[i].capture->grade.reasons == entries[i].capture->grade.reasons
                   && again[i].capture->excluded == (i == 4);
        check (same, "names, reasons and excluded flags restored");
        check (restored.getEngine().getDisplay() != nullptr, "average recomputed after restore");
        auto verifySame = true;
        for (std::size_t i = 0; verifySame && i < again.size() && i < entries.size(); ++i)
            verifySame = again[i].verify == entries[i].verify && again[i].correctionId == entries[i].correctionId;
        check (verifySame, "verify captures restored");
        check (restored.getEngine().getApplied() == engine.getApplied()
                   && restored.getEngine().getAppliedId() == engine.getAppliedId() && restored.getEngine().hasPrevious(),
               "applied and previous corrections restored");
        check (restored.getCustomTarget() == proc.getCustomTarget(), "custom target restored");
        restored.setRateAndBufferSizeDetails (fs, blockSize);
        restored.prepareToPlay (fs, blockSize);
        auto restoredEq = restored.getEngine().getApplied();
        for (const auto& b : restored.getVoicingSections())
            restoredEq.push_back (b);
        check (restoredEq.size() > engine.getApplied().size()
                   && worstMismatchDb (impulseThrough (restored), restoredEq) < 0.02,
               "restored plugin plays the same correction and voicing");
        std::cout << "    state size " << state.getSize() / 1024 << " KB\n";
    }

    std::cout << "Rendering the editor\n";
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
        auto* ours = dynamic_cast<AdaptiveRoomEQEditor*> (editor.get());
        editor->setSize (1200, 820);
        for (int i = 0; i < 20; ++i)
            pump (proc);
        const auto stem = outPath.upToLastOccurrenceOf (".", false, false);
        writeSnapshot (*editor, outPath);
        ours->showTab (AdaptiveRoomEQEditor::Tab::correct);
        writeSnapshot (*editor, stem + "-correct.png");
        ours->selectVoicingBand (1);
        ours->showTab (AdaptiveRoomEQEditor::Tab::voicing);
        writeSnapshot (*editor, stem + "-voicing.png");

        std::cout << "Editing on the graph\n";
        ResponseGraph* graph = nullptr;
        for (auto* c : editor->getChildren())
            if (auto* gr = dynamic_cast<ResponseGraph*> (c))
                graph = gr;
        check (graph != nullptr, "graph found");
        if (graph != nullptr)
        {
            auto source = juce::Desktop::getInstance().getMainMouseSource();
            const auto event = [&] (juce::Point<float> at, juce::Point<float> down, int clicks, bool dragged)
            {
                const auto now = juce::Time::getCurrentTime();
                return juce::MouseEvent (source, at, {}, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, graph, graph, now, down, now, clicks, dragged);
            };
            const auto drag = [&] (juce::Point<float> from, juce::Point<float> to)
            {
                graph->mouseDown (event (from, from, 1, false));
                graph->mouseDrag (event ((from + to) / 2.0f, from, 1, true));
                graph->mouseDrag (event (to, from, 1, true));
                graph->mouseUp (event (to, from, 1, true));
            };

            // Band 5 (a bell at 1.5 kHz): drag it right and up.
            setParam (proc, "v5On", 1.0f);
            graph->refresh();
            const auto h = graph->getVoicingHandlePosition (4);
            drag (h, h + juce::Point<float> (60.0f, -25.0f));
            const auto v = proc.getEqSettings().voicing[4];
            check (v.freq > 1600.0 && v.gainDb > 1.0, "dragging a voicing handle sets frequency (" + juce::String (v.freq, 0)
                                                           + " Hz) and gain (" + juce::String (v.gainDb, 1) + " dB)");
            graph->mouseDoubleClick (event (graph->getVoicingHandlePosition (4), graph->getVoicingHandlePosition (4), 2, false));
            check (! proc.getEqSettings().voicing[4].on, "double-click switches the band off");

            // Custom target: drag its second point down 3 dB-ish; double-click adds a point.
            proc.setCustomTarget ({ "Custom", { { 50.0, 3.0 }, { 1000.0, 0.0 }, { 10000.0, -2.0 } } });
            setParam (proc, "target", static_cast<float> (AdaptiveRoomEQProcessor::targetCustom));
            waitForDisplay (proc, [] (const MeasurementEngine::Display&) { return true; });
            graph->setData (proc.getEngine().getDisplay(), -1);
            const auto p1 = graph->getTargetPointPosition (1);
            drag (p1, p1 + juce::Point<float> (0.0f, 30.0f));
            const auto moved = proc.getCustomTarget();
            check (moved.points.size() == 3 && moved.points[1].second < -1.0 && std::abs (moved.points[1].first - 1000.0) < 50.0,
                   "dragging a target point moves it (" + juce::String (moved.points[1].second, 1) + " dB)");
            const auto emptySpot = graph->getTargetPointPosition (1) + juce::Point<float> (120.0f, 0.0f);
            graph->mouseDoubleClick (event (emptySpot, emptySpot, 2, false));
            check (proc.getCustomTarget().points.size() == 4, "double-click adds a target point");

            // Save it, load it back.
            check (proc.saveCustomTarget ("Harness test target").wasOk(), "custom target saved");
            const auto file = AdaptiveRoomEQProcessor::getTargetsFolder().getChildFile ("Harness test target.json");
            const auto saved = proc.getCustomTarget();
            proc.setCustomTarget ({ "Custom", { { 1000.0, 0.0 } } });
            check (proc.loadTarget (file).wasOk() && proc.getCustomTarget().points == saved.points, "and loaded back");
            file.deleteFile();
            setParam (proc, "target", 0.0f);
        }
    }

    standaloneChecks (outPath.upToLastOccurrenceOf (".", false, false) + "-standalone.png");

    std::cout << (failures == 0 ? "All checks passed\n" : juce::String (failures) + " check(s) failed\n");
    return failures == 0 ? 0 : 1;
}
