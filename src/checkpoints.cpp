// Copyright (c) 2009-2017 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <checkpoints.h>

#include <chain.h>
#include <chainparams.h>
#include <reverse_iterator.h>
#include <validation.h>

#include <stdint.h>


namespace Checkpoints {

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
