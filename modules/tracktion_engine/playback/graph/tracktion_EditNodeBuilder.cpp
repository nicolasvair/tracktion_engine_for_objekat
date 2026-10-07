/*
    ,--.                     ,--.     ,--.  ,--.
  ,-'  '-.,--.--.,--,--.,---.|  |,-.,-'  '-.`--' ,---. ,--,--,      Copyright 2024
  '-.  .-'|  .--' ,-.  | .--'|     /'-.  .-',--.| .-. ||      \   Tracktion Software
    |  |  |  |  \ '-'  \ `--.|  \  \  |  |  |  |' '-' '|  ||  |       Corporation
    `---' `--'   `--`--'`---'`--'`--' `---' `--' `---' `--''--'    www.tracktion.com

    Tracktion Engine uses a GPL/commercial licence - see LICENCE.md for details.
*/


// N.B. There are some limitations to this at the moment:
// - Only works with audio clips
// - Only works with WaveAudioClips which have setUsesProxy (false) on them
//
// Patch local Objekat : sélecteur de variante de nœud pour les ContainerClips.
//   1 = DynamicOffsetNode : les clips enfants sont aplatis dans le graphe de l'Edit,
//       avec un décalage temporel. Pas de graphe local, donc pas de PlayerContext.
//   0 = ContainerClipNode : le container possède son propre PlayHead / ProcessState /
//       player. C'est la variante dont dépendent les aux internes et les branches de
//       plugins parallèles, et la seule qui donne la paresse recherchée (un groupe qui
//       ne joue pas ne coûte rien).
// Surchargeable depuis les réglages de build.
#ifndef USE_DYNAMIC_OFFSET_CONTAINER_CLIP
 #define USE_DYNAMIC_OFFSET_CONTAINER_CLIP 0
#endif


namespace tracktion::inline engine
{

//==============================================================================
//==============================================================================
namespace
{
    enum class ClipRole
    {
        arranger,
        launcher
    };

    template<typename PluginType>
    juce::Array<PluginType*> getAllPluginsOfType (Edit& edit)
    {
        juce::Array<PluginType*> plugins;

        // N.B. There is a bit of a hack here checking if the plugin is actually still in the Edit
        // as they are removed from the PluginCache async and we don't want to flush it every time
        // we call this method. This should probably be moved to an EditItemCache like Clips and Tracks
        for (auto p : edit.getPluginCache().getPlugins())
            if (auto pt = dynamic_cast<PluginType*> (p))
                if (pt->state.getParent().isValid() && pt->state.getRoot() == edit.state)
                    plugins.add (pt);

        return plugins;
    }

    using namespace tracktion::graph;

    int getSidechainBusID (EditItemID sidechainSourceID)
    {
        constexpr size_t sidechainMagicNum = 0xb2275e7216a2;
        return static_cast<int> (hash (sidechainMagicNum, sidechainSourceID.getRawID()));
    }

    int getRackInputBusID (EditItemID rackID)
    {
        constexpr size_t rackInputMagicNum = 0x7261636b496e;
        return static_cast<int> (hash (rackInputMagicNum, rackID.getRawID()));
    }

    int getRackOutputBusID (EditItemID rackID)
    {
        constexpr size_t rackOutputMagicNum = 0x7261636b4f7574;
        return static_cast<int> (hash (rackOutputMagicNum, rackID.getRawID()));
    }

    int getWaveInputDeviceBusID (EditItemID trackItemID)
    {
        constexpr size_t waveMagicNum = 0xc1abde;
        return static_cast<int> (hash (waveMagicNum, trackItemID.getRawID()));
    }

    int getMidiInputDeviceBusID (EditItemID trackItemID)
    {
        constexpr size_t midiMagicNum = 0x9a2762;
        return static_cast<int> (hash (midiMagicNum, trackItemID.getRawID()));
    }

    bool isSidechainSource (Track& t)
    {
        const auto itemID = t.itemID;

        for (auto p : t.edit.getPluginCache().getPlugins())
            if (p->getSidechainSourceID() == itemID)
                return true;

        return false;
    }

    AudioTrack* getTrackContainingTrackDevice (Edit& edit, WaveInputDevice& device)
    {
        for (auto t : getAudioTracks (edit))
            if (&t->getWaveInputDevice() == &device)
                return t;

        return nullptr;
    }

    AudioTrack* getTrackContainingTrackDevice (Edit& edit, MidiInputDevice& device)
    {
        for (auto t : getAudioTracks (edit))
            if (&t->getMidiInputDevice() == &device)
                return t;

        return nullptr;
    }

    int getNumChannelsFromDevice (OutputDevice& device)
    {
        if (auto waveDevice = dynamic_cast<WaveOutputDevice*> (&device))
            return waveDevice->getChannelSet().size();

        return 0;
    }

    juce::Array<RackInstance*> getInstancesForRack (RackType& type)
    {
        juce::Array<RackInstance*> instances;

        for (auto ri : getAllPluginsOfType<RackInstance> (type.edit))
            if (ri->type.get() == &type)
                instances.add (ri);

        return instances;
    }

    juce::Array<RackInstance*> getEnabledInstancesForRack (RackType& type)
    {
        auto instances = getInstancesForRack (type);
        instances.removeIf ([] (auto instance) { return ! instance->isEnabled(); });

        return instances;
    }

    // If we're rendering and try to render a track in a submix,
    // only render it if the parent track isn't included in the allowed tracks
    // This allows us to render tracks contained inside submixes without the
    // parent submix effects applied
    bool shouldRenderTrackInSubmix (Track& t, const CreateNodeParams& params)
    {
        jassert (t.isPartOfSubmix());

        if (! params.forRendering)
            return false;

        if (params.allowedTracks == nullptr)
            return false;

        for (auto allowedTrack : *params.allowedTracks)
            if (t.isAChildOf (*allowedTrack))
                return false;

        return true;
    }

    juce::Array<Track*> addImplicitSubmixChildTracks (const juce::Array<Track*> originalTracks)
    {
        if (originalTracks.isEmpty())
            return {};

        auto tracks = originalTracks;

        // Iterate all original tracks
        // If any tracks are submix tracks, check if their parents are included or any of their children
        // If not, add all children recusively
        // Ensure there are no duplicates
        for (auto track : originalTracks)
        {
            if (auto st = dynamic_cast<FolderTrack*> (track);
                st != nullptr && st->isSubmixFolder())
            {
                bool shouldSkip = false;

                // First check for parents
                for (auto potentialParent : originalTracks)
                {
                    if (track->isAChildOf (*potentialParent))
                    {
                        shouldSkip = true;
                        break;
                    }
                }

                // Then children
                for (auto potentialChild : originalTracks)
                {
                    if (potentialChild->isAChildOf (*track))
                    {
                        shouldSkip = true;
                        break;
                    }
                }

                if (shouldSkip)
                    continue;

                // Otherwise add all the children
                for (auto childTrack : st->getAllSubTracks (true))
                    tracks.addIfNotAlreadyThere (childTrack);
            }
        }

        return tracks;
    }

    SpeedFadeDescription getSpeedFadeDescription (const AudioClipBase& clip)
    {
        if (clip.getFadeInBehaviour() == AudioClipBase::speedRamp
            || clip.getFadeOutBehaviour() == AudioClipBase::speedRamp)
        {
            SpeedFadeDescription desc;
            const auto clipPos = clip.getPosition();

            if (clip.getFadeInBehaviour() == AudioClipBase::speedRamp)
            {
                desc.inTimeRange = TimeRange (clipPos.getStart(), clip.getFadeIn());
                desc.fadeInType = clip.getFadeInType();
            }
            else
            {
                desc.inTimeRange = TimeRange (clipPos.getStart(), TimeDuration());
            }

            if (clip.getFadeOutBehaviour() == AudioClipBase::speedRamp)
            {
                desc.outTimeRange = TimeRange (clipPos.getEnd() - clip.getFadeOut(), clip.getFadeOut());
                desc.fadeOutType = clip.getFadeOutType();
            }
            else
            {
                desc.outTimeRange = TimeRange (clipPos.getEnd(), TimeDuration());
            }

            return desc;
        }

        return {};
    }

    std::optional<WarpMap> getWarpMap (const AudioClipBase& clip)
    {
        if (! clip.getWarpTime())
            return {};

        WarpMap map;

        for (auto m : clip.getWarpTimeManager().getMarkers())
            map.push_back ({ m->sourceTime, m->warpTime });

        return map;
    }

    /**
        Returns a tempo::Sequence with the key changes required for a clip to sync to the chord track.
    */
    std::optional<tempo::Sequence> getChordTrackSequenceIfRequired (AudioClipBase& clip)
    {
        if (! clip.getAutoPitch())
            return {};

        if (clip.getAutoPitchMode() != AudioClipBase::chordTrackMono)
            return {};

        if (auto pg = clip.getPatternGenerator())
        {
            // First get the properties that are static for the whole clip
            const auto clipRootKey = clip.getLoopInfo().getRootNote() % 12;
            const auto clipTransposeSemitones = clip.getTransposeSemiTones (false);
            const auto scale = static_cast<int> (pg->scaleType.get());

            // Next get the progression in Edit-time
            juce::OwnedArray<PatternGenerator::ProgressionItem> progression;
            pg->getFlattenedChordProgression (progression, true);

            // Then iterate the progression
            std::vector<tempo::KeyChange> keyChanges;
            auto editTempoSequencePosition = createPosition (clip.edit.tempoSequence);
            BeatPosition beatPos;

            for (auto p : progression)
            {
                // Find the key (pitch/scale) of the Edit
                editTempoSequencePosition.set (beatPos);
                const auto editKey = editTempoSequencePosition.getKey();

                const int scaleNote = editKey.pitch % 12;
                int chordTrackPitchDelta = 0;

                // If this section has a chord, find the pitch offset for it
                if (p->chordName.get().isNotEmpty())
                {
                    const int chordNote = p->getRootNote (scaleNote, Scale (static_cast<Scale::ScaleType> (editKey.scale)));
                    chordTrackPitchDelta = chordNote - scaleNote;
                }

                // Then find the base transposition from the Edit's key and clip's key
                int transposeBase = scaleNote - clipRootKey;

                while (transposeBase > 6)  transposeBase -= 12;
                while (transposeBase < -6) transposeBase += 12;

                // Shift by the section's octave
                transposeBase += p->octave * 12;

                // Put the three transposition sections back together and add it as a KeyChange
                const int finalPitch = transposeBase + chordTrackPitchDelta + clipTransposeSemitones;
                keyChanges.push_back ({ beatPos, { finalPitch, scale } });

                beatPos = beatPos + p->lengthInBeats;
            }

            // Finally copy tempo data from Edit's tempo sequence
            std::vector<tempo::TempoChange> tempoChanges;
            std::vector<tempo::TimeSigChange> timeSigChanges;

            {
                for (auto ts : clip.edit.tempoSequence.getTempos())
                    tempoChanges.push_back ({ ts->startBeatNumber.get(), ts->bpm.get(), ts->curve.get() });

                for (auto ts : clip.edit.tempoSequence.getTimeSigs())
                    timeSigChanges.push_back ({ ts->startBeatNumber.get(), ts->numerator.get(), ts->denominator.get(), ts->triplets.get() });
            }

            const bool useDenominator = clip.edit.engine.getEngineBehaviour().lengthOfOneBeatDependsOnTimeSignature();
            tempo::Sequence seq (std::move (tempoChanges), std::move (timeSigChanges), std::move (keyChanges),
                                    useDenominator ? tempo::LengthOfOneBeat::dependsOnTimeSignature
                                                   : tempo::LengthOfOneBeat::isAlwaysACrotchet);

            return seq;
        }

        return {};
    }

