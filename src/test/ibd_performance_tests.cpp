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
#include <hash.h>
#include <net.h>
#include <net_processing.h>
#include <netmessagemaker.h>
#include <protocol.h>
#include <streams.h>
#include <consensus/params.h>
#include <primitives/block.h>
#include <test/test_bitcoin.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstdint>
#include <vector>

struct IBDPerformanceTestingSetup : public TestingSetup
{
    IBDPerformanceTestingSetup() : TestingSetup(CBaseChainParams::REGTEST) {}
};


static void QueueHeadersMessage(CNode& peer, const std::vector<CBlockHeader>& headers)
{
    const CChainParams& chainparams = Params();
    CSerializedNetMsg serialized =
        CNetMsgMaker(PROTOCOL_VERSION).Make(NetMsgType::HEADERS, headers);

    CMessageHeader header(chainparams.MessageStart(),
                          serialized.command.c_str(),
                          serialized.data.size());
    const uint256 checksum = Hash(serialized.data.begin(), serialized.data.end());
    memcpy(header.pchChecksum, checksum.begin(), CMessageHeader::CHECKSUM_SIZE);

    CDataStream header_stream(SER_NETWORK, PROTOCOL_VERSION);
    header_stream << header;

    CNetMessage incoming(chainparams.MessageStart(), SER_NETWORK, PROTOCOL_VERSION);
    BOOST_REQUIRE_EQUAL(
        incoming.readHeader(reinterpret_cast<const char*>(header_stream.data()),
                            header_stream.size()),
        0);
    BOOST_REQUIRE_EQUAL(
        incoming.readData(reinterpret_cast<const char*>(serialized.data.data()),
                          serialized.data.size()),
        0);
    BOOST_REQUIRE(incoming.complete());

    LOCK(peer.cs_vProcessMsg);
    peer.nProcessQueueSize += incoming.vRecv.size() + CMessageHeader::HEADER_SIZE;
    peer.vProcessMsg.push_back(std::move(incoming));
}

BOOST_FIXTURE_TEST_SUITE(ibd_performance_tests, IBDPerformanceTestingSetup)

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



BOOST_AUTO_TEST_CASE(headers_message_requests_next_batch)
{
    static const size_t HEADERS_PER_BATCH = MAX_HEADERS_RESULTS;

    const CChainParams& chainparams = Params();
    BOOST_REQUIRE(chainActive.Tip() != nullptr);
    BOOST_REQUIRE(IsInitialBlockDownload());

    SOCKET socket = INVALID_SOCKET;
    in_addr ipv4_addr;
    ipv4_addr.s_addr = 0x0100007f;
    CAddress address(CService(ipv4_addr, 18444), NODE_NETWORK);
    CNode peer(1, NODE_NETWORK, 0, socket, address, 0, 0,
               CAddress(), std::string(), false);
    peer.SetRecvVersion(PROTOCOL_VERSION);
    peer.SetSendVersion(PROTOCOL_VERSION);

    CConnmanTest::AddNode(peer);
    peerLogic->InitializeNode(&peer);

    CBlockHeader previous = chainparams.GenesisBlock().GetBlockHeader();
    uint256 previous_hash = previous.GetHash();
    std::vector<CBlockHeader> headers;
    headers.reserve(HEADERS_PER_BATCH);

    for (size_t i = 0; i < HEADERS_PER_BATCH; ++i) {
        CBlockHeader header;
        header.nVersion = previous.nVersion;
        header.hashPrevBlock = previous_hash;
        header.hashMerkleRoot.SetNull();
        header.nTime = previous.nTime + 1;
        header.nBits = previous.nBits;
        header.nNonce = static_cast<uint32_t>(i + 1);
        headers.push_back(header);
        previous = header;
        previous_hash = header.GetHash();
    }

    QueueHeadersMessage(peer, headers);

    const size_t send_before = peer.vSendMsg.size();
    std::atomic<bool> interrupt(false);
    peerLogic->ProcessMessages(&peer, interrupt);
    const size_t send_after = peer.vSendMsg.size();

    BOOST_CHECK_EQUAL(send_after, send_before + 1);
    BOOST_CHECK(mapBlockIndex.count(previous_hash) == 1);

    bool update_connection_time = false;
    peerLogic->FinalizeNode(peer.GetId(), update_connection_time);
    CConnmanTest::ClearNodes();
}

BOOST_AUTO_TEST_SUITE_END()
