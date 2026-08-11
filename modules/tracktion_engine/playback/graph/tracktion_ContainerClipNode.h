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


//==============================================================================
//==============================================================================
/** Patch local Objekat — source d'un BUS D'AUX interne à un ContainerClip.

    Émet la somme des envois (`ContainerAuxSend`) qui visent l'aux, puis la chaîne de FX de
    l'aux et ses fades sont posées par-dessus par `createNodeForContainerClip`, et le tout est
    sommé avec la sortie sèche du container.

    **Le seul point non négociable de ce nœud : `dependency`.** Il s'agit du `CombiningNode`
    des enfants du container, déclaré en ENTRÉE DIRECTE alors que son audio n'est pas utilisé.
    C'est cette arête, et elle seule, qui garantit que tous les envois ont écrit avant qu'on
    les lise : deux nœuds frères d'un `SummingNode` n'ont aucun ordre relatif garanti. Le
    `ConnectedNode` de la branche sèche possède le même nœud — même partage de `shared_ptr`
    que le dépliage d'un bloc parallèle, où c'est déjà éprouvé.

    N.B. `ClearBuffers::no` : le nœud vide lui-même son buffer avant de sommer. Ne pas s'en
    remettre au framework ici — @see le patch `0012`, où ~15 types comptaient sur le
    `CombiningNode` pour vider et où plus personne ne le faisait.

    **Alignement (PDC).** Un tap est prélevé au MILIEU du graphe — à la fin de la chaîne de son
    émetteur — alors que toute l'égalisation de latence se fait en aval : le rembourrage qui met
    les clips d'une lane d'accord entre eux, puis les `LatencyNode` que le `SummingNode` pose
    entre ses branches. La copie humide manque donc de tout ce retard-là, et d'un montant qui
    diffère d'un émetteur à l'autre.

    Ce nœud le rattrape avec une règle en une ligne : **retarder le tap i de
    `référence − tapLatence_i`, et DÉCLARER `référence`**. La référence est la latence du nœud
    de contenu — celui-là même auquel le retour sera sommé. Il suffit alors que
    `tapLatence_i + retard_i` soit égal à la latence déclarée pour que l'égalisation d'aval
    retombe exactement juste, quelle que soit la précision par ailleurs de la PDC amont : c'est
    la définition même de ce que « déclarer une latence » veut dire dans ce graphe. La référence
    majore toujours les taps (elle est un max sur des chemins qui les contiennent), donc les
    retards sont positifs.

    Corollaire : les bornes de la fenêtre de l'aux se lisent sur le temps du MATÉRIAU, décalé de
    la latence déclarée — même convention que `PluginNode`, qui recule le temps d'edit qu'il
    donne à ses plugins.
*/
class ObjAuxReturnNode final : public tracktion::graph::Node,
                               public TracktionEngineNode
{
public:
    ObjAuxReturnNode (ProcessState& editProcessState,
                      std::shared_ptr<tracktion::graph::Node> dependency,
                      std::vector<Plugin::Ptr> senders,
                      TimeRange auxTimeRange,
                      int numChannels,
                      size_t nodeID,
                      int referenceLatencyNumSamples);

    //==============================================================================
    tracktion::graph::NodeProperties getNodeProperties() override;
    std::vector<tracktion::graph::Node*> getDirectInputNodes() override;
    void prepareToPlay (const tracktion::graph::PlaybackInitialisationInfo&) override;
    bool isReadyToProcess() override;
    void process (ProcessContext&) override;

private:
    //==============================================================================
    /** Ligne à retard d'UN tap. Circulaire, allouée dans prepareToPlay, et seulement pour les
        envois dont le retard est non nul — le cas courant (aucune latence nulle part) ne coûte
        donc rien. Elle avance à chaque bloc même quand l'envoi n'a rien écrit : une ligne à
        retard qui saute des blocs ne retarde plus de ce qu'elle annonce.
    */
    struct TapDelay
    {
        juce::AudioBuffer<float> buffer;
        int numSamples = 0;
        int writePos = 0;
    };

    void addTap (const juce::AudioBuffer<float>* tap, int tapNumSamples,
                 TapDelay*, choc::buffer::ChannelArrayView<float> dest);

    //==============================================================================
    std::shared_ptr<tracktion::graph::Node> dependency;

    // Les Ptr tiennent les plugins en vie ; le vecteur parallèle évite un dynamic_cast par bloc.
    std::vector<Plugin::Ptr> senderPlugins;
    std::vector<ContainerAuxSend*> senders;
    // Parallèle à `senders` : la ligne à retard de chacun (vide si son retard est nul).
    std::vector<TapDelay> tapDelays;

    // Bornes de l'aux, en temps d'EDIT. Comparables telles quelles au temps local du container :
    // son offset vaut le début de son étendue, donc temps local == temps edit (invariant posé
    // côté application par refreshContainerSpanForKey).
    const TimeRange auxRange;

    const int numChannels;
    const size_t auxNodeID;
    // Latence déclarée = celle du nœud de contenu auquel ce retour sera sommé. @see la doc.
    const int referenceLatency;
};


} // namespace tracktion::inline engine
