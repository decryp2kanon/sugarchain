// Copyright (c) 2026 The Sugarchain developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/*
 * IBD header-processing benchmark/regression test.
 *
 * This deliberately uses the production ProcessNewBlockHeaders() path so that
 * changes to header validation/index insertion are measured by the same code
 * used during IBD.  The benchmark does not impose a wall-clock pass/fail
 * threshold (which would be flaky across machines); it prints throughput so
 * before/after commits can be compared on the same host.
 */

#include <chainparams.h>
#include <consensus/params.h>
#include <primitives/block.h>
#include <net.h>
#include <net_processing.h>
#include <protocol.h>
#include <test/test_bitcoin.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstdint>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(ibd_performance_tests, TestingSetup)

BOOST_AUTO_TEST_CASE(header_batch_processing_benchmark)
{
    static const size_t HEADERS_PER_BATCH = 2000;
    static const size_t BATCH_COUNT = 10;

    const CChainParams& chainparams = Params();
    const Consensus::Params& consensus = chainparams.GetConsensus();

    BOOST_REQUIRE(chainActive.Tip() != nullptr);
    BOOST_REQUIRE(IsInitialBlockDownload());

    CBlockHeader previous = chainparams.GenesisBlock().GetBlockHeader();
    uint256 previous_hash = previous.GetHash();

    const auto benchmark_start = std::chrono::steady_clock::now();
    int64_t total_headers = 0;

    for (size_t batch = 0; batch < BATCH_COUNT; ++batch) {
        std::vector<CBlockHeader> headers;
        headers.reserve(HEADERS_PER_BATCH);

        for (size_t i = 0; i < HEADERS_PER_BATCH; ++i) {
            CBlockHeader header;
            header.nVersion = previous.nVersion;
            header.hashPrevBlock = previous_hash;
            header.hashMerkleRoot.SetNull();
            header.nTime = previous.nTime + 1;
            header.nBits = previous.nBits;
            header.nNonce = static_cast<uint32_t>(batch * HEADERS_PER_BATCH + i + 1);

            headers.push_back(header);
            previous = header;
            previous_hash = header.GetHash();
        }

        CValidationState state;
        const CBlockIndex* pindex_last = nullptr;

        const auto batch_start = std::chrono::steady_clock::now();
        BOOST_REQUIRE_MESSAGE(
            ProcessNewBlockHeaders(headers, state, chainparams, &pindex_last),
            "ProcessNewBlockHeaders failed in batch " << batch
                << ": " << FormatStateMessage(state));
        const auto batch_end = std::chrono::steady_clock::now();

        BOOST_REQUIRE(pindex_last != nullptr);
        total_headers += headers.size();

        const double batch_seconds =
            std::chrono::duration<double>(batch_end - batch_start).count();
        const double batch_rate =
            batch_seconds > 0.0 ? headers.size() / batch_seconds : 0.0;

        BOOST_TEST_MESSAGE(
            "IBD header batch " << (batch + 1) << "/" << BATCH_COUNT
            << ": " << headers.size() << " headers in "
            << batch_seconds << " s (" << batch_rate << " headers/s)");
    }

    const auto benchmark_end = std::chrono::steady_clock::now();
    const double total_seconds =
        std::chrono::duration<double>(benchmark_end - benchmark_start).count();
    const double headers_per_second =
        total_seconds > 0.0 ? total_headers / total_seconds : 0.0;
    const double batches_per_second =
        total_seconds > 0.0 ? BATCH_COUNT / total_seconds : 0.0;

    BOOST_CHECK_EQUAL(total_headers,
                      static_cast<int64_t>(HEADERS_PER_BATCH * BATCH_COUNT));

    BOOST_TEST_MESSAGE(
        "IBD HEADER BENCHMARK: " << total_headers << " headers, "
        << BATCH_COUNT << " x " << HEADERS_PER_BATCH << " batches, "
        << total_seconds << " s, "
        << headers_per_second << " headers/s, "
        << batches_per_second << " batches/s");
}


