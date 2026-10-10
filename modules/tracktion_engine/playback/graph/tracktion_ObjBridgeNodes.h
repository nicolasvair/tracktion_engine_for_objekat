/*
    OBJEKAT — the audio bridge: the three graph nodes (docs/plan_sidechain.md §5.4).

    BridgeTapNode     copies a source's signal into the tap plugin's ring, passing it through unchanged.
    BridgeReaderNode  reads the ring, D stream samples back, and declares the latency X.
    BridgeGateNode    an ordering edge: waits for the units of a lower rank, produces nothing.

    Nothing here allocates, locks or constructs a std::function in process().
*/

#pragma once

namespace tracktion::inline engine
{

//==============================================================================
/** A pass-through node that records its input into a ring, indexed by STREAM sample.

    A SEND tap (BridgeTapSource::processesTapCopy) runs its plugin on the COPY it records — once per
    block, never in sub-blocks, never on the signal that passes through — and writes the result:
    that is the send level applied at the tap (docs/plan_sends_rework.md §A.2). Such a plugin is
    initialised here like a PluginNode would, and its edit time is moved back by the input's age.

    It reports its input's properties unchanged (latency included): the tap's age is the age of
    its input, which is what the readers' arithmetic needs. It never relies on `numOutputNodes`
    (-1 inside a TimedNode: the trap that killed LatencyMaskingNode, patch 0019).

    A send that carries plugins has a SECOND input, `wet`: the head node below, then the send's
    mini-chain. The node is not a sum — what it outputs is still the dry input, untouched, and that
    is what it declares; the wet is what the level is applied to and what the ring receives, at the
    age the builder registered for it (the wet's own latency). docs/plan_sends_rework.md §D.2.A. */
class BridgeTapNode final : public tracktion::graph::Node,
                            public TracktionEngineNode,
                            public BridgeRankedNode
{
public:
    BridgeTapNode (ProcessState&,
                   std::shared_ptr<tracktion::graph::Node> input,
                   std::unique_ptr<tracktion::graph::Node> wet,     // null = no mini-chain
                   Plugin::Ptr tapPlugin,
                   BridgeTapSource&,   // the tap's persistent state; the ring is reached through BridgeBuild
                   std::shared_ptr<BridgeBuild>,
                   int rank,
                   double sampleRate, int blockSize, bool rendering);
    ~BridgeTapNode() override;

    //==============================================================================
    tracktion::graph::NodeProperties getNodeProperties() override;
    std::vector<tracktion::graph::Node*> getDirectInputNodes() override;
    bool isReadyToProcess() override;
    void prepareToPlay (const tracktion::graph::PlaybackInitialisationInfo&) override;
    void process (ProcessContext&) override;

    int getBridgeRank() const override                  { return rank; }

private:
    std::shared_ptr<tracktion::graph::Node> input;      // shared with the head node when there is a wet
    std::unique_ptr<tracktion::graph::Node> wet;
    Plugin::Ptr tapPlugin;
    BridgeTapSource& source;
    std::shared_ptr<BridgeBuild> build;
    const int rank;
    const EditItemID tapID;
    std::shared_ptr<objbridge::Ring> ring;      // set in prepareToPlay, null = nobody reads this tap

    // Copy mode (a send): the plugin runs on `scratch`, which is what the ring receives.
    const bool copyMode;
    const bool isRendering;
    bool pluginInitialised = false;
    TimeDuration automationAdjustmentTime;
    juce::AudioBuffer<float> scratch;           // 2 channels, sized in prepareToPlay
    MidiMessageArray midiScratch;
};

//==============================================================================
/** The head of a send's wet branch: a 2-channel COPY of the tapped signal, on which the send does what
    precedes its plugins (BridgeTapSource::processTapHead — the sender's window and fades), at the edit
    time the signal was made. The second consumer of the tap's input; the send's mini-chain follows. */
class BridgeTapHeadNode final : public tracktion::graph::Node,
                                public TracktionEngineNode
{
public:
    BridgeTapHeadNode (ProcessState&, std::shared_ptr<tracktion::graph::Node> input,
                       Plugin::Ptr tapPlugin, BridgeTapSource&, double sampleRate);

    //==============================================================================
    tracktion::graph::NodeProperties getNodeProperties() override;
    std::vector<tracktion::graph::Node*> getDirectInputNodes() override;
    bool isReadyToProcess() override;
    void process (ProcessContext&) override;

private:
    std::shared_ptr<tracktion::graph::Node> input;
    Plugin::Ptr tapPlugin;                      // keeps `source` alive
    BridgeTapSource& source;
    const EditItemID tapID;
    TimeDuration adjustmentTime;
};

//==============================================================================
/** Delivers a key: stereo audio read from a tap's ring, `delay` stream samples back, declaring
    the latency `X` fixed when it was registered (BridgeBuild::registerReader).

    In a TimedNode (a clip's chain) it is a LEAF and must be ready at once; with a rank >= 1 it owns
    a BridgeGateNode, which stays an always-ready leaf there (no ranked node lives in a clip chain —
    object taps report -1), and does its work when the reader sits in the root or a container graph. */
class BridgeReaderNode final : public tracktion::graph::Node,
                               public TracktionEngineNode
{
public:
    BridgeReaderNode (ProcessState&, std::shared_ptr<BridgeBuild>, int readerIndex, int rank, size_t nodeID);

    //==============================================================================
    tracktion::graph::NodeProperties getNodeProperties() override;
    std::vector<tracktion::graph::Node*> getDirectInputNodes() override;
    bool isReadyToProcess() override;
    void prepareToPlay (const tracktion::graph::PlaybackInitialisationInfo&) override;
    void process (ProcessContext&) override;

private:
    std::shared_ptr<BridgeBuild> build;
    const int readerIndex;
    const size_t nodeID;
    const int declaredLatencyNumSamples;
    std::unique_ptr<tracktion::graph::Node> gate;       // BridgeGateNode, rank >= 1 only
    BridgeBuild::ReaderPlan plan;
    BridgeBuild::Counters* counters = nullptr;
};

//==============================================================================
/** Waits for every BridgeRankedNode of a lower rank to have processed this block. Produces nothing:
    it exists to be a direct input, so the scheduler runs the readers' unit after the writers'.

    It never adds an edge to a tap (a tap lives inside a TimedNode, where an outer edge would have
    it scheduled twice): it waits for the pool tracks' combiners and the stems' taps, which are
    ordinary nodes of the enclosing graph. A candidate whose subgraph reaches this gate would close
    a cycle and is refused — the app's ranks are trusted, but a wrong rank must never hang the player. */
class BridgeGateNode final : public tracktion::graph::Node
{
public:
    BridgeGateNode (int rank, size_t nodeID, std::shared_ptr<BridgeBuild>);

    //==============================================================================
    tracktion::graph::TransformResult transform (TransformOptions&) override;
    tracktion::graph::NodeProperties getNodeProperties() override;
    std::vector<tracktion::graph::Node*> getDirectInputNodes() override;
    bool isReadyToProcess() override;
    void process (ProcessContext&) override;

private:
    const int rank;
    const size_t nodeID;
    std::shared_ptr<BridgeBuild> build;
    bool hasTransformed = false;
    std::vector<tracktion::graph::Node*> waitsFor;
};

} // namespace tracktion::inline engine
