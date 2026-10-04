/*
    OBJEKAT — the audio bridge: BridgeBuild.
    See tracktion_ObjBridge.h and docs/plan_sidechain.md.
*/

namespace tracktion::inline engine
{

namespace
{
    // Edit* -> its newest published build. A FUNCTION-LOCAL static guarded by a mutex (engine patch
    // 0032: function statics are shared between threads that build graphs at the same time).
    struct LatestMap
    {
        std::mutex mutex;
        std::map<const Edit*, std::weak_ptr<const BridgeBuild>> map;
    };

    LatestMap& getLatestMap()
    {
        static LatestMap latest;
        return latest;
    }

    const char* statusName (objbridge::ReaderStatus s)
    {
        switch (s)
        {
            case objbridge::ReaderStatus::aligned:       return "aligned";
            case objbridge::ReaderStatus::late:          return "late";
            case objbridge::ReaderStatus::overDeclared:  return "over_declared";
            case objbridge::ReaderStatus::sourceAbsent:  return "source_absent";
        }

        return "?";
    }
}

//==============================================================================
BridgeBuild::BridgeBuild (Edit& e, uint64_t id, int pass, double sr, int bs)
    : edit (e), buildID (id), passIndex (pass), sampleRate (sr), blockSize (bs)
{
}

BridgeBuild::~BridgeBuild() = default;

uint64_t BridgeBuild::nextBuildID() noexcept
{
    static std::atomic<uint64_t> counter { 0 };
    return ++counter;
}

//==============================================================================
const BridgeBuild::TapEntry* BridgeBuild::findTap (EditItemID id) const
{
    for (auto& t : taps)
        if (t.id == id)
            return &t;

    return nullptr;
}

void BridgeBuild::registerTap (Plugin& tapPlugin, BridgeTapSource& source, int ageNumSamples, int rank)
{
    const std::lock_guard<std::mutex> lock (mutex);

    TapEntry entry;
    entry.id = tapPlugin.itemID;
    entry.plugin = &tapPlugin;
    entry.source = &source;
    entry.age = ageNumSamples;
    entry.rank = rank;

    for (auto& t : taps)
    {
        if (t.id == entry.id)
        {
            t = std::move (entry);      // the same plugin built twice in one pass: the last one wins
            return;
        }
    }

    taps.push_back (std::move (entry));
}

bool BridgeBuild::hasTap (EditItemID tapPluginID) const
{
    if (! tapPluginID.isValid())
        return false;

    auto p = edit.getPluginCache().getPluginFor (tapPluginID);

    // Still attached: the cache is pruned by a timer, so a plugin removed a moment ago can linger.
    return p != nullptr
            && dynamic_cast<BridgeTapSource*> (p.get()) != nullptr
            && p->state.getParent().isValid();
}

int BridgeBuild::registerReader (EditItemID tapPluginID, EditItemID destPluginID, Plugin* destPlugin,
                                 Consumer consumer, int referenceLatency, uint64_t groupKey)
{
    int cached = -1;

    if (auto p = edit.getPluginCache().getPluginFor (tapPluginID))
        if (auto* src = dynamic_cast<BridgeTapSource*> (p.get()))
            cached = objbridge::scaleCachedAge (src->cachedAgeNumSamples, src->cachedAgeSampleRate, sampleRate);

    const std::lock_guard<std::mutex> lock (mutex);

    ReaderEntry r;
    r.tapID = tapPluginID;
    r.destID = destPluginID;
    r.destPlugin = destPlugin;
    r.consumer = consumer;
    r.referenceLatency = referenceLatency;
    r.declared = objbridge::declaredLatency (referenceLatency, cached);
    r.groupKey = groupKey;

    readers.push_back (std::move (r));
    readerCounters.emplace_back();
    return (int) readers.size() - 1;
}

int BridgeBuild::getDeclaredLatency (int readerIndex) const
{
    jassert (juce::isPositiveAndBelow (readerIndex, (int) readers.size()));
    return readers[(size_t) readerIndex].declared;
}

void BridgeBuild::noteGateEdges (int added, int refused)
{
    gateEdges += added;
    gateRefused += refused;
}

//==============================================================================
bool BridgeBuild::finalise()
{
    const std::lock_guard<std::mutex> lock (mutex);
    bool again = false;

    // 1. Every reader, now that every tap of the pass exists (containers' content included: it is
    //    constructed synchronously by createNodeForContainerClip).
    for (auto& r : readers)
    {
        const TapEntry* tap = findTap (r.tapID);

        r.sourceAge = tap != nullptr ? tap->age : 0;
        r.resolution = objbridge::resolve (r.referenceLatency, r.declared, tap != nullptr, r.sourceAge);

        if (r.resolution.status == objbridge::ReaderStatus::late
             || r.resolution.status == objbridge::ReaderStatus::overDeclared)
            again = true;
    }

    converged = ! again;
    return again;
}

void BridgeBuild::allocateRings()
{
    const std::lock_guard<std::mutex> lock (mutex);

    // Every ring: the tap plugin's own if it is big enough, else a new one that only the graphs
    //    built from now on will reference (a ring is never resized in place).
    for (auto& t : taps)
    {
        int maxDelay = 0;

        for (auto& r : readers)
            if (r.tapID == t.id)
                maxDelay = std::max (maxDelay, r.resolution.delay);

        const int needed = objbridge::requiredRingCapacity (maxDelay, blockSize);

        if (t.source->ring != nullptr
             && t.source->ring->getNumChannels() == 2
             && t.source->ring->getCapacity() >= needed)
        {
            t.ring = t.source->ring;
        }
        else
        {
            t.ring = std::make_shared<objbridge::Ring> (2, needed);
            t.source->ring = t.ring;
            ++t.source->ringGeneration;
        }
    }
}

void BridgeBuild::publish()
{
    auto self = shared_from_this();
    auto& latest = getLatestMap();
    const std::lock_guard<std::mutex> lock (latest.mutex);

    for (auto it = latest.map.begin(); it != latest.map.end();)
        it = it->second.expired() ? latest.map.erase (it) : std::next (it);

    latest.map[&edit] = self;
}

std::shared_ptr<const BridgeBuild> BridgeBuild::latestFor (const Edit& e)
{
    auto& latest = getLatestMap();
    const std::lock_guard<std::mutex> lock (latest.mutex);

    if (auto it = latest.map.find (&e); it != latest.map.end())
        return it->second.lock();

    return {};
}

//==============================================================================
BridgeBuild::ReaderPlan BridgeBuild::readerPlan (int readerIndex) const
{
    ReaderPlan plan;

    if (! juce::isPositiveAndBelow (readerIndex, (int) readers.size()))
        return plan;

    auto& r = readers[(size_t) readerIndex];
    plan.ring = ringForTap (r.tapID);
    plan.delay = r.resolution.delay;
    plan.status = r.resolution.status;
    return plan;
}

std::shared_ptr<objbridge::Ring> BridgeBuild::ringForTap (EditItemID id) const
{
    if (auto* t = findTap (id))
        return t->ring;

    return {};
}

BridgeBuild::Counters& BridgeBuild::counters (int readerIndex) const
{
    jassert (juce::isPositiveAndBelow (readerIndex, (int) readerCounters.size()));
    return readerCounters[(size_t) readerIndex];
}

//==============================================================================
BridgeReport BridgeBuild::getReport() const
{
    const std::lock_guard<std::mutex> lock (mutex);
    BridgeReport report;

    report.build.id = buildID;
    report.build.passes = passIndex + 1;
    report.build.converged = converged;
    report.build.sampleRate = sampleRate;
    report.build.blockSize = blockSize;
    report.build.gateEdges = gateEdges.load();
    report.build.gateRefused = gateRefused.load();

    for (auto& t : taps)
    {
        BridgeReport::Tap out;
        out.tap = t.id;
        out.source = t.plugin != nullptr ? t.plugin->state[objbridge_ids::source].toString() : juce::String();
        out.rank = t.rank;
        out.age = t.age;
        out.cachedAge = t.source->cachedAgeNumSamples;
        out.ringGeneration = t.source->ringGeneration;

        if (t.ring != nullptr)
        {
            out.ringCapacity = t.ring->getCapacity();
            out.latestEnd = t.ring->getLatestEnd();

            std::array<std::array<int64_t, 2>, 4> snapshot {};
            const int n = t.ring->getRunsSnapshot (snapshot);

            for (int i = 0; i < n; ++i)
                out.runs.emplace_back (snapshot[(size_t) i][0], snapshot[(size_t) i][1]);
        }

        report.taps.push_back (std::move (out));
    }

    for (size_t i = 0; i < readers.size(); ++i)
    {
        auto& r = readers[i];
        BridgeReport::Reader out;

        out.plugin = r.destID;
        out.destInstance = juce::String::toHexString ((juce::int64) (intptr_t) r.destPlugin.get());
        out.tap = r.tapID;
        out.consumer = r.consumer == Consumer::sidechain ? "sidechain" : "auxInput";
        out.rank = r.destPlugin != nullptr ? (int) r.destPlugin->state.getProperty (objbridge_ids::rank, 0) : 0;
        out.lRef = r.referenceLatency;
        out.declared = r.declared;
        out.sourceAge = r.sourceAge;
        out.delay = r.resolution.delay;
        out.status = statusName (r.resolution.status);
        out.alignmentErrorSamples = r.resolution.alignmentError;
        out.blocksRead = readerCounters[i].blocksRead.load();
        out.blocksUncovered = readerCounters[i].blocksUncovered.load();
        out.blocksTorn = readerCounters[i].blocksTorn.load();

        report.readers.push_back (std::move (out));
    }

    return report;
}

} // namespace tracktion::inline engine
