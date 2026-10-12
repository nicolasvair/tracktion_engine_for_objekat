/*
    ,--.                     ,--.     ,--.  ,--.
  ,-'  '-.,--.--.,--,--.,---.|  |,-.,-'  '-.`--' ,---. ,--,--,      Copyright 2024
  '-.  .-'|  .--' ,-.  | .--'|     /'-.  .-',--.| .-. ||      \   Tracktion Software
    |  |  |  |  \ '-'  \ `--.|  \  \  |  |  |  |' '-' '|  ||  |       Corporation
    `---' `--'   `--`--'`---'`--'`--' `---' `--' `---' `--''--'    www.tracktion.com

    Tracktion Engine uses a GPL/commercial licence - see LICENCE.md for details.
*/

namespace tracktion { inline namespace engine
{

//==============================================================================
ChannelRemappingNode::ChannelRemappingNode (std::unique_ptr<tracktion::graph::Node> inputNode,
                                            ChannelMap channelMapToUse)
    : mode (Mode::ExplicitMapping),
      input (std::move (inputNode)),
      channelMap (std::move (channelMapToUse))
{
    jassert (input != nullptr);

    // Need to clear buffers since we use add() for accumulation
    setOptimisations ({ tracktion::graph::ClearBuffers::yes,
                        tracktion::graph::AllocateAudioBuffer::yes });
}

ChannelRemappingNode::ChannelRemappingNode (std::unique_ptr<tracktion::graph::Node> processorNode,
                                            ChannelConfiguration inputConfig,
                                            ChannelConfiguration processorConfig)
    : mode (Mode::ProcessorPassthrough),
      input (std::move (processorNode)),
      inputChannelConfig (std::move (inputConfig)),
      processorChannelConfig (std::move (processorConfig))
{
    jassert (input != nullptr);
    jassert (! inputChannelConfig.isEmpty());

    // An empty processorConfig means "pass-through: handles all channels",
    // so treat it as matching the input config
    if (processorChannelConfig.isEmpty())
        processorChannelConfig = inputChannelConfig;

    // Cache the original input node - it's the first direct input of the processor
    // We need this for passthrough mode to access channels beyond what the processor handles
    auto processorInputs = input->getDirectInputNodes();

    if (! processorInputs.empty())
        originalInputNode = processorInputs[0];

    setOptimisations ({ tracktion::graph::ClearBuffers::no,
                        tracktion::graph::AllocateAudioBuffer::yes });
}

//==============================================================================
tracktion::graph::NodeProperties ChannelRemappingNode::getNodeProperties()
{
    auto props = input->getNodeProperties();

    constexpr size_t channelRemappingMagicHash = size_t (0x6368616e72656d); // "chanrem"

    if (mode == Mode::ExplicitMapping)
    {
        props.hasAudio = ! channelMap.isEmpty();
        props.numberOfChannels = channelMap.getRequiredOutputChannels();

        // Include mapping in hash
        for (const auto& e : channelMap.entries)
        {
            hash_combine (props.nodeID, e.source);
            hash_combine (props.nodeID, e.dest);
        }
    }
    else // ProcessorPassthrough
    {
        const auto numInputChannels = inputChannelConfig.getNumChannels();
        const auto numProcessorChannels = processorChannelConfig.getNumChannels();

        // Output channel count depends on mode:
        // - Passthrough mode (input > processor): output = input channels
        // - Expansion mode (input < processor): output = processor channels
        props.numberOfChannels = std::max (numInputChannels, numProcessorChannels);

        hash_combine (props.nodeID, numInputChannels);
        hash_combine (props.nodeID, numProcessorChannels);
    }

    if (props.nodeID != 0)
        hash_combine (props.nodeID, channelRemappingMagicHash);

    return props;
}

std::vector<tracktion::graph::Node*> ChannelRemappingNode::getDirectInputNodes()
{
    return { input.get() };
}

bool ChannelRemappingNode::isReadyToProcess()
{
    return input->hasProcessed();
}

void ChannelRemappingNode::prepareToPlay (const tracktion::graph::PlaybackInitialisationInfo& info)
{
    // Nothing special needed - input handles its own preparation

    // Patch local Objekat (0041) — room for a copy of the input, used only when the output
    // buffer turns out to be the input buffer (@see processExplicitMapping).
    if (mode == Mode::ExplicitMapping)
    {
        const auto numInputChannels = std::max (1, input->getNodeProperties().numberOfChannels);
        aliasedInputCopy.resize ({ static_cast<choc::buffer::ChannelCount> (numInputChannels),
                                   static_cast<choc::buffer::FrameCount> (std::max (1, info.blockSize)) });
    }
}

namespace
{
    // Patch local Objekat (0041) — whether two views overlap in memory (channel by channel).
    bool viewsShareMemory (const choc::buffer::ChannelArrayView<float>& a,
                           const choc::buffer::ChannelArrayView<float>& b)
    {
        const auto na = a.getNumFrames(), nb = b.getNumFrames();

        if (na == 0 || nb == 0)
            return false;

        for (choc::buffer::ChannelCount i = 0; i < a.getNumChannels(); ++i)
        {
            const float* pa = a.getChannel (i).data.data;

            for (choc::buffer::ChannelCount j = 0; j < b.getNumChannels(); ++j)
            {
                const float* pb = b.getChannel (j).data.data;

                if (pa < pb + nb && pb < pa + na)
                    return true;
            }
        }

        return false;
    }
}

void ChannelRemappingNode::process (ProcessContext& pc)
{
    if (mode == Mode::ExplicitMapping)
        processExplicitMapping (pc);
    else
        processProcessorPassthrough (pc);
}

//==============================================================================
void ChannelRemappingNode::processExplicitMapping (ProcessContext& pc)
{
    auto inputBuffers = input->getProcessedOutput();

    // Always pass through MIDI
    pc.buffers.midi.copyFrom (inputBuffers.midi);

    auto sourceAudio = inputBuffers.audio;

    // Patch local Objekat (0041) — the mapping ADDS into its destination, which is only right on a
    // cleared buffer (ClearBuffers::yes). When the destination IS the input — every node of a linear
    // TimedNode chain processes in one shared view, the clip's plugin list being one — the input was
    // added onto itself: a mono source converted to stereo for the first plugin of a clip's chain came
    // out at twice its level (+6.02 dB, both channels), and any remap read sources it had already
    // overwritten. So, when they overlap, the input is copied aside, the destination cleared, and the
    // mapping reads the copy: exactly what a non-aliased (cleared) destination would have received.
    if (viewsShareMemory (sourceAudio, pc.buffers.audio))
    {
        const auto numChannels = std::min (sourceAudio.getNumChannels(), aliasedInputCopy.getNumChannels());
        const auto numFrames = std::min (sourceAudio.getNumFrames(), aliasedInputCopy.getNumFrames());
        jassert (numChannels == sourceAudio.getNumChannels() && numFrames == sourceAudio.getNumFrames());

        auto copyView = aliasedInputCopy.getView().getFirstChannels (numChannels).getStart (numFrames);
        copy (copyView, sourceAudio.getFirstChannels (numChannels).getStart (numFrames));
        pc.buffers.audio.clear();
        sourceAudio = copyView;
    }

    // Remap audio channels - use add() to accumulate (allows multiple sources to one dest)
    for (const auto& e : channelMap.entries)
    {
        if (e.source < static_cast<int> (sourceAudio.getNumChannels())
            && e.dest < static_cast<int> (pc.buffers.audio.getNumChannels()))
        {
            auto destChan = pc.buffers.audio.getChannel (static_cast<choc::buffer::ChannelCount> (e.dest));
            auto srcChan = sourceAudio.getChannel (static_cast<choc::buffer::ChannelCount> (e.source));

            if (e.gain == 1.0f)
                add (destChan, srcChan);
            else
                add (destChan, srcChan, e.gain);
        }
    }
}

void ChannelRemappingNode::processProcessorPassthrough (ProcessContext& pc)
{
    auto& destAudio = pc.buffers.audio;
    auto& destMidi = pc.buffers.midi;

    const auto numInputChannels = inputChannelConfig.getNumChannels();
    const auto numProcessorChannels = processorChannelConfig.getNumChannels();

    // Get the processor's output
    auto processorOutput = input->getProcessedOutput();
    const auto numProcessorOutputChannels = static_cast<int> (processorOutput.audio.getNumChannels());

    // Copy MIDI from processor
    destMidi.copyFrom (processorOutput.midi);

    // Copy processed channels from processor output
    const auto numChannelsToCopyFromProcessor = std::min (numProcessorOutputChannels,
                                                          static_cast<int> (destAudio.getNumChannels()));

    if (numChannelsToCopyFromProcessor > 0)
    {
        auto destProcessedChannels = destAudio.getFirstChannels (static_cast<choc::buffer::ChannelCount> (numChannelsToCopyFromProcessor));
        auto srcProcessedChannels = processorOutput.audio.getFirstChannels (static_cast<choc::buffer::ChannelCount> (numChannelsToCopyFromProcessor));
        copy (destProcessedChannels, srcProcessedChannels);
    }

    // PASSTHROUGH MODE: Copy extra channels from original input that the processor didn't handle
    if (numInputChannels > numProcessorChannels && originalInputNode != nullptr)
    {
        auto originalInput = originalInputNode->getProcessedOutput();
        const auto numOriginalChannels = static_cast<int> (originalInput.audio.getNumChannels());
        const auto numPassthroughChannels = numInputChannels - numProcessorChannels;

        // Copy channels from processor channel count onwards
        for (int ch = 0; ch < numPassthroughChannels; ++ch)
        {
            const auto srcChannel = numProcessorChannels + ch;
            const auto destChannel = numProcessorChannels + ch;

            if (srcChannel < numOriginalChannels && destChannel < static_cast<int> (destAudio.getNumChannels()))
            {
                copy (destAudio.getChannel (static_cast<choc::buffer::ChannelCount> (destChannel)),
                      originalInput.audio.getChannel (static_cast<choc::buffer::ChannelCount> (srcChannel)));
            }
            else if (destChannel < static_cast<int> (destAudio.getNumChannels()))
            {
                // Source channel doesn't exist, fill with silence
                destAudio.getChannel (static_cast<choc::buffer::ChannelCount> (destChannel)).clear();
            }
        }
    }

    // EXPANSION MODE: The processor already outputs enough channels, nothing extra to do
    // (processor output was already copied above)
}

}} // namespace tracktion { inline namespace engine
