// Copyright (c) 2026 The Sugarchain developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef SUGARCHAIN_IBD_METRICS_H
#define SUGARCHAIN_IBD_METRICS_H

#include <util.h>

#include <atomic>
#include <cstdint>

namespace ibdmetrics {

struct Counters {
    std::atomic<int64_t> received{0}, pow_calls{0}, pow_hits{0}, pow_misses{0}, pow_us{0};
    std::atomic<int64_t> disk_writes{0}, disk_us{0}, connected{0}, connect_us{0};
    std::atomic<int64_t> next_log_us{0}, last_log_us{0};
};

inline Counters& GetCounters() { static Counters counters; return counters; }
inline bool Enabled() { static const bool enabled = gArgs.GetBoolArg("-ibdmetrics", false); return enabled; }
inline int64_t Now() { return GetTimeMicros(); }

inline void MaybeLog()
{
    Counters& c = GetCounters();
    const int64_t now = Now();
    int64_t next = c.next_log_us.load(std::memory_order_relaxed);
    if (next == 0) {
        if (c.next_log_us.compare_exchange_strong(next, now + 10000000, std::memory_order_relaxed))
            c.last_log_us.store(now, std::memory_order_relaxed);
        return;
    }
    if (now < next || !c.next_log_us.compare_exchange_strong(next, now + 10000000, std::memory_order_relaxed))
        return;

    const int64_t last = c.last_log_us.exchange(now, std::memory_order_relaxed);
    const int64_t received = c.received.exchange(0, std::memory_order_relaxed);
    const int64_t pow_calls = c.pow_calls.exchange(0, std::memory_order_relaxed);
    const int64_t pow_hits = c.pow_hits.exchange(0, std::memory_order_relaxed);
    const int64_t pow_misses = c.pow_misses.exchange(0, std::memory_order_relaxed);
    const int64_t pow_us = c.pow_us.exchange(0, std::memory_order_relaxed);
    const int64_t disk_writes = c.disk_writes.exchange(0, std::memory_order_relaxed);
    const int64_t disk_us = c.disk_us.exchange(0, std::memory_order_relaxed);
    const int64_t connected = c.connected.exchange(0, std::memory_order_relaxed);
    const int64_t connect_us = c.connect_us.exchange(0, std::memory_order_relaxed);

    LogPrintf("IBDMETRICS interval_us=%d received=%d pow_calls=%d pow_hits=%d "
              "pow_misses=%d pow_us=%d disk_writes=%d disk_us=%d connected=%d connect_us=%d\n",
              now - last, received, pow_calls, pow_hits, pow_misses, pow_us,
              disk_writes, disk_us, connected, connect_us);
}

inline void RecordReceived()
{
    if (!Enabled()) return;
    GetCounters().received.fetch_add(1, std::memory_order_relaxed);
    MaybeLog();
}

inline void RecordPow(bool cache_hit, int64_t elapsed_us)
{
    if (!Enabled()) return;
    Counters& c = GetCounters();
    c.pow_calls.fetch_add(1, std::memory_order_relaxed);
    (cache_hit ? c.pow_hits : c.pow_misses).fetch_add(1, std::memory_order_relaxed);
    c.pow_us.fetch_add(elapsed_us, std::memory_order_relaxed);
    MaybeLog();
}

inline void RecordDiskWrite(int64_t elapsed_us)
{
    if (!Enabled()) return;
    Counters& c = GetCounters();
    c.disk_writes.fetch_add(1, std::memory_order_relaxed);
    c.disk_us.fetch_add(elapsed_us, std::memory_order_relaxed);
    MaybeLog();
}

inline void RecordConnected(int64_t elapsed_us)
{
    if (!Enabled()) return;
    Counters& c = GetCounters();
    c.connected.fetch_add(1, std::memory_order_relaxed);
    c.connect_us.fetch_add(elapsed_us, std::memory_order_relaxed);
    MaybeLog();
}

} // namespace ibdmetrics

#endif // SUGARCHAIN_IBD_METRICS_H
