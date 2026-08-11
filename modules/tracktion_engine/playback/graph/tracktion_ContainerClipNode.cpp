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

    // N.B. on suit la contiguïté de l'EDIT, pas la nôtre. Hors fenêtre le CombiningNode ne nous
    // appelle pas, donc nos blocs à nous ne se suivent pas — tentant d'en faire un saut pour que
    // le graphe interne se réinitialise. À ne pas faire : les nœuds internes lisent par position
    // absolue, ils n'ont pas besoin du saut pour se placer, et le signaler ferait flusher les
    // readers des WaveNode (bloc de silence + fade, cf. tracktion_WaveNode.cpp) juste à l'entrée
    // du groupe — soit les premières ms perdues. Ce que le saut aurait purgé — les FIFOs de
    // latence — l'est déjà par le pré-roll de ContainerClip::getHead().
    //
    // Même raison de ne PAS passer par isContiguousWithPreviousBlock(), qui est faux au bouclage
    // de l'Edit (in/out du transport) alors que rien n'a sauté : le début de la boucle partait
    // au silence à chaque tour. Les deux fonctions ne diffèrent que par ce drapeau —
    // setPosition() == overridePosition() + userInteraction() — donc la position atterrit au même
    // endroit dans les deux cas. C'est d'ailleurs ce que fait WaveNode lui-même, qui exempte
    // explicitement isFirstBlockOfLoop() de sa purge.
    if (! editPlayHeadState.didPlayheadJump())
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

//==============================================================================
//==============================================================================
ObjAuxReturnNode::ObjAuxReturnNode (ProcessState& editProcessState,
                                    std::shared_ptr<tracktion::graph::Node> dependencyToUse,
                                    std::vector<Plugin::Ptr> sendersToUse,
                                    TimeRange auxTimeRange,
                                    int numChannelsToUse,
                                    size_t nodeIDToUse,
                                    int referenceLatencyNumSamples)
    : TracktionEngineNode (editProcessState),
      dependency (std::move (dependencyToUse)),
      senderPlugins (std::move (sendersToUse)),
      auxRange (auxTimeRange),
      numChannels (std::max (1, numChannelsToUse)),
      auxNodeID (nodeIDToUse),
      referenceLatency (std::max (0, referenceLatencyNumSamples))
{
    assert (dependency);

    for (auto& p : senderPlugins)
        if (auto send = dynamic_cast<ContainerAuxSend*> (p.get()))
            senders.push_back (send);

    // Le retard de chaque tap est connu dès ici : sa latence de prélèvement a été relevée à la
    // construction de la chaîne de son émetteur, qui précède forcément la nôtre (le nœud de
    // contenu est notre dépendance, et les envois vivent dedans). @see la doc de la classe.
    tapDelays.resize (senders.size());

    for (size_t i = 0; i < senders.size(); ++i)
        tapDelays[i].numSamples = std::max (0, referenceLatency - senders[i]->getTapLatencyNumSamples());

    setOptimisations ({ tracktion::graph::ClearBuffers::no,
                        tracktion::graph::AllocateAudioBuffer::yes });
}

tracktion::graph::NodeProperties ObjAuxReturnNode::getNodeProperties()
{
    tracktion::graph::NodeProperties props;
    props.hasAudio = true;
    props.hasMidi = false;
    props.numberOfChannels = numChannels;
    // Ce que ce nœud émet est aussi vieux que ce que le nœud de contenu émet : les taps ont été
    // remis à cet âge-là un par un. Le déclarer est ce qui fait retomber juste l'égalisation
    // d'aval. @see la doc de la classe.
    props.latencyNumSamples = referenceLatency;
    props.nodeID = auxNodeID;

    return props;
}

std::vector<tracktion::graph::Node*> ObjAuxReturnNode::getDirectInputNodes()
{
    // L'audio de cette entrée n'est PAS consommé — seule l'arête compte, pour l'ordre.
    return { dependency.get() };
}

