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
#include <consensus/validation.h>
#include <primitives/block.h>
#include <test/test_bitcoin.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstdint>
#include <vector>

// Run these tests on REGTEST so they are deterministic and do not depend on
// the user's mainnet datadir or on a live network connection.
struct IBDPerformanceTestingSetup : public TestingSetup
{
    IBDPerformanceTestingSetup() : TestingSetup(CBaseChainParams::REGTEST) {}
};


// Place a serialized P2P message in the mock peer's normal receive queue.
static void QueueNetMessage(CNode& peer, CSerializedNetMsg&& serialized)
{
    const CChainParams& chainparams = Params();
    // Construct the 24-byte network message header and checksum so the normal
    // message parser accepts this as a genuine inbound P2P message.
    CMessageHeader header(chainparams.MessageStart(),
                          serialized.command.c_str(),
                          serialized.data.size());
    const uint256 checksum = Hash(serialized.data.begin(), serialized.data.end());
    memcpy(header.pchChecksum, checksum.begin(), CMessageHeader::CHECKSUM_SIZE);

    CDataStream header_stream(SER_NETWORK, PROTOCOL_VERSION);
    header_stream << header;

    // Feed the serialized bytes through CNetMessage's normal header/data
    // parsing path.  A failure here means the synthetic wire message itself is
    // malformed, not that header validation is slow.
    CNetMessage incoming(chainparams.MessageStart(), SER_NETWORK, PROTOCOL_VERSION);
    BOOST_REQUIRE(
        incoming.readHeader(reinterpret_cast<const char*>(header_stream.data()),
                            header_stream.size()) > 0);
    BOOST_REQUIRE(
        incoming.readData(reinterpret_cast<const char*>(serialized.data.data()),
                        serialized.data.size()) > 0);
    BOOST_REQUIRE(incoming.complete());

    // Queue the completed message exactly where ProcessMessages() expects an
    // inbound peer message.  No production function visibility is changed.
    LOCK(peer.cs_vProcessMsg);
    peer.nProcessQueueSize += incoming.vRecv.size() + CMessageHeader::HEADER_SIZE;
    peer.vProcessMsg.push_back(std::move(incoming));
}

static void QueueHeadersMessage(CNode& peer, const std::vector<CBlockHeader>& headers)
{
    // Empty CBlocks include the zero transaction count required after each
    // header on the wire, matching the production GETHEADERS response.
    const std::vector<CBlock> wire_headers(headers.begin(), headers.end());
    QueueNetMessage(peer, CNetMsgMaker(PROTOCOL_VERSION).Make(
        NetMsgType::HEADERS, wire_headers));
}

BOOST_FIXTURE_TEST_SUITE(ibd_performance_tests, IBDPerformanceTestingSetup)

BOOST_AUTO_TEST_CASE(headers_are_prioritized_during_ibd)
{
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
    peer.fSuccessfullyConnected = true;
    peer.nVersion = PROTOCOL_VERSION;

    CConnmanTest::AddNode(peer);
    peerLogic->InitializeNode(&peer);
    peer.fPauseSend = false;

    // Queue an ordinary message first, as a stand-in for already received
    // block traffic. The following header must be selected on the first call.
    QueueNetMessage(peer, CNetMsgMaker(PROTOCOL_VERSION).Make(
        NetMsgType::PING, uint64_t{1}));

    CBlockHeader header;
    header.nVersion = 0x20000000;
    header.hashPrevBlock = chainActive.Tip()->GetBlockHash();
    header.hashMerkleRoot.SetNull();
    header.nTime = chainActive.Tip()->GetBlockTime() + 600;
    header.nBits = chainActive.Tip()->nBits;
    while (!CheckProofOfWork(header.GetHash(), header.nBits,
                             chainparams.GetConsensus())) {
        ++header.nNonce;
    }
    QueueHeadersMessage(peer, {header});

    std::atomic<bool> interrupt(false);
    BOOST_CHECK(peerLogic->ProcessMessages(&peer, interrupt));
    BOOST_CHECK_EQUAL(mapBlockIndex.count(header.GetHash()), 1U);
    {
        LOCK(peer.cs_vProcessMsg);
        BOOST_REQUIRE_EQUAL(peer.vProcessMsg.size(), 1U);
        BOOST_CHECK_EQUAL(peer.vProcessMsg.front().hdr.GetCommand(),
                          NetMsgType::PING);
    }

    bool update_connection_time = false;
    peerLogic->FinalizeNode(peer.GetId(), update_connection_time);
    CConnmanTest::ClearNodes();
}

