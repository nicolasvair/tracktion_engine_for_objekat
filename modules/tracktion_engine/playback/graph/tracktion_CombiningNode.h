/*
    ,--.                     ,--.     ,--.  ,--.
  ,-'  '-.,--.--.,--,--.,---.|  |,-.,-'  '-.`--' ,---. ,--,--,      Copyright 2024
  '-.  .-'|  .--' ,-.  | .--'|     /'-.  .-',--.| .-. ||      \   Tracktion Software
    |  |  |  |  \ '-'  \ `--.|  \  \  |  |  |  |' '-' '|  ||  |       Corporation
    `---' `--'   `--`--'`---'`--'`--' `---' `--' `---' `--''--'    www.tracktion.com

    Tracktion Engine uses a GPL/commercial licence - see LICENCE.md for details.
*/

namespace tracktion::inline engine {

/** An Node that mixes a sequence of clips of other nodes.

    This node takes a set of input Nodes with associated start + end times,
    and mixes together their output.

    It initialises and releases its inputs as required according to its current
    play position.
*/
class CombiningNode final : public tracktion::graph::Node,
                            public TracktionEngineNode,
                            public BridgeRankedNode
{
public:
    CombiningNode (EditItemID, ProcessState&);
    ~CombiningNode() override;

    //==============================================================================
    /** Adds an input node to be played at a given time range.

        The offset is relative to the combining node's zero-time, so the input node's
        time of 0 is equal to its (start + offset) relative to the combiner node's start.

        Any nodes passed-in will be deleted by this node when required.
    */
    void addInput (std::unique_ptr<Node>, TimeRange, bool ignoreLatency = false);

    /** Adds an input node to be played at a given beat range.

        The offset is relative to the combining node's zero-time, so the input node's
        time of 0 is equal to its (start + offset) relative to the combiner node's start.

        Any nodes passed-in will be deleted by this node when required.
    */
    void addInput (std::unique_ptr<Node>, BeatRange, bool ignoreLatency = false);

    /** Returns the number of inputs added. */
    int getNumInputs() const;

    //==============================================================================
    // Objekat — the audio bridge's scheduling order (docs/plan_sidechain.md §5.4, §5.5).
    // A combiner is a unit the bridge's gates may wait for, and may itself wait for a gate:
    // a reader inside its clips reads a key that a LOWER-rank unit writes, so the lower rank
    // has to have finished the block first. Construction only.

    /** The bridge rank of this combiner. Default 0: every existing pool track is a gate target
        for a rank >= 1, which is what makes "everything below me" right with no list of names. */
    void setBridgeRank (int newRank)                    { bridgeRank = newRank; }

    /** A node this combiner must wait for before processing (a BridgeGateNode). Its output is
        ignored: it is a pure ordering edge. With none set, the combiner is exactly what it was. */
    void setOrderingGate (std::unique_ptr<tracktion::graph::Node> gate)     { orderingGate = std::move (gate); }

    int getBridgeRank() const override                  { return bridgeRank; }

    /** Returns the inputs that have been added.
        N.B. This is a bit of a temporary hack to ensure WaveNodes can access previous
        Nodes that have been added via a CombinngNode. This will be cleaned up in the future.
    */
    std::vector<Node*> getInternalNodes() override;

    //==============================================================================
    std::vector<Node*> getDirectInputNodes() override;
    tracktion::graph::NodeProperties getNodeProperties() override;
    void prepareToPlay (const tracktion::graph::PlaybackInitialisationInfo&) override;
    bool isReadyToProcess() override;
    void prefetchBlock (juce::Range<int64_t> /*referenceSampleRange*/) override;
    void process (ProcessContext&) override;

    size_t getAllocatedBytes() const override;

private:
    const EditItemID itemID;

    struct TimedNode;
    juce::OwnedArray<TimedNode> inputs;
    juce::OwnedArray<juce::Array<TimedNode*>> groups;
    std::atomic<bool> isReadyToProcessBlock { false };
    choc::buffer::ChannelArrayBuffer<float> tempAudioBuffer;
    MidiMessageArray noteOffEventsToSend;

    tracktion::graph::NodeProperties nodeProperties;

    int bridgeRank = 0;
    std::unique_ptr<tracktion::graph::Node> orderingGate;

    void prefetchGroup (juce::Range<int64_t>, TimeRange, BeatRange);
    void queueNoteOffsForClipsNoLongerPresent (const CombiningNode&);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CombiningNode)
};

} // namespace tracktion::inline engine
