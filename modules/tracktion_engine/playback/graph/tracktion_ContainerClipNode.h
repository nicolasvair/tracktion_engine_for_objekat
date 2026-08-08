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
    // par la chaîne retombe alignée sur la timeline de l'Edit. La latence n'est PAS remontée au
    // CombiningNode de la piste : createNodeForClips passe ignoreLatency pour les clips
    // auto-compensés. @see Clip::compensatesOwnPluginLatency, ContainerClip::getHead.
    //
    // Corollaire : le temps d'edit vu par les plugins de cette chaîne doit être corrigé de +L,
    // faute de quoi ils se croient L trop tôt. PluginNode::setReadAheadNumSamples s'en charge,
    // posé par createNodeForContainerClip. Ce n'est pas cosmétique : un plugin qui se sert de
    // PluginRenderContext::editTime comme d'une porte coupait les L premiers samples du groupe.
    const int pluginLatencyNumSamples = 0;
    std::unique_ptr<tempo::Sequence::Position> tempoPosition;

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


} // namespace tracktion::inline engine
