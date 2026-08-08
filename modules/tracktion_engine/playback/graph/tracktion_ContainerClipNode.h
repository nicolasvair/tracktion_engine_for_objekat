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
/**
*/
class ContainerClipNode final : public tracktion::graph::Node,
                                public TracktionEngineNode
{
public:
    ContainerClipNode (ProcessState& editProcessState,
                       EditItemID containerClipID,
                       BeatRange clipPosition,
                       BeatDuration clipOffset,
                       BeatRange clipLoopRange,
                       std::unique_ptr<Node>,
                       int pluginLatencyNumSamples = 0);

    //==============================================================================
    tracktion::graph::NodeProperties getNodeProperties() override;
    std::vector<Node*> getDirectInputNodes() override;
    std::vector<Node*> getInternalNodes() override;
    void prepareToPlay (const tracktion::graph::PlaybackInitialisationInfo&) override;
    bool isReadyToProcess() override;
    void process (ProcessContext&) override;

private:
    //==============================================================================
    const EditItemID containerClipID;
    const BeatRange clipPosition, loopRange;
    const BeatDuration clipOffset;

    // Patch local Objekat — latence de la PluginList posée AUTOUR de ce nœud, en samples.
    // On lit le contenu du container avec cette avance, pour que la sortie retardée d'autant
    // par la chaîne retombe alignée sur la timeline de l'Edit. @see ContainerClip::getHead.
    //
    // Écart connu et assumé : PluginNode règle son automationAdjustmentTime sur SA latence
    // (-L1 pour le premier plugin), sans savoir que le matériau qu'il reçoit est déjà avancé
    // de L. L'automation de la chaîne du container est donc décalée de L (quelques ms) par
    // rapport au modèle à pistes. Audio et MIDI, eux, restent alignés à l'échantillon près.
    const int pluginLatencyNumSamples = 0;
    std::unique_ptr<tempo::Sequence::Position> tempoPosition;

    // Patch local Objekat — fin du dernier bloc RÉELLEMENT processé. Le CombiningNode ne nous
    // appelle pas hors fenêtre : nos blocs à nous ne sont donc pas contigus, même quand ceux de
    // l'Edit le sont. @see process().
    int64_t lastProcessedReferenceSampleEnd = -1;

    std::unique_ptr<tracktion::graph::Node> input;
    NodeProperties nodeProperties { input->getNodeProperties() };

    struct PlayerContext
    {
        graph::PlayHead playHead;
        graph::PlayHeadState playHeadState { playHead };
        ProcessState processState { playHeadState };
        TracktionNodePlayer player { processState };
    };

    std::shared_ptr<PlayerContext> playerContext;
};


//==============================================================================
//==============================================================================
/** Patch local Objekat — laisse passer l'audio et le MIDI, mais déclare une latence NULLE.

    Posé autour d'un ContainerClip et de sa chaîne de plugins. Le contenu du container est
    déjà lu en avance de la latence de la chaîne (@see ContainerClipNode), donc la sortie est
    alignée : annoncer la latence des plugins vers l'extérieur ferait compenser une deuxième
    fois. Surtout, ça la ferait remonter au CombiningNode de la piste, alors que tout l'intérêt
    est justement qu'il n'ait rien à compenser et reste paresseux.
*/
class LatencyMaskingNode final : public tracktion::graph::Node
{
public:
    LatencyMaskingNode (std::unique_ptr<Node> inputNode)
        : input (std::move (inputNode))
    {
        assert (input);
        setOptimisations ({ tracktion::graph::ClearBuffers::no,
                            tracktion::graph::AllocateAudioBuffer::yes });
    }

    tracktion::graph::NodeProperties getNodeProperties() override
    {
        auto props = input->getNodeProperties();
        props.latencyNumSamples = 0;

        if (props.nodeID != 0)
            hash_combine (props.nodeID, static_cast<size_t> (0x4c61744d61736bull)); // "LatMask"

        return props;
    }

    std::vector<Node*> getDirectInputNodes() override       { return { input.get() }; }
    bool isReadyToProcess() override                        { return input->hasProcessed(); }

    void prepareToPlay (const tracktion::graph::PlaybackInitialisationInfo& info) override
    {
        // Même optimisation que ReturnNode : si on est le seul consommateur, on lit
        // directement le buffer de l'entrée au lieu d'en recopier un par bloc.
        if (! info.enableNodeMemorySharing)
            return;

        if (input->numOutputNodes > 1 || ! input->canShareOutputBuffer())
            return;

        if (input->getNodeProperties().numberOfChannels >= getNodeProperties().numberOfChannels)
        {
            canUseSourceBuffers = true;
            setOptimisations ({ tracktion::graph::ClearBuffers::no,
                                tracktion::graph::AllocateAudioBuffer::no });
        }
    }

    void preProcess (choc::buffer::FrameCount, juce::Range<int64_t>) override
    {
        if (canUseSourceBuffers)
            setBufferViewToUse (input.get(), input->getProcessedOutput().audio);
    }

    void process (ProcessContext& pc) override
    {
        auto source = input->getProcessedOutput();
        setAudioOutput (input.get(), source.audio);
        pc.buffers.midi.copyFrom (source.midi);
    }

private:
    std::unique_ptr<Node> input;
    bool canUseSourceBuffers = false;
};

} // namespace tracktion::inline engine
