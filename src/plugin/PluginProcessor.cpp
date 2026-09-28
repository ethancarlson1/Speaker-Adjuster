#include "plugin/PluginProcessor.h"
#include "plugin/PluginEditor.h"

namespace
{
namespace ParamIds
{
const juce::ParameterID sweepLength { "sweepLength", 1 };
const juce::ParameterID sweepsPerPosition { "sweepsPerPosition", 1 };
const juce::ParameterID sweepSpeaker { "sweepSpeaker", 1 };
const juce::ParameterID sweepLevel { "sweepLevel", 1 };
const juce::ParameterID smoothing { "smoothing", 1 };
const juce::ParameterID signal { "measureSignal", 1 };
const juce::ParameterID noiseLength { "noiseLength", 1 };
const juce::ParameterID target { "target", 1 };
const juce::ParameterID correctionOn { "correctionOn", 1 };
const juce::ParameterID amount { "correctionAmount", 1 };
const juce::ParameterID maxCut { "maxCut", 1 };
const juce::ParameterID maxBoost { "maxBoost", 1 };
const juce::ParameterID rangeLo { "rangeLo", 1 };
const juce::ParameterID rangeHi { "rangeHi", 1 };
const juce::ParameterID voicingOn { "voicingOn", 1 };
const juce::ParameterID loudOn { "loudOn", 1 };
const juce::ParameterID loudRef { "loudRef", 1 };
const juce::ParameterID loudAmount { "loudAmount", 1 };
const juce::ParameterID loudMaxLow { "loudMaxLow", 1 };
const juce::ParameterID loudMaxHigh { "loudMaxHigh", 1 };
const juce::ParameterID loudSpeed { "loudSpeed", 1 };
const juce::ParameterID loudSource { "loudSource", 1 };
const juce::ParameterID loudHighPass { "loudHighPass", 1 };

juce::ParameterID voicing (int band, const char* what)
{
    return { "v" + juce::String (band + 1) + what, 1 };
}
} // namespace ParamIds

constexpr double sweepSeconds[] = { 2.0, 5.0, 10.0 };
constexpr double noiseSeconds[] = { 10.0, 20.0, 30.0 };
constexpr int smoothingFractions[] = { 3, 4, 6 };

bool loadedAsStandalone()
{
    return juce::PluginHostType::getPluginLoadedAs() == juce::AudioProcessor::wrapperType_Standalone;
}

// Log-frequency parameter range (even spacing per octave on a slider).
juce::NormalisableRange<float> logRange (float lo, float hi)
{
    return { lo, hi,
             [] (float start, float end, float v) { return start * std::pow (end / start, v); },
             [] (float start, float end, float v) { return std::log (v / start) / std::log (end / start); },
             [] (float, float, float v) { return v; } };
}

juce::String hzText (float v, int)
{
    return v >= 1000.0f ? juce::String (v / 1000.0f, v >= 10000.0f ? 1 : 2) + " kHz" : juce::String (juce::roundToInt (v)) + " Hz";
}

// Lock-free "max" so the editor can read-and-reset peaks between blocks.
void updatePeak (std::atomic<float>& peak, float value) noexcept
{
    auto current = peak.load (std::memory_order_relaxed);
    while (value > current && ! peak.compare_exchange_weak (current, value, std::memory_order_relaxed))
    {
    }
}
} // namespace

juce::AudioProcessor::BusesProperties AdaptiveRoomEQProcessor::makeBuses (bool isStandalone)
{
    // The standalone app is a measurement tool: its only input is the mic, and
    // the user picks the physical mic input and speaker output in the editor.
    if (isStandalone)
        return BusesProperties()
            .withInput ("Measurement Mic", juce::AudioChannelSet::mono(), true)
            .withOutput ("Output", juce::AudioChannelSet::stereo(), true);

    return BusesProperties()
        .withInput ("Input", juce::AudioChannelSet::stereo(), true)
        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
        .withInput ("Measurement Mic", juce::AudioChannelSet::mono(), true);
}

