/*
    OBJEKAT — the audio bridge: public interface.

    A key (a plugin's sidechain input; later an aux's input) travels from a TAP, at the end of a
    source's chain, to a READER in the destination's chain, through a ring that belongs to the tap
    plugin and outlives every graph. Neither Tracktion's sends nor its sidechain wires can do that:
    a container is a closed local graph, and an edge cannot enter or leave it. The design, the
    latency model and the ranks are in docs/plan_sidechain.md (§4, §5).

    The nodes themselves (tap, reader, gate) are private to playback:
    playback/graph/tracktion_ObjBridgeNodes.h.
*/

#pragma once

#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include "tracktion_ObjBridgeCore.h"

namespace tracktion::inline engine
{

namespace objbridge_ids
{
    /** int: on pool AudioTracks, child clips, aux clips, tap plugins and destination plugins.
        Absent = 0 (for a tap: -1, "never a gate target"). */
    inline const juce::Identifier rank   ("objBridgeRank");

    /** string: the model UUID a tap plugin serves (report only). */
    inline const juce::Identifier source ("objBridgeSource");
}

//==============================================================================
/** Implemented by an app plugin that marks a TAP point. The builder never processes it through a
    PluginNode: it builds a BridgeTapNode in its place (createPluginNodeForList). */
struct BridgeTapSource
{
    virtual ~BridgeTapSource() = default;

    // Message thread only (builder + app), never the audio thread.
    int    cachedAgeNumSamples = -1;           /**< The tap's age at the previous build. -1 = never built. */
    double cachedAgeSampleRate = 0.0;          /**< The sample rate that age was taken at. */
    std::shared_ptr<objbridge::Ring> ring;     /**< Swapped only when it must GROW; old graphs keep theirs. */
    int    ringGeneration = 0;

    // Phase 2 — a bridge SEND: the aux it targets (invalid for a sidechain tap), and whether the
    // tap must run the plugin on its copy (the send level).
    virtual EditItemID getBridgeTargetAuxClipID() const     { return {}; }
    virtual bool processesTapCopy() const                   { return false; }
};

//==============================================================================
/** A node the gates may wait for: pool-track / per-rank CombiningNodes, stem BridgeTapNodes.
    getBridgeRank() < 0 means "never a gate target" (object taps, which live inside TimedNodes). */
struct BridgeRankedNode
{
    virtual ~BridgeRankedNode() = default;
    virtual int getBridgeRank() const = 0;
};

//==============================================================================
/** Plain structs mirroring docs/plan_sidechain.md §4.8, filled by BridgeBuild::getReport(). */
struct BridgeReport
{
    struct Build
    {
        uint64_t id = 0;
        int passes = 0;
        bool converged = false;
        double sampleRate = 0.0;
        int blockSize = 0;
        int gateEdges = 0, gateRefused = 0;
    };

    struct Tap
    {
        EditItemID tap;
        juce::String source;                     // the model UUID, from objbridge_ids::source
        int rank = -1, age = 0, cachedAge = -1;
        int ringCapacity = 0, ringGeneration = 0;
        int64_t latestEnd = 0;
        std::vector<std::pair<int64_t, int64_t>> runs;   // [start, end) in stream samples, newest first
    };

    struct Reader
    {
        EditItemID plugin;                       // the destination plugin (the app maps it to its model UUID)
        juce::String destInstance;               // hex pointer of the live plugin: proves no reload across an undo
        EditItemID tap;
        juce::String consumer;                   // "sidechain" | "auxInput"
        int rank = 0;
        int lRef = 0, declared = 0, sourceAge = 0, delay = 0;
        juce::String status;                     // "aligned" | "late" | "over_declared" | "source_absent"
        int alignmentErrorSamples = 0;
        uint32_t blocksRead = 0, blocksUncovered = 0, blocksTorn = 0;
    };

    Build build;
    std::vector<Tap> taps;
    std::vector<Reader> readers;
};

//==============================================================================
/** One per construction PASS of createNodeForEdit. Created and finalised on the message thread;
    read-only afterwards except the per-reader atomic counters. */
class BridgeBuild : public std::enable_shared_from_this<BridgeBuild>
{
public:
    BridgeBuild (Edit&, uint64_t buildID, int passIndex, double sampleRate, int blockSize);
    ~BridgeBuild();

