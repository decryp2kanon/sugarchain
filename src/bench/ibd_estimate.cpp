// Copyright (c) 2026 The Sugarchain developers
// Distributed under the MIT software license, see the accompanying file COPYING.
// Test-only translation unit: access the actual private validation entry points
// without exporting a production API or changing consensus/IBD implementation.
#include <validation.cpp>
#include <crypto/sha256.h>
#include <key.h>
#include <univalue.h>
#include <fstream>
#include <iostream>
#include <chrono>
#include <scheduler.h>

namespace {
void Require(bool ok, const std::string& why) { if (!ok) throw std::runtime_error(why); }
int Skip(int h) { auto invert = [](int n) { return n & (n - 1); }; return h < 2 ? 0 : (h & 1) ? invert(invert(h - 1)) + 1 : invert(h); }
// Plan exactly the same ancestor walk as CBlockIndex::GetAncestor. Both edges
// are materialized; missing fixture ancestry must never be silently invented.
void Route(std::set<int>& needed, int from, int to) {
    if (to < 0 || to > from) return;
    while (from > to) {
        needed.insert(from); needed.insert(from - 1); needed.insert(Skip(from));
        int s = Skip(from), p = Skip(from - 1);
        from = (s == to || (s > to && !(p < s - 2 && p >= to))) ? s : from - 1;
    }
    needed.insert(to);
}
CDiskBlockIndex Index(CDBWrapper& db, const uint256& hash) {
    CDiskBlockIndex d;
    Require(db.Read(std::make_pair('b', hash), d), "source index missing " + hash.ToString());
    Require(d.GetBlockHash() == hash, "source index hash mismatch");
    return d;
}
FILE* SourceFile(const fs::path& source, const char* prefix, int number, unsigned offset) {
    FILE* f = fsbridge::fopen(source / "blocks" / strprintf("%s%05d.dat", prefix, number), "rb");
    Require(f != nullptr, "missing source block/undo file");
    if (fseek(f, offset, SEEK_SET)) { fclose(f); throw std::runtime_error("source seek failed"); }
    return f;
}
struct Sample { CDiskBlockIndex index; CBlock block; CBlockUndo undo; std::vector<unsigned char> raw; };
std::vector<Sample> ReadWindow(CDBWrapper& db, const fs::path& source, const UniValue& w) {
    int count = w["count"].get_int();
    Require(count > 0 && count <= 10000, "invalid window size");
    std::vector<Sample> samples(count);
    uint256 hash = uint256S(w["end_hash"].get_str());
    for (int i = count - 1; i >= 0; --i) {
        auto& s = samples[i]; s.index = Index(db, hash);
        Require(s.index.nHeight == w["end"].get_int() - count + 1 + i, "window height mismatch");
        Require((s.index.nStatus & (BLOCK_HAVE_DATA | BLOCK_HAVE_UNDO)) == (BLOCK_HAVE_DATA | BLOCK_HAVE_UNDO), "source is pruned or has no undo");
        CAutoFile blockfile(SourceFile(source, "blk", s.index.nFile, s.index.nDataPos), SER_DISK, CLIENT_VERSION);
        blockfile >> s.block;
        Require(s.block.GetHash() == hash, "block hash mismatch");
        CAutoFile undofile(SourceFile(source, "rev", s.index.nFile, s.index.nUndoPos), SER_DISK, CLIENT_VERSION);
        CHashVerifier<CAutoFile> verifier(&undofile);
        verifier << s.index.hashPrev; verifier >> s.undo;
        uint256 checksum; undofile >> checksum;
        Require(checksum == verifier.GetHash(), "undo checksum mismatch");
        Require(s.undo.vtxundo.size() + 1 == s.block.vtx.size(), "undo transaction count mismatch");
        CDataStream raw(SER_NETWORK, PROTOCOL_VERSION); raw << s.block;
        s.raw.assign(raw.begin(), raw.end());
        hash = s.index.hashPrev;
    }
    return samples;
}
UniValue Plan(CDBWrapper& db, const fs::path& source, const UniValue& input) {
    UniValue result(UniValue::VARR);
    CDiskBlockIndex assumed;
    const bool have_assumed = db.Read(std::make_pair('b', Params().GetConsensus().defaultAssumeValid), assumed);
    for (const auto& w : input["windows"].getValues()) {
        auto samples = ReadWindow(db, source, w);
        std::set<int> needed;
        for (const auto& s : samples) {
            int h = s.index.nHeight;
            Route(needed, h - 1, Skip(h));
            Route(needed, h - 1, Params().GetConsensus().BIP34Height);
            Route(needed, input["tip_height"].get_int(), h);
            if (have_assumed) Route(needed, assumed.nHeight, h);
            for (size_t t = 1; t < s.block.vtx.size(); ++t) {
                const auto& tx = *s.block.vtx[t];
                Require(s.undo.vtxundo[t-1].vprevout.size() == tx.vin.size(), "undo input count mismatch");
                for (size_t v = 0; v < tx.vin.size(); ++v) {
                    auto sequence = tx.vin[v].nSequence;
                    if (tx.nVersion >= 2 && !(sequence & CTxIn::SEQUENCE_LOCKTIME_DISABLE_FLAG) && (sequence & CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG)) {
                        int target = std::max(0, int(s.undo.vtxundo[t-1].vprevout[v].nHeight) - 1);
                        Route(needed, h, target);
                        for (int j = std::max(0, target-10); j <= target; ++j) needed.insert(j);
                    }
                }
            }
        }
        UniValue known(UniValue::VOBJ);
        known.pushKV(std::to_string(input["tip_height"].get_int()), input["tip_hash"].get_str());
        if (have_assumed) known.pushKV(std::to_string(assumed.nHeight), assumed.GetBlockHash().ToString());
        const int start = samples.front().index.nHeight;
        uint256 prev = samples.front().index.hashPrev;
        for (int h = start - 1; h >= std::max(0, start - int(Params().GetConsensus().nPowAveragingWindow) - 12); --h) {
            auto d = Index(db, prev); Require(d.nHeight == h, "context height mismatch");
            known.pushKV(std::to_string(h), prev.ToString()); needed.insert(h); prev = d.hashPrev;
        }
        UniValue heights(UniValue::VARR);
        for (int h : needed) if (h < start || h > samples.back().index.nHeight) heights.push_back(h);
        UniValue entry = w; entry.pushKV("needed", heights); entry.pushKV("known", known);
        entry.pushKV("tip_height", input["tip_height"].get_int());
        entry.pushKV("tip_hash", input["tip_hash"].get_str());
        entry.pushKV("tip_chainwork", input["tip_chainwork"].get_str());
        result.push_back(entry);
    }
    return result;
}
double Seconds(std::chrono::steady_clock::time_point begin) { return std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count(); }
UniValue Measure(CDBWrapper& source_db, const fs::path& source, const UniValue& w) {
    auto samples = ReadWindow(source_db, source, w);
    const int start = samples.front().index.nHeight;
    std::map<int, CBlockIndex*> ancestors;
    for (const auto& key : w["context"].getKeys()) {
        int height = std::stoi(key);
        if (height >= start) continue;
        uint256 hash = uint256S(w["context"][key].get_str());
        auto d = Index(source_db, hash); Require(d.nHeight == height, "ancestor height mismatch");
        auto p = new CBlockIndex(d);
        auto it = mapBlockIndex.emplace(hash, p).first; p->phashBlock = &it->first;
        p->pprev = nullptr; p->pskip = nullptr; ancestors[height] = p;
    }
    for (auto item : ancestors) {
        auto p = item.second;
        auto prev = ancestors.find(item.first - 1), skip = ancestors.find(Skip(item.first));
        if (prev != ancestors.end()) p->pprev = prev->second;
        if (skip != ancestors.end() && skip->first < item.first) p->pskip = skip->second;
    }
    auto parent = ancestors.at(start - 1);
    parent->nChainTx = w["parent_txcount"].get_int64();
    parent->nChainWork = UintToArith256(uint256S(w["parent_chainwork"].get_str()));
    chainActive.SetTip(parent);
    Require(parent->GetBlockHash() == samples.front().block.hashPrevBlock, "wrong parent");
    pblocktree.reset(new CBlockTreeDB(8 << 20));
    pcoinsdbview.reset(new CCoinsViewDB(8 << 20));
    pcoinsTip.reset(new CCoinsViewCache(pcoinsdbview.get()));
    // Restore only pre-window inputs. Outputs made inside the window must be
    // produced by ConnectBlock, never pre-seeded from undo.
    std::set<uint256> created;
    size_t inputs = 0, transactions = 0, bytes = 0;
    for (auto& s : samples) {
        for (size_t t = 0; t < s.block.vtx.size(); ++t) {
            const auto& tx = *s.block.vtx[t];
            if (t) for (size_t v = 0; v < tx.vin.size(); ++v) {
                ++inputs;
                if (!created.count(tx.vin[v].prevout.hash)) {
                    Coin coin(s.undo.vtxundo[t-1].vprevout[v]);
                    Require(!pcoinsTip->HaveCoin(tx.vin[v].prevout), "duplicate external input");
                    pcoinsTip->AddCoin(tx.vin[v].prevout, std::move(coin), false);
                }
            }
            created.insert(tx.GetHash()); ++transactions;
        }
        bytes += s.raw.size();
    }
    pcoinsTip->SetBestBlock(parent->GetBlockHash());
    Require(pcoinsTip->Flush(), "fixture coins flush failed");
    pcoinsTip.reset(new CCoinsViewCache(pcoinsdbview.get()));
    // Force independent Yespower work on every sampled header. Source index
    // evidence is not installed for any of these hashes.
    std::vector<CBlockHeader> headers;
    for (const auto& s : samples) headers.push_back(s.block.GetBlockHeader());
    CValidationState state;
    auto begin = std::chrono::steady_clock::now();
    Require(ProcessNewBlockHeaders(headers, state, Params()), "header validation: " + FormatStateMessage(state));
    double header_seconds = Seconds(begin);
    for (const auto& s : samples) Require(mapBlockIndex.at(s.block.GetHash())->nStatus & BLOCK_POW_CHECKED, "missing independently checked PoW");
    // Regression checks outside the timer: accepted-header evidence must not
    // authorize a changed target, through either header or full-block ingress.
    auto invalid = std::make_shared<CBlock>(samples.front().block);
    invalid->nBits = 0;
    CValidationState rejected;
    Require(!ProcessNewBlockHeaders({invalid->GetBlockHeader()}, rejected, Params()), "invalid header accepted");
    Require(!ProcessNewBlock(Params(), invalid, true, nullptr), "invalid block accepted");
    // Only after independent header verification install the authentic best
    // header/assumevalid ancestry. This reproduces the ordinary default script
    // skipping rule without giving sampled headers checkpoint/PoW evidence.
    for (const auto& sample : samples) ancestors[sample.index.nHeight] = mapBlockIndex.at(sample.block.GetHash());
    for (const auto& key : w["context"].getKeys()) {
        int height = std::stoi(key);
        if (height < start) continue;
        Require(height > samples.back().index.nHeight, "fixture overlaps measured headers");
        uint256 hash = uint256S(w["context"][key].get_str());
        auto d = Index(source_db, hash); Require(d.nHeight == height, "future context height mismatch");
        auto p = new CBlockIndex(d);
        auto it = mapBlockIndex.emplace(hash, p).first; p->phashBlock = &it->first;
        p->pprev = nullptr; p->pskip = nullptr; ancestors[height] = p;
    }
    for (auto item : ancestors) {
        auto prev = ancestors.find(item.first - 1), skip = ancestors.find(Skip(item.first));
        if (prev != ancestors.end()) item.second->pprev = prev->second;
        if (skip != ancestors.end() && skip->first < item.first) item.second->pskip = skip->second;
    }
    pindexBestHeader = ancestors.at(w["tip_height"].get_int());
    Require(pindexBestHeader->GetBlockHash() == uint256S(w["tip_hash"].get_str()), "tip hash mismatch");
    pindexBestHeader->nChainWork = UintToArith256(uint256S(w["tip_chainwork"].get_str()));
    // Preallocate the active-chain height vector outside the timed loop.
    chainActive.SetTip(mapBlockIndex.at(samples.back().block.GetHash()));
    chainActive.SetTip(parent);
    Require(IsInitialBlockDownload(), "sample is too recent for an IBD fixture; select an older snapshot");
    begin = std::chrono::steady_clock::now();
    for (const auto& s : samples) {
        CDataStream raw(s.raw, SER_NETWORK, PROTOCOL_VERSION);
        auto block = std::make_shared<CBlock>(); raw >> *block;
        bool is_new = false;
        Require(ProcessNewBlock(Params(), block, true, &is_new), "ProcessNewBlock rejected sample");
        Require(is_new && chainActive.Tip()->GetBlockHash() == block->GetHash(), "sample did not connect as a new tip");
    }
    SyncWithValidationInterfaceQueue();
    {
        LOCK(cs_main);
        Require(FlushStateToDisk(Params(), state, FLUSH_STATE_ALWAYS), "final state flush failed");
    }
    SyncWithValidationInterfaceQueue();
    double block_seconds = Seconds(begin);
    UniValue out(UniValue::VOBJ);
    out.pushKV("stratum", w["stratum"].get_int()); out.pushKV("start", start);
    out.pushKV("count", int(samples.size())); out.pushKV("header_seconds", header_seconds);
    out.pushKV("block_seconds", block_seconds); out.pushKV("transactions", int64_t(transactions));
    out.pushKV("script_threads", nScriptCheckThreads);
    out.pushKV("assumevalid", hashAssumeValid.ToString());
    out.pushKV("inputs", int64_t(inputs)); out.pushKV("bytes", int64_t(bytes));
    pcoinsTip.reset(); pcoinsdbview.reset(); pblocktree.reset(); UnloadBlockIndex();
    return out;
}
}
int main(int argc, char** argv) {
    try {
        gArgs.ParseParameters(argc, argv); SetupEnvironment(); SelectParams(CBaseChainParams::MAIN);
        fPrintToDebugLog = false; fPrintToConsole = false; fCheckBlockIndex = false;
        gArgs.ForceSetArg("-fast-ibd", "0");
        // Match the node's default assumevalid and script-worker settings.
        hashAssumeValid = Params().GetConsensus().defaultAssumeValid;
        nScriptCheckThreads = std::min(MAX_SCRIPTCHECK_THREADS, GetNumCores());
        if (nScriptCheckThreads <= 1) nScriptCheckThreads = 0;
        nMinimumChainWork = UintToArith256(Params().GetConsensus().nMinimumChainWork);
        nCoinCacheUsage = 450 << 20;
        SHA256AutoDetect(); RandomInit(); ECC_Start(); ECCVerifyHandle verify;
        InitSignatureCache(); InitScriptExecutionCache();
        std::ifstream file(gArgs.GetArg("-input", ""));
        std::string json((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        UniValue input; Require(input.read(json), "invalid input JSON");
        const fs::path source = fs::canonical(gArgs.GetArg("-source", ""));
        const fs::path source_index = fs::canonical(gArgs.GetArg("-source-index", ""));
        Require(fs::canonical(GetDataDir()) != source, "test datadir must differ from source");
        Require(source_index != fs::canonical(source / "blocks" / "index"), "source index must be a private snapshot");
        CDBWrapper source_db(source_index, 32 << 20, false, false);
        CScheduler scheduler;
        boost::thread_group threads;
        for (int i = 1; i < nScriptCheckThreads; ++i) threads.create_thread(&ThreadScriptCheck);
        threads.create_thread([&scheduler] { scheduler.serviceQueue(); });
        GetMainSignals().RegisterBackgroundSignalScheduler(scheduler);
        struct Cleanup {
            boost::thread_group& threads;
            ~Cleanup() {
                SyncWithValidationInterfaceQueue();
                threads.interrupt_all(); threads.join_all();
                GetMainSignals().UnregisterBackgroundSignalScheduler();
            }
        } cleanup{threads};
        UniValue result = gArgs.GetBoolArg("-plan", false) ? Plan(source_db, source, input) : Measure(source_db, source, input);
        std::cout << result.write() << std::endl;
        ECC_Stop(); return 0;
    } catch (const std::exception& e) { std::cerr << "ibd_estimate: " << e.what() << std::endl; return 1; }
}