AdaptiveRoomEQProcessor::AdaptiveRoomEQProcessor()
    : AudioProcessor (makeBuses (loadedAsStandalone())),
      standalone (loadedAsStandalone()),
      parameters (*this, nullptr, "Parameters", createParameterLayout())
{
    // Parameter callbacks can arrive on the audio thread, so the engine polls
    // the smoothing choice and correction settings from its message-thread timer instead.
    engine.setSmoothingSource ([this] { return getSmoothingFraction(); });
    engine.setCorrectionSettingsSource ([this] { return getCorrectionSettings(); });
    engine.onPlayingCorrectionChanged = [this] (const std::vector<roomeq::Band>& bands)
    {
        CorrectionSet set;
        for (const auto& b : bands)
            if (set.count < CorrectionSet::maxBands)
                set.bands[static_cast<std::size_t> (set.count++)] = b;
        eq.setCorrection (set);
    };

    correctionOnParam = parameters.getRawParameterValue (ParamIds::correctionOn.getParamID());
    amountParam = parameters.getRawParameterValue (ParamIds::amount.getParamID());
    voicingOnParam = parameters.getRawParameterValue (ParamIds::voicingOn.getParamID());
    for (int i = 0; i < roomeq::numVoicingBands; ++i)
    {
        auto& v = voicingParams[static_cast<std::size_t> (i)];
        v.on = parameters.getRawParameterValue (ParamIds::voicing (i, "On").getParamID());
        v.type = parameters.getRawParameterValue (ParamIds::voicing (i, "Type").getParamID());
        v.freq = parameters.getRawParameterValue (ParamIds::voicing (i, "Freq").getParamID());
        v.gain = parameters.getRawParameterValue (ParamIds::voicing (i, "Gain").getParamID());
        v.q = parameters.getRawParameterValue (ParamIds::voicing (i, "Q").getParamID());
    }

    auto& lp = loudnessParams;
    lp.on = parameters.getRawParameterValue (ParamIds::loudOn.getParamID());
    lp.reference = parameters.getRawParameterValue (ParamIds::loudRef.getParamID());
    lp.amount = parameters.getRawParameterValue (ParamIds::loudAmount.getParamID());
    lp.maxLow = parameters.getRawParameterValue (ParamIds::loudMaxLow.getParamID());
    lp.maxHigh = parameters.getRawParameterValue (ParamIds::loudMaxHigh.getParamID());
    lp.speed = parameters.getRawParameterValue (ParamIds::loudSpeed.getParamID());
    lp.source = parameters.getRawParameterValue (ParamIds::loudSource.getParamID());
    lp.highPass = parameters.getRawParameterValue (ParamIds::loudHighPass.getParamID());

    loudnessControl.setEnvironmentSource ([this]
    {
        LoudnessController::Environment env;
        env.fs = getSampleRate();
        env.referenceSpl = loudnessParams.reference->load();
        env.micConnected = isMicConnected();
        env.signalLevelDbfs = raw (ParamIds::sweepLevel);
        env.available = ! standalone;
        return env;
    });
}

