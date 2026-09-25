// Copyright (c) 2009-2017 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <checkpoints.h>

#include <chain.h>
#include <chainparams.h>
#include <reverse_iterator.h>
#include <validation.h>

#include <stdint.h>
#include <algorithm>
#include <stdexcept>


namespace Checkpoints {

    HeaderSync::HeaderSync(int start_height, const uint256& start_hash, const CCheckpointData& checkpoints)
        : m_start_height(start_height), m_start_hash(start_hash),
          m_checkpoints(checkpoints.mapCheckpoints), m_height(start_height), m_prev(start_hash)
    {
        if (start_height < 0 || m_checkpoints.empty() || start_height >= m_checkpoints.rbegin()->first)
            throw std::invalid_argument("invalid checkpoint header sync range");
    }

    bool HeaderSync::Process(const std::vector<CBlockHeader>& headers, std::vector<CBlockHeader>& authenticated)
    {
        authenticated.clear();
        m_authenticated.clear();
        if (m_failed || m_complete || headers.empty() || headers.size() > CHUNK_SIZE)
            return false;
        for (const auto& header : headers) {
            if (header.hashPrevBlock != m_prev) {
                m_failed = true;
                break;
            }
            const uint256 hash = header.GetHash();
            ++m_height;
            const auto cp = m_checkpoints.find(m_height);
            if (cp != m_checkpoints.end() && cp->second != hash) {
                m_failed = true;
                break;
            }
            m_prev = hash;
            const bool at_end = m_height == m_checkpoints.rbegin()->first;
            const bool at_chunk = (m_height - m_start_height) % CHUNK_SIZE == 0 || at_end;
            if (!m_replaying) {
                if (at_chunk) m_commitments.push_back(hash);
                if (at_end) {
                    // The endpoint commits to every first-pass header, including
                    // all saved chunk endpoints. Discard trailing untrusted data.
                    m_replaying = true;
                    m_height = m_start_height;
                    m_prev = m_start_hash;
                    return true;
                }
            } else {
                m_buffer.push_back(header);
                if (at_chunk) {
                    if (m_replay_chunk >= m_commitments.size() || hash != m_commitments[m_replay_chunk++]) {
                        m_failed = true;
                        break;
                    }
                    for (const auto& committed : m_buffer) {
                        m_authenticated.insert(committed.GetHash());
                        authenticated.push_back(committed);
                    }
                    m_buffer.clear();
                }
                if (at_end) {
                    m_complete = true;
                    return true;
                }
            }
        }
        if (m_failed) {
            authenticated.clear();
            m_authenticated.clear();
            m_buffer.clear();
            m_commitments.clear();
        }
        return !m_failed;
    }

    bool HeaderSync::Authenticates(const uint256& hash, const CCheckpointData& checkpoints) const
    {
        return !m_failed && m_replaying && m_checkpoints == checkpoints.mapCheckpoints &&
               m_authenticated.count(hash) != 0;
    }

    std::shared_ptr<HeaderSync> HeaderSync::Resume(const CCheckpointData& checkpoints) const
    {
        AssertLockHeld(cs_main);
        if (!m_replaying || m_failed || m_checkpoints != checkpoints.mapCheckpoints) return nullptr;
        auto resumed = std::make_shared<HeaderSync>(m_start_height, m_start_hash, checkpoints);
        resumed->m_replaying = true;
        resumed->m_commitments = m_commitments;
        // Hash equality with an authenticated commitment is essential: neither
        // best-header work nor a peer's last received height is proof of ancestry.
        for (size_t i = m_commitments.size(); i > 0; --i) {
            const auto it = mapBlockIndex.find(m_commitments[i - 1]);
            const int height = std::min<size_t>(m_start_height + i * CHUNK_SIZE, m_checkpoints.rbegin()->first);
            if (it == mapBlockIndex.end() || it->second->nHeight != height ||
                !it->second->IsValid(BLOCK_VALID_TREE) ||
                !(it->second->nStatus & BLOCK_CHECKPOINT_CHECKED)) continue;
            if (height == m_checkpoints.rbegin()->first) return nullptr;
            resumed->m_height = height;
            resumed->m_prev = m_commitments[i - 1];
            resumed->m_replay_chunk = i;
            break;
        }
        return resumed;
    }

    bool CheckBlock(int nHeight, const uint256& hash, const CCheckpointData& data)
    {
        const auto i = data.mapCheckpoints.find(nHeight);
        return i == data.mapCheckpoints.end() || hash == i->second;
    }

    CBlockIndex* GetLastCheckpoint(const CCheckpointData& data)
    {
        const MapCheckpoints& checkpoints = data.mapCheckpoints;

        for (const MapCheckpoints::value_type& i : reverse_iterate(checkpoints))
        {
            const uint256& hash = i.second;
            BlockMap::const_iterator t = mapBlockIndex.find(hash);
            if (t != mapBlockIndex.end())
                return t->second;
        }
        return nullptr;
    }

    bool IsAncestorOfLastCheckpoint(const CBlockIndex* pindex, const CCheckpointData& data)
    {
        if (pindex == nullptr)
            return false;

        const CBlockIndex* pcheckpoint = GetLastCheckpoint(data);
        return pcheckpoint != nullptr &&
               pindex->nHeight <= pcheckpoint->nHeight &&
               pcheckpoint->GetAncestor(pindex->nHeight) == pindex;
    }

} // namespace Checkpoints
