#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

#include <algorithm>

/*==============================================================================
  PedalBoard - lock free transport between the three audio threads.

  Two AudioDeviceManager instances and the VB-CABLE stream each call back on
  their own thread. Those threads are coupled only through the objects below, so
  no thread ever waits on another one:

    * AudioRingBuffer  - single producer / single consumer sample queue. When it
      is full the producer drops the oldest samples rather than blocking, and
      when it is empty the consumer emits silence. Both events are counted and
      surfaced in the UI instead of being hidden.
    * SeqSnapshot      - the effect chain order, published by the message thread
      and read once per block by the audio thread.
==============================================================================*/
namespace pedal
{

/**
    Single producer, single consumer interleaved sample queue.

    Producer is always the higher priority thread that owns the clock we are
    feeding from; consumer advances by whole blocks so the callback size stays
    constant.
*/
class AudioRingBuffer
{
public:
    /** Must be called from a thread where neither end is running (device closed). */
    void reset (int numChannels, int capacityPerChannel) noexcept
    {
        channels = numChannels;
        capacity = capacityPerChannel;
        auto size = 1;
        while (size < capacity)
            size <<= 1;
        slotSize = size;
        mask = size - 1;
        storage.assign (static_cast<std::size_t> (channels) * static_cast<std::size_t> (size), 0.0f);
        writeHead.store (0, std::memory_order_relaxed);
        readHead.store (0, std::memory_order_relaxed);
        overflowCount.store (0, std::memory_order_relaxed);
        underflowCount.store (0, std::memory_order_relaxed);
        primed.store (false, std::memory_order_relaxed);
    }

    int getCapacity() const noexcept { return slotSize; }
    int getNumChannels() const noexcept { return channels; }

    int getAvailableRead() const noexcept
    {
        const auto w = writeHead.load (std::memory_order_acquire);
        const auto r = readHead.load (std::memory_order_relaxed);
        return static_cast<int> (w - r);
    }

    int getOverflowCount() const noexcept  { return overflowCount.load (std::memory_order_relaxed); }
    int getUnderflowCount() const noexcept { return underflowCount.load (std::memory_order_relaxed); }

    /** Zeroes the two diagnostic counters. Neither head moves, so any thread may
        call it; it exists so a restarted stream is reported on its own record
        instead of on the accumulated total of every stream since launch. */
    void resetCounters() noexcept
    {
        overflowCount.store (0, std::memory_order_relaxed);
        underflowCount.store (0, std::memory_order_relaxed);

        /*  A new stream also starts unprimed. The output callback can run before
            the input callback has produced its first block, and that first empty
            read is the stream starting, not the consumer starving. Counting it
            would put a permanent "1" on the status bar of every healthy launch. */
        primed.store (false, std::memory_order_relaxed);
    }

    /** Producer side. Drops the oldest samples if the queue would overflow.

        Only the producer touches writeHead, only the consumer touches
        readHead - except here, where on overflow the producer deliberately
        pushes the consumer's head forward so that stale audio is discarded
        rather than the producer being made to wait. That costs one glitched
        block in an already late situation, which is the correct trade for a
        realtime thread never blocking. */
    void write (const float* const* source, int numCh, int numSamples) noexcept
    {
        if (numSamples <= 0 || channels == 0)
            return;

        const auto toWrite = std::min (numSamples, slotSize);
        const auto w = writeHead.load (std::memory_order_relaxed);
        const auto r = readHead.load (std::memory_order_acquire);

        if (w + toWrite - r > slotSize)
        {
            advanceReadHead (w + toWrite - slotSize);
            overflowCount.fetch_add (1, std::memory_order_relaxed);
        }

        const auto ch = std::min (numCh, channels);

        for (int c = 0; c < ch; ++c)
        {
            auto* base = storage.data() + static_cast<std::size_t> (c) * slotSize;
            const auto first = static_cast<int> (w & mask);
            const auto head = std::min (toWrite, slotSize - first);

            std::memcpy (base + first, source[c], static_cast<std::size_t> (head) * sizeof (float));

            if (head < toWrite)
                std::memcpy (base, source[c] + head,
                             static_cast<std::size_t> (toWrite - head) * sizeof (float));
        }

        writeHead.store (w + toWrite, std::memory_order_release);
        primed.store (true, std::memory_order_release);
    }