juce::AudioProcessorValueTreeState::ParameterLayout AdaptiveRoomEQProcessor::createParameterLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add (std::make_unique<AudioParameterChoice> (ParamIds::sweepLength, "Sweep length",
                                                        StringArray { "2 s", "5 s", "10 s" }, 1));
    layout.add (std::make_unique<AudioParameterChoice> (ParamIds::sweepsPerPosition, "Sweeps per position",
                                                        StringArray { "1", "2", "3" }, 1));
    layout.add (std::make_unique<AudioParameterChoice> (ParamIds::sweepSpeaker, "Sweep speaker",
                                                        StringArray { "Left", "Right" }, 0));
    layout.add (std::make_unique<AudioParameterChoice> (ParamIds::signal, "Measurement signal",
                                                        StringArray { "Sweep", "Pink noise" }, 0));
    layout.add (std::make_unique<AudioParameterChoice> (ParamIds::noiseLength, "Noise length",
                                                        StringArray { "10 s", "20 s", "30 s" }, 1));
    // The ID keeps its original name so saved sessions still load; it's the peak
    // level of whichever signal is used.
    layout.add (std::make_unique<AudioParameterFloat> (ParamIds::sweepLevel, "Signal level",
                                                       NormalisableRange<float> (-40.0f, 0.0f, 0.5f), -12.0f,
                                                       AudioParameterFloatAttributes().withLabel ("dBFS")));
    layout.add (std::make_unique<AudioParameterChoice> (ParamIds::smoothing, "Smoothing",
                                                        StringArray { "1/3 octave", "1/4 octave", "1/6 octave" }, 2));

    // Correction.
    const auto hz = AudioParameterFloatAttributes().withStringFromValueFunction (hzText);
    const auto db = AudioParameterFloatAttributes().withLabel ("dB");
    layout.add (std::make_unique<AudioParameterChoice> (ParamIds::target, "Target",
                                                        StringArray { "Flat", "House", "Speech", "Custom" }, 0));
    layout.add (std::make_unique<AudioParameterBool> (ParamIds::correctionOn, "Correction", true));
    layout.add (std::make_unique<AudioParameterFloat> (ParamIds::amount, "Correction amount",
                                                       NormalisableRange<float> (0.0f, 100.0f, 1.0f), 100.0f,
                                                       AudioParameterFloatAttributes().withLabel ("%")));
    layout.add (std::make_unique<AudioParameterFloat> (ParamIds::maxCut, "Max cut",
                                                       NormalisableRange<float> (0.0f, 24.0f, 0.5f), 12.0f, db));
    layout.add (std::make_unique<AudioParameterFloat> (ParamIds::maxBoost, "Max boost",
                                                       NormalisableRange<float> (0.0f, 6.0f, 0.5f), 3.0f, db));
    layout.add (std::make_unique<AudioParameterFloat> (ParamIds::rangeLo, "Correct from", logRange (20.0f, 500.0f), 20.0f, hz));
    layout.add (std::make_unique<AudioParameterFloat> (ParamIds::rangeHi, "Correct up to", logRange (1000.0f, 20000.0f),
                                                       20000.0f, hz));

    // Voicing EQ.
    layout.add (std::make_unique<AudioParameterBool> (ParamIds::voicingOn, "Voicing EQ", true));
    const StringArray types { "Bell", "Low shelf", "High shelf", "High-pass 12 dB", "High-pass 24 dB",
                              "Low-pass 12 dB", "Low-pass 24 dB" };
    for (int i = 0; i < roomeq::numVoicingBands; ++i)
    {
        const auto d = roomeq::defaultVoicingBand (i);
        const auto name = "Voicing " + String (i + 1) + " ";
        auto group = std::make_unique<AudioProcessorParameterGroup> ("voicing" + String (i + 1), "Voicing band " + String (i + 1), "|");
        group->addChild (std::make_unique<AudioParameterBool> (ParamIds::voicing (i, "On"), name + "on", false));
        group->addChild (std::make_unique<AudioParameterChoice> (ParamIds::voicing (i, "Type"), name + "type", types,
                                                                 static_cast<int> (d.type)));
        group->addChild (std::make_unique<AudioParameterFloat> (ParamIds::voicing (i, "Freq"), name + "frequency",
                                                                logRange (20.0f, 20000.0f), static_cast<float> (d.freq), hz));
        group->addChild (std::make_unique<AudioParameterFloat> (ParamIds::voicing (i, "Gain"), name + "gain",
                                                                NormalisableRange<float> (-18.0f, 18.0f, 0.1f), 0.0f, db));
        group->addChild (std::make_unique<AudioParameterFloat> (
            ParamIds::voicing (i, "Q"), name + "Q", logRange (0.3f, 8.0f), static_cast<float> (d.q),
            AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (v, 2); })));
        layout.add (std::move (group));
    }

    // Loudness compensation.
    auto loud = std::make_unique<AudioProcessorParameterGroup> ("loudness", "Loudness", "|");
    loud->addChild (std::make_unique<AudioParameterBool> (ParamIds::loudOn, "Loudness compensation", true));
    loud->addChild (std::make_unique<AudioParameterFloat> (ParamIds::loudRef, "Reference level",
                                                           NormalisableRange<float> (60.0f, 110.0f, 0.5f), 95.0f,
                                                           AudioParameterFloatAttributes().withLabel ("dB(C)")));
    loud->addChild (std::make_unique<AudioParameterFloat> (ParamIds::loudAmount, "Loudness amount",
                                                           NormalisableRange<float> (0.0f, 100.0f, 1.0f), 100.0f,
                                                           AudioParameterFloatAttributes().withLabel ("%")));
    loud->addChild (std::make_unique<AudioParameterFloat> (ParamIds::loudMaxLow, "Max low boost",
                                                           NormalisableRange<float> (0.0f, 12.0f, 0.5f), 8.0f, db));
    loud->addChild (std::make_unique<AudioParameterFloat> (ParamIds::loudMaxHigh, "Max high boost",
                                                           NormalisableRange<float> (0.0f, 8.0f, 0.5f), 4.0f, db));
    loud->addChild (std::make_unique<AudioParameterFloat> (
        ParamIds::loudSpeed, "Level speed", logRange (1.0f, 20.0f), 5.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " s"; })));
    loud->addChild (std::make_unique<AudioParameterChoice> (ParamIds::loudSource, "Level from",
                                                           StringArray { "Plugin output", "Mic" }, 0));
    loud->addChild (std::make_unique<AudioParameterBool> (ParamIds::loudHighPass, "Protective high-pass", false));
    layout.add (std::move (loud));
    return layout;
}