    /** A fresh id for one createNodeForEdit call (not one per pass). */
    static uint64_t nextBuildID() noexcept;

    //==============================================================================
    // Construction (message thread).

    /** Called by the builder as it builds a tap node: `ageNumSamples` is the age of the tap's input. */
    void registerTap (Plugin& tapPlugin, BridgeTapSource&, int ageNumSamples, int rank);

    enum class Consumer { sidechain, auxInput };

    /** Looks the tap plugin up by id (Edit::getPluginCache().getPluginFor, checked still attached to
        this Edit). Returns the reader's index; X is computed NOW from the tap's cached age, since the
        tap may be built after the reader, in another track or at another depth. */
    int  registerReader (EditItemID tapPluginID, EditItemID destPluginID, Plugin* destPlugin,
                         Consumer, int referenceLatency, uint64_t groupKey);

    /** What the reader declares (X), fixed at registration. */
    int  getDeclaredLatency (int readerIndex) const;

    /** A BridgeTapSource plugin with that id exists in the Edit. */
    bool hasTap (EditItemID tapPluginID) const;

    /** From BridgeGateNode::transform (any thread that builds a graph). */
    void noteGateEdges (int added, int refused);

    //==============================================================================
    // End of pass (message thread).

    /** Resolves every reader (objbridge::resolve) and returns TRUE if another pass is needed (any
        reader late or over-declared). A reader whose tap was not built in this pass is `sourceAbsent`
        and asks for nothing. Touches no ring: this pass may be thrown away. */
    bool finalise();

    /** Only for the pass that is KEPT, just before publish(): sizes every ring — adopts the tap
        plugin's own if its capacity suffices, else allocates a new Ring and stores it on the plugin
        (ringGeneration++). Done once, so a discarded pass never replaces a ring (and drops the key's
        history) for the next pass to replace it again. */
    void allocateRings();

    /** Becomes latestFor (edit). */
    void publish();

    //==============================================================================
    // After finalise (prepare / audio thread, read-only).

    struct ReaderPlan
    {
        std::shared_ptr<objbridge::Ring> ring;
        int delay = 0;
        objbridge::ReaderStatus status = objbridge::ReaderStatus::sourceAbsent;
    };

    ReaderPlan readerPlan (int readerIndex) const;
    std::shared_ptr<objbridge::Ring> ringForTap (EditItemID) const;

    struct Counters
    {
        std::atomic<uint32_t> blocksRead { 0 }, blocksUncovered { 0 }, blocksTorn { 0 };
    };

    /** A stable address: the counters live in a std::deque that only ever grows at its end. */
    Counters& counters (int readerIndex) const;

    //==============================================================================
    // The app.

    /** The newest published build of that Edit, or null. Guarded by a mutex (message + render threads). */
    static std::shared_ptr<const BridgeBuild> latestFor (const Edit&);

    /** Message thread. */
    BridgeReport getReport() const;

private:
    struct TapEntry
    {
        EditItemID id;
        Plugin::Ptr plugin;
        BridgeTapSource* source = nullptr;
        int age = 0, rank = -1;
        std::shared_ptr<objbridge::Ring> ring;
    };

    struct ReaderEntry
    {
        EditItemID tapID, destID;
        Plugin::Ptr destPlugin;
        Consumer consumer = Consumer::sidechain;
        int referenceLatency = 0, declared = 0;
        uint64_t groupKey = 0;
        objbridge::ReaderResolution resolution;
        int sourceAge = 0;
    };

    const TapEntry* findTap (EditItemID) const;

    Edit& edit;
    const uint64_t buildID;
    const int passIndex;
    const double sampleRate;
    const int blockSize;

    mutable std::mutex mutex;                  // registration only: never taken on the audio thread
    std::vector<TapEntry> taps;
    std::vector<ReaderEntry> readers;
    mutable std::deque<Counters> readerCounters;

    std::atomic<int> gateEdges { 0 }, gateRefused { 0 };
    bool converged = false;

    JUCE_DECLARE_NON_COPYABLE (BridgeBuild)
};

} // namespace tracktion::inline engine
