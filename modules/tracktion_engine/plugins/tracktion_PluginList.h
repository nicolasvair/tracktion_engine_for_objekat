/*
    ,--.                     ,--.     ,--.  ,--.
  ,-'  '-.,--.--.,--,--.,---.|  |,-.,-'  '-.`--' ,---. ,--,--,      Copyright 2024
  '-.  .-'|  .--' ,-.  | .--'|     /'-.  .-',--.| .-. ||      \   Tracktion Software
    |  |  |  |  \ '-'  \ `--.|  \  \  |  |  |  |' '-' '|  ||  |       Corporation
    `---' `--'   `--`--'`---'`--'`--' `---' `--' `---' `--''--'    www.tracktion.com

    Tracktion Engine uses a GPL/commercial licence - see LICENCE.md for details.
*/

namespace tracktion::inline engine {

/** Holds a sequence of plugins.
    Used for tracks + clips + one of these holds the master plugins.
*/
class PluginList
{
public:
    PluginList (Edit&);
    ~PluginList();

    //==============================================================================
    Edit& getEdit() const                                   { return edit; }
    Clip* getOwnerClip() const                              { return ownerClip; }
    Track* getOwnerTrack() const                            { return ownerTrack; }

    void initialise (const juce::ValueTree&);
    void releaseObjects();

    void setTrackAndClip (Track*, Clip*);
    void updateTrackProperties();

    //==============================================================================
    Plugin** begin() const;
    Plugin** end() const;
    int size() const;

    Plugin* operator[] (int index) const;
    bool contains (const Plugin*) const;
    int indexOf (const Plugin*) const;

    Plugin::Array getPlugins() const;
    void sendMirrorUpdateToAllPlugins (Plugin&) const;

    void clear();
    bool needsConstantBufferSize();
    bool canInsertPlugin();

    Plugin::Ptr insertPlugin (const juce::ValueTree&, int index);
    void insertPlugin (const Plugin::Ptr&, int index, SelectionManager* selectionManagerToSelect);
    void addDefaultTrackPlugins (bool useVCA);
    void addPluginsFrom (const juce::ValueTree&, bool clearFirst, bool atStart);

    template <typename PluginType>
    PluginType* findFirstPluginOfType() const
    {
        for (auto af : *this)
            if (auto f = dynamic_cast<PluginType*> (af))
                return f;

        return {};
    }

    template <typename PluginType>
    juce::Array<PluginType*> getPluginsOfType() const
    {
        juce::Array<PluginType*> plugins;

        for (auto af : *this)
            if (auto f = dynamic_cast<PluginType*> (af))
                plugins.add (f);

        return plugins;
    }

    //==============================================================================
    juce::ValueTree state;

private:
    //==============================================================================
    Edit& edit;
    Track* ownerTrack = nullptr;
    Clip* ownerClip = nullptr;

    struct ObjectList;
    std::unique_ptr<ObjectList> list;

    juce::UndoManager* getUndoManager() const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginList)
};


//==============================================================================
/** Patch local Objekat — interface d'un plugin qui, au lieu d'une chaîne, en contient N
    en PARALLÈLE.

    `createPluginNodeForList` reconnaît cette interface et déplie le plugin en
    `SummingNode { branche… }`, chaque branche partant de la même entrée via un
    `ConnectedNode`. `SummingNode::createLatencyNodes()` égalise ensuite les branches
    entre elles.

    Pourquoi pas un `RackInstance`, comme le suggérait la spec : `createNodeForRackInstance`
    ne construit pas le rack sur place, il pose des `SendNode`/`ReturnNode` sur des bus et le
    graphe du rack est bâti à la RACINE de l'Edit. `ReturnNode::findSendNodes` cherche alors
    ses sends dans le graphe en cours de transformation — invisible depuis le graphe local
    d'un container. C'est la règle du container : frontière de graphe, rien ne traverse.

    Le plugin lui-même ne traite aucun audio, il est purement structurel : tout se joue à la
    construction du nœud.
*/
struct ParallelPluginBlock
{
    virtual ~ParallelPluginBlock() = default;

    /** Type de l'arbre qui porte la chaîne d'une branche : un enfant `<BRANCH>` de l'état du
        plugin. C'est un hôte légitime de `PluginList` — @see PluginList::initialise, qui
        l'accepte au même titre qu'une piste ou un clip.

        Déclaré ici pour que le tag soit un CONTRAT de l'interface : l'implémentation concrète
        vit côté application, et la même chaîne de caractères écrite des deux côtés n'en serait
        pas un.
    */
    static const juce::Identifier branchTreeType;

    /** Les branches, dans l'ordre. Une liste vide laisse simplement passer l'entrée. */
    virtual std::vector<PluginList*> getParallelBranches() = 0;