float AdaptiveRoomEQProcessor::raw (const juce::ParameterID& id) const noexcept
{
    return parameters.getRawParameterValue (id.getParamID())->load();
}

EqSettings AdaptiveRoomEQProcessor::getEqSettings() const noexcept
{
    EqSettings s;
    s.correctionOn = correctionOnParam->load() > 0.5f;
    s.amount = juce::jlimit (0.0, 1.0, static_cast<double> (amountParam->load()) / 100.0);
    s.voicingOn = voicingOnParam->load() > 0.5f;
    for (std::size_t i = 0; i < s.voicing.size(); ++i)
    {
        const auto& p = voicingParams[i];
        auto& v = s.voicing[i];
        v.on = p.on->load() > 0.5f;
        v.type = static_cast<roomeq::VoicingType> (juce::jlimit (0, roomeq::numVoicingTypes - 1, juce::roundToInt (p.type->load())));
        v.freq = p.freq->load();
        v.gainDb = p.gain->load();
        v.q = p.q->load();
    }
    return s;
}

LoudnessSettings AdaptiveRoomEQProcessor::getLoudnessSettings() const noexcept
{
    const auto& p = loudnessParams;
    LoudnessSettings s;
    s.on = p.on->load() > 0.5f;
    s.useMic = p.source->load() > 0.5f;
    s.hpBaseHz = loudnessControl.getHighpassBase();
    auto& c = s.config;
    c.referenceSpl = p.reference->load();
    c.amount = juce::jlimit (0.0, 1.0, static_cast<double> (p.amount->load()) / 100.0);
    c.maxLowDb = p.maxLow->load();
    c.maxHighDb = p.maxHigh->load();
    c.speedS = juce::jlimit (1.0, 20.0, static_cast<double> (p.speed->load()));
    c.hpTrack = p.highPass->load() > 0.5f;
    return s;
}

std::vector<roomeq::Band> AdaptiveRoomEQProcessor::getVoicingSections() const
{
    std::vector<roomeq::Band> out;
    const auto s = getEqSettings();
    if (! s.voicingOn)
        return out;
    for (const auto& v : s.voicing)
    {
        const auto sec = roomeq::voicingSections (v);
        for (int i = 0; i < sec.count; ++i)
            out.push_back (sec.bands[static_cast<std::size_t> (i)]);
    }
    return out;
}

void AdaptiveRoomEQProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    // Audio is stopped here, so an unfinished measurement can be dropped safely
    // (its sample rate may no longer match).
    recorder.abortWhileStopped();
    eq.prepare (sampleRate, getEqSettings());
    loudness.prepare (sampleRate, samplesPerBlock, getLoudnessSettings());
}

void AdaptiveRoomEQProcessor::releaseResources()
{
    recorder.abortWhileStopped();
    loudness.abortTapWhileStopped();
}

