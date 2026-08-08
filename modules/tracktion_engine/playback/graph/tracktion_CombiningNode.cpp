/*
    ,--.                     ,--.     ,--.  ,--.
  ,-'  '-.,--.--.,--,--.,---.|  |,-.,-'  '-.`--' ,---. ,--,--,      Copyright 2024
  '-.  .-'|  .--' ,-.  | .--'|     /'-.  .-',--.| .-. ||      \   Tracktion Software
    |  |  |  |  \ '-'  \ `--.|  \  \  |  |  |  |' '-' '|  ||  |       Corporation
    `---' `--'   `--`--'`---'`--'`--' `---' `--' `---' `--''--'    www.tracktion.com

    Tracktion Engine uses a GPL/commercial licence - see LICENCE.md for details.
*/

#include "tracktion_LoopingMidiNode.h"

#define USE_PARTITION_INSERTION 1

namespace tracktion::inline engine {

namespace combining_node_utils
{
    // how much extra time to give a track before it gets cut off - to allow for plugins
    // that ring on.
    static constexpr BeatDuration decayTimeAllowance { 8_bd };
    static constexpr int secondsPerGroup = 8;

    static inline constexpr int timeToGroupIndex (TimePosition t) noexcept
    {
        return static_cast<int> (t.inSeconds()) / secondsPerGroup;
    }
}

//==============================================================================
struct CombiningNode::TimedNode
{
    TimedNode (std::unique_ptr<Node> sourceNode, BeatRange t)
        : time (t), node (std::move (sourceNode))
    {
        // Patch local Objekat — DFS post-ordre dédupliqué au lieu d'une descente linéaire.
        // L'original supposait un seul input par nœud (« This doesn't work with parallel
        // input Nodes ») : un SummingNode dans la plugin-list d'un clip — c'est ce qu'est un
        // bloc de plugins parallèles — le cassait net. Le post-ordre garantit qu'un nœud est
        // processé après tous ses inputs, et la déduplication qu'un nœud partagé entre deux
        // branches ne l'est qu'une fois.
        nodesToProcess = tracktion::graph::getNodes (*node, tracktion::graph::VertexOrdering::postordering);
        inspectChain();

        if (! isLinearChain)
        {
            // La chaîne d'un TimedNode n'est jamais transformée : le graphe englobant ne la voit
            // pas (CombiningNode::getDirectInputNodes() renvoie {}). Sans transformation, un
            // SummingNode ne poserait pas ses LatencyNode et les branches parallèles sortiraient
            // désalignées. On la traite donc comme le petit graphe qu'elle est — mais seulement
            // quand elle se ramifie : sur une chaîne série, transform() n'a rien à faire et on
            // s'épargne un coût payé par clip, à chaque reconstruction.
            nodesToProcess = tracktion::graph::transformNodes (*node, false);
            leafNodes.clear();
            inspectChain();
        }
    }

    std::vector<Node*> getNodes() const
    {
        return nodesToProcess;
    }

    void prepareToPlay (const tracktion::graph::PlaybackInitialisationInfo& info,
                        choc::buffer::ChannelArrayView<float> view)
    {
        auto info2 = info;

        info2.deallocateAudioBuffer = nullptr;

        // Patch local Objekat — le buffer unique partagé par toute la chaîne ne vaut que pour
        // une chaîne LINÉAIRE, où chaque nœud traite sur place le résultat du précédent. Dès
        // qu'un nœud a plusieurs entrées (bloc de plugins parallèles → SummingNode), les
        // branches écriraient toutes dans la même vue et s'écraseraient avant d'être sommées.
        if (isLinearChain)
        {
            info2.allocateAudioBuffer = [view] (choc::buffer::Size size) -> tracktion::graph::NodeBuffer
                                        {
                                            jassert (size.numFrames == view.getNumFrames());
                                            jassert (size.numChannels <= view.getNumChannels());

                                            return { view.getFirstChannels (size.numChannels), {} };
                                        };

            for (auto n : nodesToProcess)
                n->initialise (info2);

            return;
        }

        // Un buffer par nœud. N.B. il ne suffit PAS de laisser chaque nœud allouer le sien :
        // beaucoup déclarent ClearBuffers::no (ContainerClipNode, FadeInOutNode, LatencyNode,
        // les mesureurs…) parce que dans le montage partagé c'est le CombiningNode qui vide.
        // Un buffer jamais vidé s'additionne bloc après bloc — larsen. On reprend donc le
        // contrat à notre compte : on fournit les buffers, on les vide (@see process).
        //
        // Dimensionnés comme `view`, qui couvre déjà le plus large des nœuds — c'est ce
        // qu'affirmait le jassert du chemin partagé. Alloué ici, jamais sur le thread audio.
        ownedBuffers.resize (nodesToProcess.size());

        for (size_t i = 0; i < nodesToProcess.size(); ++i)
        {
            auto* buffer = &ownedBuffers[i];
            buffer->resize (view.getSize());

            info2.allocateAudioBuffer = [buffer] (choc::buffer::Size size) -> tracktion::graph::NodeBuffer
                                        {
                                            jassert (size.numFrames == buffer->getSize().numFrames);
                                            jassert (size.numChannels <= buffer->getSize().numChannels);

                                            return { buffer->getView().getFirstChannels (size.numChannels), {} };
                                        };

            nodesToProcess[i]->initialise (info2);
        }
    }

