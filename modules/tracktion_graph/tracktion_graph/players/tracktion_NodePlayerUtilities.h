/*
    ,--.                     ,--.     ,--.  ,--.
  ,-'  '-.,--.--.,--,--.,---.|  |,-.,-'  '-.`--' ,---. ,--,--,      Copyright 2024
  '-.  .-'|  .--' ,-.  | .--'|     /'-.  .-',--.| .-. ||      \   Tracktion Software
    |  |  |  |  \ '-'  \ `--.|  \  \  |  |  |  |' '-' '|  ||  |       Corporation
    `---' `--'   `--`--'`---'`--'`--' `---' `--' `---' `--''--'    www.tracktion.com

    Tracktion Engine uses a GPL/commercial licence - see LICENCE.md for details.
*/

#pragma once

#include <span>

/** OBJEKAT — sonde de mesure des reconstructions de graphe (étape 1).
    Activée à 1 par défaut en Debug. Mettre à 0 (ou définir OBJ_GRAPH_PROFILE=0
    dans les build settings) pour la désactiver complètement : le code disparaît.
*/
#ifndef OBJ_GRAPH_PROFILE
 #if JUCE_DEBUG
  #define OBJ_GRAPH_PROFILE 1
 #else
  #define OBJ_GRAPH_PROFILE 0
 #endif
#endif

#if OBJ_GRAPH_PROFILE
 #include <atomic>
 #include <chrono>
 #include <cstdio>
 #include <cstdlib>
#endif

namespace tracktion::inline graph {

namespace node_player_utils
{
    /** Returns true if all the nodes in this collection have a unique nodeID. */
    template<typename Collection>
    bool areNodeIDsUnique (Collection&& nodes, bool ignoreZeroIDs)
    {
        std::vector<size_t> nodeIDs;

        for (auto n : nodes)
            nodeIDs.push_back (n->getNodeProperties().nodeID);

        std::sort (nodeIDs.begin(), nodeIDs.end());

        if (ignoreZeroIDs)
            nodeIDs.erase (std::remove_if (nodeIDs.begin(), nodeIDs.end(),
                                           [] (auto nID) { return nID == 0; }),
                           nodeIDs.end());

        auto duplicateIDs = [] (auto input)
                            {
                                std::vector<size_t> duplicates;
                                std::unordered_set<size_t> seen;
                                seen.reserve (input.size());

                                for (auto num : input)
                                {
                                    if (seen.find (num) != seen.end())
                                        duplicates.push_back (num);
                                    else
                                        seen.insert (num);
                                }

                                return duplicates;
                            } (nodeIDs);

       #if JUCE_DEBUG
        if (! duplicateIDs.empty())
        {
            auto getNodeTypeStrings = [&nodes] (auto id)
            {
                std::string idStrings;

                for (auto n : nodes)
                    if (n->getNodeProperties().nodeID == id)
                        idStrings += std::string (typeid (*n).name()) += ", ";

                return idStrings;
            };

            for (auto id : duplicateIDs)
                DBG("\t" << id << ": " << getNodeTypeStrings (id));
        }
       #endif

        return duplicateIDs.empty();
    }

    /** Returns true if all the nodes in the graph have a unique nodeID. */
    static inline bool areNodeIDsUnique (Node& node, bool ignoreZeroIDs)
    {
        std::vector<Node*> nodes;
        visitNodes (node, [&] (Node& n) { nodes.push_back (&n); }, false);
        return areNodeIDsUnique (nodes, ignoreZeroIDs);
    }

    /** Returns true if all there are any feedback loops in the graph. */
    static inline bool areThereAnyCycles (const std::vector<Node*>& orderedNodes)
    {
        size_t numCycles = 0;

        // Iterate from the first node to the last
        // Find each input of the Node
        // Ensure that the input is in a lower position than the current
        for (auto iter = orderedNodes.begin(); iter != orderedNodes.end(); ++iter)
        {
            auto node = *iter;
            auto position = std::distance (orderedNodes.begin(), iter);

            for (auto inputNode : node->getDirectInputNodes())
            {
                const auto inputPosition = std::distance (orderedNodes.begin(),
                                                          std::find (orderedNodes.begin(), orderedNodes.end(), inputNode));

                if (inputPosition > position)
                    ++numCycles;
            }
        }

        return numCycles > 0;
    }