bool AdaptiveRoomEQProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto mainOut = layouts.getMainOutputChannelSet();
    if (mainOut != juce::AudioChannelSet::stereo())
        return false;

    if (standalone)
        return layouts.getMainInputChannelSet() == juce::AudioChannelSet::mono();

    if (layouts.getMainInputChannelSet() != mainOut)
        return false;

    // The mic sidechain is mono. Hosts may also leave it disconnected (disabled).
    // Some hosts only offer stereo sidechains; accept that and use the first channel.
    if (layouts.inputBuses.size() > micBusIndex)
    {
        const auto mic = layouts.getChannelSet (true, micBusIndex);
        if (! mic.isDisabled() && mic != juce::AudioChannelSet::mono() && mic != juce::AudioChannelSet::stereo())
            return false;
    }
    return true;
}

const float* AdaptiveRoomEQProcessor::findMic (juce::AudioBuffer<float>& buffer)
{
    if (standalone)
        return getTotalNumInputChannels() > 0 ? buffer.getReadPointer (0) : nullptr;

    if (auto* micBus = getBus (true, micBusIndex); micBus != nullptr && micBus->isEnabled())
    {
        const auto mic = getBusBuffer (buffer, true, micBusIndex);
        if (mic.getNumChannels() > 0)
            return mic.getReadPointer (0);
    }
    return nullptr;
}

void AdaptiveRoomEQProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const auto numSamples = buffer.getNumSamples();

    // Plugin: the mic sidechain sits on a channel above the main outputs.
    // Standalone: the mic is input channel 0, which shares memory with output
    // channel 0; the recorder reads each mic sample before writing that output
    // sample, so the sweep never overwrites the recording.
    auto mainOut = getBusBuffer (buffer, false, 0);
    const auto* mic = findMic (buffer);
    if (mic != nullptr)
    {
        const auto range = juce::FloatVectorOperations::findMinAndMax (mic, numSamples);
        updatePeak (micPeak, juce::jmax (range.getEnd(), -range.getStart()));
    }

    // Normal measurements replace the EQ'd program with the raw test signal (and
    // "Measure from music" records the EQ'd program as its reference): EQ first.
    // Verify measurements play the test signal through the EQ: EQ last.
    const auto settings = getEqSettings();
    const auto throughEq = recorder.playsThroughEq();
    auto* const* channels = mainOut.getArrayOfWritePointers();
    const auto numChannels = mainOut.getNumChannels();
    if (! throughEq)
    {
        if (standalone)
            eq.skip (settings);     // the app only ever outputs test signals
        else
            eq.process (channels, numChannels, numSamples, settings);
    }
    const auto wroteOutput = recorder.process (channels, numChannels, mic, numSamples, throughEq);
    if (throughEq)
        eq.process (channels, numChannels, numSamples, settings);

    // The standalone app never passes its input (the mic) to the speakers.
    if (standalone && ! wroteOutput)
        mainOut.clear();

    // Loudness compensation (the standalone app has no program to compensate).
    // Flat and not tracking while a measurement or calibration plays.
    if (! standalone)
        loudness.process (channels, numChannels, mic, numSamples, getLoudnessSettings(), recorder.isActive());

    updatePeak (outputPeak, mainOut.getMagnitude (0, numSamples));
}

bool AdaptiveRoomEQProcessor::isMicConnected() const
{
    if (standalone)
        return getTotalNumInputChannels() > 0;
    const auto* micBus = getBus (true, micBusIndex);
    return micBus != nullptr && micBus->isEnabled();
}

MeasurementEngine::SweepSettings AdaptiveRoomEQProcessor::getSweepSettings() const
{
    MeasurementEngine::SweepSettings s;
    const auto index = [this] (const juce::ParameterID& id)
    { return juce::roundToInt (parameters.getRawParameterValue (id.getParamID())->load()); };
    s.seconds = sweepSeconds[juce::jlimit (0, 2, index (ParamIds::sweepLength))];
    s.repeats = juce::jlimit (0, 2, index (ParamIds::sweepsPerPosition)) + 1;
    // Standalone: the speaker is chosen as a physical output in the editor, and
    // the sweep always plays on the first output channel.
    s.channel = standalone ? 0 : juce::jlimit (0, 1, index (ParamIds::sweepSpeaker));
    s.levelDbfs = parameters.getRawParameterValue (ParamIds::sweepLevel.getParamID())->load();
    return s;
}

bool AdaptiveRoomEQProcessor::isNoiseSelected() const
{
    return parameters.getRawParameterValue (ParamIds::signal.getParamID())->load() > 0.5f;
}