BOOST_AUTO_TEST_CASE(header_batch_processing_benchmark)
{
    // Inputs for this benchmark.  Increase BATCH_COUNT for a longer run.
    // 2000 matches the protocol's normal maximum HEADERS response size.
    static const size_t HEADERS_PER_BATCH = 2000;
    static const size_t BATCH_COUNT = 10;

    const CChainParams& chainparams = Params();
    const Consensus::Params& consensus = chainparams.GetConsensus();

    BOOST_REQUIRE(chainActive.Tip() != nullptr);
    BOOST_REQUIRE(IsInitialBlockDownload());

    const CBlockIndex* tip = chainActive.Tip();

    CBlockHeader previous = tip->GetBlockHeader();
    uint256 previous_hash = tip->GetBlockHash();

    const auto benchmark_start = std::chrono::steady_clock::now();
    int64_t total_headers = 0;

    for (size_t batch = 0; batch < BATCH_COUNT; ++batch) {
        std::vector<CBlockHeader> headers;
        headers.reserve(HEADERS_PER_BATCH);

        // Generate one continuous synthetic header chain.  Each new header
        // points to the hash of the previous one, just like a real chain.
        for (size_t i = 0; i < HEADERS_PER_BATCH; ++i) {
            CBlockHeader header;
            header.nVersion = 0x20000000;
            header.hashPrevBlock = previous_hash;
            header.hashMerkleRoot.SetNull();
            header.nTime = previous.nTime + 600;
            header.nBits = previous.nBits;
            header.nNonce = 0;
            while (!CheckProofOfWork(header.GetHash(),
                                    header.nBits,
                                    consensus)) {
                header.nNonce++;
            }
            headers.push_back(header);
            previous = header;
            previous_hash = header.GetHash();
        }

        CValidationState state;
        const CBlockIndex* pindex_last = nullptr;

        // Time only the production header-validation/index-insertion path.
        // Header construction above is deliberately outside this timer.
        const auto batch_start = std::chrono::steady_clock::now();

        bool result = ProcessNewBlockHeaders(headers, state, chainparams, &pindex_last);

        BOOST_REQUIRE_MESSAGE(
            result,
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
            "STATE height=" << chainActive.Height()
            << " map=" << mapBlockIndex.size());

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
    // A full HEADERS response is the important boundary: production code
    // should interpret MAX_HEADERS_RESULTS (normally 2000) as 'peer may have
    // more' and immediately queue the next GETHEADERS request.
    static const size_t HEADERS_PER_BATCH = MAX_HEADERS_RESULTS;

    const CChainParams& chainparams = Params();
    BOOST_REQUIRE(chainActive.Tip() != nullptr);
    BOOST_REQUIRE(IsInitialBlockDownload());

    // This is an in-process mock peer: no TCP connection is opened.  The peer
    // exists only so the real message-processing code has normal CNode state.
    SOCKET socket = INVALID_SOCKET;
    in_addr ipv4_addr;
    ipv4_addr.s_addr = 0x0100007f;
    CAddress address(CService(ipv4_addr, 18444), NODE_NETWORK);
    CNode peer(1, NODE_NETWORK, 0, socket, address, 0, 0,
               CAddress(), std::string(), false);
    peer.SetRecvVersion(PROTOCOL_VERSION);
    peer.SetSendVersion(PROTOCOL_VERSION);
    peer.fSuccessfullyConnected = true;
    peer.nVersion = PROTOCOL_VERSION;

    CConnmanTest::AddNode(peer);
    peerLogic->InitializeNode(&peer);
    // The fixture does not configure a send-buffer limit; initialization
    // queues VERSION and may pause sending. Allow the inbound message through.
    peer.fPauseSend = false;

    const CBlockIndex* tip = chainActive.Tip();

    CBlockHeader previous = tip->GetBlockHeader();
    uint256 previous_hash = tip->GetBlockHash();

    std::vector<CBlockHeader> headers;
    headers.reserve(HEADERS_PER_BATCH);

    // Create exactly one full 2000-header response, continuous from genesis.
    for (size_t i = 0; i < HEADERS_PER_BATCH; ++i) {
        CBlockHeader header;
        header.nVersion = 0x20000000;
        header.hashPrevBlock = previous_hash;
        header.hashMerkleRoot.SetNull();
        header.nTime = previous.nTime + 600;
        header.nBits = previous.nBits;

        header.nNonce = 0;
        while (!CheckProofOfWork(header.GetHash(),
                                 header.nBits,
                                 chainparams.GetConsensus())) {
            header.nNonce++;
        }

        headers.push_back(header);
        previous = header;
        previous_hash = header.GetHash();
    }

    // Put the synthetic HEADERS message into the peer's normal receive queue.
    QueueHeadersMessage(peer, headers);

    // Record the send queue size before processing.  Processing a full 2000
    // HEADERS message should append exactly one continuation GETHEADERS.
    const size_t send_before = peer.vSendMsg.size();
    std::atomic<bool> interrupt(false);
    // Public production entry point.  Internally this dispatches HEADERS to the
    // existing static ProcessHeadersMessage() without exposing that function.
    while (peerLogic->ProcessMessages(&peer, interrupt)) {
    }
    const size_t send_after = peer.vSendMsg.size();

    bool update_connection_time = false;
    peerLogic->FinalizeNode(peer.GetId(), update_connection_time);
    CConnmanTest::ClearNodes();

    // Each nonempty outbound message has separate header and payload buffers.
    BOOST_REQUIRE_EQUAL(send_after, send_before + 2);
    CDataStream response_header(peer.vSendMsg[send_before], SER_NETWORK, PROTOCOL_VERSION);
    CMessageHeader response(chainparams.MessageStart());
    response_header >> response;
    BOOST_CHECK_EQUAL(response.GetCommand(), NetMsgType::GETHEADERS);
    BOOST_CHECK_EQUAL(response.nMessageSize, peer.vSendMsg[send_before + 1].size());

    CDataStream response_payload(peer.vSendMsg[send_before + 1], SER_NETWORK, PROTOCOL_VERSION);
    CBlockLocator locator;
    uint256 stop_hash;
    response_payload >> locator >> stop_hash;
    BOOST_REQUIRE(!locator.vHave.empty());
    BOOST_CHECK(locator.vHave.front() == previous_hash);
    BOOST_CHECK(stop_hash.IsNull());
    BOOST_CHECK(response_payload.empty());

    // The final header reached the real block-index processing path.
    BOOST_CHECK_EQUAL(mapBlockIndex.count(previous_hash), 1U);
}

BOOST_AUTO_TEST_SUITE_END()