    bool isReadyToProcess() const
    {
        // N.B. toutes les feuilles, pas seulement la première : une chaîne parallèle en a
        // plusieurs, et une seule prête ne suffit pas.
        for (auto n : leafNodes)
            if (! n->isReadyToProcess())
                return false;

        return true;
    }

    void prefetchBlock (juce::Range<int64_t> referenceSampleRange)
    {
        for (auto n : nodesToProcess)
            n->prepareForNextBlock (referenceSampleRange);

       #if JUCE_DEBUG
        hasPrefetched = true;
       #endif
    }

    void process (ProcessContext& pc)
    {
       #if JUCE_DEBUG
        jassert (hasPrefetched);
       #endif

        // Patch local Objekat — vide ce qu'on a fourni, comme le CombiningNode le fait pour le
        // buffer partagé : les nœuds en ClearBuffers::no comptent dessus. Vide en chaîne série,
        // où les nœuds partagent le buffer du CombiningNode. @see prepareToPlay
        for (auto& b : ownedBuffers)
            b.clear();

        // Process all the Nodes
        for (auto n : nodesToProcess)
            n->process (pc.numSamples, pc.referenceSampleRange);

        // Then get the output from the source Node
        auto nodeOutput = node->getProcessedOutput();
        const auto numDestChannels = pc.buffers.audio.getNumChannels();
        const auto numChannelsToAdd = std::min (nodeOutput.audio.getNumChannels(), numDestChannels);

        if (numChannelsToAdd > 0)
            add (pc.buffers.audio.getFirstChannels (numChannelsToAdd),
                 nodeOutput.audio.getFirstChannels (numChannelsToAdd));

        pc.buffers.midi.mergeFrom (nodeOutput.midi);

       #if JUCE_DEBUG
        hasPrefetched = false;
       #endif
    }

    size_t getAllocatedBytes() const
    {
        size_t size = 0;

        for (auto n : nodesToProcess)
            size += n->getAllocatedBytes();

        // Les nœuds n'allouent pas ces buffers-là, ils les reçoivent : à nous de les compter.
        for (const auto& b : ownedBuffers)
            size += b.getView().data.getBytesNeeded (b.getSize());

        return size;
    }

    const BeatRange time;

private:
    const std::unique_ptr<Node> node;
    std::vector<Node*> nodesToProcess, leafNodes;
    // Vide en chaîne série : les nœuds partagent alors le buffer du CombiningNode.
    std::vector<choc::buffer::ChannelArrayBuffer<float>> ownedBuffers;
    bool isLinearChain = true;

    /** Relève les feuilles — les seules dont la disponibilité dépende de l'extérieur, tous les
        autres nœuds ayant leurs inputs dans la liste et processés avant eux — et note si la
        chaîne se ramifie.
    */
    void inspectChain()
    {
        isLinearChain = true;

        for (auto n : nodesToProcess)
        {
            const auto numInputs = n->getDirectInputNodes().size();

            if (numInputs == 0)
                leafNodes.push_back (n);
            else if (numInputs > 1)
                isLinearChain = false;
        }
    }
   #if JUCE_DEBUG
    bool hasPrefetched = false;
   #endif