double AdaptiveRoomEQProcessor::getNoiseSeconds() const
{
    const auto index = juce::roundToInt (parameters.getRawParameterValue (ParamIds::noiseLength.getParamID())->load());
    return noiseSeconds[juce::jlimit (0, 2, index)];
}

int AdaptiveRoomEQProcessor::getSmoothingFraction() const
{
    const auto index = juce::roundToInt (parameters.getRawParameterValue (ParamIds::smoothing.getParamID())->load());
    return smoothingFractions[juce::jlimit (0, 2, index)];
}

juce::Result AdaptiveRoomEQProcessor::startSweep (int replaceId, bool verify)
{
    if (! isMicConnected())
        return juce::Result::fail ("Connect the measurement mic first");
    return engine.startSweep (getSampleRate(), getSweepSettings(), replaceId, verify);
}

juce::Result AdaptiveRoomEQProcessor::startMeasurement (int replaceId)
{
    return isNoiseSelected() ? startNoise (replaceId) : startSweep (replaceId);
}

juce::Result AdaptiveRoomEQProcessor::startVerify()
{
    return isNoiseSelected() ? startNoise (-1, true) : startSweep (-1, true);
}

juce::Result AdaptiveRoomEQProcessor::startNoise (int replaceId, bool verify)
{
    if (! isMicConnected())
        return juce::Result::fail ("Connect the measurement mic first");
    const auto s = getSweepSettings();
    return engine.startNoise (getSampleRate(), getNoiseSeconds(), s.channel, s.levelDbfs, replaceId, verify);
}

// ---------------------------------------------------------------------------
// Targets

int AdaptiveRoomEQProcessor::getTargetChoice() const
{
    return juce::jlimit (0, 3, juce::roundToInt (raw (ParamIds::target)));
}

roomeq::TargetCurve AdaptiveRoomEQProcessor::getTarget() const
{
    const auto choice = getTargetChoice();
    if (choice == targetCustom)
        return getCustomTarget();
    return roomeq::targetPresets()[static_cast<std::size_t> (choice)];
}

roomeq::TargetCurve AdaptiveRoomEQProcessor::getCustomTarget() const
{
    const std::lock_guard<std::mutex> guard (customLock);
    return customTarget;
}

void AdaptiveRoomEQProcessor::setCustomTarget (const roomeq::TargetCurve& target)
{
    const std::lock_guard<std::mutex> guard (customLock);
    customTarget.name = target.name.empty() ? std::string ("Custom") : target.name;
    customTarget.points = roomeq::sanitizeTargetPoints (target.points);
}

juce::File AdaptiveRoomEQProcessor::getTargetsFolder()
{
    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
   #if JUCE_MAC
    base = base.getChildFile ("Application Support");
   #endif
    return base.getChildFile ("Adaptive Room EQ").getChildFile ("Targets");
}

juce::Array<juce::File> AdaptiveRoomEQProcessor::getSavedTargets() const
{
    auto files = getTargetsFolder().findChildFiles (juce::File::findFiles, false, "*.json");
    files.sort();
    return files;
}

juce::Result AdaptiveRoomEQProcessor::saveCustomTarget (const juce::String& name)
{
    const auto clean = juce::File::createLegalFileName (name.trim());
    if (clean.isEmpty())
        return juce::Result::fail ("Give the target a name");
    // Saved at 0.1 Hz / 0.01 dB (plenty for a target), written as short decimals
    // so the file reads back exactly, on every platform; the session keeps the
    // same rounded points.
    auto target = getCustomTarget();
    target.name = name.trim().toStdString();
    juce::Array<juce::var> points;
    for (auto& [f, g] : target.points)
    {
        f = std::round (f * 10.0) / 10.0;
        g = std::round (g * 100.0) / 100.0;
        points.add (juce::Array<juce::var> { f, g });
    }
    auto* obj = new juce::DynamicObject();
    obj->setProperty ("name", name.trim());
    obj->setProperty ("points", points);
    const auto folder = getTargetsFolder();
    if (! folder.createDirectory())
        return juce::Result::fail ("Couldn't create " + folder.getFullPathName());
    const auto file = folder.getChildFile (clean + ".json");
    if (! file.replaceWithText (juce::JSON::toString (juce::var (obj), false, 2)))
        return juce::Result::fail ("Couldn't write " + file.getFullPathName());
    setCustomTarget (target);
    return juce::Result::ok();
}

