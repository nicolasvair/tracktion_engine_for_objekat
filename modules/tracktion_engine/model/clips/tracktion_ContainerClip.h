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
/**
    A clip that can contain multiple other clips and mix their output together.

    This makes it possible to group, move, add effects etc. to a number of clips
    easily.
*/
class ContainerClip  : public AudioClipBase,
                       public ClipOwner
{
public:
    /** Creates a ContainerClip from a given state. @see ClipOwner::insertWaveClip. */
    ContainerClip (const juce::ValueTree&, EditItemID, ClipOwner&);

    /** Destructor. */
    ~ContainerClip() override;

    using Ptr = juce::ReferenceCountedObjectPtr<ContainerClip>;

    //==============================================================================
    /** @internal */
    juce::ValueTree& getClipOwnerState() override;
    /** @internal */
    EditItemID getClipOwnerID() override;
    /** @internal */
    Selectable* getClipOwnerSelectable() override;
    /** @internal */
    Edit& getClipOwnerEdit() override;

    //==============================================================================
    /** @internal */
    juce::File getOriginalFile() const override                 { return {}; }
    /** @internal */
    bool isUsingFile (const AudioFile&) override;

    //==============================================================================
    /** @internal */
    void initialise() override;
    /** @internal */
    void cloneFrom (Clip*) override;

    //==============================================================================
    /** @internal */
    juce::String getSelectableDescription() override;
    /** @internal */
    bool isMidi() const override;

    /** @internal */
    TimeDuration getSourceLength() const override;
    /** @internal */
    HashCode getHash() const override;

    // Patch local Objekat — le container ne surcharge PLUS compensatesOwnPluginLatency() : il
    // déclare sa latence comme n'importe quel clip et la PDC globale l'aligne.
    // @see Clip::compensatesOwnPluginLatency

    //==============================================================================
    /** Patch local Objekat — ce container n'est pas un GROUPE mais un BUS D'AUX interne.

        Un aux d'objekat est un objet à part entière : position, fenêtre, fades, lane, chaîne
        de FX, splittable, gelable. Le `ContainerClip` lui apporte tout ça gratuitement — il
        ne lui manque qu'une source, et c'est justement ce que ce drapeau change : au lieu de
        jouer des enfants (il n'en a aucun), il joue la somme des envois qui le visent.

        `createNodeForContainerClip` intercepte donc ses enfants aux AVANT de bâtir son
        `CombiningNode`, et leur donne un `ObjAuxReturnNode` pour source. Un aux qui n'est
        enfant d'aucun container ne produit aucun nœud : il n'y a pas de bus local à qui il
        appartiendrait, et c'est exactement la portée décidée pour l'étape 3.

        @see ObjAuxReturnNode, ContainerAuxSend
    */
    bool isObjAuxBus() const                                    { return objAuxBus; }

    /** @see isObjAuxBus */
    void setObjAuxBus (bool shouldBeAuxBus)                     { objAuxBus = shouldBeAuxBus; }

    /** @internal */
    void setLoopDefaults() override;
    /** @internal */
    void setLoopRangeBeats (BeatRange) override;

    //==============================================================================
    /** @internal */
    void flushStateToValueTree() override;
    /** @internal */
    void pitchTempoTrackChanged() override;

private:
    //==============================================================================
    juce::ValueTree clipListState;

    // Patch local Objekat — @see isObjAuxBus
    juce::CachedValue<bool> objAuxBus;

    void clipCreated (Clip&) override;
    void clipAddedOrRemoved() override;
    void clipOrderChanged() override;
    void clipPositionChanged() override;

    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ContainerClip)
};

} // namespace tracktion::inline engine
