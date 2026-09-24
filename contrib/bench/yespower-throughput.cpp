// Copyright (c) 2026 The Sugarchain developers
// Distributed under the MIT software license, see the accompanying file COPYING.

// Raw Yespower throughput diagnostic; this is NOT an end-to-end IBD benchmark.
// Build from the repository root after building libbitcoin_crypto.a:
// g++ -O2 -pthread -Isrc contrib/bench/yespower-throughput.cpp src/crypto/libbitcoin_crypto.a -o /tmp/yespower-throughput
// Uses the exact parameters in primitives/block.cpp; no headers are accepted.
// Run on the target machine without concurrent build/test workloads.
#include <crypto/yespower-1.0.1/yespower.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
int main() {
    const yespower_params_t params{YESPOWER_1_0, 2048, 32,
        (const uint8_t*)"Satoshi Nakamoto 31/Oct/2008 Proof-of-work is essentially one-CPU-one-vote", 74};
    for (unsigned workers : {1, 4, 8, 16}) {
        std::atomic<bool> start{false}, stop{false};
        std::atomic<unsigned long> total{0};
        std::vector<std::thread> threads;
        for (unsigned t=0;t<workers;++t) threads.emplace_back([&,t] {
            yespower_local_t local; yespower_init_local(&local);
            uint8_t input[80]{}; input[0]=t;
            yespower_binary_t out;
            unsigned long n=0;
            while(!start.load()) std::this_thread::yield();
            while(!stop.load()) {
                std::memcpy(input+72,&n,sizeof(n));
                if(yespower(&local,input,sizeof(input),&params,&out)) std::abort();
                ++n;
            }
            total.fetch_add(n); yespower_free_local(&local);
        });
        auto begin=std::chrono::steady_clock::now(); start=true;
        std::this_thread::sleep_for(std::chrono::seconds(15)); stop=true;
        for(auto& thread:threads)thread.join();
        double sec=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
        std::printf("workers=%u hashes=%lu elapsed=%.3f rate=%.1f/s 44.5M_hash_hours=%.2f\n",workers,total.load(),sec,total.load()/sec,44500000./(total.load()/sec)/3600.); std::fflush(stdout);
    }
}