juce::Result AdaptiveRoomEQProcessor::loadTarget (const juce::File& file)
{
    const auto json = juce::JSON::parse (file.loadFileAsString());
    const auto* list = json["points"].getArray();
    if (list == nullptr || list->isEmpty())
        return juce::Result::fail (file.getFileName() + " isn't a target file");
    roomeq::TargetCurve t;
    t.name = json["name"].toString().toStdString();
    if (t.name.empty())
        t.name = file.getFileNameWithoutExtension().toStdString();
    for (const auto& p : *list)
        if (p.isArray() && p.size() >= 2)
            t.points.push_back ({ static_cast<double> (p[0]), static_cast<double> (p[1]) });
    if (t.points.empty())
        return juce::Result::fail (file.getFileName() + " has no points");
    setCustomTarget (t);
    if (auto* param = parameters.getParameter (ParamIds::target.getParamID()))
        param->setValueNotifyingHost (param->convertTo0to1 (static_cast<float> (targetCustom)));
    return juce::Result::ok();
}

MeasurementEngine::CorrectionSettings AdaptiveRoomEQProcessor::getCorrectionSettings() const
{
    MeasurementEngine::CorrectionSettings s;
    s.target = getTarget();
    s.config.maxCutDb = raw (ParamIds::maxCut);
    s.config.maxBoostDb = raw (ParamIds::maxBoost);
    s.config.rangeLoHz = raw (ParamIds::rangeLo);
    s.config.rangeHiHz = raw (ParamIds::rangeHi);
    s.fs = getSampleRate() > 0.0 ? getSampleRate() : 48000.0;
    return s;
}

juce::Result AdaptiveRoomEQProcessor::startProgram (int replaceId)
{
    if (standalone)
        return juce::Result::fail ("Measuring from music needs the program to pass through the plugin, "
                                   "so it's only available in the plugin inside your DAW.");
    if (! isMicConnected())
        return juce::Result::fail ("Connect the measurement mic first");
    return engine.startProgram (getSampleRate(), programSeconds, replaceId);
}

juce::AudioProcessorEditor* AdaptiveRoomEQProcessor::createEditor()
{
    return new AdaptiveRoomEQEditor (*this);
}

void AdaptiveRoomEQProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::ValueTree root ("AdaptiveRoomEQ");
    root.setProperty ("stateVersion", 1, nullptr);
    root.appendChild (parameters.copyState(), nullptr);
    root.appendChild (engine.toValueTree(), nullptr);
    root.appendChild (loudnessControl.toValueTree(), nullptr);
    const auto custom = getCustomTarget();
    juce::ValueTree ct ("CustomTarget");
    ct.setProperty ("name", juce::String::fromUTF8 (custom.name.c_str()), nullptr);
    for (const auto& [f, g] : custom.points)
    {
        juce::ValueTree pt ("Point");
        pt.setProperty ("f", f, nullptr);
        pt.setProperty ("db", g, nullptr);
        ct.appendChild (pt, nullptr);
    }
    root.appendChild (ct, nullptr);
    juce::MemoryOutputStream stream (destData, false);
    root.writeToStream (stream);
}

void AdaptiveRoomEQProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto root = juce::ValueTree::readFromData (data, static_cast<size_t> (sizeInBytes));
    if (! root.hasType ("AdaptiveRoomEQ"))
        return;
    if (const auto params = root.getChildWithName (parameters.state.getType()); params.isValid())
        parameters.replaceState (params);
    if (const auto ct = root.getChildWithName ("CustomTarget"); ct.isValid() && ct.getNumChildren() > 0)
    {
        roomeq::TargetCurve t;
        t.name = ct["name"].toString().toStdString();
        for (const auto& pt : ct)
            t.points.push_back ({ static_cast<double> (pt["f"]), static_cast<double> (pt["db"]) });
        setCustomTarget (t);
    }
    engine.fromValueTree (root.getChildWithName (MeasurementEngine::treeType));
    loudnessControl.fromValueTree (root.getChildWithName (LoudnessController::treeType));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AdaptiveRoomEQProcessor();
}