BOOST_AUTO_TEST_CASE(headers_getheaders_pipeline)
{
    static const size_t HEADERS_PER_BATCH = MAX_HEADERS_RESULTS;
    static const size_t BATCH_COUNT = 5;

    const CChainParams& chainparams = Params();
    BOOST_REQUIRE(chainActive.Tip() != nullptr);
    BOOST_REQUIRE(IsInitialBlockDownload());

    // A mock outbound peer is enough here: ProcessHeadersMessage() is the
    // production receive path and CConnman::PushMessage() queues the next
    // GETHEADERS exactly as it would during IBD.
    CAddress peer_addr(CService(), NODE_NETWORK);
    CNode peer(900001, ServiceFlags(NODE_NETWORK | NODE_WITNESS), 1000000,
               INVALID_SOCKET, peer_addr, 0, 0, CAddress(), "", false);
    peer.SetSendVersion(PROTOCOL_VERSION);
    peer.nVersion = PROTOCOL_VERSION;
    peer.fSuccessfullyConnected = true;
    peerLogic->InitializeNode(&peer);

    CBlockHeader previous = chainparams.GenesisBlock().GetBlockHeader();
    uint256 previous_hash = previous.GetHash();

    int64_t total_headers = 0;
    size_t continuation_requests = 0;
    double total_pipeline_seconds = 0.0;

    for (size_t batch = 0; batch < BATCH_COUNT; ++batch) {
        std::vector<CBlockHeader> headers;
        headers.reserve(HEADERS_PER_BATCH);

        for (size_t i = 0; i < HEADERS_PER_BATCH; ++i) {
            CBlockHeader header;
            header.nVersion = previous.nVersion;
            header.hashPrevBlock = previous_hash;
            header.hashMerkleRoot.SetNull();
            header.nTime = previous.nTime + 1;
            header.nBits = previous.nBits;
            header.nNonce = static_cast<uint32_t>(
                batch * HEADERS_PER_BATCH + i + 1);

            headers.push_back(header);
            previous = header;
            previous_hash = header.GetHash();
        }

        size_t queued_before;
        {
            LOCK(peer.cs_vSend);
            queued_before = peer.vSendMsg.size();
        }

        const auto start = std::chrono::steady_clock::now();
        BOOST_REQUIRE(ProcessHeadersMessage(
            &peer, connman, headers, chainparams, true));
        const auto end = std::chrono::steady_clock::now();

        size_t queued_after;
        {
            LOCK(peer.cs_vSend);
            queued_after = peer.vSendMsg.size();
        }

        // A full MAX_HEADERS_RESULTS response means the peer may have more.
        // The production code must immediately queue the continuation
        // GETHEADERS before returning from ProcessHeadersMessage().
        BOOST_REQUIRE_EQUAL(queued_after, queued_before + 1);

        ++continuation_requests;
        total_headers += headers.size();
        total_pipeline_seconds +=
            std::chrono::duration<double>(end - start).count();

        BOOST_TEST_MESSAGE(
            "IBD pipeline batch " << (batch + 1) << "/" << BATCH_COUNT
            << ": received " << headers.size()
            << " headers and queued next GETHEADERS in "
            << std::chrono::duration<double>(end - start).count() << " s");

        // Pretend the queued GETHEADERS was sent before the next response.
        {
            LOCK(peer.cs_vSend);
            peer.vSendMsg.clear();
            peer.nSendSize = 0;
            peer.nSendOffset = 0;
        }
    }

    BOOST_CHECK_EQUAL(continuation_requests, BATCH_COUNT);
    BOOST_CHECK_EQUAL(total_headers,
                      static_cast<int64_t>(HEADERS_PER_BATCH * BATCH_COUNT));

    const double headers_per_second =
        total_pipeline_seconds > 0.0
            ? total_headers / total_pipeline_seconds : 0.0;
    const double batches_per_second =
        total_pipeline_seconds > 0.0
            ? BATCH_COUNT / total_pipeline_seconds : 0.0;
    const double avg_continuation_ms =
        BATCH_COUNT > 0
            ? (total_pipeline_seconds * 1000.0) / BATCH_COUNT : 0.0;

    BOOST_TEST_MESSAGE(
        "IBD HEADER PIPELINE: " << total_headers << " headers, "
        << continuation_requests << " continuation GETHEADERS, "
        << headers_per_second << " headers/s, "
        << batches_per_second << " batches/s, avg HEADERS->GETHEADERS "
        << avg_continuation_ms << " ms");

    bool update_connection_time = false;
    peerLogic->FinalizeNode(peer.GetId(), update_connection_time);
}

BOOST_AUTO_TEST_SUITE_END()
