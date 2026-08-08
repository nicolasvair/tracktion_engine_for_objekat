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

} // namespace tracktion::inline engine