void ObjAuxReturnNode::prepareToPlay (const tracktion::graph::PlaybackInitialisationInfo& info)
{
    for (auto& d : tapDelays)
    {
        if (d.numSamples <= 0)
        {
            d.buffer.setSize (0, 0);
            continue;
        }

        // Capacité = retard + un bloc : une écriture de bloc ne peut alors enjamber la fin du
        // tampon qu'une fois, ce dont les deux copies ci-dessous se contentent.
        d.buffer.setSize (numChannels, d.numSamples + std::max (1, info.blockSize), false, true, true);
        d.buffer.clear();
        d.writePos = 0;
    }
}

// Somme un tap dans `dest`, à travers sa ligne à retard s'il en a une.
void ObjAuxReturnNode::addTap (const juce::AudioBuffer<float>* tap, int tapNumSamples,
                               TapDelay* delay, choc::buffer::ChannelArrayView<float> dest)
{
    const auto numDestFrames = (choc::buffer::FrameCount) dest.getNumFrames();
    const auto numDestChans  = (choc::buffer::ChannelCount) dest.getNumChannels();

    if (delay == nullptr)
    {
        // Chemin direct : aucune latence à rattraper, donc pas un octet alloué ni recopié.
        if (tap == nullptr || tapNumSamples <= 0)
            return;

        auto srcView = tracktion::graph::toBufferView (*const_cast<juce::AudioBuffer<float>*> (tap));

        const auto frames = std::min (numDestFrames, (choc::buffer::FrameCount) tapNumSamples);
        const auto chans  = std::min (numDestChans, srcView.getNumChannels());

        choc::buffer::add (dest.getChannelRange ({ 0, chans }).getStart (frames),
                           srcView.getChannelRange ({ 0, chans }).getStart (frames));
        return;
    }

    const auto capacity = (choc::buffer::FrameCount) delay->buffer.getNumSamples();

    if (capacity == 0 || numDestFrames == 0 || numDestFrames > capacity - (choc::buffer::FrameCount) delay->numSamples)
        return;   // bloc plus grand que prévu : mieux vaut ne rien émettre que déphaser la ligne

    auto line = tracktion::graph::toBufferView (delay->buffer);
    const auto chans = std::min (numDestChans, line.getNumChannels());
    const auto writePos = (choc::buffer::FrameCount) delay->writePos;

    // Découpe un intervalle circulaire de la ligne en une ou deux tranches contiguës.
    const auto sliceLine = [&] (choc::buffer::FrameCount start, choc::buffer::FrameCount num)
    {
        const auto firstNum = std::min (num, capacity - start);
        return std::pair { line.getChannelRange ({ 0, chans }).getFrameRange ({ start, start + firstNum }),
                           line.getChannelRange ({ 0, chans }).getFrameRange ({ 0, num - firstNum }) };
    };

    // 1) Écrire le bloc courant. Du silence là où l'envoi n'a rien produit : la ligne doit
    //    avancer du même nombre d'échantillons à chaque bloc, quoi qu'il arrive.
    {
        auto [w1, w2] = sliceLine (writePos, numDestFrames);
        w1.clear();
        w2.clear();

        if (tap != nullptr && tapNumSamples > 0)
        {
            auto srcView = tracktion::graph::toBufferView (*const_cast<juce::AudioBuffer<float>*> (tap));
            const auto tapChans  = std::min (chans, srcView.getNumChannels());
            const auto tapFrames = std::min (numDestFrames, (choc::buffer::FrameCount) tapNumSamples);
            const auto inFirst   = std::min (tapFrames, w1.getNumFrames());

            choc::buffer::copy (w1.getChannelRange ({ 0, tapChans }).getStart (inFirst),
                                srcView.getChannelRange ({ 0, tapChans }).getStart (inFirst));

            if (tapFrames > inFirst)
                choc::buffer::copy (w2.getChannelRange ({ 0, tapChans }).getStart (tapFrames - inFirst),
                                    srcView.getChannelRange ({ 0, tapChans }).getFrameRange ({ inFirst, tapFrames }));
        }
    }

    // 2) Lire ce qui a été écrit `numSamples` échantillons plus tôt, et le sommer.
    {
        const auto readPos = (writePos + capacity - (choc::buffer::FrameCount) delay->numSamples) % capacity;
        auto [r1, r2] = sliceLine (readPos, numDestFrames);

        choc::buffer::add (dest.getChannelRange ({ 0, chans }).getStart (r1.getNumFrames()), r1);

        if (r2.getNumFrames() > 0)
            choc::buffer::add (dest.getChannelRange ({ 0, chans }).getEnd (r2.getNumFrames()), r2);
    }

    delay->writePos = (int) ((writePos + numDestFrames) % capacity);
}