    /** Prepares a specific Node to be played and returns all the Nodes. */
    static std::unique_ptr<NodeGraph> prepareToPlay (std::unique_ptr<Node> node, NodeGraph* oldGraph,
                                                     double sampleRate, int blockSize,
                                                     std::function<NodeBuffer (choc::buffer::Size)> allocateAudioBuffer = nullptr,
                                                     std::function<void (NodeBuffer&&)> deallocateAudioBuffer = nullptr,
                                                     bool nodeMemorySharingEnabled = false,
                                                     bool disableLatencyCompensation = false)
    {
        if (node == nullptr)
            return {};

       #if OBJ_GRAPH_PROFILE
        using obj_clock = std::chrono::steady_clock;
        static std::atomic<int> objRebuildCounter { 0 };
        const auto objRebuildIndex = ++objRebuildCounter;
        const auto objT0 = obj_clock::now();
       #endif

        // First give the Nodes a chance to transform
        auto nodeGraph = createNodeGraph (std::move (node), disableLatencyCompensation);

       #if OBJ_GRAPH_PROFILE
        const auto objT1 = obj_clock::now();
       #endif

        assert (! areThereAnyCycles (nodeGraph->orderedNodes));

       #if OBJ_GRAPH_PROFILE
        const auto objT2 = obj_clock::now();
       #endif

        jassert (areNodeIDsUnique (nodeGraph->orderedNodes, true));

       #if OBJ_GRAPH_PROFILE
        const auto objT3 = obj_clock::now();
       #endif

        // Next, initialise all the nodes, this will call prepareToPlay on them
        const PlaybackInitialisationInfo info { sampleRate, blockSize,
                                                *nodeGraph, oldGraph,
                                                allocateAudioBuffer, deallocateAudioBuffer,
                                                nodeMemorySharingEnabled };

        for (auto n : nodeGraph->orderedNodes)
            n->initialise (info);

       #if OBJ_GRAPH_PROFILE
        {
            const auto objT4 = obj_clock::now();
            auto ms = [] (auto a, auto b)
            {
                return std::chrono::duration<double, std::milli> (b - a).count();
            };

            char objLine[256] = {};
            std::snprintf (objLine, sizeof (objLine),
                "[GRAPH] rebuild #%d - %d noeuds - %.1f ms  (build %.1f | cycles %.1f | ids %.1f | init %.1f)\n",
                objRebuildIndex,
                (int) nodeGraph->orderedNodes.size(),
                ms (objT0, objT4),
                ms (objT0, objT1),
                ms (objT1, objT2),
                ms (objT2, objT3),
                ms (objT3, objT4));

            // Console Xcode
            std::fputs (objLine, stderr);
            std::fflush (stderr);

            // ...et un fichier, pour que la mesure survive à un lancement hors Xcode.
            // Réécrit à zéro au premier rebuild de chaque lancement.
            if (const auto* home = std::getenv ("HOME"))
            {
                char objPath[512] = {};
                std::snprintf (objPath, sizeof (objPath), "%s/objekat-graph.log", home);

                if (auto* f = std::fopen (objPath, objRebuildIndex == 1 ? "w" : "a"))
                {
                    std::fputs (objLine, f);
                    std::fclose (f);
                }
            }
        }
       #endif

        return nodeGraph;
    }

    inline void reserveAudioBufferPool (Node* rootNode, const std::vector<Node*>& allNodes,
                                        AudioBufferPool& audioBufferPool, size_t numThreads, int blockSize)
    {
        if (rootNode == nullptr)
            return;

        // To find the number of buffers required:
        // - Find the maximum buffer::Size in the graph
        // - Multiply it by the maximum number of inputs any Node has
        // - Then multiply that by the number of threads that will be used (or the num leaf Nodes if that’s smaller)
        // - Add one for the root node so the ouput can be retained
        [[ maybe_unused ]] size_t maxNumChannels = 0, maxNumInputs = 0, numLeafNodes = 0;

        // However, this algorithm is too pessimistic as it assumes there can be
        // numThreads * maxNumInputs which is unlikely to be true.
        // It's probably better to stack up numThreads maxNumInputs and use the min of that size and numThreads

        for (auto n : allNodes)
        {
            const auto numInputs = n->getDirectInputNodes().size();
            const auto props = n->getNodeProperties();
            maxNumInputs    = std::max (maxNumInputs, numInputs);
            maxNumChannels  = std::max (maxNumChannels, (size_t) props.numberOfChannels);

            if (numInputs == 0)
                ++numLeafNodes;
        }

        const size_t numBuffersRequired = std::max ((size_t) 2, std::min (allNodes.size(), 1 + numThreads));
        audioBufferPool.reserve (numBuffersRequired, choc::buffer::Size::create (maxNumChannels, blockSize));
    }
}

}