    bool shouldMonitorTrackDevice (InputDeviceInstance& instance)
    {
        switch (instance.owner.getMonitorMode())
        {
            case InputDevice::MonitorMode::on:          return true;
            case InputDevice::MonitorMode::automatic:   return instance.isRecordingActive();
            case InputDevice::MonitorMode::off:         return false;
        };

        return false;
    }

//==============================================================================
//==============================================================================
std::unique_ptr<tracktion::graph::Node> createNodeForTrack (Track&, const CreateNodeParams&);

std::unique_ptr<tracktion::graph::Node> createPluginNodeForList (PluginList&, const TrackMuteState*, std::unique_ptr<Node>,
                                                                 tracktion::graph::PlayHeadState&, const CreateNodeParams&);

std::unique_ptr<tracktion::graph::Node> createPluginNodeForTrack (Track&, TrackMuteState&, std::unique_ptr<Node>,
                                                                 tracktion::graph::PlayHeadState&, const CreateNodeParams&);

std::unique_ptr<tracktion::graph::Node> createLiveInputNodeForDevice (InputDeviceInstance&, tracktion::graph::PlayHeadState&, const CreateNodeParams&, EditItemID);

std::unique_ptr<tracktion::graph::Node> createNodeForClips (EditItemID, const juce::Array<Clip*>&, const TrackMuteState&, const CreateNodeParams&);

std::unique_ptr<Node> createInsertReturnNode (InsertPlugin&, tracktion::graph::PlayHeadState&, const CreateNodeParams&);

//==============================================================================
//==============================================================================
/** Patch local Objekat — met à zéro les N premiers échantillons qui suivent une reprise de
    traitement non contiguë.

    Purger la FIFO d'un plugin à latence n'est pas silencieux : ce qu'on en chasse, c'est la
    queue de l'activation précédente, et elle sortirait telle quelle AVANT le clip. Mais elle
    n'est pas toujours illégitime — en boucle, elle EST le raccord attendu. Le critère n'est
    donc pas la position dans l'Edit, c'est la continuité : la sortie d'une chaîne à latence
    N ne vaut qu'après N échantillons de traitement contigu.

    « Contigu » se lit sur les échantillons de référence, pas sur le temps d'edit : un bouclage
    y est contigu (@see PlayHeadState::didPlayheadJump, distinct de isFirstBlockOfLoop), et
    c'est bien ce qu'on veut, la chaîne ayant traité sans interruption de part et d'autre du
    raccord. Un saut de tête de lecture, un stop/start ou un bloc sauté (le CombiningNode ne
    traite ses entrées que dans leur fenêtre) réamorcent au contraire le compte.
*/
class LatencyPrimingNode final : public Node,
                                 public TracktionEngineNode
{
public:
    LatencyPrimingNode (std::unique_ptr<Node> inputNode, ProcessState& ps, int numSamplesToPrime)
        : TracktionEngineNode (ps),
          input (std::move (inputNode)),
          samplesToPrime (numSamplesToPrime)
    {
        setOptimisations ({ tracktion::graph::ClearBuffers::no,
                            tracktion::graph::AllocateAudioBuffer::yes });
    }

    tracktion::graph::NodeProperties getNodeProperties() override
    {
        auto props = input->getNodeProperties();

        constexpr size_t magicHash = size_t (0x9a7c1e5b3d240f11);

        if (props.nodeID != 0)
            hash_combine (props.nodeID, magicHash);

        return props;
    }

    std::vector<Node*> getDirectInputNodes() override    { return { input.get() }; }
    bool isReadyToProcess() override                     { return input->hasProcessed(); }

    void process (ProcessContext& pc) override
    {
        auto sourceBuffers = input->getProcessedOutput();

        if (pc.referenceSampleRange.getStart() != nextExpectedReferenceSample
             || getPlayHeadState().didPlayheadJump())
            samplesRemaining = samplesToPrime;

        nextExpectedReferenceSample = pc.referenceSampleRange.getEnd();

        pc.buffers.midi.copyFrom (sourceBuffers.midi);

        if (samplesRemaining <= 0)
        {
            // Amorçage terminé : nœud passant, aucune copie.
            setAudioOutput (input.get(), sourceBuffers.audio);
            return;
        }

        graph::copyIfNotAliased (pc.buffers.audio, sourceBuffers.audio);

        const auto numToClear = std::min ((choc::buffer::FrameCount) samplesRemaining,
                                          pc.buffers.audio.getNumFrames());
        pc.buffers.audio.getStart (numToClear).clear();
        samplesRemaining -= (int) numToClear;
    }

private:
    const std::unique_ptr<Node> input;
    const int samplesToPrime;
    int samplesRemaining = 0;
    int64_t nextExpectedReferenceSample = std::numeric_limits<int64_t>::min();
};

//==============================================================================
//==============================================================================
std::unique_ptr<tracktion::graph::Node> createFadeNodeForClip (AudioClipBase& clip, EditTimeRange clipTimeRangeToUse,
                                                               std::unique_ptr<Node> node, const CreateNodeParams& params)
{
    auto fIn = clip.getFadeIn();
    auto fOut = clip.getFadeOut();

    if (fIn > 0_td || fOut > 0_td)
    {
        const bool speedIn = clip.getFadeInBehaviour() == AudioClipBase::speedRamp && fIn > 0_td;
        const bool speedOut = clip.getFadeOutBehaviour() == AudioClipBase::speedRamp && fOut > 0_td;

        auto pos = toTime (clipTimeRangeToUse, clip.edit.tempoSequence);
        node = makeNode<FadeInOutNode> (std::move (node), params.processState,
                                        speedIn ? TimeRange (pos.getStart(), pos.getStart() + juce::jmin (TimeDuration::fromSeconds (0.003), fIn))
                                                : TimeRange (pos.getStart(), pos.getStart() + fIn),
                                        speedOut ? TimeRange (pos.getEnd() - juce::jmin (TimeDuration::fromSeconds (0.003), fOut), pos.getEnd())
                                                 : TimeRange (pos.getEnd() - fOut, pos.getEnd()),
                                        clip.getFadeInType(), clip.getFadeOutType(),
                                        true);
    }

    return node;
}

//==============================================================================
std::unique_ptr<tracktion::graph::Node> createNodeForAudioClip (AudioClipBase& clip, EditItemID idToUse, EditTimeRange clipTimeRangeToUse,
                                                                bool includeARA, const CreateNodeParams& params, ClipRole role)
{
    auto& playHeadState = params.processState.playHeadState;
    const AudioFile playFile (clip.getPlaybackFile());

    if (playFile.isNull())
        return {};

    std::unique_ptr<Node> node;

    // Check if ARA should be used
    if (clip.isUsingARA())
    {
        if (! includeARA)
            return {};

        if (! clip.setupARA (true))
            return {};

        jassert (clip.getARAProxy() != nullptr);

        // Patch local Objekat — 0038 : l'ARANode est la SOURCE du clip. Il suit la même queue qu'un
        // fichier (plugins du clip, puis fondu), au lieu de sortir nu comme dans l'arbre d'origine.
        node = makeNode<ARANode> (clip, playHeadState.playHead, params.forRendering);
    }
    else
    {
        clip.tearDownARA();

        // Otherwise use audio file
        auto original = clip.getAudioFile();

        // Trigger proxy render if it needs it
        clip.beginRenderingNewProxyIfNeeded();

        if (clip.canUseProxy())
        {
            assert (role != ClipRole::launcher);
            assert (! clipTimeRangeToUse.isBeats());
            TimeDuration nodeOffset;
            double speed = 1.0;
            TimeRange loopRange;

            if (! clip.usesTimeStretchedProxy())
            {
                nodeOffset = clip.getPosition().getOffset();
                loopRange = clip.getLoopRange();
                speed = clip.getSpeedRatio();
            }

            const auto channelConfig = clip.getOutputChannelConfiguration();

            if ((clip.getFadeInBehaviour() == AudioClipBase::speedRamp && clip.getFadeIn() != 0_td)
                || (clip.getFadeOutBehaviour() == AudioClipBase::speedRamp && clip.getFadeOut() != 0_td))
            {
                SpeedFadeDescription desc;
                const auto clipPos = clip.getPosition();

                if (clip.getFadeInBehaviour() == AudioClipBase::speedRamp)
                {
                    desc.inTimeRange = TimeRange (clipPos.getStart(), clip.getFadeIn());
                    desc.fadeInType = clip.getFadeInType();
                }
                else
                {
                    desc.inTimeRange = TimeRange (clipPos.getStart(), TimeDuration());
                }

                if (clip.getFadeOutBehaviour() == AudioClipBase::speedRamp)
                {
                    desc.outTimeRange = TimeRange (clipPos.getEnd() - clip.getFadeOut(), clip.getFadeOut());
                    desc.fadeOutType = clip.getFadeOutType();
                }
                else
                {
                    desc.outTimeRange = TimeRange (clipPos.getEnd(), TimeDuration());
                }

                node = tracktion::graph::makeNode<SpeedRampWaveNode> (playFile,
                                                                      toTime (clipTimeRangeToUse, clip.edit.tempoSequence),
                                                                      nodeOffset,
                                                                      loopRange,
                                                                      clip.getLiveClipLevel(),
                                                                      speed,
                                                                      channelConfig,
                                                                      ChannelConfiguration::discreteChannels (channelConfig.getNumChannels()),
                                                                      params.processState,
                                                                      idToUse,
                                                                      params.forRendering,
                                                                      desc);
            }
            else
            {
                node = tracktion::graph::makeNode<WaveNode> (playFile,
                                                             toTime (clipTimeRangeToUse, clip.edit.tempoSequence),
                                                             nodeOffset,
                                                             loopRange,
                                                             clip.getLiveClipLevel(),
                                                             speed,
                                                             channelConfig,
                                                             ChannelConfiguration::discreteChannels (channelConfig.getNumChannels()),
                                                             params.processState,
                                                             idToUse,
                                                             params.forRendering,
                                                             params.forRendering ? nullptr : clip.getPlayhead());
            }
        }
        else
        {
            const auto timeStretcherMode = clip.getActualTimeStretchMode();
            const auto timeStretcherOpts = clip.elastiqueProOptions.get();
            const auto readAhead = params.readAheadTimeStretchNodes ? WaveNodeRealTime::ReadAhead::yes
                                                                    : WaveNodeRealTime::ReadAhead::no;

            const auto speedFadeDesc = getSpeedFadeDescription (clip);
            auto warpMap = getWarpMap (clip);
            std::optional<tempo::Sequence::Position> editTempoPosition (speedFadeDesc.isEmpty() ? std::optional<tempo::Sequence::Position>() : createPosition (clip.edit.tempoSequence));

            if (clip.getAutoTempo() || clip.getAutoPitch() || role == ClipRole::launcher)
            {
                assert (clipTimeRangeToUse.isBeats());
                std::vector<tempo::TempoChange> tempos;
                std::vector<tempo::TimeSigChange> timeSigs;
                std::vector<tempo::KeyChange> keyChanges;
                auto syncTempo = WaveNodeRealTime::SyncTempo::no;
                auto syncPitch = WaveNodeRealTime::SyncPitch::no;

                auto wi = clip.getWaveInfo();
                auto& li = clip.getLoopInfo();

                if (clip.getAutoTempo() && li.getNumBeats() > 0 && wi.hashCode != 0)
                {
                    tempos.push_back ({ 0_bp, li.getBpm (wi), 1.0 });
                    timeSigs.push_back ({ 0_bp, li.getNumerator(), li.getDenominator(), false });
                    syncTempo = WaveNodeRealTime::SyncTempo::yes;
                }
                else
                {
                    tempos.push_back ({ 0_bp, 120.0, 0.0 });
                    timeSigs.push_back ({ 0_bp, 4, 4, false });
                }

                if (clip.getAutoPitch() && li.getRootNote() != -1)
                {
                    keyChanges.push_back ({ 0_bp, { li.getRootNote(), 0 } });
                    syncPitch = WaveNodeRealTime::SyncPitch::yes;
                }

                tempo::Sequence seq (std::move (tempos),
                                     std::move (timeSigs),
                                     std::move (keyChanges),
                                     clip.edit.engine.getEngineBehaviour().lengthOfOneBeatDependsOnTimeSignature() ? tempo::LengthOfOneBeat::dependsOnTimeSignature
                                                                                                                   : tempo::LengthOfOneBeat::isAlwaysACrotchet);

                if (role == ClipRole::launcher)
                {
                    WaveNodeRealTime::BeatConfig config
                    {
                        .processState = params.processState,
                        .audioFile = playFile,
                        .timeStretchMode = timeStretcherMode,
                        .elastiqueProOptions = timeStretcherOpts,
                        .editTime = BeatRange (0_bp, BeatPosition::fromBeats (std::numeric_limits<double>::max())),
                        .offset = clip.getOffsetInBeats(),
                        .loopSection = clip.getLoopRangeBeats(),
                        .liveClipLevel = clip.getLiveClipLevel(),
                        .sourceChannelsToUse = clip.getActiveChannelConfiguration(),
                        .destChannelsToFill = ChannelConfiguration::discreteChannels (clip.getActiveChannelConfiguration().getNumChannels()),
                        .itemID = idToUse,
                        .isOfflineRender = params.forRendering,
                        .resamplingQuality = clip.getResamplingQuality(),
                        .speedFadeDescription = speedFadeDesc,
                        .editTempoSequence = std::move (editTempoPosition),
                        .warpMap = std::move (warpMap),
                        .sourceFileTempoMap = std::move (seq),
                        .syncTempo = syncTempo,
                        .syncPitch = syncPitch,
                        .chordPitchSequence = getChordTrackSequenceIfRequired (clip),
                        .pitchChangeSemitones = clip.getPitchChange(),
                        .readAhead = readAhead,
                        .playhead = params.forRendering ? nullptr : clip.getPlayhead()
                    };
                    node = makeNode<WaveNodeRealTime> (std::move (config));
                }
                else
                {
                    node = makeNode<WaveNodeRealTime> (playFile,
                                                       timeStretcherMode, timeStretcherOpts,
                                                       toBeats (clipTimeRangeToUse, clip.edit.tempoSequence),
                                                       clip.getOffsetInBeats(),
                                                       BeatRange (clip.getLoopStartBeats(), clip.getLoopLengthBeats()),
                                                       clip.getLiveClipLevel(),
                                                       clip.getActiveChannelConfiguration(),
                                                       ChannelConfiguration::discreteChannels (clip.getActiveChannelConfiguration().getNumChannels()),
                                                       params.processState,
                                                       idToUse,
                                                       params.forRendering,
                                                       clip.getResamplingQuality(),
                                                       speedFadeDesc, std::move (editTempoPosition),
                                                       std::move (warpMap),
                                                       seq, syncTempo, syncPitch,
                                                       getChordTrackSequenceIfRequired (clip),
                                                       clip.getPitchChange(),
                                                       readAhead,
                                                       params.forRendering ? nullptr : clip.getPlayhead());
                }
            }
            else
            {
                assert (role != ClipRole::launcher);
                assert (! clipTimeRangeToUse.isBeats());
                node = makeNode<WaveNodeRealTime> (playFile,
                                                   toTime (clipTimeRangeToUse, clip.edit.tempoSequence),
                                                   clip.getPosition().getOffset(),
                                                   clip.getLoopRange(),
                                                   clip.getLiveClipLevel(),
                                                   clip.getSpeedRatio(),
                                                   clip.getActiveChannelConfiguration(),
                                                   ChannelConfiguration::discreteChannels (clip.getActiveChannelConfiguration().getNumChannels()),
                                                   params.processState,
                                                   idToUse,
                                                   params.forRendering,
                                                   clip.getResamplingQuality(),
                                                   speedFadeDesc, std::move (editTempoPosition),
                                                   timeStretcherMode, timeStretcherOpts,
                                                   clip.getPitchChange(),
                                                   readAhead,
                                                   params.forRendering ? nullptr : clip.getPlayhead());
            }
        }
    }

    // Plugins
    if (params.includePlugins)
    {
        if (auto pluginList = clip.getPluginList())
        {
            for (auto p : *pluginList)
                p->initialiseFully();

            node = createPluginNodeForList (*pluginList, nullptr, std::move (node), playHeadState, params);
        }
    }

    // Create FadeInOutNode
    if (role != ClipRole::launcher)
        node = createFadeNodeForClip (clip, clipTimeRangeToUse, std::move (node), params);

    return node;
}

std::unique_ptr<tracktion::graph::Node> createNodeForAudioClip (AudioClipBase& clip, bool includeARA,
                                                                const CreateNodeParams& params, ClipRole role)
{
    if (clip.canUseProxy())
    {
        assert (role == ClipRole::arranger);
        return createNodeForAudioClip (clip, clip.itemID, clip.getEditTimeRange(), includeARA, params, role);
    }

    if (clip.getAutoTempo() || clip.getAutoPitch() || role == ClipRole::launcher)
        return createNodeForAudioClip (clip, clip.itemID, clip.getEditBeatRange(), includeARA, params, role);

    assert (role == ClipRole::arranger);
    return createNodeForAudioClip (clip, clip.itemID, clip.getEditTimeRange(), includeARA, params, role);
}

std::unique_ptr<tracktion::graph::Node> createNodeForMidiClip (MidiClip& clip, const TrackMuteState& trackMuteState,
                                                               const CreateNodeParams& params, ClipRole role)
{
    CRASH_TRACER
    const bool generateMPE = clip.getMPEMode();
    const auto timeBase = clip.canUseProxy() ? MidiList::TimeBase::seconds
                                             : MidiList::TimeBase::beatsRaw;

    const auto channels = generateMPE ? juce::Range<int> (2, 15)
                                      : juce::Range<int>::withStartAndLength (clip.getMidiChannel().getChannelNumber(), 1);

    if (timeBase == MidiList::TimeBase::beatsRaw)
    {
        std::vector<juce::MidiMessageSequence> sequences;
        sequences.emplace_back (clip.getSequence().exportToPlaybackMidiSequence (clip, timeBase, generateMPE));
        const auto clipBeatRange = role == ClipRole::launcher ? BeatRange (0_bp, BeatPosition::fromBeats (std::numeric_limits<double>::max()))
                                                              : BeatRange (clip.getStartBeat(), clip.getEndBeat());

        return graph::makeNode<LoopingMidiNode> (std::move (sequences),
                                                 channels,
                                                 generateMPE,
                                                 clipBeatRange,
                                                 clip.getLoopRangeBeats(),
                                                 clip.getOffsetInBeats(),
                                                 clip.getLiveClipLevel(),
                                                 params.processState,
                                                 clip.itemID,
                                                 clip.getQuantisation(),
                                                 clip.edit.engine.getGrooveTemplateManager().getTemplateByName (clip.getGrooveTemplate()),
                                                 clip.getGrooveStrength(),
                                                 [&trackMuteState]
                                                 {
                                                      if (! trackMuteState.shouldTrackBeAudible())
                                                         return ! trackMuteState.shouldTrackMidiBeProcessed();

                                                     return false;
                                                 });
    }

    // Use looped sequence in seconds time base
    assert (role != ClipRole::launcher);
    const auto clipTimeRange = clip.getEditTimeRange();
    const juce::Range<double> editTimeRange { clipTimeRange.getStart().inSeconds(), clipTimeRange.getEnd().inSeconds() };

    std::vector<juce::MidiMessageSequence> sequences;
    sequences.emplace_back (clip.getSequenceLooped().exportToPlaybackMidiSequence (clip, timeBase, generateMPE));

    return graph::makeNode<MidiNode> (std::move (sequences),
                                      timeBase,
                                      channels,
                                      generateMPE,
                                      editTimeRange,
                                      clip.getLiveClipLevel(),
                                      params.processState,
                                      clip.itemID,
                                      [&trackMuteState]
                                      {
                                          if (! trackMuteState.shouldTrackBeAudible())
                                              return ! trackMuteState.shouldTrackMidiBeProcessed();

                                          return false;
                                      });
}

std::unique_ptr<tracktion::graph::Node> createNodeForStepClip (StepClip& clip, const TrackMuteState& trackMuteState,
                                                               const CreateNodeParams& params, ClipRole role)
{
    CRASH_TRACER

    std::unique_ptr<tracktion::graph::Node> node;

    if (role == ClipRole::launcher)
    {
        std::vector<juce::MidiMessageSequence> sequences;

        for (int i = clip.usesProbability() ? 64 : 1; --i >= 0;)
            sequences.push_back (clip.generateMidiSequence (MidiList::TimeBase::beatsRaw));

        const auto clipBeatRange = BeatRange (0_bp, BeatPosition::fromBeats (std::numeric_limits<double>::max()));
        node = graph::makeNode<LoopingMidiNode> (std::move (sequences),
                                                 juce::Range<int> (1, 16),
                                                 false,
                                                 clipBeatRange,
                                                 clip.getLoopRangeBeats(),
                                                 clip.getOffsetInBeats(),
                                                 clip.getLiveClipLevel(),
                                                 params.processState,
                                                 clip.itemID,
                                                 QuantisationType(),
                                                 nullptr,
                                                 0.0f,
                                                 [&trackMuteState]
                                                 {
                                                     if (! trackMuteState.shouldTrackBeAudible())
                                                         return ! trackMuteState.shouldTrackMidiBeProcessed();

                                                     return false;
                                                 });
    }
    else
    {
        std::vector<juce::MidiMessageSequence> sequences;

        for (int i = clip.usesProbability() ? 64 : 1; --i >= 0;)
        {
            juce::MidiMessageSequence sequence;
            clip.generateMidiSequence (sequence);
            sequences.push_back (sequence);
        }

        const auto clipRange = clip.getEditTimeRange ();
        const juce::Range<double> editTimeRange (clipRange.getStart ().inSeconds (), clipRange.getEnd ().inSeconds ());
        node = graph::makeNode<MidiNode> (std::move (sequences),
                                          MidiList::TimeBase::seconds,
                                          juce::Range<int> (1, 16),
                                          false,
                                          editTimeRange,
                                          clip.getLiveClipLevel(),
                                          params.processState,
                                          clip.itemID,
                                          [&trackMuteState]
                                          {
                                              if (!trackMuteState.shouldTrackBeAudible ())
                                                  return !trackMuteState.shouldTrackMidiBeProcessed ();

                                              return false;
                                          });
    }

    if (node && ! clip.getListeners().isEmpty())
        node = makeNode<LiveMidiOutputNode> (clip, std::move (node));

    return node;
}

// Patch local Objekat — greffe des retours d'aux au-dessus d'un nœud de contenu.
//
//     contenu   = le nœud où les envois écrivent (CombiningNode d'un container, ou la somme
//                 des pistes à la racine de l'Edit)
//     partagé   = shared_ptr (contenu)
//     sec       = ConnectedNode (partagé)                   ← branche sèche
//     retour_i  = ObjAuxReturnNode (dépend de partagé) → FX de l'aux i → fades de l'aux i
//     résultat  = SummingNode { sec, retour_1…retour_n }
//
// L'arête `retour_i → partagé` n'est pas décorative : elle seule ordonne les envois avant leur
// lecture. Deux frères d'un SummingNode n'ont aucun ordre garanti entre eux.
//
// Les envois sont cherchés au niveau supérieur de chacune des `senderLists`. Pas dans les
// branches d'un bloc parallèle : une branche n'est pas un objet, et l'application pose l'envoi
// en fin de chaîne. Dans un container, la chaîne du container lui-même n'en fait pas partie :
// elle vit en AVAL du ContainerClipNode, donc hors de ce graphe — et viser son propre aux
// serait un bouclage.
// Les chaînes où chercher les envois d'une fratrie de clips : celle de chaque clip.
static std::vector<PluginList*> senderPluginLists (const juce::Array<Clip*>& clips)
{
    std::vector<PluginList*> result;

    for (auto c : clips)
        if (auto pl = c->getPluginList())
            result.push_back (pl);

    return result;
}

static std::unique_ptr<Node> createAuxReturns (const std::vector<PluginList*>& senderLists,
                                               const juce::Array<ContainerClip*>& auxClips,
                                               std::unique_ptr<Node> contentNode,
                                               const CreateNodeParams& params)
{
    if (auxClips.isEmpty() || contentNode == nullptr)
        return contentNode;

    auto sendersFor = [&senderLists] (EditItemID auxID)
    {
        std::vector<Plugin::Ptr> result;

        for (auto pl : senderLists)
            for (auto p : *pl)
                if (auto send = dynamic_cast<ContainerAuxSend*> (p))
                    if (send->getTargetAuxClipID() == auxID)
                        result.push_back (p);

        return result;
    };

    const auto contentProps = contentNode->getNodeProperties();
    const int numChannels = std::max (2, contentProps.numberOfChannels);

    std::shared_ptr<Node> sharedContent (std::move (contentNode));
    std::vector<std::unique_ptr<Node>> summedNodes;

    // Branche sèche : un ConnectedNode, comme le dépliage d'un bloc parallèle — le contenu est
    // désormais détenu par des shared_ptr et lu par plusieurs nœuds.
    {
        size_t nodeID = 0;
        hash_combine (nodeID, (size_t) 0x0B7A0DE7);
        hash_combine (nodeID, contentProps.nodeID);

        auto connected = std::make_unique<tracktion::graph::ConnectedNode> (nodeID);

        for (int c = 0; c < contentProps.numberOfChannels; ++c)
            connected->addAudioConnection (sharedContent, { c, c });

        if (contentProps.hasMidi)
            connected->addMidiConnection (sharedContent);

        summedNodes.push_back (std::move (connected));
    }

    for (auto auxClip : auxClips)
    {
        size_t nodeID = 0;
        hash_combine (nodeID, (size_t) 0x0B7A0E37);
        hash_combine (nodeID, auxClip->itemID.getRawID());

        // Chaîne de FX de l'aux. Elle se branche en AVAL du nœud de retour, mais celui-ci en tient
        // la liste : c'est lui qui voit les discontinuités et purge les queues. @see son process().
        std::vector<Plugin::Ptr> chainPlugins;

        if (params.includePlugins)
            if (auto pluginList = auxClip->getPluginList())
                for (auto p : *pluginList)
                    chainPlugins.push_back (p);

        // La latence du nœud de CONTENU est la référence d'alignement des taps : c'est à cette
        // somme-là que le retour ira s'ajouter. @see ObjAuxReturnNode.
        std::unique_ptr<Node> returnNode = makeNode<ObjAuxReturnNode> (params.processState,
                                                                       sharedContent,
                                                                       sendersFor (auxClip->itemID),
                                                                       std::move (chainPlugins),
                                                                       auxClip->getEditTimeRange(),
                                                                       numChannels,
                                                                       nodeID,
                                                                       contentProps.latencyNumSamples);

        if (params.includePlugins)
        {
            if (auto pluginList = auxClip->getPluginList())
            {
                // Comme pour un groupe : ces plugins ne sont dans aucune liste hôte déjà
                // initialisée, et leur latence lirait 0 au premier build.
                for (auto p : *pluginList)
                    p->initialiseFully();

                returnNode = createPluginNodeForList (*pluginList, nullptr, std::move (returnNode),
                                                      params.processState.playHeadState, params);
            }
        }

        summedNodes.push_back (createFadeNodeForClip (*auxClip, auxClip->getEditTimeRange(),
                                                      std::move (returnNode), params));
    }

    return makeNode<tracktion::graph::SummingNode> (std::move (summedNodes));
}

// Patch local Objekat — the audio bridge's scheduling order. A combiner (a pool track's, or one of a
// container's per-rank ones) is a unit the gates may wait for, and the units of rank >= 1 wait for
// every unit of a lower rank: a reader in their clips reads what those write. Rank 0 gets no gate and
// stays byte-identical to what it always was. A node that is not a CombiningNode is left alone.
// @see CombiningNode::setBridgeRank, BridgeGateNode, docs/plan_sidechain.md §5.5
static void applyBridgeRank (Node& node, int rank, EditItemID unitID, const CreateNodeParams& params)
{
    auto combiner = dynamic_cast<CombiningNode*> (&node);

    if (combiner == nullptr)
        return;

    combiner->setBridgeRank (rank);

    if (rank >= 1 && params.bridgeBuild != nullptr)
        combiner->setOrderingGate (makeNode<BridgeGateNode> (rank,
                                                             hash (hash ((size_t) 0x0B76A7E0, unitID.getRawID()), (size_t) rank),
                                                             params.bridgeBuild));
}

std::unique_ptr<tracktion::graph::Node> createNodeForContainerClip (ContainerClip& clip, [[ maybe_unused ]] const TrackMuteState& trackMuteState,
                                                                    const CreateNodeParams& params, ClipRole role)
{
    CRASH_TRACER
    const auto& allChildClips = clip.getClips();

    // Patch local Objekat — un enfant marqué « bus d'aux » n'est pas une source : il REÇOIT.
    // On l'écarte du CombiningNode et il devient une branche de retour. @see ContainerClip::isObjAuxBus
    juce::Array<Clip*> clips;
    juce::Array<ContainerClip*> auxClips;

    for (auto c : allChildClips)
    {
        if (auto cc = dynamic_cast<ContainerClip*> (c); cc != nullptr && cc->isObjAuxBus())
            auxClips.add (cc);
        else
            clips.add (c);
    }

    // Un container sans contenu ne produit rien — même s'il porte des aux, personne ne leur
    // enverrait quoi que ce soit.
    if (clips.isEmpty())
        return {};

   #if USE_DYNAMIC_OFFSET_CONTAINER_CLIP
    std::unique_ptr<Node> node;

    {
        std::vector<std::unique_ptr<Node>> nodes;

        for (auto c : clips)
        {
            if (auto acb = dynamic_cast<AudioClipBase*> (c))
            {
                assert (! acb->canUseProxy());
                assert (acb->getAutoTempo());

                if (auto clipNode = createNodeForAudioClip (*acb, false, params, ClipRole::arranger))
                    nodes.push_back (std::move (clipNode));
            }
            else
            {
                assert (false && "Only WaveAudioClips supported at the moment");
            }
        }

        auto offsetNode = std::make_unique<DynamicOffsetNode> (params.processState,
                                                               clip.itemID,
                                                               role == ClipRole::launcher ? BeatRange (0_bp, BeatPosition::fromBeats (std::numeric_limits<double>::max()))
                                                                                          : clip.getEditBeatRange(),
                                                               clip.getOffsetInBeats(),
                                                               clip.getLoopRangeBeats(),
                                                               std::move (nodes));
        node = std::move (offsetNode);
    }
   #else
    // Combiner clip and the contained clips need their own, local PlayHeadState.
    // This also needs to persist across graph rebuilds to maintain continuity.
    // Once the ContainerClipNode has been initialised it will update it's children with its own ProcessState
    // Patch local Objekat — plus de lecture anticipée (elle valait 0007/0008).
    //
    // Elle n'existait que pour faire reporter au container une latence NULLE, seul moyen à
    // l'époque de le soustraire au repli `clipsHaveLatency` qui condamnait toute la lane au
    // traitement continu. Ce repli a disparu, donc la raison d'être aussi.
    //
    // Et elle a un défaut structurel : lire L en avance suppose d'avoir tourné L avant le
    // groupe. Au retour d'une boucle dont le IN tombe sur le début du groupe, cet élan
    // n'existe pas — les L premières millisecondes du groupe sortaient perdues à chaque tour.
    // Un clip ordinaire n'a jamais eu ce problème : il déclare sa latence, la PDC globale
    // retarde le reste, et rien n'a besoin d'élan. Le container fait pareil désormais.
    const auto pluginLatencyNumSamples = 0;

    // Patch local Objekat — the audio bridge: a container's children may have different ranks (a
    // child that reads a key written by a sibling must run after it), so each rank gets its own
    // combiner. Rank 0 keeps the container's own id, and with every child at rank 0 (every session
    // without a route) the content is the single combiner it always was. The sum equalises the
    // combiners exactly as one combiner padded its lanes. senderPluginLists keeps the FULL list.
    // @see docs/plan_sidechain.md §5.5, §5.6
    auto contentNode = [&]() -> std::unique_ptr<Node>
    {
        std::map<int, juce::Array<Clip*>> clipsByRank;

        for (auto c : clips)
            clipsByRank[(int) c->state.getProperty (objbridge_ids::rank, 0)].add (c);

        if (clipsByRank.size() == 1 && clipsByRank.begin()->first == 0)
            return createNodeForClips (clip.itemID, clips, trackMuteState, params);

        std::vector<std::unique_ptr<Node>> rankNodes;

        for (auto& [rank, rankClips] : clipsByRank)      // ascending
        {
            const auto rankID = rank == 0 ? clip.itemID
                                          : EditItemID::fromRawID (clip.itemID.getRawID() ^ (0x0B51D6E000000000ull + (uint64_t) rank));

            if (auto n = createNodeForClips (rankID, rankClips, trackMuteState, params))
            {
                applyBridgeRank (*n, rank, rankID, params);
                rankNodes.push_back (std::move (n));
            }
        }

        if (rankNodes.empty())
            return {};

        if (rankNodes.size() == 1)
            return std::move (rankNodes.front());

        return std::make_unique<SummingNode> (std::move (rankNodes));
    }();

    auto node = makeNode<ContainerClipNode> (params.processState,
                                             clip.itemID,
                                             BeatRange (clip.getStartBeat(), clip.getEndBeat()),
                                             clip.getOffsetInBeats(),
                                             clip.getLoopRangeBeats(),
                                             createAuxReturns (senderPluginLists (clips), auxClips,
                                                               std::move (contentNode),
                                                               params),
                                             pluginLatencyNumSamples);
   #endif

    // Plugins
    if (params.includePlugins)
    {
        if (auto pluginList = clip.getPluginList())
        {
            for (auto p : *pluginList)
                p->initialiseFully();

            node = createPluginNodeForList (*pluginList, nullptr, std::move (node), params.processState.playHeadState, params);

        }
    }

    // Create FadeInOutNode
    if (role != ClipRole::launcher)
        return createFadeNodeForClip (clip, clip.getEditTimeRange(), std::move (node), params);

    return node;
}

std::unique_ptr<tracktion::graph::Node> createNodeForClip (Clip& clip, const TrackMuteState& trackMuteState,
                                                           const CreateNodeParams& params, ClipRole role)
{
    if (clip.disabled)
        return {};

    // N.B. This must be checked first as a ContainerClip is an AudioClipBase
    if (auto containerClip = dynamic_cast<ContainerClip*> (&clip))
        return createNodeForContainerClip (*containerClip, trackMuteState, params, role);

    // Patch local Objekat — 0038 : un clip ARA entre comme un autre dans createNodeForClips (donc dans
    // le CombiningNode de la piste OU du container : rangs, latence de lane, head/tail, allowedClips).
    if (auto audioClip = dynamic_cast<AudioClipBase*> (&clip))
        return createNodeForAudioClip (*audioClip, true, params, role);

    if (auto midiClip = dynamic_cast<MidiClip*> (&clip))
        return createNodeForMidiClip (*midiClip, trackMuteState, params, role);

    if (auto stepClip = dynamic_cast<StepClip*> (&clip))
        return createNodeForStepClip (*stepClip, trackMuteState, params, role);

    return {};
}

std::unique_ptr<tracktion::graph::Node> createNodeForClips (EditItemID trackID, const juce::Array<Clip*>& clips,
                                                            const TrackMuteState& trackMuteState, const CreateNodeParams& params)
{
    // If there are no clips, we still need to send note-offs for clips that might have been deleted whilst still playing
    // In the future, this will be removed during the transform stage
    if (clips.size() == 0)
        return std::make_unique<CombiningNode> (trackID, params.processState);

    // Upmix any clip nodes with fewer channels than the max so they sum correctly
    auto matchChannelCounts = [] (std::vector<std::unique_ptr<Node>>& nodes)
    {
        int maxChannels = 0;
        for (auto& n : nodes)
            maxChannels = std::max (maxChannels, n->getNodeProperties().numberOfChannels);

        for (auto& n : nodes)
        {
            auto ch = n->getNodeProperties().numberOfChannels;

            if (ch > 0 && ch < maxChannels)
                n = makeNode<ChannelRemappingNode> (std::move (n), ChannelMap::conversion (ch, maxChannels));
        }
    };

    // Patch local Objekat — plus de repli vers un SummingNode quand un clip a de la latence.
    //
    // Le motif d'origine était que le CombiningNode ne traite pas ses entrées en continu, donc
    // que les FIFOs de latence n'étaient jamais purgées. Mais il condamnait TOUTE la piste au
    // traitement continu pour un seul clip — et sur cette branche c'est précisément la paresse
    // du CombiningNode qui fait tenir 1000 groupes dont un seul joue.
    //
    // Trois pièces remplacent ce repli :
    //   1. getHead()/getTail() répondent au grief là où il se pose : pendant le pré-roll de L,
    //      le clip pousse du silence dans sa chaîne, ce qui chasse le résidu de l'activation
    //      précédente ; la queue (2L) draine le reste ;
    //   2. le CombiningNode remonte la plus grande latence de ses entrées, et la PDC globale
    //      aligne la piste comme n'importe quelle autre ;
    //   3. `laneLatency` ci-dessous égalise les entrées entre elles — sans quoi, la piste étant
    //      décalée de L en aval, un clip SANS plugin à latence sortirait L trop tôt.
    // @see Clip::getHead, Clip::compensatesOwnPluginLatency
    //
    // Patch local Objekat — l'égalisation se calcule sur les latences de NŒUD, plus sur
    // `Clip::getPluginLatencySeconds()`.
    //
    // Ce dernier ne compte que la plugin-list DU CLIP. C'était exact tant qu'un clip n'était
    // qu'une source et sa chaîne ; ça ne l'est plus depuis qu'un `ContainerClip` déclare aussi
    // la latence de son CONTENU (`ContainerClipNode::nodeProperties = input->getNodeProperties()`).
    // Un groupe qui contenait un plugin à latence n'était donc pas égalisé avec ses frères de
    // piste : le `CombiningNode` remontait bien le max RÉEL, si bien que le frère sans latence
    // sortait en avance de cette latence-là (1000 samples ≈ 21 ms à 48 kHz, audible sur un
    // transitoire). Le nœud est la seule source de vérité — c'est lui qui retarde.
    struct ClipNodeEntry
    {
        Clip* clip = nullptr;
        std::unique_ptr<Node> node;
        int latencyNumSamples = 0;
        TimeRange timeRange;
        bool ignoreLatency = false;
    };

    // Les nœuds d'abord : leur latence ne se connaît qu'une fois bâtis.
    std::vector<ClipNodeEntry> clipEntries;

    for (auto clip : clips)
        if (params.allowedClips == nullptr || params.allowedClips->contains (clip))
            if (auto clipNode = createNodeForClip (*clip, trackMuteState, params, ClipRole::arranger))
            {
                const auto latency = clipNode->getNodeProperties().latencyNumSamples;
                // Patch local Objekat — invariant I1 of the audio bridge: no node reads ahead, so a tap's
                // age is the true age of what it recorded. A clip that compensated its own latency
                // (reading its material early) would make a tap on its chain report an age too high.
                // @see docs/plan_sidechain.md §4.7
               #if JUCE_DEBUG
                if (params.bridgeBuild != nullptr && clip->compensatesOwnPluginLatency())
                    if (auto* chain = clip->getPluginList())
                        for (auto p : *chain)
                            if (dynamic_cast<BridgeTapSource*> (p) != nullptr)
                            {
                                DBG ("[BRIDGE] read-ahead clip carries a tap: its age is wrong");
                                jassertfalse;
                            }
               #endif

                clipEntries.push_back ({ clip, std::move (clipNode), latency, {}, clip->compensatesOwnPluginLatency() });
            }

    int laneLatencyNumSamples = 0;

    for (auto& e : clipEntries)
        if (! e.ignoreLatency)
            laneLatencyNumSamples = std::max (laneLatencyNumSamples, e.latencyNumSamples);

    const auto laneLatencySeconds = laneLatencyNumSamples / params.sampleRate;

    // Complète la chaîne d'un clip pour qu'elle retarde d'autant que la plus lente de la lane,
    // et élargit sa fenêtre d'activation de ce même montant : le rembourrage a lui aussi une
    // FIFO à remplir avant le clip et à vider après.
    for (auto& e : clipEntries)
    {
        auto& clip = *e.clip;
        auto timeRange = clip.getPosition().time;
        auto head = clip.getHead();
        auto tail = clip.getTail();

        if (laneLatencyNumSamples > 0 && ! e.ignoreLatency)
        {
            if (const auto padSamples = laneLatencyNumSamples - e.latencyNumSamples; padSamples > 0)
                e.node = makeNode<tracktion::graph::LatencyNode> (std::move (e.node), padSamples);

            head = std::max (head, TimeDuration::fromSeconds (laneLatencySeconds));
            tail = std::max (tail, TimeDuration::fromSeconds (laneLatencySeconds * 2.0));
        }

        // Bâillon d'amorçage : la sortie de la chaîne ne vaut qu'après autant d'échantillons de
        // traitement contigu que sa latence. Avant ça, c'est la queue de l'activation
        // précédente qui sort. @see LatencyPrimingNode
        //
        // Après rembourrage, toute branche non auto-compensée retarde exactement de la latence
        // de la lane : c'est ça qu'il faut amorcer. Une branche auto-compensée, elle, ne déclare
        // rien — on ne peut lire ce qu'elle retarde que sur le modèle.
        const auto primeSamples = e.ignoreLatency
                                    ? juce::roundToInt (clip.getPluginLatencySeconds() * params.sampleRate)
                                    : std::max (laneLatencyNumSamples, e.latencyNumSamples);

        if (primeSamples > 0)
            e.node = makeNode<LatencyPrimingNode> (std::move (e.node), params.processState, primeSamples);

        e.timeRange = timeRange.withStart (timeRange.getStart() - head)
                               .withEnd (timeRange.getEnd() + tail);
    }

    // Extract nodes, match channels, then put back
    std::vector<std::unique_ptr<Node>> nodeVec;
    for (auto& e : clipEntries)
        nodeVec.push_back (std::move (e.node));

    matchChannelCounts (nodeVec);

    for (size_t i = 0; i < clipEntries.size(); ++i)
        clipEntries[i].node = std::move (nodeVec[i]);

    auto combiner = std::make_unique<CombiningNode> (trackID, params.processState);

    for (auto& e : clipEntries)
        combiner->addInput (std::move (e.node), e.timeRange, e.ignoreLatency);

    return combiner;
}

std::vector<std::unique_ptr<SlotControlNode>> createNodeForLauncherClips (const ClipSlotList& slotList,
                                                                          const TrackMuteState& trackMuteState, const CreateNodeParams& params)
{
    std::vector<std::unique_ptr<SlotControlNode>> nodes;

    for (auto slot : slotList.getClipSlots())
    {
        auto clip = slot->getClip();

        if (! clip)
            continue;

        if (params.allowedClips == nullptr || params.allowedClips->contains (clip))
        {
            if (auto clipNode = createNodeForClip (*clip, trackMuteState, params, ClipRole::launcher))
            {
                std::shared_ptr<LaunchHandle> launchHandle;

                if (auto acb = dynamic_cast<AudioClipBase*> (clip))
                    launchHandle = acb->getLaunchHandle();
                else if (auto mc = dynamic_cast<MidiClip*> (clip))
                    launchHandle = mc->getLaunchHandle();
                else if (auto sc = dynamic_cast<StepClip*> (clip))
                    launchHandle = sc->getLaunchHandle();
                else
                    assert (false);

                std::optional<BeatDuration> clipDuration = clip->isLooping() ? std::optional<BeatDuration>()
                                                                             : clip->getLengthInBeats();

                switch (clip->followActionDurationType.get())
                {
                    case Clip::FollowActionDurationType::beats:
                        if (auto afterBeats = clip->followActionBeats.get(); afterBeats > 0_bd)
                            clipDuration = afterBeats;

                        break;
                    case Clip::FollowActionDurationType::loops:
                        if (clip->isLooping())
                        {
                            if (auto afterLoops = clip->followActionNumLoops.get(); afterLoops > 0.0)
                                clipDuration = (clip->getLoopLengthBeats() * afterLoops) - clip->getOffsetInBeats();
                        }
                        else
                        {
                            clipDuration = clip->getLengthInBeats() - clip->getOffsetInBeats();
                        }
                        break;
                }

                auto controlNode = std::make_unique<SlotControlNode> (params.processState,
                                                                      std::move (launchHandle),
                                                                      clipDuration,
                                                                      createFollowAction (*clip),
                                                                      slot->itemID,
                                                                      std::move (clipNode));

                nodes.push_back (std::move (controlNode));
            }
        }
    }

    return nodes;
}


//==============================================================================
std::unique_ptr<tracktion::graph::Node> createNodeForFrozenAudioTrack (AudioTrack& track, tracktion::graph::PlayHeadState& playHeadState, const CreateNodeParams& params)
{
    jassert (! params.forRendering);

    const bool processMidiWhenMuted = track.state.getProperty (IDs::processMidiWhenMuted, false);
    auto trackMuteState = std::make_unique<TrackMuteState> (track, false, processMidiWhenMuted);
    auto node = tracktion::graph::makeNode<WaveNode> (AudioFile (track.edit.engine, TemporaryFileManager::getFreezeFileForTrack (track)),
                                                     TimeRange (TimePosition(), track.getLengthIncludingInputTracks()),
                                                     TimeDuration(), TimeRange(), LiveClipLevel(),
                                                     1.0, track.getChannelConfiguration(), track.getChannelConfiguration(),
                                                     params.processState,
                                                     track.itemID,
                                                     params.forRendering);

    // Plugins
    if (params.includePlugins)
        node = createPluginNodeForTrack (track, *trackMuteState, std::move (node), playHeadState, params);

    if (isSidechainSource (track))
        node = makeNode<SendNode> (std::move (node), getSidechainBusID (track.itemID));

    node = makeNode<TrackMutingNode> (std::move (trackMuteState), std::move (node), false);

    return node;
}

// Patch local Objekat — 0038 : n'est plus appelée (les clips ARA passent par createNodeForClips).
[[maybe_unused]] std::unique_ptr<tracktion::graph::Node> createARAClipsNode (const juce::Array<Clip*>& clips, const TrackMuteState&, const CreateNodeParams& params)
{
    juce::Array<AudioClipBase*> araClips;

    for (auto clip : clips)
        if (params.allowedClips == nullptr || params.allowedClips->contains (clip))
            if (auto acb = dynamic_cast<AudioClipBase*> (clip))
                if (acb->isUsingARA())
                    araClips.add (acb);

    if (araClips.size() == 0)
        return {};

    std::vector<std::unique_ptr<Node>> nodes;

    for (auto araClip : araClips)
        if (auto araNode = createNodeForAudioClip (*araClip, true, params, ClipRole::arranger))
            nodes.push_back (createFadeNodeForClip (*araClip, araClip->getEditTimeRange(), std::move (araNode), params));

    if (nodes.size() == 1)
        return std::move (nodes.front());

    return std::make_unique<SummingNode> (std::move (nodes));
}

std::unique_ptr<tracktion::graph::Node> createClipsNode (AudioTrack& at, const TrackMuteState& trackMuteState,
                                                         const CreateNodeParams& params)
{
    std::vector<std::unique_ptr<Node>> arrangerNodes;
    const auto trackID = at.itemID;
    const auto& clips = at.getClips();

    if (auto clipsNode = createNodeForClips (trackID, clips, trackMuteState, params))
    {
        // Patch local Objekat — the pool track's rank. @see applyBridgeRank
        applyBridgeRank (*clipsNode, (int) at.state.getProperty (objbridge_ids::rank, 0), trackID, params);
        arrangerNodes.push_back (std::move (clipsNode));
    }

    // Patch local Objekat — 0038 : plus de chemin ARA à part. createNodeForClips joue déjà les clips
    // ARA (createNodeForClip → createNodeForAudioClip includeARA = true) ; un second appel les jouerait
    // deux fois. @see createARAClipsNode

    if (! params.allowClipSlots)
    {
        if (arrangerNodes.empty())
            return {};

        if (arrangerNodes.size() == 1)
            return std::move (arrangerNodes.front());

        return  std::make_unique<SummingNode> (std::move (arrangerNodes));
    }

    auto launcherNodes = createNodeForLauncherClips (at.getClipSlotList(), trackMuteState, params);

    if (arrangerNodes.empty() && launcherNodes.empty())
        return {};

    std::unique_ptr<Node> arrangerNode;

    if (arrangerNodes.size() == 1)
        arrangerNode = std::move (arrangerNodes.front());
    else if (arrangerNodes.size() > 1)
        arrangerNode = std::make_unique<SummingNode> (std::move (arrangerNodes));

    return makeNode<ArrangerLauncherSwitchingNode> (params.processState, at, std::move (arrangerNode), std::move (launcherNodes));
}

std::unique_ptr<tracktion::graph::Node> createLiveInputNodeForDevice (InputDeviceInstance& inputDeviceInstance, tracktion::graph::PlayHeadState& playHeadState,
                                                                      const CreateNodeParams& params, EditItemID trackID)
{
    if (auto midiDevice = dynamic_cast<MidiInputDevice*> (&inputDeviceInstance.getInputDevice()))
    {
        if (midiDevice->isTrackDevice())
            if (auto sourceTrack = getTrackContainingTrackDevice (inputDeviceInstance.edit, *midiDevice))
                return makeNode<TrackMidiInputDeviceNode> (*midiDevice, makeNode<ReturnNode> (getMidiInputDeviceBusID (sourceTrack->itemID)), params.processState,
                                                           shouldMonitorTrackDevice (inputDeviceInstance));

        if (HostedAudioDeviceInterface::isHostedMidiInputDevice (*midiDevice))
            return makeNode<HostedMidiInputDeviceNode> (inputDeviceInstance, *midiDevice, playHeadState, params.processState);

        return makeNode<MidiInputDeviceNode> (inputDeviceInstance, *midiDevice, midiDevice->getMPESourceID(), playHeadState, trackID);
    }
    else if (auto waveDevice = dynamic_cast<WaveInputDevice*> (&inputDeviceInstance.getInputDevice()))
    {
        if (waveDevice->isTrackDevice())
            if (auto sourceTrack = getTrackContainingTrackDevice (inputDeviceInstance.edit, *waveDevice))
            {
                waveDevice->setChannelConfiguration (sourceTrack->getChannelConfiguration());
                return makeNode<TrackWaveInputDeviceNode> (params.processState,
                                                           *waveDevice,
                                                           makeNode<ReturnNode> (getWaveInputDeviceBusID (sourceTrack->itemID)),
                                                           shouldMonitorTrackDevice (inputDeviceInstance));
            }

        return makeNode<WaveInputDeviceNode> (inputDeviceInstance, *waveDevice,
                                              waveDevice->getChannelSet());
    }

    return {};
}

std::unique_ptr<tracktion::graph::Node> createLiveInputsNode (AudioTrack& track, tracktion::graph::PlayHeadState& playHeadState, const CreateNodeParams& params)
{
    std::vector<std::unique_ptr<tracktion::graph::Node>> nodes;

    if (! params.forRendering)
        if (auto context = track.edit.getCurrentPlaybackContext())
            for (auto in : context->getAllInputs())
                if ((in->isLivePlayEnabled (track) || in->getInputDevice().isTrackDevice()) && in->getTargets().contains (track.itemID))
                    if (auto node = createLiveInputNodeForDevice (*in, playHeadState, params, track.itemID))
                        nodes.push_back (std::move (node));

    if (nodes.empty())
        return {};

    if (nodes.size() == 1)
        return std::move (nodes.front());

    return std::make_unique<SummingNode> (std::move (nodes));
}

std::unique_ptr<tracktion::graph::Node> createSidechainInputNodeForPlugin (Plugin& plugin, std::unique_ptr<Node> node,
                                                                           const CreateNodeParams& params)
{
    const auto sidechainSourceID = plugin.getSidechainSourceID();
    const bool usesSidechain = ! plugin.isMissing() && sidechainSourceID.isValid();

    if (! usesSidechain)
        return node;

    // This is complicated because the first two source channels will always be the track the plugin is on
    // Any additional channels will be from the sidechain source track
    // So we really have two channel maps, one from the plugin's track to the plugin and one from the sidechain track to the plugin
    ChannelMap directChannelMap, sidechainChannelMap;

    // Wires always use two source channels for the plugin's track, regardless of the
    // track's actual channel count (see guessSidechainRouting() and the sidechain editor UI)
    constexpr int trackChannels = 2;

    for (int i = 0; i < plugin.getNumWires(); ++i)
    {
        if (auto w = plugin.getWire (i))
        {
            const int sourceIndex = w->sourceChannelIndex;
            const int destIndex = w->destChannelIndex;

            if (sourceIndex < trackChannels)
                directChannelMap.entries.emplace_back (sourceIndex, destIndex);
            else
                sidechainChannelMap.entries.emplace_back (sourceIndex - trackChannels, destIndex);
        }
    }

    if (directChannelMap.isEmpty() && sidechainChannelMap.isEmpty())
        return node;

    const bool hasDirectChannels = ! directChannelMap.isEmpty();
    auto directInput = std::move (node);

    if (! directChannelMap.isIdentity())
        directInput = makeNode<ChannelRemappingNode> (std::move (directInput), std::move (directChannelMap));

    std::unique_ptr<Node> sidechainInput;

    // Patch local Objekat — the audio bridge. A key named by a tap plugin (not by a track) is read
    // from the tap's ring instead of through a ReturnNode: the ring crosses the containers' boundaries,
    // which an edge cannot. L_d is the age of the DIRECT input, read AFTER its channel pre-conversion
    // (a ChannelRemappingNode keeps latency). Anything else falls through to the native path, so a
    // sidechain naming a track still works. @see docs/plan_sidechain.md §4.2, §5.6
    if (params.bridgeBuild != nullptr && params.bridgeBuild->hasTap (sidechainSourceID))
    {
        // A reader alone in a linear TimedNode chain would share one buffer with its own
        // ChannelRemappingNode, whose explicit mapping ADDS into its destination: the key would be
        // summed into itself. The app never produces it (guessSidechainRouting wires 0->0, 1->1).
        if (! hasDirectChannels)
        {
            DBG ("[BRIDGE] reader without a direct channel: refused");
            return directInput;
        }

        const auto readerIndex = params.bridgeBuild->registerReader (sidechainSourceID, plugin.itemID, &plugin,
                                                                     BridgeBuild::Consumer::sidechain,
                                                                     directInput->getNodeProperties().latencyNumSamples,
                                                                     plugin.itemID.getRawID());
        const int readerRank = (int) plugin.state.getProperty (objbridge_ids::rank, 0);

        sidechainInput = makeNode<BridgeReaderNode> (params.processState, params.bridgeBuild, readerIndex, readerRank,
                                                     hash ((size_t) 0x0B71D6E5, plugin.itemID.getRawID()));
    }
    else
    {
        sidechainInput = makeNode<ReturnNode> (getSidechainBusID (sidechainSourceID),
                                               std::make_optional (static_cast<size_t> (plugin.itemID.getRawID())));
    }

    sidechainInput = makeNode<ChannelRemappingNode> (std::move (sidechainInput), std::move (sidechainChannelMap));

    if (! hasDirectChannels)
        return sidechainInput;

    auto sumNode = makeSummingNode ({ directInput.release(), sidechainInput.release() });

    return sumNode;
}

std::unique_ptr<tracktion::graph::Node> createNodeForPlugin (Plugin& plugin, const TrackMuteState* trackMuteState, std::unique_ptr<Node> node,
                                                             const CreateNodeParams& params)
{
    jassert (node != nullptr);

    if (plugin.isDisabled())
        return node;

    if (auto ep = dynamic_cast<ExternalPlugin*> (&plugin))
        if (ep->isInitialisingAsync())
            return node;

    if (! plugin.isEnabled() && ! params.includeBypassedPlugins)
        return node;

    // Query the incoming channel count from the input node
    const int incomingChannels = node->getNodeProperties().numberOfChannels;

    // Query how many channels the plugin can handle
    const auto busses = plugin.getBusses();
    const auto mainInputBus = busses.inputs.empty() ? ChannelConfiguration{} : busses.inputs.front();
    const int pluginInputChannels = mainInputBus.getNumChannels();

    // For pass-through plugins (e.g. LevelMeter, VolumeAndPan), the declared input config
    // may be stereo but the plugin can handle any channel count. Use the actual output count
    // for the incoming channels to determine the effective channel capacity.
    const int effectivePluginChannels = std::max (pluginInputChannels,
                                                  plugin.getNumOutputChannelsGivenInputs (incomingChannels));

    // Determine maxNumChannels for the PluginNode
    // If the plugin is on a track/clip without sidechain, use the effective channel count.
    // If a sidechain is connected we leave it unlimited so the plugin can still process
    // the wider input buffer (main + sidechain channels). The sidechain channels are
    // then trimmed off the output below by wrapping the PluginNode in a ChannelRemappingNode.
    int maxNumChannels = -1;
    const bool usesSidechain = plugin.getSidechainSourceID().isValid();

    if (! usesSidechain)
        // OBJEKAT — `Clip::isClipState (parent)` plutôt que `getOwnerClip() != nullptr` : on ne
        // demande ici QUE « ce plugin est-il sur un clip ? », et getOwnerClip répond en cherchant
        // le clip dans tout l'Edit (findClipForID, O(N)) — O(N²) sur la reconstruction, 75 % de
        // son temps sur PERREO WUB 2 (la chaîne de chaque objet vit sur la plugin-list de son
        // clip). Le test sur l'arbre de valeurs donne la même réponse en O(1).
        if (plugin.getOwnerTrack() != nullptr || Clip::isClipState (plugin.state.getParent()))
            maxNumChannels = effectivePluginChannels;

    // If the input has fewer channels than the plugin expects, pre-convert (e.g. mono→stereo)
    // so the plugin receives the right number of channels. This must happen before the
    // sidechain input is summed in: sidechain wires assume two track channels, and a
    // conversion applied after the sum would squash the sidechain channels back down
    if (incomingChannels < pluginInputChannels
        && incomingChannels > 0
        && pluginInputChannels > 0)
    {
        node = tracktion::graph::makeNode<ChannelRemappingNode> (std::move (node),
                                                                 ChannelMap::conversion (incomingChannels, pluginInputChannels));
    }

    node = createSidechainInputNodeForPlugin (plugin, std::move (node), params);

    // Create the PluginNode
    auto pluginNode = tracktion::graph::makeNode<PluginNode> (std::move (node),
                                                              plugin,
                                                              params.sampleRate, params.blockSize,
                                                              trackMuteState, params.processState,
                                                              params.forRendering, params.includeBypassedPlugins,
                                                              maxNumChannels);

    // If input has more channels than the plugin handles, wrap output in passthrough mode
    // so extra channels pass through unprocessed
    if (incomingChannels > effectivePluginChannels
        && incomingChannels > 0
        && effectivePluginChannels > 0)
    {
        return tracktion::graph::makeNode<ChannelRemappingNode> (std::move (pluginNode),
                                                                 ChannelConfiguration::discreteChannels (incomingChannels),
                                                                 mainInputBus);
    }

    // When a sidechain is connected, the plugin processes a wider buffer that
    // includes the sidechain channels as inputs. Trim those off the output so
    // only the main-bus channels propagate to downstream nodes.
    if (usesSidechain && effectivePluginChannels > 0)
        return tracktion::graph::makeNode<ChannelRemappingNode> (std::move (pluginNode),
                                                                 ChannelMap::identity (effectivePluginChannels));

    return pluginNode;
}

std::unique_ptr<tracktion::graph::Node> createNodeForRackInstance (RackInstance& rackInstance, std::unique_ptr<Node> node,
                                                                   ProcessState& processState, SampleRateAndBlockSize sampleRateAndBlockSize)
{
    jassert (node != nullptr);

    if (! rackInstance.isEnabled())
        return node;

    const auto rackInputID = getRackInputBusID (rackInstance.rackTypeID);
    const auto rackOutputID = getRackOutputBusID (rackInstance.rackTypeID);

    // The input to the instance is referenced by the dry signal path
    auto* inputNode = node.get();

    // Expand incoming channels to match the Rack's expected input count (e.g. mono→stereo),
    // mirroring the same logic applied to regular plugins in createNodeForPlugin().
    const int incomingChannels = node->getNodeProperties().numberOfChannels;
    const int rackInputChannels = rackInstance.getNumInputChannels();

    if (incomingChannels < rackInputChannels
        && incomingChannels > 0
        && rackInputChannels > 0)
    {
        node = tracktion::graph::makeNode<ChannelRemappingNode> (std::move (node),
                                                                 ChannelMap::conversion (incomingChannels, rackInputChannels));
    }

    // Send — use numInputChannels
    // N.B. the channel indices from the RackInstance start at 1 so we need to subtract this to get a 0-indexed channel
    RackInstanceNode::ChannelMap sendChannelMap;
    const int numSendChannels = rackInstance.getNumInputChannels();

    for (int i = 0; i < numSendChannels; ++i)
    {
        if (i < rackInstance.getNumChannelMappings())
            sendChannelMap.push_back ({ i, rackInstance.getInputMapping (i) - 1,
                                        rackInstance.getInputGainParam (i) });
    }

    node = makeNode<RackInstanceNode> (rackInstance, std::move (node), std::move (sendChannelMap), processState, sampleRateAndBlockSize);
    node = makeNode<SendNode> (std::move (node), rackInputID);
    node = makeNode<ReturnNode> (makeNode<SinkNode> (std::move (node)), rackOutputID);

    // Return — use numOutputChannels
    RackInstanceNode::ChannelMap returnChannelMap;
    const int numReturnChannels = rackInstance.getNumOutputChannels();

    for (int i = 0; i < numReturnChannels; ++i)
    {
        if (i < rackInstance.getNumChannelMappings())
            returnChannelMap.push_back ({ rackInstance.getOutputMapping (i) - 1, i,
                                          rackInstance.getOutputGainParam (i) });
    }

    node = makeNode<RackInstanceNode> (rackInstance, std::move (node), std::move (returnChannelMap), processState, sampleRateAndBlockSize);

    return makeNode<RackReturnNode> (std::move (node),
                                     [wetGain = rackInstance.wetGain] { return wetGain->getCurrentValue(); },
                                     inputNode,
                                     [dryGain = rackInstance.dryGain] { return dryGain->getCurrentValue(); });
}

// Patch local Objekat — déplie un bloc de plugins parallèles en SummingNode { branche… }.
// Chaque branche part de la MÊME entrée, partagée par un ConnectedNode : le nœud d'entrée est
// donc détenu par des shared_ptr, pas par la chaîne unique_ptr. Le DFS dédupliqué de
// CombiningNode::TimedNode ne le processe qu'une fois. @see ParallelPluginBlock
std::unique_ptr<tracktion::graph::Node> createNodeForParallelBlock (ParallelPluginBlock& block, Plugin& plugin,
                                                                    const TrackMuteState* trackMuteState,
                                                                    std::unique_ptr<Node> input,
                                                                    tracktion::graph::PlayHeadState& playHeadState,
                                                                    const CreateNodeParams& params)
{
    auto branches = block.getParallelBranches();

    branches.erase (std::remove (branches.begin(), branches.end(), nullptr), branches.end());

    // Aucune branche : le bloc est transparent. Une seule : inutile de payer un ConnectedNode
    // et un SummingNode pour ça, on la met en série.
    if (branches.empty())
        return input;

    if (branches.size() == 1)
        return createPluginNodeForList (*branches.front(), trackMuteState, std::move (input), playHeadState, params);

    const auto inputProps = input->getNodeProperties();
    std::shared_ptr<Node> sharedInput (std::move (input));
    std::vector<std::unique_ptr<Node>> branchNodes;

    for (size_t i = 0; i < branches.size(); ++i)
    {
        size_t nodeID = 0;
        hash_combine (nodeID, plugin.itemID.getRawID());
        hash_combine (nodeID, i);

        auto connected = std::make_unique<tracktion::graph::ConnectedNode> (nodeID);

        for (int c = 0; c < inputProps.numberOfChannels; ++c)
            connected->addAudioConnection (sharedInput, { c, c });

        if (inputProps.hasMidi)
            connected->addMidiConnection (sharedInput);

        branchNodes.push_back (createPluginNodeForList (*branches[i], trackMuteState,
                                                        std::move (connected), playHeadState, params));
    }

    return makeNode<tracktion::graph::SummingNode> (std::move (branchNodes));
}

std::unique_ptr<tracktion::graph::Node> createPluginNodeForList (PluginList& list, const TrackMuteState* trackMuteState, std::unique_ptr<Node> node,
                                                                 tracktion::graph::PlayHeadState& playHeadState, const CreateNodeParams& params)
{
    for (auto p : list)
    {
        if (! params.forRendering && p->isFrozen())
            continue;

        // Patch local Objekat — un envoi d'aux relève ici la latence accumulée à son point de
        // prélèvement : c'est le seul endroit où on la connaisse, et c'est le nombre exact du
        // graphe, pas une reconstitution côté modèle (le contenu d'un container, les FIFOs
        // d'un bloc parallèle et les plugins précédents y sont déjà tous comptés).
        // @see ContainerAuxSend::setTapLatencyNumSamples
        if (auto auxSend = dynamic_cast<ContainerAuxSend*> (p))
            auxSend->setTapLatencyNumSamples (node != nullptr ? node->getNodeProperties().latencyNumSamples : 0);

        // Patch local Objekat — a bridge tap: the plugin is never processed, a BridgeTapNode takes its
        // place. It records the age of its input (the tap's age, which the readers align on) on the
        // plugin, for the NEXT build to start from, and in this pass's registry for the readers of
        // THIS pass. @see docs/plan_sidechain.md §4.3, §5.6
        if (auto tap = dynamic_cast<BridgeTapSource*> (p))
        {
            if (node == nullptr)
                continue;

            const int age = node->getNodeProperties().latencyNumSamples;
            const int rank = (int) p->state.getProperty (objbridge_ids::rank, -1);

            tap->cachedAgeNumSamples = age;
            tap->cachedAgeSampleRate = params.sampleRate;

            if (params.bridgeBuild != nullptr)
                params.bridgeBuild->registerTap (*p, *tap, age, rank);

            node = makeNode<BridgeTapNode> (params.processState, std::move (node), p, *tap, params.bridgeBuild, rank);
        }
        else if (auto meterPlugin = dynamic_cast<LevelMeterPlugin*> (p))
        {
            node = makeNode<LevelMeasurerProcessingNode> (std::move (node), *meterPlugin);
        }
        else if (auto sendPlugin = dynamic_cast<AuxSendPlugin*> (p))
        {
            if (sendPlugin->isEnabled())
                node = makeNode<AuxSendNode> (std::move (node), sendPlugin->busNumber,
                                              SampleRateAndBlockSize { params.sampleRate, params.blockSize },
                                              *sendPlugin,
                                              playHeadState, trackMuteState,
                                              list.getEdit().engine.getEngineBehaviour().shouldProcessAuxSendWhenTrackIsMuted (*sendPlugin));
        }
        else if (auto returnPlugin = dynamic_cast<AuxReturnPlugin*> (p))
        {
            if (returnPlugin->isEnabled())
                node = makeNode<ReturnNode> (std::move (node), returnPlugin->busNumber);
        }
        else if (auto parallelBlock = dynamic_cast<ParallelPluginBlock*> (p))
        {
            if (p->isEnabled())
                node = createNodeForParallelBlock (*parallelBlock, *p, trackMuteState,
                                                   std::move (node), playHeadState, params);
        }
        else if (auto rackInstance = dynamic_cast<RackInstance*> (p))
        {
            node = createNodeForRackInstance (*rackInstance, std::move (node), params.processState,
                                              SampleRateAndBlockSize { params.sampleRate, params.blockSize });
        }
        else if (auto insertPlugin = dynamic_cast<InsertPlugin*> (p))
        {
            if (! insertPlugin->isEnabled())
                continue;

            if (auto insertReturnNode = createInsertReturnNode (*insertPlugin, playHeadState, params))
                node = makeNode<InsertNode> (std::move (node), *insertPlugin, std::move (insertReturnNode),
                                             SampleRateAndBlockSize { params.sampleRate, params.blockSize });
        }
        else
        {
            node = createNodeForPlugin (*p, trackMuteState, std::move (node), params);
        }
    }

    return node;
}

std::unique_ptr<tracktion::graph::Node> createModifierNodeForList (ModifierList* list,
                                                                   Modifier::ProcessingPosition position,
                                                                   TrackMuteState* trackMuteState,
                                                                   std::unique_ptr<Node> node,
                                                                   tracktion::graph::PlayHeadState& playHeadState,
                                                                   const CreateNodeParams& params)
{
    if (list != nullptr)
    {
        for (auto& modifier : list->getModifiers())
        {
            if (modifier->getProcessingPosition() != position)
                continue;

            node = makeNode<ModifierNode> (std::move (node), modifier, params.sampleRate, params.blockSize,
                                           trackMuteState, playHeadState, params.forRendering,
                                           ModifierNode::ClearOutputs::no);
        }
    }

    return node;
}

std::unique_ptr<tracktion::graph::Node> createPluginNodeForTrack (Track& t,
                                                                  TrackMuteState& trackMuteState,
                                                                  std::unique_ptr<Node> node,
                                                                  tracktion::graph::PlayHeadState& playHeadState,
                                                                  const CreateNodeParams& params)
{
    node = createModifierNodeForList (t.getModifierList(), Modifier::ProcessingPosition::preFX,
                                      &trackMuteState, std::move (node), playHeadState, params);

    if (params.includePlugins)
        node = createPluginNodeForList (t.pluginList, &trackMuteState, std::move (node), playHeadState, params);

    node = createModifierNodeForList (t.getModifierList(), Modifier::ProcessingPosition::postFX,
                                      &trackMuteState, std::move (node), playHeadState, params);

    return node;
}

juce::Array<Track*> getDirectInputTracks (AudioTrack& at)
{
    juce::Array<Track*> inputTracks;

    for (auto track : getAudioTracks (at.edit))
        if (! track->isPartOfSubmix() && track != &at && track->getOutput().outputsToDestTrack (at))
            inputTracks.add (track);

    for (auto track : getTracksOfType<FolderTrack> (at.edit, true))
        if (! track->isPartOfSubmix() && track->getOutput() != nullptr && track->getOutput()->outputsToDestTrack (at))
            inputTracks.add (track);

    return inputTracks;
}

std::unique_ptr<tracktion::graph::Node> createTrackCompNode (AudioTrack& at, std::unique_ptr<tracktion::graph::Node> node, const CreateNodeParams& params)
{
    if (at.getCompGroup() == -1)
        return node;

    if (auto tc = at.edit.getTrackCompManager().getTrackComp (&at))
    {
        const auto crossfadeTimeMs = at.edit.engine.getPropertyStorage().getProperty (SettingID::compCrossfadeMs, 20.0);
        const auto crossfadeTime = TimeDuration::fromSeconds (static_cast<double> (crossfadeTimeMs) / 1000.0);

        const auto nonMuteTimes = tc->getNonMuteTimes (at, crossfadeTime);
        const auto muteTimes = TrackCompManager::TrackComp::getMuteTimes (nonMuteTimes);

        if (muteTimes.isEmpty())
            return node;

        node = makeNode<TimedMutingNode> (std::move (node), std::move (muteTimes), params.processState.playHeadState);

        for (auto r : nonMuteTimes)
        {
            auto fadeIn = r.withLength (crossfadeTime) - 0.0001s;
            auto fadeOut = fadeIn.movedToEndAt (r.getEnd() + 0.0001s);

            if (! (fadeIn.isEmpty() && fadeOut.isEmpty()))
                node = makeNode<FadeInOutNode> (std::move (node),
                                                params.processState,
                                                TimeRange { fadeIn.getStart(), fadeIn.getEnd() },
                                                TimeRange { fadeOut.getStart(), fadeOut.getEnd() },
                                                AudioFadeCurve::convex,
                                                AudioFadeCurve::convex, false);
        }
    }

    return node;
}

std::unique_ptr<tracktion::graph::Node> createNodeForAudioTrack (AudioTrack& at, const CreateNodeParams& params)
{
    CRASH_TRACER
    jassert (at.isProcessing (false));
    auto& playHeadState = params.processState.playHeadState;

    if (! params.forRendering && at.isFrozen (AudioTrack::individualFreeze))
        return createNodeForFrozenAudioTrack (at, playHeadState, params);

    auto inputTracks = getDirectInputTracks (at);
    const bool processMidiWhenMuted = at.state.getProperty (IDs::processMidiWhenMuted, false);
    auto clipsMuteState = std::make_unique<TrackMuteState> (at, true, processMidiWhenMuted);
    auto trackMuteState = std::make_unique<TrackMuteState> (at, false, processMidiWhenMuted);

    if (params.tracksToProcessWhileMuted.contains (at.itemID))
    {
        clipsMuteState->setKeepProcessingWhileMuted();
        trackMuteState->setKeepProcessingWhileMuted();
    }

    std::unique_ptr<Node> node = createClipsNode (at, *clipsMuteState, params);
    if (node)
    {
        // When recording, clips should be muted but the plugin should still be audible so use two muting Nodes
        node = makeNode<TrackMutingNode> (std::move (clipsMuteState), std::move (node), true);

        // If we have any inputs, we need a third muting Node to fully block the clips whilst recording
        // The above muting node will still let clips sound if they are going to a aux send, sidechain etc.
        if (at.edit.engine.getEngineBehaviour().shouldProcessMutedTracks()
            && ! at.edit.getEditInputDevices().getDevicesForTargetTrack (at).isEmpty())
        {
            node = makeNode<TrackMutingNode> (std::make_unique<TrackMuteState> (at, true, processMidiWhenMuted),
                                              std::move (node), false);
        }

        node = createTrackCompNode (at, std::move (node), params);
    }

    auto liveInputNode = createLiveInputsNode (at, playHeadState, params);

    // OBJEKAT — LiveMidiOutputNode n'existe que pour appeler
    // Listener::recordedMidiMessageSentToPlugins sur les écouteurs de la piste.
    // Or LiveMidiInjectingNode s'inscrit lui-même comme écouteur (dans son
    // constructeur) et implémente ce rappel par un corps VIDE. Sur une piste sans
    // véritable écouteur, on créait donc un nœud par piste dont l'unique
    // destinataire est un autre nœud du graphe, qui jette le message.
    //
    // Pire, la condition se mordait la queue : au premier build il n'y a pas
    // encore d'écouteur, les LiveMidiInjectingNode s'inscrivent, et la
    // reconstruction SUIVANTE ajoute un LiveMidiOutputNode par piste — d'où une
    // reconstruction complète supplémentaire après chaque geste d'édition.
    // Mesuré : 348 nœuds sur 3765 (9 %) + une reconstruction sur deux.
    //
    // On ne compte donc que les écouteurs qui ne sont pas des nœuds du graphe.
    // Dès qu'un vrai écouteur existe (témoin d'activité MIDI, etc.), le nœud est
    // recréé comme avant : le comportement observable est inchangé.
    if (node)
    {
        const auto& trackListeners = at.getListeners().getListeners();

        const bool hasRealListener = std::any_of (trackListeners.begin(), trackListeners.end(),
                                                  [] (auto* l)
                                                  {
                                                      return dynamic_cast<LiveMidiInjectingNode*> (l) == nullptr;
                                                  });

        if (hasRealListener)
            node = makeNode<LiveMidiOutputNode> (at, std::move (node));
    }

    // OBJEKAT — LiveMidiInjectingNode était créé sur TOUTE piste ayant un nœud,
    // alors qu'il ne sert qu'à recevoir du MIDI joué en direct : entrée MIDI
    // surveillée, ou notes-guides envoyées depuis l'interface (piano-roll,
    // audition). Une piste ne portant qu'un objet sonore audio ne peut rien
    // recevoir de tel. Mesuré : 348 nœuds sur 3765 (9 %).
    //
    // Deux portes, larges à dessein : une entrée assignée à la piste, ou un
    // plugin capable de produire du son sans entrée audio (instrument virtuel,
    // la cible des notes-guides). Dès qu'une des deux est vraie, le nœud est
    // créé comme avant.
    if (node)
    {
        const bool hasLiveInput = ! at.edit.getEditInputDevices().getDevicesForTargetTrack (at).isEmpty();

        bool hasInstrument = false;

        if (! hasLiveInput)
            for (auto plugin : at.pluginList)
                if (plugin->producesAudioWhenNoAudioInput())
                {
                    hasInstrument = true;
                    break;
                }

        if (hasLiveInput || hasInstrument)
            node = makeNode<LiveMidiInjectingNode> (at, std::move (node));
    }

    if (node == nullptr && inputTracks.isEmpty() && liveInputNode == nullptr)
    {
        // If there are synths on the track, create a stub Node to feed them
        for (auto plugin : at.pluginList)
        {
            if (plugin->producesAudioWhenNoAudioInput())
            {
                node = makeNode<SilentNode> (2);
                break;
            }
        }

        if (! node)
            return {};
    }

    if (liveInputNode)
    {
        if (node)
        {
            auto sumNode = std::make_unique<SummingNode>();
            sumNode->addInput (std::move (node));
            sumNode->addInput (std::move (liveInputNode));
            node = std::move (sumNode);
        }
        else
        {
            node = std::move (liveInputNode);
        }
    }

    if (! inputTracks.isEmpty())
    {
        auto sumNode = std::make_unique<SummingNode>();

        if (node)
            sumNode->addInput (std::move (node));

        for (auto inputTrack : inputTracks)
            if (auto n = createNodeForTrack (*inputTrack, params))
                sumNode->addInput (std::move (n));

        node = std::move (sumNode);
    }

    node = createPluginNodeForTrack (at, *trackMuteState, std::move (node), playHeadState, params);

    if (isSidechainSource (at))
        node = makeNode<SendNode> (std::move (node), getSidechainBusID (at.itemID));

    node = makeNode<TrackMutingNode> (std::move (trackMuteState), std::move (node), false);

    if (! params.forRendering)
    {
        if (at.getWaveInputDevice().isEnabled())
            node = makeNode<SendNode> (std::move (node), getWaveInputDeviceBusID (at.itemID));

        if (at.getMidiInputDevice().isEnabled())
            node = makeNode<SendNode> (std::move (node), getMidiInputDeviceBusID (at.itemID));
    }

    return node;
}

//==============================================================================
// Patch local Objekat — défini plus bas, à côté de son pendant top-level.
static std::unique_ptr<Node> createSubmixAuxReturns (FolderTrack&, std::unique_ptr<Node>,
                                                     const CreateNodeParams&);

std::unique_ptr<tracktion::graph::Node> createNodeForSubmixTrack (FolderTrack& submixTrack, const CreateNodeParams& params)
{
    CRASH_TRACER
    jassert (submixTrack.isSubmixFolder());

    juce::Array<AudioTrack*> subAudioTracks;
    juce::Array<FolderTrack*> subFolderTracks;

    for (auto t : submixTrack.getAllSubTracks (false))
    {
        if (auto ft = dynamic_cast<AudioTrack*> (t))
            subAudioTracks.add (ft);

        if (auto ft = dynamic_cast<FolderTrack*> (t))
            subFolderTracks.add (ft);
    }

    if (subAudioTracks.isEmpty() && subFolderTracks.isEmpty())
        return {};

    auto sumNode = std::make_unique<tracktion::graph::SummingNode>();
    sumNode->setDoubleProcessingPrecision (submixTrack.edit.engine.getPropertyStorage().getProperty (SettingID::use64Bit, false));

    // Create nodes for any submix tracks
    for (auto ft : subFolderTracks)
    {
        if (params.allowedTracks != nullptr && ! params.allowedTracks->contains (ft))
            continue;

        if (! ft->isProcessing (true))
            continue;

        if (ft->isSubmixFolder())
        {
            if (auto node = createNodeForSubmixTrack (*ft, params))
                sumNode->addInput (std::move (node));
        }
        else
        {
            for (auto at : ft->getAllAudioSubTracks (false))
                if (params.allowedTracks == nullptr || params.allowedTracks->contains (at))
                    if (auto node = createNodeForAudioTrack (*at, params))
                        sumNode->addInput (std::move (node));
        }
    }

    // Then add any audio tracks
    for (auto at : subAudioTracks)
        if (params.allowedTracks == nullptr || params.allowedTracks->contains (at))
            if (at->isProcessing (true))
                if (auto node = createNodeForAudioTrack (*at, params))
                    sumNode->addInput (std::move (node));

    if (sumNode->getDirectInputNodes().empty())
        return {};

    // Finally the effects
    std::unique_ptr<Node> node = std::move (sumNode);

    // Patch local Objekat — les retours des aux de CE stem se somment ici, sur ses pistes filles :
    // en amont de sa chaîne, de son VU et de son mute, pour que le humide soit solidaire du bus.
    node = createSubmixAuxReturns (submixTrack, std::move (node), params);

    auto trackMuteState = std::make_unique<TrackMuteState> (submixTrack, false, false);

    if (params.tracksToProcessWhileMuted.contains (submixTrack.itemID))
        trackMuteState->setKeepProcessingWhileMuted();

    node = createPluginNodeForTrack (submixTrack, *trackMuteState, std::move (node), params.processState.playHeadState, params);

    node = makeNode<TrackMutingNode> (std::move (trackMuteState), std::move (node), false);

    return node;
}

//==============================================================================
std::unique_ptr<tracktion::graph::Node> createNodeForTrack (Track& track, const CreateNodeParams& params)
{
    if (auto t = dynamic_cast<AudioTrack*> (&track))
    {
        if (! t->isProcessing (true))
            return {};

        if (! t->createsOutput())
            return {};

        if (t->isPartOfSubmix() && ! shouldRenderTrackInSubmix (*t, params))
            return {};

        if (! params.forRendering && t->isFrozen (Track::groupFreeze))
            return {};

        return createNodeForAudioTrack (*t, params);
    }

    if (auto t = dynamic_cast<FolderTrack*> (&track))
    {
        if (! t->isSubmixFolder())
            return {};

        if (t->isPartOfSubmix() && ! shouldRenderTrackInSubmix (*t, params))
            return {};

        if (t->getOutput() == nullptr)
            return {};

        return createNodeForSubmixTrack (*t, params);
    }

    return {};
}

//==============================================================================
std::unique_ptr<Node> createNodeForRackType (RackType& rackType, const CreateNodeParams& params)
{
    const auto rackInputID = getRackInputBusID (rackType.itemID);
    const auto rackOutputID = getRackOutputBusID (rackType.itemID);

    auto rackInputNode = makeNode<ReturnNode> (rackInputID);
    auto rackNode = RackNodeBuilder::createRackNode (rackType, params.sampleRate, params.blockSize, std::move (rackInputNode),
                                                     params.processState, params.forRendering);
    auto rackOutputNode = makeNode<SendNode> (std::move (rackNode), rackOutputID);

    return makeNode<SinkNode> (std::move (rackOutputNode));
}

std::vector<std::unique_ptr<Node>> createNodesForRacks (RackTypeList& rackTypeList,
                                                        const CreateNodeParams& params)
{
    std::vector<std::unique_ptr<Node>> nodes;

    for (auto rackType : rackTypeList.getTypes())
        if (getEnabledInstancesForRack (*rackType).size() > 0)
            if (auto rackNode = createNodeForRackType (*rackType, params))
                nodes.push_back (std::move (rackNode));

    return nodes;
}

std::unique_ptr<Node> createRackNode (std::unique_ptr<Node> input,
                                      RackTypeList& rackTypeList,
                                      const CreateNodeParams& params)
{
    // Finally add the RackType Nodes
    auto rackNodes = createNodesForRacks (rackTypeList, params);

    if (rackNodes.empty())
        return input;

    auto sumNode = std::make_unique<SummingNode> (std::move (rackNodes));
    sumNode->addInput (std::move (input));
    input = std::move (sumNode);

    return input;
}

//==============================================================================
std::unique_ptr<Node> createInsertReturnNode (InsertPlugin& insert,
                                              tracktion::graph::PlayHeadState& playHeadState,
                                              const CreateNodeParams& params)
{
    if (insert.getReturnDeviceType() != InsertPlugin::noDevice)
        for (auto i : insert.edit.getAllInputDevices())
            if (i->owner.getName() == insert.inputDevice)
                return createLiveInputNodeForDevice (*i, playHeadState, params, EditItemID());

    return {};
}

std::unique_ptr<Node> createInsertSendNode (InsertPlugin& insert, OutputDevice& device)
{
    if (insert.outputDevice != device.getName())
        return {};

    return makeNode<InsertSendNode> (insert);
}

//==============================================================================
std::unique_ptr<tracktion::graph::Node> createGroupFreezeNodeForDevice (Edit& edit,
                                                                        OutputDevice& device,
                                                                        ProcessState& processState)
{
    CRASH_TRACER

    for (auto& freezeFile : TemporaryFileManager::getFrozenTrackFiles (edit))
    {
        const auto outId = TemporaryFileManager::getDeviceIDFromFreezeFile (edit, freezeFile);

        if (device.getDeviceID() == outId)
        {
            AudioFile af (edit.engine, freezeFile);
            const auto length = TimeDuration::fromSeconds (af.getLength());

            if (length <= 0.0s)
                return {};

            auto node = tracktion::graph::makeNode<WaveNode> (af, TimeRange (0.0s, length),
                                                             0.0s, TimeRange(), LiveClipLevel(),
                                                             1.0,
                                                             ChannelConfiguration::canonical (af.getInfo().numChannels),
                                                             ChannelConfiguration::canonical (af.getInfo().numChannels),
                                                             processState,
                                                             EditItemID::fromRawID ((uint64_t) device.getName().hash()),
                                                             false);

            return makeNode<TrackMutingNode> (std::make_unique<TrackMuteState> (edit), std::move (node), false);
        }
    }

    return {};
}

//==============================================================================
std::unique_ptr<tracktion::graph::Node> createNodeForDevice (EditPlaybackContext& epc,
                                                             OutputDevice& device,
                                                             PlayHeadState& playHeadState,
                                                             std::unique_ptr<Node> node)
{
    if (auto waveDevice = dynamic_cast<WaveOutputDevice*> (&device))
    {
        ChannelMap channelMap;
        int sourceIndex = 0;

        for (const auto& channel : waveDevice->getChannels())
        {
            if (channel.indexInDevice != -1)
                channelMap.entries.emplace_back (sourceIndex, channel.indexInDevice);

            ++sourceIndex;
        }

        return tracktion::graph::makeNode<ChannelRemappingNode> (std::move (node), std::move (channelMap));
    }
    else if (auto midiInstance = dynamic_cast<MidiOutputDeviceInstance*> (epc.getOutputFor (&device)))
    {
        return tracktion::graph::makeNode<MidiOutputDeviceInstanceInjectingNode> (*midiInstance, std::move (node),
                                                                                  playHeadState.playHead);
    }

    return {};
}

std::unique_ptr<tracktion::graph::Node> createMasterPluginsNode (Edit& edit,
                                                                 tracktion::graph::PlayHeadState& playHeadState,
                                                                 std::unique_ptr<Node> node,
                                                                 const CreateNodeParams& params)
{
    if (! params.includeMasterPlugins)
        return node;

    auto tempoTrack = edit.getTempoTrack();
    auto tempoModList = tempoTrack != nullptr ? tempoTrack->getModifierList() : nullptr;

    node = createModifierNodeForList (tempoModList, Modifier::ProcessingPosition::preFX,
                                      nullptr, std::move (node), playHeadState, params);

    auto masterTrack = edit.getMasterTrack();
    auto masterModList = masterTrack != nullptr ? masterTrack->getModifierList() : nullptr;

    node = createModifierNodeForList (masterModList, Modifier::ProcessingPosition::preFX,
                                      nullptr, std::move (node), playHeadState, params);

    node = createPluginNodeForList (edit.getMasterPluginList(), nullptr, std::move (node), playHeadState, params);

    node = createModifierNodeForList (tempoModList, Modifier::ProcessingPosition::postFX,
                                      nullptr, std::move (node), playHeadState, params);
    node = createModifierNodeForList (masterModList, Modifier::ProcessingPosition::postFX,
                                      nullptr, std::move (node), playHeadState, params);

    if (auto masterVolPlugin = edit.getMasterVolumePlugin())
        node = createNodeForPlugin (*masterVolPlugin, nullptr, std::move (node), params);

    return node;
}

// Patch local Objekat — retours des aux TOP-LEVEL, greffés à la RACINE de l'Edit, juste au-dessus
// de la somme des pistes et donc juste avant la chaîne master.
//
// Pourquoi à la racine et pas piste par piste : « une lane = une piste », donc un émetteur
// top-level et l'aux qu'il vise vivent sur des AudioTrack DIFFÉRENTES. Faire dépendre le retour
// de chacune des pistes émettrices demanderait un fan-out par piste, et surtout ouvrirait des
// CYCLES — un objet de la lane 3 envoyant vers un aux de la lane 0 pendant qu'un objet de la
// lane 0 envoie vers un aux de la lane 3 ; `areThereAnyCycles` assert dessus, et il faudrait
// détecter et casser l'envoi fautif. Dépendre de la SOMME des pistes supprime les deux
// problèmes d'un coup : tout ce qui émet est en amont, par construction.
//
// Le retour dépend de la somme des pistes mais n'en consomme pas l'audio — c'est la même arête
// d'ordonnancement qu'à l'intérieur d'un container.
//
// Contrepartie assumée : ce retour est schedulé à CHAQUE bloc de la session, il n'y a aucune
// fenêtre de container pour le borner. C'est le coût fixe que le niveau 1 évitait, ici borné à
// « un par aux top-level », pas un par groupe. On ne peut PAS le gater en le posant dans un
// TimedNode : la chaîne d'un TimedNode n'est pas ordonnancée par le graphe englobant (mais lui
// est exposée par getInternalNodes) — c'est le piège qui a tué LatencyMaskingNode — et un
// retour endormi cesserait de consommer les taps, qui ressortiraient au réveil (piège 0014).
//
// « RACINE » ne veut plus dire « tout l'Edit » : un aux posé dans un FolderTrack submix est monté
// DANS ce folder (@see createSubmixAuxReturns). Sans ça, le humide d'un objet de stem ressortait
// au-dessus de tous les bus — hors du VU du stem, hors de son mute, hors de son bounce, et
// « Σ stems = mix » devenait faux dès qu'un envoi existait. Le niveau d'un aux est donc son
// FOLDER, comme le niveau d'un aux de groupe est son container.
//
// Un aux appartient à un niveau, mais son recrutement d'ÉMETTEURS descend : le montage à la
// racine accepte les envois des stems, qui lui sont forcément antérieurs. Une réverbe unique
// partagée par plusieurs stems tient à ça, et à rien d'autre (@see createTopLevelAuxReturns).

// Le folder submix le plus proche au-dessus de `t`, nullptr s'il n'y en a aucun. C'est ce qui
// dit à quel NIVEAU une piste — donc les aux et les émetteurs qu'elle porte — appartient.
static FolderTrack* nearestSubmixAncestor (Track& t)
{
    for (auto p = t.getParentTrack(); p != nullptr; p = p->getParentTrack())
        if (auto ft = dynamic_cast<FolderTrack*> (p); ft != nullptr && ft->isSubmixFolder())
            return ft;

    return nullptr;
}

// Recense, dans un ensemble de pistes, les bus d'aux d'une part et les chaînes où chercher les
// envois qui les visent d'autre part. Commun aux deux niveaux de montage.
static void collectAuxClipsAndSenders (const juce::Array<Track*>& tracks,
                                       juce::Array<ContainerClip*>& auxClips,
                                       std::vector<PluginList*>& senderLists)
{
    juce::Array<Clip*> senderClips;

    for (auto t : tracks)
    {
        auto ct = dynamic_cast<ClipTrack*> (t);

        if (ct == nullptr)
            continue;

        for (auto c : ct->getClips())
        {
            if (auto cc = dynamic_cast<ContainerClip*> (c); cc != nullptr && cc->isObjAuxBus())
                auxClips.add (cc);
            else
                senderClips.add (c);
        }

        // La chaîne de la PISTE compte aussi, au cas où l'hôte en pose une : dans le modèle
        // d'Objekat les pistes du pool ne portent aucun plugin (toute chaîne d'objet vit sur
        // son clip), donc la boucle n'y trouve rien — mais rien n'oblige un hôte à faire ça.
        senderLists.push_back (&ct->pluginList);
    }

    for (auto pl : senderPluginLists (senderClips))
        senderLists.push_back (pl);
}

// Un rendu RESTREINT (gel d'un objet) ne monte aucun retour, à aucun niveau : un aux est un objet
// à lui, qui joue de son côté — le cuire dans l'émetteur le compterait deux fois.
static bool shouldBuildAuxReturns (const Node* node, const CreateNodeParams& params)
{
    return node != nullptr
            && params.includePlugins
            && params.allowedTracks == nullptr
            && params.allowedClips == nullptr;
}

// Un aux monté à la racine accepte les envois de TOUT l'Edit, stems compris, alors que ses aux
// à lui restent ceux des pistes racine. La dissymétrie n'est pas une faveur : le folder submix
// d'un stem EST l'un des inputs de la somme dont ce retour dépend (createNodeForEdit le pousse
// dans le vecteur du device de sortie — et même détaché du Main, il y entre enveloppé d'un
// SinkNode). Tout ce qui vit dans un stem est donc en amont par construction, ses taps sont
// écrits avant d'être lus, et dépendre d'une somme continue d'interdire les cycles.
//
// L'inverse reste impossible et le restera : un émetteur de la racine n'est PAS en amont de la
// somme des pistes filles d'un stem (@see createSubmixAuxReturns). D'où la règle applicative
// « un envoi monte, il ne descend pas » (@see -isSend:routableToAux:). Deux stems frères ne se
// voient pas davantage — c'est ce qui rend une réverbe partagée possible au Main, et là seulement.
//
// La PDC suit sans rien ajouter : le retard d'un tap vaut « référence − latence du tap », et
// l'égalisation posée entre le tap et le point de référence amène le sec exactement à la latence
// de référence, quelle que soit la traversée (chaîne du stem comprise). @see ObjAuxReturnNode.
static std::unique_ptr<Node> createTopLevelAuxReturns (Edit& edit,
                                                       std::unique_ptr<Node> tracksNode,
                                                       const CreateNodeParams& params)
{
    if (! shouldBuildAuxReturns (tracksNode.get(), params))
        return tracksNode;

    juce::Array<ContainerClip*> allAuxClips;
    std::vector<PluginList*> senderLists;
    collectAuxClipsAndSenders (getAllTracks (edit), allAuxClips, senderLists);

    // Les aux d'un stem appartiennent à leur folder, qui monte les siens : ne restent ici que
    // ceux des pistes racine. Les émetteurs, eux, viennent d'être recensés partout.
    juce::Array<ContainerClip*> auxClips;

    for (auto cc : allAuxClips)
        if (auto t = cc->getTrack(); t != nullptr && nearestSubmixAncestor (*t) == nullptr)
            auxClips.add (cc);

    return createAuxReturns (senderLists, auxClips, std::move (tracksNode), params);
}

// Retours des aux d'un FolderTrack submix, greffés sur la somme de SES pistes filles — donc en
// amont de la chaîne du bus, de son VU et de son mute. C'est ce qui rend le humide solidaire du
// stem : il passe par ses FX, il compte dans son niveau, il se tait avec lui.
//
// La contrainte d'ordonnancement est la même qu'à la racine, un cran plus bas : le retour dépend
// de la somme des pistes du folder, donc seuls les émetteurs DE CE FOLDER sont garantis en amont.
// Un envoi venu d'un autre stem — ou de la racine — serait lu sans ordre établi ; on ne recense
// donc ici que les pistes du folder, et l'application refuse ces envois de son côté
// (@see -isSend:routableToAux:). C'est la moitié DESCENDANTE de la règle : un envoi monte vers
// l'aux d'un bus qui contient déjà son émetteur (@see createTopLevelAuxReturns), il ne descend pas.
//
// Les pistes d'un submix IMBRIQUÉ sont exclues : elles appartiennent à leur propre niveau, et
// c'est l'appel récursif de createNodeForSubmixTrack qui y montera leurs aux.
static std::unique_ptr<Node> createSubmixAuxReturns (FolderTrack& submixTrack,
                                                     std::unique_ptr<Node> tracksNode,
                                                     const CreateNodeParams& params)
{
    if (! shouldBuildAuxReturns (tracksNode.get(), params))
        return tracksNode;

    juce::Array<Track*> ownTracks;

    for (auto t : submixTrack.getAllSubTracks (true))
        if (nearestSubmixAncestor (*t) == &submixTrack)
            ownTracks.add (t);

    juce::Array<ContainerClip*> auxClips;
    std::vector<PluginList*> senderLists;
    collectAuxClipsAndSenders (ownTracks, auxClips, senderLists);

    return createAuxReturns (senderLists, auxClips, std::move (tracksNode), params);
}

std::unique_ptr<tracktion::graph::Node> createMasterFadeInOutNode (Edit& edit,
                                                                   std::unique_ptr<Node> node,
                                                                   const CreateNodeParams& params)
{
    if (! params.includeMasterPlugins)
        return node;

    if (edit.masterFadeIn > 0_td || edit.masterFadeOut > 0_td)
    {
        auto length = toPosition (edit.getLength());
        return makeNode<FadeInOutNode> (std::move (node), params.processState,
                                        TimeRange { 0_tp, edit.masterFadeIn },
                                        TimeRange { length - edit.masterFadeOut, length },
                                        edit.masterFadeInType.get(),
                                        edit.masterFadeOutType.get(),
                                        true);
    }

    return node;
}

}

//==============================================================================
// Patch local Objekat — the audio bridge's construction passes (docs/plan_sidechain.md §4.3).
//
// A reader must declare its latency X when it is CONSTRUCTED, but the true age of its tap is only
// known once the tap is constructed too — and the builder may reach the destination before the
// source (another track, another depth). So a reader declares X from the age its tap had at the
// PREVIOUS build (cached on the tap plugin), and at the end of the pass BridgeBuild::finalise
// checks every reader against the true ages. If one is late (X < true age) or over-declared
// (X > what it needs), the pass is thrown away and built again, the taps now caching the truth.
//
// In steady state (ages unchanged) and for any key younger than its destination this costs no extra
// pass. A discarded graph was never prepared, so dropping it on this thread is safe.
// Renders converge in their own call: an export is never misaligned for want of a timer.
static constexpr int maxBridgePasses = 8;

template<typename BuildOnePass>
static std::unique_ptr<tracktion::graph::Node> buildWithBridgePasses (Edit& edit, const CreateNodeParams& params,
                                                                      BuildOnePass&& buildOnePass)
{
    const auto buildID = BridgeBuild::nextBuildID();
    std::unique_ptr<tracktion::graph::Node> node;

    for (int pass = 0;; ++pass)
    {
        auto build = std::make_shared<BridgeBuild> (edit, buildID, pass, params.sampleRate, params.blockSize);
        auto passParams = params;
        passParams.bridgeBuild = build;

        node = buildOnePass (passParams);

        const bool anotherPassNeeded = build->finalise();

        if (! anotherPassNeeded || pass + 1 >= maxBridgePasses)
        {
            build->allocateRings();
            build->publish();
            break;
        }

        node.reset();
    }

    return node;
}

static std::unique_ptr<tracktion::graph::Node> createNodeForEditPass (EditPlaybackContext& epc, std::atomic<double>& audibleTimeToUpdate, const CreateNodeParams& params)
{
    Edit& edit = epc.edit;
    auto& playHeadState = params.processState.playHeadState;
    auto insertPlugins = getAllPluginsOfType<InsertPlugin> (edit);

    using TrackNodeVector = std::vector<std::unique_ptr<tracktion::graph::Node>>;
    std::map<OutputDevice*, TrackNodeVector> deviceNodes;
    std::vector<OutputDevice*> devicesWithFrozenNodes;

    for (auto t : getAllTracks (edit))
    {
        if (params.allowedTracks != nullptr && ! params.allowedTracks->contains (t))
            continue;

        if (auto output = getTrackOutput (*t))
        {
            if (auto device = output->getOutputDevice (false))
            {
                if (! device->isEnabled())
                    continue;

                if (! params.forRendering && t->isFrozen (Track::groupFreeze))
                {
                    if (std::find (devicesWithFrozenNodes.begin(), devicesWithFrozenNodes.end(), device)
                        != devicesWithFrozenNodes.end())
                       continue;

                    if (auto node = createGroupFreezeNodeForDevice (edit, *device, params.processState))
                    {
                        deviceNodes[device].push_back (std::move (node));
                        devicesWithFrozenNodes.push_back (device);
                    }
                }
                else if (auto node = createNodeForTrack (*t, params))
                {
                    deviceNodes[device].push_back (std::move (node));
                }
            }
        }
    }

    // Add bus tracks (tracks with no output device/destination that still need
    // processing for their plugin chain to participate in send/return or rack routing)
    if (auto defaultDevice = edit.engine.getDeviceManager().getDefaultWaveOutDevice())
    {
        for (auto t : getAllTracks (edit))
        {
            if (params.allowedTracks != nullptr && ! params.allowedTracks->contains (t))
                continue;

            if (auto output = getTrackOutput (*t))
            {
                if (output->getOutputDevice (false) != nullptr || output->getDestinationTrack() != nullptr)
                    continue;
            }
            else
            {
                continue;
            }

            if (auto node = createNodeForTrack (*t, params))
                deviceNodes[defaultDevice].push_back (makeNode<SinkNode> (std::move (node)));
        }
    }

    // Add deviceNodes for any devices only being used by InsertPlugins
    for (auto ins : insertPlugins)
    {
        if (ins->getSendDeviceType() != InsertPlugin::noDevice)
        {
            if (auto device = edit.engine.getDeviceManager().findOutputDeviceWithName (ins->outputDevice))
            {
                auto& trackNodeVector = deviceNodes[device];
                juce::ignoreUnused (trackNodeVector);
                // We don't need to add anything to the vector, just ensure the device is in the map
            }
        }
    }

    // Add deviceNodes for any devices only being used by the click track
    for (int i = edit.engine.getDeviceManager().getNumOutputDevices(); --i >= 0;)
    {
        if (auto device = edit.engine.getDeviceManager().getOutputDeviceAt (i))
        {
            if (! edit.isClickTrackDevice (*device))
                continue;

            auto& trackNodeVector = deviceNodes[device];
            juce::ignoreUnused (trackNodeVector);
            // We don't need to add anything to the vector, just ensure the device is in the map
        }
    }

    // Add deviceNodes for any devices only being used by the MIDI clock or MTC
    for (int i = edit.engine.getDeviceManager().getNumMidiOutDevices(); --i >= 0;)
    {
        if (auto device = edit.engine.getDeviceManager().getMidiOutDevice (i))
        {
            const bool isSendingMidi = device->isSendingClock()
                                        || device->isSendingTimecode()
                                        || device->isSendingControllerMidiClock();

            if (! isSendingMidi)
                continue;

            auto& trackNodeVector = deviceNodes[device];
            juce::ignoreUnused (trackNodeVector);
            // We don't need to add anything to the vector, just ensure the device is in the map
        }
    }


    auto outputNode = std::make_unique<tracktion::graph::SummingNode>();

    for (auto& deviceAndTrackNode : deviceNodes)
    {
        auto device = deviceAndTrackNode.first;
        jassert (device != nullptr);
        auto tracksVector = std::move (deviceAndTrackNode.second);

        auto sumNode = std::make_unique<SummingNode> (std::move (tracksVector));
        sumNode->setDoubleProcessingPrecision (edit.engine.getPropertyStorage().getProperty (SettingID::use64Bit, false));

        // Create nodes for any insert plugins
        bool deviceIsBeingUsedAsInsert = false;

        for (auto ins : insertPlugins)
        {
            if (ins->isFrozen())
                continue;

            if (ins->outputDevice != device->getName())
                continue;

            if (auto sendNode = createInsertSendNode (*ins, *device))
            {
                sumNode->addInput (std::move (sendNode));
                deviceIsBeingUsedAsInsert = true;
            }
        }

        std::unique_ptr<Node> node = std::move (sumNode);

        if (! deviceIsBeingUsedAsInsert)
        {
            if (edit.engine.getDeviceManager().getDefaultWaveOutDeviceID() == device->getDeviceID())
            {
                // Patch local Objekat — les aux top-level se somment aux pistes, avant le master.
                node = createTopLevelAuxReturns (edit, std::move (node), params);
                node = createMasterPluginsNode (edit, playHeadState, std::move (node), params);

                if (auto waveDevice = dynamic_cast<WaveOutputDevice*> (device))
                {
                    const int deviceChannels = (int) waveDevice->getChannels().size();
                    const int incomingChannels = node->getNodeProperties().numberOfChannels;

                    if (incomingChannels > deviceChannels)
                        node = tracktion::graph::makeNode<ChannelRemappingNode> (std::move (node),
                                                                                 ChannelMap::conversion (incomingChannels, deviceChannels));
                }

                node = makeNode<LevelMeasuringNode> (std::move (node), epc.masterLevels);
            }

            node = createMasterFadeInOutNode (edit, std::move (node), params);
            node = EditNodeBuilder::insertOptionalLastStageNode (std::move (node));

            if (params.insertOptionalLastStageNodeForDevice && device != nullptr)
                node = params.insertOptionalLastStageNodeForDevice (*device, params, std::move (node));

            if (edit.getIsPreviewEdit() && node != nullptr)
                if (auto previewMeasurer = edit.getPreviewLevelMeasurer())
                    node = makeNode<SharedLevelMeasuringNode> (std::move (previewMeasurer), std::move (node));
        }

        // Convert signal to device channel count at the device boundary
        if (auto waveDevice = dynamic_cast<WaveOutputDevice*> (device))
        {
            const int incomingChannels = node->getNodeProperties().numberOfChannels;
            const int deviceChannels = (int) waveDevice->getChannels().size();

            if (incomingChannels > 0 && incomingChannels < deviceChannels)
                node = tracktion::graph::makeNode<ChannelRemappingNode> (std::move (node),
                                                                         ChannelMap::conversion (incomingChannels, deviceChannels));
        }

        if (edit.isClickTrackDevice (*device))
        {
            auto clickAndTracksNode = makeSummingNode ({ node.release(),
                                                         makeNode<ClickNode> (edit, getNumChannelsFromDevice (*device),
                                                                              device->isMidi(), playHeadState.playHead).release() });
            node = std::move (clickAndTracksNode);
        }

        if (auto outputDeviceNode = createNodeForDevice (epc, *device, playHeadState, std::move (node)))
            outputNode->addInput (std::move (outputDeviceNode));
    }

    std::unique_ptr<Node> finalNode (std::move (outputNode));
    finalNode = createRackNode (std::move (finalNode), edit.getRackList(), params);
    finalNode = makeNode<PlayHeadPositionNode> (params.processState, std::move (finalNode), audibleTimeToUpdate);

    return finalNode;
}

static std::unique_ptr<tracktion::graph::Node> createNodeForEditPass (Edit& edit, const CreateNodeParams& params)
{
    std::vector<std::unique_ptr<tracktion::graph::Node>> trackNodes;
    auto& playHeadState = params.processState.playHeadState;

    for (auto t : getAllTracks (edit))
    {
        if (params.allowedTracks != nullptr && ! params.allowedTracks->contains (t))
            continue;

        // Skip submix children (they're built by their parent) and tracks with no TrackOutput
        if (auto output = getTrackOutput (*t))
        {
            if (output->getDestinationTrack() != nullptr)
                continue;
        }
        else
        {
            continue;
        }

        if (auto node = createNodeForTrack (*t, params))
            trackNodes.push_back (std::move (node));
    }

    auto sumNode = std::make_unique<SummingNode> (std::move (trackNodes));
    sumNode->setDoubleProcessingPrecision (edit.engine.getPropertyStorage().getProperty (SettingID::use64Bit, false));

    auto node = std::unique_ptr<Node> (std::move (sumNode));
    // Patch local Objekat — les aux top-level se somment aux pistes, avant le master.
    node = createTopLevelAuxReturns (edit, std::move (node), params);
    node = createMasterPluginsNode (edit, playHeadState, std::move (node), params);
    node = createMasterFadeInOutNode (edit, std::move (node), params);
    node = createRackNode (std::move (node), edit.getRackList(), params);

    return node;
}

std::unique_ptr<tracktion::graph::Node> createNodeForEdit (EditPlaybackContext& epc, std::atomic<double>& audibleTimeToUpdate, const CreateNodeParams& params)
{
    return buildWithBridgePasses (epc.edit, params,
                                  [&] (const CreateNodeParams& passParams)
                                  {
                                      return createNodeForEditPass (epc, audibleTimeToUpdate, passParams);
                                  });
}

std::unique_ptr<tracktion::graph::Node> createNodeForEdit (Edit& edit, const CreateNodeParams& originalParams)
{
    auto params = originalParams;

    // Once, before the passes: the implicit submix children are added through the caller's array.
    if (params.implicitlyIncludeSubmixChildTracks && params.allowedTracks != nullptr)
        *params.allowedTracks = addImplicitSubmixChildTracks (*params.allowedTracks);

    return buildWithBridgePasses (edit, params,
                                  [&] (const CreateNodeParams& passParams)
                                  {
                                      return createNodeForEditPass (edit, passParams);
                                  });
}

std::unique_ptr<tracktion::graph::Node> createGeneratorPluginNode (const Plugin::Ptr& plugin,
                                                                   const CreateNodeParams& params,
                                                                   std::unique_ptr<tracktion::graph::Node> output)
{
    if (plugin == nullptr)
        return output;

    auto generator = makeNode<PluginNode> (makeNode<SilentNode> (2),
                                           plugin, params.sampleRate, params.blockSize, nullptr,
                                           params.processState, params.forRendering, true, -1);

    return makeSummingNode ({ output.release(), generator.release() });
}

std::function<std::unique_ptr<tracktion::graph::Node> (std::unique_ptr<tracktion::graph::Node>)> EditNodeBuilder::insertOptionalLastStageNode
    = [] (std::unique_ptr<tracktion::graph::Node> input) { return input; };

} // namespace tracktion::inline engine
