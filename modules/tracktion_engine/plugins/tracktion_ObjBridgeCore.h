/*
    OBJEKAT — the audio bridge's pure core.

    No JUCE, no Tracktion, nothing but the standard library: this header is compiled by the engine
    AND by `tools/test_bridge_core.cpp` on any machine, so the half of the bridge that has nothing
    behind it (the ring that carries a key from a tap to its readers, the latency arithmetic) can be
    asserted with no engine and no screen. The design is in docs/plan_sidechain.md (§4, §5.2).
*/

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace tracktion { inline namespace engine { namespace objbridge
{

//==============================================================================
/** A stretch of CONTIGUOUS stream samples a tap has written: [start, end). */
struct Run
{
    std::atomic<int64_t> start { 0 }, end { 0 };
};

//==============================================================================
/** The key's buffer: a ring indexed by STREAM sample (the device clock every graph shares), so the
    ring IS the delay line — a reader asks for [s - D, s - D + n) and gets what was written there.

    One writer (the tap node, audio thread), any number of readers (the reader nodes). The graph's
    ordering makes the writer and the readers of one block sequential; the seqlock below only DETECTS
    an ordering bug (a torn read answers silence), it does not make the payload thread-safe. */
class Ring
{
public:
    /** Message thread / prepare only. `capacity` is rounded UP to a power of two, `numChannels`
        clamped to [1, 2]. */
    Ring (int numChannelsToUse, int capacityToUse)
        : numChannels (std::min (2, std::max (1, numChannelsToUse)))
    {
        int c = 2;
        while (c < capacityToUse && c < (1 << 30))
            c <<= 1;

        capacity = c;
        data.assign ((size_t) numChannels * (size_t) capacity, 0.0f);
    }

    int getCapacity() const noexcept     { return capacity; }
    int getNumChannels() const noexcept  { return numChannels; }

    /** End of the newest run, 0 if there is none. */
    int64_t getLatestEnd() const noexcept
    {
        return numRuns.load (std::memory_order_acquire) > 0 ? runs[0].end.load (std::memory_order_acquire) : 0;
    }

    /** For the report only (message thread, racy by nature): copies the runs, newest first, as
        [start, end) pairs into `out` and returns how many. */
    int getRunsSnapshot (std::array<std::array<int64_t, 2>, 4>& out) const noexcept
    {
        const int n = std::min (numRuns.load (std::memory_order_acquire), maxRuns);

        for (int i = 0; i < n; ++i)
        {
            out[(size_t) i][0] = runs[(size_t) i].start.load (std::memory_order_relaxed);
            out[(size_t) i][1] = runs[(size_t) i].end.load (std::memory_order_relaxed);
        }

        return n;
    }

    /** AUDIO THREAD, single writer. Writes src[c][0..numFrames) at stream samples
        [streamStart, streamStart + numFrames). A source with ONE channel is duplicated on channel 1;
        channels beyond numChannels are ignored. Starts a NEW run when streamStart is not the newest
        run's end, or when forceNewRun (a transport jump). Never allocates, never locks.

        A new run that starts BEFORE an older run's end drops that older run: stream time only moves
        forward, so such a run is "from the future" and its slots may be overwritten by this one —
        keeping it would let a read return material that is no longer there. Every run that is kept
        therefore lies entirely before the newer ones, which is what makes `read`'s validity bound
        (latestEnd - capacity) exact for all of them. */
    void write (int64_t streamStart, int numFrames, const float* const* src, int numSrcChannels,
                bool forceNewRun) noexcept
    {
        if (numFrames <= 0)
            return;

        seq.fetch_add (1, std::memory_order_relaxed);               // odd: a write is in progress
        std::atomic_thread_fence (std::memory_order_release);

        int n = numRuns.load (std::memory_order_relaxed);

        if (n == 0 || forceNewRun || streamStart != runs[0].end.load (std::memory_order_relaxed))
        {
            // Forget the runs that reach into the stretch about to be written (see above).
            int drop = 0;   // runs[0..drop) are the newer ones that reach into the new span

            while (drop < n && runs[(size_t) drop].end.load (std::memory_order_relaxed) > streamStart)
                ++drop;

            if (drop > 0)
            {
                for (int i = drop; i < n; ++i)
                {
                    runs[(size_t) (i - drop)].start.store (runs[(size_t) i].start.load (std::memory_order_relaxed), std::memory_order_relaxed);
                    runs[(size_t) (i - drop)].end.store   (runs[(size_t) i].end.load   (std::memory_order_relaxed), std::memory_order_relaxed);
                }

                n -= drop;
            }

            const int last = std::min (n, maxRuns - 1);

            for (int i = last; i > 0; --i)
            {
                runs[(size_t) i].start.store (runs[(size_t) (i - 1)].start.load (std::memory_order_relaxed), std::memory_order_relaxed);
                runs[(size_t) i].end.store   (runs[(size_t) (i - 1)].end.load   (std::memory_order_relaxed), std::memory_order_relaxed);
            }

            runs[0].start.store (streamStart, std::memory_order_relaxed);
            runs[0].end.store (streamStart + numFrames, std::memory_order_relaxed);
            numRuns.store (std::min (n + 1, maxRuns), std::memory_order_relaxed);
        }
        else
        {
            runs[0].end.store (streamStart + numFrames, std::memory_order_relaxed);
        }

        // A block longer than the ring keeps only its tail: the older part is already "overwritten".
        const int skip = numFrames > capacity ? numFrames - capacity : 0;
        const int count = numFrames - skip;
        const int mask = capacity - 1;
        const int first = (int) ((uint64_t) (streamStart + skip) & (uint64_t) mask);
        const int slice1 = std::min (count, capacity - first);

        for (int c = 0; c < numChannels; ++c)
        {
            float* dst = data.data() + (size_t) c * (size_t) capacity;
            const int srcCh = numSrcChannels <= 0 ? -1 : std::min (c, numSrcChannels - 1);

            if (srcCh < 0 || src == nullptr || src[srcCh] == nullptr)
            {
                std::fill (dst + first, dst + first + slice1, 0.0f);
                std::fill (dst, dst + (count - slice1), 0.0f);
                continue;
            }

            const float* s = src[srcCh] + skip;
            std::copy (s, s + slice1, dst + first);
            std::copy (s + slice1, s + count, dst);
        }

        std::atomic_thread_fence (std::memory_order_release);
        seq.fetch_add (1, std::memory_order_release);               // even: the write is complete
    }

    struct ReadResult
    {
        int framesCovered = 0;
        bool torn = false;
    };

    /** AUDIO THREAD, any number of readers. Fills dest[c][0..numFrames) with what was written for
        [streamStart, streamStart + numFrames); ZERO where no run covers it or where the ring has
        already overwritten it (older than getLatestEnd() - capacity). Destination channels beyond the
        ring's are zeroed. torn = a write happened during the read (seqlock): dest is zeroed entirely. */
    ReadResult read (int64_t streamStart, int numFrames, float* const* dest, int numDestChannels) const noexcept
    {
        ReadResult result;

        if (numFrames <= 0)
            return result;

        for (int c = 0; c < numDestChannels; ++c)
            if (dest[c] != nullptr)
                std::fill (dest[c], dest[c] + numFrames, 0.0f);

        const uint32_t s1 = seq.load (std::memory_order_acquire);

        if ((s1 & 1u) != 0)
        {
            result.torn = true;
            return result;
        }

        const int n = numRuns.load (std::memory_order_relaxed);
        int64_t latestEnd = 0;

        if (n > 0)
            latestEnd = runs[0].end.load (std::memory_order_relaxed);

        const int64_t validFrom = latestEnd - (int64_t) capacity;
        const int64_t reqEnd = streamStart + numFrames;
        const int mask = capacity - 1;
        const int channelsToCopy = std::min (numDestChannels, numChannels);
        int covered = 0;

        for (int i = 0; i < n; ++i)      // newest first; the kept runs are disjoint (see write)
        {
            const int64_t lo = std::max ({ streamStart, runs[(size_t) i].start.load (std::memory_order_relaxed), validFrom });
            const int64_t hi = std::min (reqEnd, runs[(size_t) i].end.load (std::memory_order_relaxed));

            if (hi <= lo)
                continue;

            const int count = (int) (hi - lo);
            const int destOffset = (int) (lo - streamStart);
            const int first = (int) ((uint64_t) lo & (uint64_t) mask);
            const int slice1 = std::min (count, capacity - first);

            for (int c = 0; c < channelsToCopy; ++c)
            {
                if (dest[c] == nullptr)
                    continue;

                const float* src = data.data() + (size_t) c * (size_t) capacity;
                std::copy (src + first, src + first + slice1, dest[c] + destOffset);
                std::copy (src, src + (count - slice1), dest[c] + destOffset + slice1);
            }

            covered += count;
        }

        std::atomic_thread_fence (std::memory_order_acquire);

        if (seq.load (std::memory_order_relaxed) != s1)
        {
            for (int c = 0; c < numDestChannels; ++c)
                if (dest[c] != nullptr)
                    std::fill (dest[c], dest[c] + numFrames, 0.0f);

            result.torn = true;
            return result;
        }

        result.framesCovered = covered;
        return result;
    }

private:
    static constexpr int maxRuns = 4;   // newest first

    std::vector<float> data;            // channel-major, numChannels x capacity
    int numChannels = 2, capacity = 0;
    std::array<Run, maxRuns> runs;
    std::atomic<int> numRuns { 0 };
    std::atomic<uint32_t> seq { 0 };    // odd while writing
};

//==============================================================================
// Latency arithmetic (docs/plan_sidechain.md §4.2). Ages are in stream samples.

enum class ReaderStatus { aligned, late, overDeclared, sourceAbsent };

struct ReaderResolution
{
    int delay = 0;
    ReaderStatus status = ReaderStatus::sourceAbsent;
    int alignmentError = 0;
};

/** X = max(L_ref, cachedAge): what a reader DECLARES. A negative cachedAge means "never built". */
inline int declaredLatency (int referenceLatency, int cachedSourceAge) noexcept
{
    return std::max (referenceLatency, std::max (0, cachedSourceAge));
}

/** Once the tap's TRUE age is known. sourceBuilt false -> {0, sourceAbsent, 0}. trueAge > declared ->
    {0, late, trueAge - declared}: the key arrives late and nothing can be done but build again.
    Otherwise delay = declared - trueAge, and the status is overDeclared iff the reader says more
    than it needs to (declared > max(referenceLatency, trueAge)). */
inline ReaderResolution resolve (int referenceLatency, int declared, bool sourceBuilt, int trueAge) noexcept
{
    ReaderResolution r;

    if (! sourceBuilt)
        return r;

    if (trueAge > declared)
    {
        r.delay = 0;
        r.status = ReaderStatus::late;
        r.alignmentError = trueAge - declared;
        return r;
    }

    r.delay = declared - trueAge;
    r.status = declared > std::max (referenceLatency, trueAge) ? ReaderStatus::overDeclared
                                                                : ReaderStatus::aligned;
    return r;
}

// Phase 2 (an aux's input): every reader of one aux shares one declared X = max(ref, max cached),
// and the group is late / over-declared as a whole. The signature is reserved here and implemented
// in step 2.1 — nothing in phase 1 calls it.

/** Ring sizing: the next power of two >= maxDelay + 2 x blockSize, and >= 4 x blockSize. */
inline int requiredRingCapacity (int maxDelay, int blockSize) noexcept
{
    const int64_t block = std::max (1, blockSize);
    const int64_t need = std::max<int64_t> (std::max<int64_t> (0, maxDelay) + 2 * block, 4 * block);
    int64_t c = 2;

    while (c < need && c < ((int64_t) 1 << 30))
        c <<= 1;

    return (int) c;
}

/** A cached age taken at another sample rate: round (age x newRate / oldRate). An unknown rate (or an
    unknown age) gives -1, "never built". */
inline int scaleCachedAge (int cachedAge, double cachedRate, double newRate) noexcept
{
    if (cachedAge < 0 || cachedRate <= 0.0 || newRate <= 0.0)
        return -1;

    if (cachedRate == newRate)
        return cachedAge;

    return (int) ((double) cachedAge * newRate / cachedRate + 0.5);
}

}}} // namespace tracktion::engine::objbridge