    /** Latence du bloc : la branche la plus longue, chaque branche étant la somme SÉRIE de
        ses plugins. Récursif, un bloc pouvant en contenir un autre.

        À renvoyer depuis `Plugin::getLatencySeconds()` de l'implémentation concrète : sans
        ça la PDC du container sous-compenserait, silencieusement.
    */
    static double getMaxBranchLatencySeconds (ParallelPluginBlock& block)
    {
        double maxLatency = 0.0;

        for (auto branch : block.getParallelBranches())
        {
            if (branch == nullptr)
                continue;

            double branchLatency = 0.0;

            for (auto p : *branch)
                branchLatency += std::max (0.0, p->getLatencySeconds());

            maxLatency = std::max (maxLatency, branchLatency);
        }

        return maxLatency;
    }
};

inline const juce::Identifier ParallelPluginBlock::branchTreeType ("BRANCH");


//==============================================================================
/** Patch local Objekat — interface d'un plugin qui ENVOIE vers un bus d'aux interne au
    ContainerClip englobant.

    Le bus est strictement local : l'émetteur et l'aux visé doivent être enfants directs du
    MÊME container. C'est la règle de frontière de graphe — le graphe local d'un container est
    transformé séparément, donc un `SendNode`/`ReturnNode` posé dedans ne se verrait pas.

    **Pourquoi un plugin plutôt qu'un nœud.** `CombiningNode::getInternalNodes()` renvoie tous
    les nœuds de toutes ses chaînes `TimedNode` au graphe englobant. Comme la chaîne de chaque
    objet vit sur la plugin-list de son CLIP, tout nœud inséré là est visible du transform
    externe, qui le câblerait comme dépendance d'un return schedulé en continu — alors que le
    `CombiningNode` ne traite ses `TimedNode` que dans leur fenêtre. C'est ce qui a tué
    `LatencyMaskingNode`. Un plugin, lui, est enveloppé dans un `PluginNode` parfaitement
    ordinaire : aucun transform ne s'y intéresse.

    **Pourquoi un buffer PAR ENVOI et non un buffer par bus.** Un buffer partagé devrait être
    alloué quelque part et sa durée de vie recouper celle de deux graphes pendant une
    reconstruction. Ici chaque envoi possède le sien, dimensionné dans son propre
    `Plugin::initialise` — au bon moment, sur le bon thread, avec la bonne taille de bloc — et
    le nœud de retour se contente de sommer ceux qui ont écrit. Aucune propriété partagée,
    aucun atomique, aucune énigme de durée de vie. Un envoi qui ne joue pas n'écrit rien : le
    silence est gratuit.

    @see ObjAuxReturnNode, createNodeForContainerClip
*/
struct ContainerAuxSend
{
    virtual ~ContainerAuxSend() = default;

    /** Le clip aux visé. Doit être un enfant direct du même container, sinon l'envoi est
        ignoré à la construction du graphe (et journalisé côté application).
    */
    virtual EditItemID getTargetAuxClipID() const = 0;

    /** Rend le buffer écrit pendant CE bloc, ou nullptr si l'envoi n'a pas tourné (hors de la
        fenêtre de son clip, bypassé, niveau à zéro). Consomme le drapeau : deux appels dans le
        même bloc ne rendent la donnée qu'une fois. Appelé par le nœud de retour, donc après
        toute la chaîne d'émetteurs — l'arête de dépendance vers le `CombiningNode` le garantit.
    */
    virtual const juce::AudioBuffer<float>* getAndClearAuxTap (int& numSamples) = 0;

    //==============================================================================
    /** Latence ACCUMULÉE en amont du point de prélèvement, en échantillons — relevée à la
        construction du graphe par `createPluginNodeForList`, qui la lit sur le nœud d'entrée
        de cet envoi.

        Pourquoi le nœud de retour en a besoin : le tap est prélevé au milieu du graphe, à la
        fin de la chaîne de son émetteur, alors que la PDC — celle qui égalise les clips d'une
        même lane entre eux, puis les branches d'un `SummingNode` — n'opère qu'en AVAL de ce
        point. Le signal sec de l'émetteur y gagne encore du retard que sa copie humide n'a
        pas. Sans correctif, le retour sonne en avance de la latence propre de l'émetteur, et
        d'un montant DIFFÉRENT par émetteur.

        La correction est exacte pour un retard `d_i` tel que `d_i + tapLatence_i` soit égal
        pour tous les envois d'un même retour, ET égal à la latence que ce retour DÉCLARE :
        c'est la seule condition pour que l'égalisation en aval retombe juste. @see
        ObjAuxReturnNode.

        Inaudible sur une réverbe, mais rédhibitoire dès qu'on veut « Σ stems = mix, à
        l'échantillon près ».
    */
    void setTapLatencyNumSamples (int n) noexcept           { tapLatencyNumSamples = std::max (0, n); }

    /** @see setTapLatencyNumSamples */
    int getTapLatencyNumSamples() const noexcept            { return tapLatencyNumSamples; }

private:
    int tapLatencyNumSamples = 0;
};

} // namespace tracktion::inline engine
