/*
    OBJEKAT — the audio bridge: the three graph nodes. See tracktion_ObjBridgeNodes.h.
*/

namespace tracktion::inline engine
{

namespace
{
    constexpr size_t bridgeTapMagicHash     = size_t (0x62726964676574);    // "bridget"
    constexpr size_t bridgeReaderMagicHash  = size_t (0x62726964676572);    // "bridger"
    constexpr size_t bridgeGateMagicHash    = size_t (0x62726964676761);    // "bridgga"
    constexpr size_t gateCacheKey           = size_t (0x0B76A7E0C0DEull);   // TransformCache key
}

//==============================================================================
BridgeTapNode::BridgeTapNode (ProcessState& ps,
                              std::unique_ptr<tracktion::graph::Node> inputNode,
                              Plugin::Ptr tapPluginToUse,
                              BridgeTapSource&,
                              std::shared_ptr<BridgeBuild> buildToUse,
                              int rankToUse)
    : TracktionEngineNode (ps),
      input (std::move (inputNode)),
      tapPlugin (std::move (tapPluginToUse)),
      build (std::move (buildToUse)),
      rank (rankToUse),
      tapID (tapPlugin->itemID)
{
    jassert (input != nullptr);

    // The output IS the input (copied unless aliased): nothing to clear, the copy overwrites it.
    setOptimisations ({ tracktion::graph::ClearBuffers::no,
                        tracktion::graph::AllocateAudioBuffer::yes });
}

tracktion::graph::NodeProperties BridgeTapNode::getNodeProperties()
{
    auto props = input->getNodeProperties();     // latency UNCHANGED: the tap's age is its input's

    if (props.nodeID != 0)
    {
        hash_combine (props.nodeID, (size_t) tapID.getRawID());
        hash_combine (props.nodeID, bridgeTapMagicHash);
    }

    return props;
}

std::vector<tracktion::graph::Node*> BridgeTapNode::getDirectInputNodes()
{
    return { input.get() };
}

bool BridgeTapNode::isReadyToProcess()
{
    return input->hasProcessed();
}

void BridgeTapNode::prepareToPlay (const tracktion::graph::PlaybackInitialisationInfo&)
{
    // The ring was sized by BridgeBuild::finalise; nothing is allocated here.
    ring = build != nullptr ? build->ringForTap (tapID) : nullptr;
}

void BridgeTapNode::process (ProcessContext& pc)
{
    auto sourceBuffers = input->getProcessedOutput();

    // In a linear TimedNode chain the two views are the same buffer (tracktion_CombiningNode.cpp),
    // hence copyIfNotAliased.
    tracktion::graph::copyIfNotAliased (pc.buffers.audio, sourceBuffers.audio);
    pc.buffers.midi.copyFrom (sourceBuffers.midi);

    if (ring == nullptr)
        return;

    float* channels[2] = { nullptr, nullptr };
    const int numChannels = std::min (2, (int) pc.buffers.audio.getNumChannels());

    for (int c = 0; c < numChannels; ++c)
        channels[c] = pc.buffers.audio.getIterator ((choc::buffer::ChannelCount) c).sample;

    // didPlayheadJump excludes loop wraps: a folding container keeps ONE run.
    ring->write (pc.referenceSampleRange.getStart(), (int) pc.numSamples,
                 channels, numChannels, getPlayHeadState().didPlayheadJump());
}

//==============================================================================
BridgeReaderNode::BridgeReaderNode (ProcessState& ps, std::shared_ptr<BridgeBuild> buildToUse,
                                    int index, int rank, size_t id)
    : TracktionEngineNode (ps),
      build (std::move (buildToUse)),
      readerIndex (index),
      nodeID (id),
      declaredLatencyNumSamples (build->getDeclaredLatency (index))
{
    if (rank >= 1)
        gate = tracktion::graph::makeNode<BridgeGateNode> (rank, nodeID ^ size_t (0x6A7E), build);

    // Every sample of the output is written by process (or cleared there).
    setOptimisations ({ tracktion::graph::ClearBuffers::no,
                        tracktion::graph::AllocateAudioBuffer::yes });
}

tracktion::graph::NodeProperties BridgeReaderNode::getNodeProperties()
{
    tracktion::graph::NodeProperties props;
    props.hasAudio = true;
    props.hasMidi = false;
    props.numberOfChannels = 2;
    props.latencyNumSamples = declaredLatencyNumSamples;     // X, fixed at construction
    props.nodeID = nodeID;

    if (props.nodeID != 0)
        hash_combine (props.nodeID, bridgeReaderMagicHash);

    return props;
}

std::vector<tracktion::graph::Node*> BridgeReaderNode::getDirectInputNodes()
{
    if (gate != nullptr)
        return { gate.get() };

    return {};
}

bool BridgeReaderNode::isReadyToProcess()
{
    return gate == nullptr || gate->hasProcessed();
}

void BridgeReaderNode::prepareToPlay (const tracktion::graph::PlaybackInitialisationInfo&)
{
    plan = build->readerPlan (readerIndex);
    counters = &build->counters (readerIndex);
}

void BridgeReaderNode::process (ProcessContext& pc)
{
    auto audio = pc.buffers.audio;

    if (plan.ring == nullptr || plan.status == objbridge::ReaderStatus::sourceAbsent)
    {
        audio.clear();
        return;
    }

    float* dest[2] = { nullptr, nullptr };

    for (int c = 0; c < 2; ++c)
        dest[c] = audio.getIterator ((choc::buffer::ChannelCount) c).sample;

    // If the reference range is not as long as the block (speed compensation, I2) the ring's run
    // logic is what makes the key silent: nothing special to do here.
    const auto result = plan.ring->read (pc.referenceSampleRange.getStart() - (int64_t) plan.delay,
                                         (int) pc.numSamples, dest, 2);

    counters->blocksRead.fetch_add (1, std::memory_order_relaxed);

    if (result.framesCovered < (int) pc.numSamples)
        counters->blocksUncovered.fetch_add (1, std::memory_order_relaxed);

    if (result.torn)
        counters->blocksTorn.fetch_add (1, std::memory_order_relaxed);
}

//==============================================================================
BridgeGateNode::BridgeGateNode (int r, size_t id, std::shared_ptr<BridgeBuild> buildToUse)
    : rank (r), nodeID (id), build (std::move (buildToUse))
{
    // No channels, no audio: there is nothing to allocate or clear.
    setOptimisations ({ tracktion::graph::ClearBuffers::no,
                        tracktion::graph::AllocateAudioBuffer::no });
}

tracktion::graph::NodeProperties BridgeGateNode::getNodeProperties()
{
    tracktion::graph::NodeProperties props;
    props.hasAudio = false;
    props.hasMidi = false;
    props.numberOfChannels = 0;
    props.latencyNumSamples = 0;
    props.nodeID = nodeID;

    if (props.nodeID != 0)
        hash_combine (props.nodeID, bridgeGateMagicHash);

    return props;
}

tracktion::graph::TransformResult BridgeGateNode::transform (TransformOptions& options)
{
    // Once: the candidates are collected on the first call, as ReturnNode::findSendNodes does.
    if (hasTransformed)
        return tracktion::graph::TransformResult::none;

    hasTransformed = true;

    using RankedNodes = std::vector<std::pair<tracktion::graph::Node*, BridgeRankedNode*>>;
    RankedNodes allRanked;

    if (auto cached = options.cache.getCachedProperty<RankedNodes> (gateCacheKey))
    {
        allRanked = *cached;
    }
    else
    {
        for (auto n : options.postOrderedNodes)
            if (auto rn = dynamic_cast<BridgeRankedNode*> (n))
                allRanked.emplace_back (n, rn);

        options.cache.cacheProperty (gateCacheKey, allRanked);
    }

    int added = 0, refused = 0;

    for (auto& [node, ranked] : allRanked)
    {
        const int r = ranked->getBridgeRank();

        if (node == this || r < 0 || r >= rank)
            continue;

        // A would-be cycle: the candidate's own graph reaches this gate.
        bool reachesThis = false;

        tracktion::graph::visitNodes (*node, [&] (tracktion::graph::Node& n) { if (&n == this) reachesThis = true; }, true);

        if (reachesThis)
        {
            ++refused;
            continue;
        }

        waitsFor.push_back (node);
        ++added;
    }

    if (build != nullptr)
        build->noteGateEdges (added, refused);

    return added > 0 ? tracktion::graph::TransformResult::connectionsMade
                     : tracktion::graph::TransformResult::none;
}

std::vector<tracktion::graph::Node*> BridgeGateNode::getDirectInputNodes()
{
    return waitsFor;
}

bool BridgeGateNode::isReadyToProcess()
{
    for (auto n : waitsFor)
        if (! n->hasProcessed())
            return false;

    return true;
}

void BridgeGateNode::process (ProcessContext&)
{
}

} // namespace tracktion::inline engine
