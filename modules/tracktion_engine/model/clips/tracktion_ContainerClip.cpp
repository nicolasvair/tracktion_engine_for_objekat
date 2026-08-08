/*
    ,--.                     ,--.     ,--.  ,--.
  ,-'  '-.,--.--.,--,--.,---.|  |,-.,-'  '-.`--' ,---. ,--,--,      Copyright 2024
  '-.  .-'|  .--' ,-.  | .--'|     /'-.  .-',--.| .-. ||      \   Tracktion Software
    |  |  |  |  \ '-'  \ `--.|  \  \  |  |  |  |' '-' '|  ||  |       Corporation
    `---' `--'   `--`--'`---'`--'`--' `---' `--' `---' `--''--'    www.tracktion.com

    Tracktion Engine uses a GPL/commercial licence - see LICENCE.md for details.
*/

namespace tracktion::inline engine {

ContainerClip::ContainerClip (const juce::ValueTree& v, EditItemID clipID, ClipOwner& targetParent)
    : AudioClipBase (v, clipID, Type::container, targetParent)
{
}

ContainerClip::~ContainerClip()
{
    notifyListenersOfDeletion();
}

//==============================================================================
void ContainerClip::initialise()
{
    clipListState = state.getOrCreateChildWithName (IDs::CLIPLIST, getUndoManager());
    initialiseClipOwner (edit, clipListState);

    AudioClipBase::initialise();
}

void ContainerClip::cloneFrom (Clip* c)
{
    if (auto other = dynamic_cast<ContainerClip*> (c))
    {
        AudioClipBase::cloneFrom (other);
        auto list = state.getChildWithName (IDs::CLIPLIST);
        copyValueTree (list, other->state.getChildWithName (IDs::CLIPLIST), nullptr);

        Selectable::changed();
    }
}

//==============================================================================
juce::String ContainerClip::getSelectableDescription()
{
    return TRANS("Container Clip") + " - \"" + getName() + "\"";
}

bool ContainerClip::isMidi() const
{
    for (auto c : getClips())
        if (c->isMidi())
            return true;

    return false;
}

TimeDuration ContainerClip::getSourceLength() const
{
    auto l = getLoopRange().getLength();

    for (auto c : getClips())
        l = std::max (l, toDuration (c->getPosition().getEnd()));

    return l;
}

double ContainerClip::getPluginLatencySeconds() const
{
    double latency = 0.0;

    // getPluginList() n'existe qu'en version non-const dans Clip ; le calcul, lui, ne
    // modifie rien.
    //
    // N.B. ne PAS filtrer sur isEnabled() : PluginNode::getNodeProperties() ajoute la latence
    // sans regarder l'état d'activation, et un plugin externe bypassé retarde quand même le
    // signal via son latencyProcessor (canProcessBypassed). Filtrer ici sous-estimerait L, et
    // le container serait retardé sans lecture anticipée ni pré-roll : en phase grâce à la PDC
    // globale, mais amputé de ses L premières secondes.
    if (auto pluginList = const_cast<ContainerClip*> (this)->getPluginList())
        for (auto p : *pluginList)
            latency += std::max (0.0, p->getLatencySeconds());

    return latency;
}

TimeDuration ContainerClip::getHead() const
{
    // Pré-roll : la chaîne doit tourner L avant le clip pour que ses FIFOs soient pleines
    // au moment où le premier échantillon utile doit sortir.
    return TimeDuration::fromSeconds (getPluginLatencySeconds());
}

TimeDuration ContainerClip::getTail() const
{
    // Queue : ×2 sur le tail. L pour purger les FIFOs de latence, L de marge pour ce que la
    // latence déclarée ne dit pas (queues de reverb/delay courtes). C'est le comportement que
    // le modèle à pistes donnait gratuitement, et que le container coupait net à sa borne.
    return TimeDuration::fromSeconds (getPluginLatencySeconds() * 2.0);
}

HashCode ContainerClip::getHash() const
{
    size_t hash = 0;

    for (auto c : getClipsOfType<AudioClipBase> (*this))
        hash_combine (hash, c->getHash());

    return static_cast<HashCode> (hash);
}

void ContainerClip::setLoopDefaults()
{
    auto& ts = edit.tempoSequence;
    auto pos = getPosition();

    if (loopInfo.getNumerator() == 0)
        loopInfo.setNumerator (ts.getTimeSigAt (pos.getStart()).numerator);

    if (loopInfo.getDenominator() == 0)
        loopInfo.setDenominator (ts.getTimeSigAt (pos.getStart()).denominator);

    if (loopInfo.getNumBeats() == 0.0)
        loopInfo.setNumBeats (getSourceLength().inSeconds() * (ts.getTempoAt (pos.getStart()).getBpm() / 60.0));
}

void ContainerClip::setLoopRangeBeats (BeatRange newRangeBeats)
{
    const auto newStartBeat  = juce::jmax (0_bp, newRangeBeats.getStart());
    const auto newLengthBeat = juce::jmax (0_bd, newRangeBeats.getLength());

    if (loopStartBeats != newStartBeat || loopLengthBeats != newLengthBeat)
    {
        Clip::setSpeedRatio (1.0);
        setAutoTempo (true);

        loopStartBeats  = newStartBeat;
        loopLengthBeats = newLengthBeat;
    }
}

void ContainerClip::flushStateToValueTree()
{
    for (auto c : getClips())
        c->flushStateToValueTree();

    AudioClipBase::flushStateToValueTree();
}

void ContainerClip::pitchTempoTrackChanged()
{
    for (auto c : getClips())
        c->pitchTempoTrackChanged();

    AudioClipBase::pitchTempoTrackChanged();
}

//==============================================================================
juce::ValueTree& ContainerClip::getClipOwnerState()
{
    return clipListState;
}

EditItemID ContainerClip::getClipOwnerID()
{
    return itemID;
}

Selectable* ContainerClip::getClipOwnerSelectable()
{
    return this;
}

Edit& ContainerClip::getClipOwnerEdit()
{
    return edit;
}

//==============================================================================
bool ContainerClip::isUsingFile (const AudioFile& af)
{
    if (AudioClipBase::isUsingFile (af))
        return true;

    for (auto c : getClipsOfType<AudioClipBase> (*this))
        if (c->isUsingFile (af))
            return true;

    return false;
}

void ContainerClip::clipCreated (Clip&)
{
}

void ContainerClip::clipAddedOrRemoved()
{
}

void ContainerClip::clipOrderChanged()
{
}

void ContainerClip::clipPositionChanged()
{
}


} // namespace tracktion::inline engine