    /** Consumer side. Fills numSamples so the callback length stays fixed;
        missing input is zero filled, and reported as an underflow once the
        producer has written at least one block (see primed).

        The read head only ever moves forward, and never past the write head. An
        earlier version advanced it by numSamples unconditionally, so a single
        underflow left readHead ahead of writeHead; because the producer and the
        consumer then advanced at the same rate, the deficit never healed and the
        output was silence for the rest of the session.
        @returns the number of real samples copied. */
    int read (float* const* destination, int numCh, int numSamples) noexcept
    {
        if (numSamples <= 0 || channels == 0)
            return 0;

        const auto w = writeHead.load (std::memory_order_acquire);
        auto r = readHead.load (std::memory_order_relaxed);

        int available = static_cast<int> (w - r);
        if (available < 0)
            available = 0;

        const auto toCopy = std::min (available, numSamples);

        // Only starvation after the producer has delivered something counts. The
        // very first block arrives before the input path exists at all.
        if (toCopy < numSamples && primed.load (std::memory_order_acquire))
            underflowCount.fetch_add (1, std::memory_order_relaxed);

        const auto ch = std::min (numCh, channels);

        for (int c = 0; c < ch; ++c)
        {
            const auto* base = storage.data() + static_cast<std::size_t> (c) * slotSize;
            auto first = static_cast<int> (r & mask);
            auto head = std::min (toCopy, slotSize - first);

            std::memcpy (destination[c], base + first, static_cast<std::size_t> (head) * sizeof (float));

            if (head < toCopy)
                std::memcpy (destination[c] + head, base,
                             static_cast<std::size_t> (toCopy - head) * sizeof (float));

            if (toCopy < numSamples)
                std::memset (destination[c] + toCopy, 0,
                             static_cast<std::size_t> (numSamples - toCopy) * sizeof (float));
        }

        advanceReadHead (std::min (r + numSamples, w));
        return toCopy;
    }

    /** Number of samples the consumer will be able to read at most. */
    int peekAvailable() const noexcept
    {
        const auto w = writeHead.load (std::memory_order_acquire);
        const auto r = readHead.load (std::memory_order_acquire);
        return static_cast<int> (std::max (int64_t (0), w - r));
    }

    /** Consumer-side flush: drops everything unread.

        Only the thread that calls read() may do this, because it is the only one
        allowed to move readHead. Use it after a clock change, when what is queued
        was sampled at a rate that no longer exists. */
    void flushAsConsumer() noexcept
    {
        readHead.store (writeHead.load (std::memory_order_acquire), std::memory_order_release);
    }

    /** Producer-side flush: abandons whatever has not been consumed yet.

        Only the thread that calls write() may do this, because it is the only one
        allowed to move writeHead. Symmetrical with flushAsConsumer() for the other
        end of the same ring. */
    void flushAsProducer() noexcept
    {
        writeHead.store (readHead.load (std::memory_order_acquire), std::memory_order_release);
    }

private:
    /** Moves the read head forward, never backwards. Both ends call it: on
        overflow the producer discards the oldest audio, on underflow the
        consumer catches up with what actually exists. A plain store would let
        the slower of the two write a stale, smaller head back over the other's
        decision. */
    void advanceReadHead (int64_t target) noexcept
    {
        auto current = readHead.load (std::memory_order_relaxed);

        while (target > current)
            if (readHead.compare_exchange_weak (current, target,
                                                std::memory_order_release,
                                                std::memory_order_relaxed))
                break;
    }

    std::vector<float> storage;
    std::atomic<int64_t> writeHead { 0 }, readHead { 0 };
    std::atomic<int> overflowCount { 0 }, underflowCount { 0 };
    std::atomic<bool> primed { false };
    int channels = 0, capacity = 0, slotSize = 0, mask = 0;
};

/**
    A three slot seqlock used to publish a plain data snapshot from the message
    thread to one audio thread.

    The writer never blocks. The reader copies the whole structure once per
    callback and retries only if it happened to straddle a write, which is a
    handful of nanoseconds for a 16 entry pointer array.
*/
template <typename Payload>
class SeqSnapshot
{
public:
    void publish (const Payload& value) noexcept
    {
        const auto s = sequence.load (std::memory_order_relaxed);
        sequence.store (s + 1, std::memory_order_release);   // odd: write in progress
        payload = value;
        sequence.store (s + 2, std::memory_order_release);   // even: visible
    }

    Payload acquire() const noexcept
    {
        /*  Bounded, because this runs on the audio thread. An unbounded retry is
            correct but not real-time safe: if the writer is preempted between its
            two sequence stores, the reader would spin for as long as the scheduler
            keeps it off the CPU. Four attempts cost nanoseconds; if they all lose,
            the previous good snapshot is one block old, which is exactly what the
            chain would have rendered anyway. Only this thread touches `lastGood`. */
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            const auto s1 = sequence.load (std::memory_order_acquire);
            if (s1 & 1)
                continue;

            auto copy = payload;
            const auto s2 = sequence.load (std::memory_order_acquire);

            if (s1 == s2)
            {
                lastGood = copy;
                return copy;
            }
        }

        return lastGood;
    }

private:
    mutable Payload payload {};
    mutable Payload lastGood {};
    std::atomic<uint32_t> sequence { 0 };
};

} // namespace pedal
