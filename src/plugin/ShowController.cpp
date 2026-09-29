#include "plugin/ShowController.h"

#include "roomeq/loudness.h"

#include <algorithm>
#include <cmath>

namespace
{
const juce::Identifier bandsId { "bands" }, storedAtId { "storedAt" }, versionId { "version" };

juce::MemoryBlock pack (const std::vector<double>& values)
{
    juce::MemoryBlock block (values.size() * sizeof (float));
    auto* out = static_cast<float*> (block.getData());
    for (std::size_t i = 0; i < values.size(); ++i)
        out[i] = static_cast<float> (values[i]);
    return block;
}

std::vector<double> unpack (const juce::var& v)
{
    std::vector<double> values;
    if (const auto* block = v.getBinaryData())
    {
        const auto* in = static_cast<const float*> (block->getData());
        values.assign (in, in + block->getSize() / sizeof (float));
    }
    return values;
}

int finiteCount (const std::vector<double>& v)
{
    return static_cast<int> (std::count_if (v.begin(), v.end(), [] (double x) { return std::isfinite (x); }));
}
} // namespace

ShowController::ShowController (TapRecorder& tapToUse) : tap (tapToUse)
{
    startTimerHz (10);
}

ShowController::~ShowController()
{
    stopTimer();
    pool.removeAllJobs (true, 10000);
}

ShowController::Environment ShowController::environment() const
{
    return environmentSource ? environmentSource() : Environment {};
}

bool ShowController::startTap (Step newStep, double seconds)
{
    const auto fs = environment().fs;
    auto request = std::make_unique<TapRequest>();
    const auto n = static_cast<std::size_t> (seconds * fs);
    request->output.assign (n, 0.0f);
    request->mic.assign (n, 0.0f);
    if (! tap.start (std::move (request)))
        return false;
    tapFs = fs;
    step = newStep;
    blockSpoiled = false;
    return true;
}

juce::Result ShowController::storeReference()
{
    const auto env = environment();
    if (! env.available)
        return juce::Result::fail ("Show tracking runs in the plugin inside your DAW or host.");
    if (env.fs <= 0.0)
        return juce::Result::fail ("Audio isn't running yet");
    if (! env.micSignal)
        return juce::Result::fail ("There's no signal on the mic input. Route the measurement mic to the plugin's sidechain "
                                   "input, and check its gain and phantom power, then try again.");
    if (env.measuring)
        return juce::Result::fail ("Wait for the measurement to finish");
    if (step == Step::storing || storeRequested)
        return juce::Result::fail ("Already storing a reference");
    if (step == Step::tracking)
    {
        storeRequested = true;   // starts once the block in flight has stopped
        tap.cancel();
    }
    else if (! startTap (Step::storing, referenceSeconds))
    {
        return juce::Result::fail ("Still recording; try again in a moment");
    }
    status = "Storing the reference: keep the music (or pink noise) playing for 30 s";
    sendChangeMessage();
    return juce::Result::ok();
}

void ShowController::cancelStore()
{
    storeRequested = false;
    if (step == Step::storing)
        tap.cancel();
}

void ShowController::clearReference()
{
    if (step != Step::idle)
        tap.cancel();
    storeRequested = false;
    ++generation;
    {
        const std::lock_guard<std::mutex> guard (stateLock);
        reference.clear();
        tracker.reset();
        storedAt = 0;
        blocksHeard = blocksDropped = 0;
    }
    status = "Reference cleared; show tracking is off";
    sendChangeMessage();
}

float ShowController::getProgress() const
{
    return step == Step::storing ? tap.getProgress() : 0.0f;
}

ShowController::Info ShowController::getInfo() const
{
    Info i;
    const std::lock_guard<std::mutex> guard (stateLock);
    i.hasReference = ! reference.empty();
    i.storedAt = storedAt;
    i.referenceBands = finiteCount (reference);
    if (tracker)
        i.state = tracker->state();
    i.blocksHeard = blocksHeard;
    i.blocksDropped = blocksDropped;
    return i;
}

