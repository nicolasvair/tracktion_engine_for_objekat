/*
    ,--.                     ,--.     ,--.  ,--.
  ,-'  '-.,--.--.,--,--.,---.|  |,-.,-'  '-.`--' ,---. ,--,--,      Copyright 2024
  '-.  .-'|  .--' ,-.  | .--'|     /'-.  .-',--.| .-. ||      \   Tracktion Software
    |  |  |  |  \ '-'  \ `--.|  \  \  |  |  |  |' '-' '|  ||  |       Corporation
    `---' `--'   `--`--'`---'`--'`--' `---' `--' `---' `--''--'    www.tracktion.com

    Tracktion Engine uses a GPL/commercial licence - see LICENCE.md for details.
*/

namespace tracktion::inline engine {

//==============================================================================
//==============================================================================
ContainerClipNode::ContainerClipNode (ProcessState& editProcessState,
                                      EditItemID clipID,
                                      BeatRange position,
                                      BeatDuration offset,
                                      BeatRange clipLoopRange,
                                      std::unique_ptr<Node> inputNode,
                                      int latencyNumSamples)
    : TracktionEngineNode (editProcessState),
      containerClipID (clipID),
      clipPosition (position),
      loopRange (clipLoopRange),
      clipOffset (offset),
      pluginLatencyNumSamples (std::max (0, latencyNumSamples)),
      input (std::move (inputNode))
{
    if (auto parentTempoPosition = getProcessState().getTempoSequencePosition())
    {
        tempoPosition = std::make_unique<tempo::Sequence::Position> (*parentTempoPosition);

        // At the moment, this won't work correctly with complex tempo changes and the
        // contained clips will use the tempo conversions of the main Edit. This needs
        // to be added by using the ProcessState directly for tempo conversions with a
        // specified offset
//ddd        jassert (getProcessState().getTempoSequence()->getNumTempos() == 1);
    }

    assert (input);
    setOptimisations ({ tracktion::graph::ClearBuffers::no,
                        tracktion::graph::AllocateAudioBuffer::yes });
}

//==============================================================================
tracktion::graph::NodeProperties ContainerClipNode::getNodeProperties()
{
    // Reset the NodeID as we need to be findable between graph loads to keep the same internal NodePlayer
    nodeProperties.nodeID = 0;

    // Calculated from hashing a string view of "ContainerClipNode"
    const auto hashSalt = 9088803362895930667;
    hash_combine (nodeProperties.nodeID, hashSalt);
    hash_combine (nodeProperties.nodeID, containerClipID.getRawID());

    return nodeProperties;
}

std::vector<tracktion::graph::Node*> ContainerClipNode::getDirectInputNodes()
{
    return {};
}

std::vector<Node*> ContainerClipNode::getInternalNodes()
{
    // Patch local Objekat — le graphe englobant ne doit PAS voir nos nœuds internes.
    //
    // createNodeMap() recurse dans getInternalNodes() pour bâtir sortedNodes, la table
    // que findNode/findNodeWithID balaient sur l'ANCIEN graphe à chaque reconstruction.
    // Or nos nœuds internes n'appartiennent pas au graphe englobant : ils vivent dans
    // le NodeGraph local du player du container, dont la durée de vie est indépendante.
    // Deux façons de dangling en découlent :
    //   - les pointeurs sont relevés AVANT que le player local ne prenne le sous-arbre
    //     et ne le re-transforme (transformNodes peut détruire des nœuds) ;
    //   - poser un nouveau graphe local détruit le précédent, alors que l'ancien graphe
    //     englobant — toujours en cours d'utilisation comme nodeGraphToReplace — pointe
    //     encore dessus.
    // D'où un dynamic_cast sur mémoire libérée (EXC_BAD_ACCESS) dès la 2e reconstruction.
    //
    // getDirectInputNodes() renvoie déjà {} pour la même raison d'isolement : le container
    // est une frontière de graphe. Le ContainerClipNode lui-même reste dans orderedNodes,
    // donc findNodeWithID<ContainerClipNode> le retrouve et la continuité du PlayerContext
    // est préservée ; la continuité des nœuds internes, elle, se fait via le graphe local.
    //
    // Contrepartie assumée : un clip qui entre dans un groupe (ou en sort) ne retrouve pas
    // son prédécesseur d'un graphe à l'autre — au pire une discontinuité au moment du
    // regroupement, là où l'ancien comportement plantait.
    return {};
}

void ContainerClipNode::prepareToPlay (const tracktion::graph::PlaybackInitialisationInfo& info)
{
    if (info.nodeGraphToReplace != nullptr)
        if (auto oldNode = findNodeWithID<ContainerClipNode> (*info.nodeGraphToReplace, getNodeProperties().nodeID))
            playerContext = oldNode->playerContext;

    if (! playerContext)
    {
        playerContext = std::make_shared<PlayerContext>();
        playerContext->player.setNumThreads (0);

        // We need to create our own Tempo::Position as we'll apply an offset so it stays in sync with the Edit's tempo sequence
        // Make sure we do this before we overwrite the default ProcessState
        if (auto tempoSequence = getProcessState().getTempoSequence())
            playerContext->processState.setTempoSequence (tempoSequence);
    }

    if (! input)
    {
        assert (playerContext->player.getSampleRate() == info.sampleRate);
        return;
    }

    // Set the ProcessState used for all the child nodes so they use the local time, not the Edit time
    visitNodes (*input,
                [localProcessState = &playerContext->processState] (auto& node)
                {
                    if (auto ten = dynamic_cast<TracktionEngineNode*> (&node))
                        ten->setProcessState (*localProcessState);

                    for (auto internalNode : node.getInternalNodes())
                        if (auto ten = dynamic_cast<TracktionEngineNode*> (internalNode))
                            ten->setProcessState (*localProcessState);
                }, true);

    playerContext->player.setNode (std::move (input),
                                   info.sampleRate, info.blockSize);
}

bool ContainerClipNode::isReadyToProcess()
{
    return true;
}

void ContainerClipNode::process (ProcessContext& pc)
{
    const auto sectionEditSampleRange = getTimelineSampleRange();
    const auto sampleRate = getSampleRate();

    // Bornes du clip en samples d'Edit, décalées de -L : à l'instant T on produit le matériau
    // de T+L, pour que la chaîne de plugins — qui retarde de L — le remette en place.
    const TimeRange clipTimeRange (tempoPosition->set (clipPosition.getStart()),
                                   tempoPosition->set (clipPosition.getEnd()));
    const auto clipSampleRangeUnshifted = toSamples (clipTimeRange, sampleRate);
    const juce::Range<int64_t> clipSampleRange (clipSampleRangeUnshifted.getStart() - pluginLatencyNumSamples,
                                                clipSampleRangeUnshifted.getEnd()   - pluginLatencyNumSamples);

    // Fenêtre d'activation élargie : L avant (les FIFOs se remplissent), 2L après (elles se
    // vident). Mêmes marges que ContainerClip::getHead()/getTail() donne au CombiningNode ;
    // si elles ne concordaient pas, le garde ci-dessous annulerait le pré-roll qu'on vient
    // de demander.
    const auto activeSampleRange = clipSampleRange.withStart (clipSampleRange.getStart() - pluginLatencyNumSamples)
                                                  .withEnd (clipSampleRange.getEnd() + 2 * pluginLatencyNumSamples);

    if (sectionEditSampleRange.getEnd() <= activeSampleRange.getStart()
        || sectionEditSampleRange.getStart() >= activeSampleRange.getEnd())
       return;

    // Set playHead loop range using loopRange
    // Find ref offset from clip time
    // If playhead position was overriden, pass this on to the PlayHeadState
    // Process buffer
    // Add an offset to ProcessState so the tempo positions can be synced up

    auto& player = playerContext->player;

    auto& editPlayHead = getPlayHead();
    auto& editPlayHeadState = getPlayHeadState();
    auto& localPlayHead = playerContext->playHead;

    // Calculate sample positions of clip as these will vary when the tempo changes
    const auto editStartBeatOfLocalTimeline = clipPosition.getStart() - clipOffset;
    const auto editStartTimeOfLocalTimeline = tempoPosition->set (editStartBeatOfLocalTimeline);

    const TimeRange loopTimeRange (tempoPosition->set (loopRange.getStart()),
                                   tempoPosition->set (loopRange.getEnd()));
    const auto loopRangeSamples = toSamples (loopTimeRange, sampleRate);


    // Updated the PlayHead so the position/play setting below is in sync
    localPlayHead.setReferenceSampleRange (pc.referenceSampleRange);

    // We don't want to update the playhead position as we'll do that manually below to avoid triggering playhead jumps
    if (! loopRangeSamples.isEmpty() && localPlayHead.getLoopRange() != loopRangeSamples)
        localPlayHead.setLoopRange (true, loopRangeSamples, false);

    // Syncronise positions
    const auto playheadOffset = toSamples (editStartTimeOfLocalTimeline, sampleRate);
    playerContext->processState.setPlaybackSpeedRatio (getPlaybackSpeedRatio());

    // Lecture anticipée : +L AVANT le wrap de boucle, sinon l'avance sortirait de la plage
    // bouclée au lieu de repasser au début.
    int64_t newPosition = editPlayHead.getPosition() - playheadOffset + loopRangeSamples.getStart()
                            + pluginLatencyNumSamples;

    if (localPlayHead.isLooping())
        newPosition = localPlayHead.linearPositionToLoopPosition (newPosition, localPlayHead.getLoopRange());

    // Réactivation après une période hors fenêtre : le CombiningNode ne nous a pas appelés
    // pendant que le clip était hors du bloc, donc nos blocs à nous ne se suivent pas — même
    // quand ceux de l'Edit se suivent. Sans ce test on prendrait overridePosition() et le
    // graphe interne ne verrait aucun saut : notes MIDI suspendues, FIFOs de plugins pleines
    // de matériau périmé. setPosition() marque l'interaction, PlayHeadState local propage le
    // saut, et les nœuds internes se remettent d'aplomb (cf. PluginNode : all-notes-off sur
    // didPlayheadJump).
    const bool contiguousForUs = lastProcessedReferenceSampleEnd == pc.referenceSampleRange.getStart();
    lastProcessedReferenceSampleEnd = pc.referenceSampleRange.getEnd();

    if (contiguousForUs && editPlayHeadState.isContiguousWithPreviousBlock())
        localPlayHead.overridePosition (newPosition);
    else
        localPlayHead.setPosition (newPosition);

    // Syncronise playing state
    if (editPlayHead.isStopped() && ! localPlayHead.isStopped())
        localPlayHead.stop();

    if (editPlayHead.isPlaying() && ! localPlayHead.isPlaying())
        localPlayHead.play();

    assert (! localPlayHead.isLooping() || localPlayHead.getLoopRange().contains (localPlayHead.getPosition()));

    // Process
    ProcessContext localPC { pc.numSamples, pc.referenceSampleRange,
                             { pc.buffers.audio, pc.buffers.midi } };
    player.process (localPC);

    // Silence any samples before or after our edit time range.
    // N.B. les bornes sont celles décalées de -L : c'est le CONTENU du container qu'on borne
    // ici, pas la queue de sa chaîne de plugins — celle-ci vit en aval et doit continuer à
    // sonner pendant le tail.
    {
        const auto destBuffer = pc.buffers.audio;
        auto numSamplesToClearAtStart = std::min (clipSampleRange.getStart() - sectionEditSampleRange.getStart(), (SampleCount) destBuffer.getNumFrames());
        auto numSamplesToClearAtEnd = std::min (sectionEditSampleRange.getEnd() - clipSampleRange.getEnd(), (SampleCount) destBuffer.getNumFrames());

        if (numSamplesToClearAtStart > 0)
            destBuffer.getStart ((choc::buffer::FrameCount) numSamplesToClearAtStart).clear();

        if (numSamplesToClearAtEnd > 0)
            destBuffer.getEnd ((choc::buffer::FrameCount) numSamplesToClearAtEnd).clear();
    }
}

} // namespace tracktion::inline engine