bool ObjAuxReturnNode::isReadyToProcess()
{
    return dependency->hasProcessed();
}

void ObjAuxReturnNode::process (ProcessContext& pc)
{
    auto destAudio = pc.buffers.audio;
    destAudio.clear();

    const auto numDestFrames = (choc::buffer::FrameCount) destAudio.getNumFrames();
    const auto numDestChans  = (choc::buffer::ChannelCount) destAudio.getNumChannels();

    if (numDestFrames == 0 || numDestChans == 0)
        return;

    // Les taps sont TOUJOURS consommés, y compris hors de la fenêtre de l'aux : un tap laissé
    // en attente ressortirait tel quel à la réactivation suivante. C'est exactement le piège
    // du pré-roll (patch 0014) — ce qu'on croit jeter, on le rend audible plus tard. Idem pour
    // les lignes à retard : elles avancent à chaque bloc, tap ou pas.
    for (size_t i = 0; i < senders.size(); ++i)
    {
        int numSamples = 0;
        // nullptr : l'envoi n'a pas tourné ce bloc — son émetteur est hors de sa fenêtre, ou
        // bypassé. C'est le cas nominal et il ne coûte rien.
        auto tap = senders[i]->getAndClearAuxTap (numSamples);
        auto* delay = tapDelays[i].numSamples > 0 ? &tapDelays[i] : nullptr;

        if (tap == nullptr && delay == nullptr)
            continue;

        addTap (tap, numSamples, delay, destAudio);
    }

    // Bornes de l'aux. Sans ça il sonnerait hors de sa propre fenêtre dès qu'un émetteur joue :
    // le nœud n'a pas de CombiningNode au-dessus de lui pour l'activer par tranches — c'est le
    // prix de l'arête de dépendance. Même geste que ContainerClipNode::process pour son contenu.
    //
    // N.B. les bornes sont décalées de +L : ce qu'on émet à l'instant T est le matériau de
    // T − L (on déclare L de latence), et c'est l'âge du MATÉRIAU qui doit tomber dans la
    // fenêtre. Même convention que PluginNode, qui recule d'autant le temps d'edit qu'il donne
    // à ses plugins.
    {
        const auto sectionRange = getTimelineSampleRange();
        const auto clipRange = toSamples (auxRange, getSampleRate()) + (SampleCount) referenceLatency;

        auto numToClearAtStart = std::min (clipRange.getStart() - sectionRange.getStart(),
                                           (SampleCount) numDestFrames);
        auto numToClearAtEnd   = std::min (sectionRange.getEnd() - clipRange.getEnd(),
                                           (SampleCount) numDestFrames);

        if (numToClearAtStart > 0)
            destAudio.getStart ((choc::buffer::FrameCount) numToClearAtStart).clear();

        if (numToClearAtEnd > 0)
            destAudio.getEnd ((choc::buffer::FrameCount) numToClearAtEnd).clear();
    }
}

} // namespace tracktion::inline engine