void ShowController::update()
{
    const auto env = environment();
    auto changed = false;

    if (step != Step::idle && (env.measuring || ! env.micSignal))
        blockSpoiled = true;

    if (auto done = tap.collect())
    {
        const auto was = step;
        step = Step::idle;
        changed = true;
        if (done->cancelled)
        {
            if (was == Step::storing)
                status = "Storing the reference stopped";
        }
        else if (was == Step::storing)
        {
            if (blockSpoiled)
                status = "A measurement played or the mic went quiet while storing: store the reference again";
            else
                analyse (std::move (done), true);
        }
        else if (was == Step::tracking)
        {
            if (blockSpoiled)
            {
                const std::lock_guard<std::mutex> guard (stateLock);
                ++blocksDropped;
            }
            else
            {
                analyse (std::move (done), false);
            }
        }
    }

    std::vector<Result> results;
    {
        const std::lock_guard<std::mutex> guard (mailbox->lock);
        results.swap (mailbox->results);
    }
    for (const auto& r : results)
    {
        --pending;
        handle (r);
        changed = true;
    }

    // Keep going: a reference that was asked for, else the next show block.
    if (step == Step::idle && env.available && env.fs > 0.0 && ! tap.isBusy())
    {
        if (storeRequested)
        {
            storeRequested = false;
            startTap (Step::storing, referenceSeconds);
        }
        else
        {
            bool tracking = false;
            {
                const std::lock_guard<std::mutex> guard (stateLock);
                tracking = tracker.has_value();
            }
            if (tracking && env.micSignal && ! env.measuring)
                startTap (Step::tracking, blockSeconds);
        }
    }

    if (changed)
        sendChangeMessage();
}

void ShowController::analyse (std::unique_ptr<TapRequest> done, bool isReference)
{
    ++pending;
    if (isReference)
        status = "Analysing the reference...";
    std::shared_ptr<TapRequest> t = std::move (done);
    pool.addJob ([mb = mailbox, t, fs = tapFs, isReference, gen = generation]
    {
        Result r;
        r.reference = isReference;
        r.generation = gen;
        r.bands = roomeq::transferBandsDb (std::vector<double> (t->output.begin(), t->output.end()),
                                           std::vector<double> (t->mic.begin(), t->mic.end()), fs);
        const std::lock_guard<std::mutex> guard (mb->lock);
        mb->results.push_back (std::move (r));
    });
}

void ShowController::handle (const Result& r)
{
    if (r.generation != generation)
        return;   // measured before the reference changed
    if (r.reference)
    {
        const auto heard = finiteCount (r.bands);
        if (! roomeq::referenceIsUsable (r.bands))
        {
            status = "Not enough of the music reached the mic clearly (" + juce::String (heard)
                     + " of 22 bands): play full-range music or pink noise a little louder, and store it again";
            return;
        }
        {
            const std::lock_guard<std::mutex> guard (stateLock);
            reference = r.bands;
            tracker.emplace (reference);
            storedAt = juce::Time::currentTimeMillis();
            blocksHeard = blocksDropped = 0;
        }
        ++generation;   // show blocks analysed before this reference don't count
        status = "Reference stored (" + juce::String (heard) + " of 22 bands heard clearly); tracking the show";
        return;
    }
    const std::lock_guard<std::mutex> guard (stateLock);
    if (tracker)
    {
        tracker->addBlock (r.bands);
        ++blocksHeard;
    }
}

juce::ValueTree ShowController::toValueTree() const
{
    juce::ValueTree t (treeType);
    const std::lock_guard<std::mutex> guard (stateLock);
    t.setProperty (versionId, 1, nullptr);
    if (! reference.empty())
    {
        t.setProperty (bandsId, pack (reference), nullptr);
        t.setProperty (storedAtId, storedAt, nullptr);
    }
    return t;
}

void ShowController::fromValueTree (const juce::ValueTree& t)
{
    const std::lock_guard<std::mutex> guard (stateLock);
    reference.clear();
    tracker.reset();
    storedAt = 0;
    blocksHeard = blocksDropped = 0;
    if (! t.hasType (treeType))
        return;
    auto bands = unpack (t[bandsId]);
    if (bands.size() == roomeq::recheckBands().size() && roomeq::referenceIsUsable (bands))
    {
        reference = std::move (bands);
        tracker.emplace (reference);
        storedAt = static_cast<juce::int64> (t[storedAtId]);
    }
}