    JUCE_DECLARE_NON_COPYABLE (TimedNode)
};

//==============================================================================
CombiningNode::CombiningNode (EditItemID id, ProcessState& ps)
    : TracktionEngineNode (ps),
      itemID (id)
{
    jassert (getProcessState().getTempoSequence());
    hash_combine (nodeProperties.nodeID, itemID);
}

CombiningNode::~CombiningNode() {}

void CombiningNode::addInput (std::unique_ptr<Node> input, TimeRange time, bool ignoreLatency)
{
    jassert (time.getEnd() <= Edit::getMaximumEditEnd());
    addInput (std::move (input), toBeats (*getProcessState().getTempoSequence(), time), ignoreLatency);
}

void CombiningNode::addInput (std::unique_ptr<Node> input, BeatRange beatRange, bool ignoreLatency)
{
    assert (input != nullptr);

    if (beatRange.isEmpty())
        return;

    auto props = input->getNodeProperties();

    nodeProperties.hasAudio |= props.hasAudio;
    nodeProperties.hasMidi |= props.hasMidi;
    nodeProperties.numberOfChannels = std::max (nodeProperties.numberOfChannels, props.numberOfChannels);
    // Patch local Objekat — une entrée qui compense elle-même sa latence (@see
    // Clip::compensatesOwnPluginLatency) est déjà alignée : elle lit son matériau en avance de
    // ce que sa chaîne retarde. Remonter sa latence la ferait compenser une deuxième fois, et
    // surtout elle contaminerait toute la piste.
    if (! ignoreLatency)
        nodeProperties.latencyNumSamples = std::max (nodeProperties.latencyNumSamples, props.latencyNumSamples);

    hash_combine (nodeProperties.nodeID, props.nodeID);

   #if USE_PARTITION_INSERTION
    const auto lower = std::partition_point (inputs.begin(), inputs.end(),
                                             [&] (const auto& i)
                                             {
                                                return i->time.getStart() < beatRange.getStart();
                                            });
    int i = static_cast<int> (std::distance (inputs.begin(), lower));
   #else
    int i;
    for (i = 0; i < inputs.size(); ++i)
        if (inputs.getUnchecked (i)->time.getStart() >= beatRange.getStart())
            break;
   #endif

    beatRange = BeatRange (beatRange.getStart(), beatRange.getLength() + combining_node_utils::decayTimeAllowance);
    auto tan = inputs.insert (i, new TimedNode (std::move (input), beatRange));

    // add the node to any groups it's near to.
    const auto& ts = *getProcessState().getTempoSequence();
    const auto overlapTime = TimeDuration::fromSeconds (combining_node_utils::secondsPerGroup / 2 + 2);
    const auto timeRange = toTime (ts, beatRange).expanded (overlapTime);
    const auto start = std::max (0, combining_node_utils::timeToGroupIndex (timeRange.getStart()));
    const auto end   = std::max (0, combining_node_utils::timeToGroupIndex (timeRange.getEnd()));

    while (groups.size() <= end)
        groups.add (new juce::Array<TimedNode*>());

    for (i = start; i <= end; ++i)
    {
        auto g = groups.getUnchecked (i);

       #if USE_PARTITION_INSERTION
        const auto lowerGroup = std::partition_point (g->begin(), g->end(),
                                                      [&] (auto in)
                                                      {
                                                          return in->time.getStart() < beatRange.getStart();
                                                      });
        const int j = static_cast<int> (std::distance (g->begin(), lowerGroup));
       #else
        int j;
        for (j = 0; j < g->size(); ++j)
            if (g->getUnchecked (j)->time.getStart() >= beatRange.getStart())
                break;
       #endif

        jassert (tan != nullptr);
        g->insert (j, tan);
    }
}

int CombiningNode::getNumInputs() const
{
    return inputs.size();
}

std::vector<Node*> CombiningNode::getInternalNodes()
{
    std::vector<Node*> leafNodes;

    for (auto i : inputs)
        for (auto n : i->getNodes())
            leafNodes.push_back (n);

    return leafNodes;
}

std::vector<tracktion::graph::Node*> CombiningNode::getDirectInputNodes()
{
    return {};
}

tracktion::graph::NodeProperties CombiningNode::getNodeProperties()
{
    return nodeProperties;
}

void CombiningNode::prepareToPlay (const tracktion::graph::PlaybackInitialisationInfo& info)
{
    isReadyToProcessBlock.store (true, std::memory_order_release);
    tempAudioBuffer.resize (choc::buffer::Size::create ((choc::buffer::ChannelCount) nodeProperties.numberOfChannels,
                                                        (choc::buffer::FrameCount) info.blockSize));

    for (auto& i : inputs)
    {
        i->prepareToPlay (info, tempAudioBuffer.getView());

        if (! i->isReadyToProcess())
            isReadyToProcessBlock.store (false, std::memory_order_release);
    }

    // Inspect the old graph to find clips that need to be killed
    if (info.nodeGraphToReplace != nullptr)
    {
        if (auto oldNode = findNode<CombiningNode> (*info.nodeGraphToReplace,
                                                    [id = itemID] (auto& cn) { return cn.itemID == id; }))
        {
            queueNoteOffsForClipsNoLongerPresent (*oldNode);
        }
    }
}

bool CombiningNode::isReadyToProcess()
{
    return isReadyToProcessBlock.load (std::memory_order_acquire);
}

void CombiningNode::prefetchBlock (juce::Range<int64_t> referenceSampleRange)
{
    SCOPED_REALTIME_CHECK

    const auto editTime = getEditTimeRange();
    prefetchGroup (referenceSampleRange, editTime, getEditBeatRange());

    // Update ready to process state based on nodes intersecting this time
    isReadyToProcessBlock.store (true, std::memory_order_release);

    if (auto g = groups[combining_node_utils::timeToGroupIndex (editTime.getStart())])
    {
        for (auto tan : *g)
        {
            if (! tan->isReadyToProcess())
            {
                isReadyToProcessBlock.store (false, std::memory_order_release);
                break;
            }
        }
    }
}

void CombiningNode::process (ProcessContext& pc)
{
    const auto editBeats = getEditBeatRange();

    SCOPED_REALTIME_CHECK
    const auto initialEvents = pc.buffers.midi.size();

    // Merge any note-offs from clips that have been deleted
    pc.buffers.midi.mergeFromAndClear (noteOffEventsToSend);

    // Then process the list
    if (auto g = groups[combining_node_utils::timeToGroupIndex (getEditTimeRange().getStart())])
    {
        for (auto tan : *g)
        {
            if (tan->time.getEnd() > editBeats.getStart())
            {
                if (tan->time.getStart() >= editBeats.getEnd())
                    break;

                // Clear the allocated storage
                tempAudioBuffer.clear();

                // Then process the buffer.
                // This will use the local buffer for the Nodes in the TimedNode and put the result in pc.buffers
                tan->process (pc);
            }
        }
    }

    if (pc.buffers.midi.size() > initialEvents)
        pc.buffers.midi.sortByTimestamp();
}

size_t CombiningNode::getAllocatedBytes() const
{
    size_t size = tempAudioBuffer.getView().data.getBytesNeeded (tempAudioBuffer.getSize());

    for (const auto& i : inputs)
        size += i->getAllocatedBytes();

    return size;
}

void CombiningNode::prefetchGroup (juce::Range<int64_t> referenceSampleRange, TimeRange editTime, BeatRange editBeats)
{
    if (auto g = groups[combining_node_utils::timeToGroupIndex (editTime.getStart())])
    {
        for (auto tan : *g)
        {
            if (tan->time.getEnd() > editBeats.getStart())
            {
                if (tan->time.getStart() >= editBeats.getEnd())
                    break;

                tan->prefetchBlock (referenceSampleRange);
            }
        }
    }
}

void CombiningNode::queueNoteOffsForClipsNoLongerPresent (const CombiningNode& oldCombiningNode)
{
    // Find any LoopingMidiNodes that are no longer present
    // Add note-offs for any note-ons they have
    std::vector<EditItemID> currentNodeIDs;

    for (auto timedNode : inputs)
        for (auto node : timedNode->getNodes())
            if (auto loopingMidiNode = dynamic_cast<LoopingMidiNode*> (node))
                currentNodeIDs.push_back (loopingMidiNode->getItemID());

    for (auto oldTimedNode : oldCombiningNode.inputs)
    {
        for (auto oldNode : oldTimedNode->getNodes())
        {
            if (auto oldLoopingMidiNode = dynamic_cast<LoopingMidiNode*> (oldNode))
            {
                if (std::find (currentNodeIDs.begin(), currentNodeIDs.end(), oldLoopingMidiNode->getItemID())
                    == currentNodeIDs.end())
                {
                    oldLoopingMidiNode->getActiveNoteList()->iterate ([this, mpeSourceID = oldLoopingMidiNode->getMPESourceID()]
                                                                      (int chan, int note)
                                                                      {
                                                                          noteOffEventsToSend.addMidiMessage (juce::MidiMessage::noteOff (chan, note), mpeSourceID);
                                                                      });
                }
            }
        }
    }
}

} // namespace tracktion::inline engine
