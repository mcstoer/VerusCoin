// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin Core developers
// Copyright (c) 2019 Michael Toutonghi
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "amount.h"
#include "chainparams.h"
#include "consensus/consensus.h"
#include "consensus/validation.h"
#include "core_io.h"
#ifdef ENABLE_MINING
#include "crypto/equihash.h"
#endif
#include "init.h"
#include "main.h"
#include "metrics.h"
#include "miner.h"
#include "net.h"
#include "pow.h"
#include "rpc/server.h"
#include "txmempool.h"
#include "util.h"
#include "validationinterface.h"
#ifdef ENABLE_WALLET
#include "wallet/wallet.h"
#endif
#include "timedata.h"

#include <stdint.h>

#include <boost/assign/list_of.hpp>

#include <univalue.h>

#include "rpc/pbaasrpc.h"
#include "pbaas/crosschainrpc.h"

using namespace std;

extern int32_t ASSETCHAINS_ALGO, ASSETCHAINS_EQUIHASH, ASSETCHAINS_LWMAPOS;
extern char ASSETCHAINS_SYMBOL[KOMODO_ASSETCHAIN_MAXLEN];
extern uint64_t ASSETCHAINS_STAKED;
extern int32_t KOMODO_MININGTHREADS;
extern bool VERUS_MINTBLOCKS;
extern uint8_t NOTARY_PUBKEY33[33];
extern uint160 ASSETCHAINS_CHAINID;
extern uint160 VERUS_CHAINID;

arith_uint256 komodo_PoWtarget(int32_t *percPoSp,arith_uint256 target,int32_t height,int32_t goalperc);

// NOTE: Assumes a conclusive result; if result is inconclusive, it must be handled by caller
static UniValue BIP22ValidationResult(const CValidationState& state)
{
    if (state.IsValid())
        return NullUniValue;

    std::string strRejectReason = state.GetRejectReason();
    if (state.IsError())
        throw JSONRPCError(RPC_VERIFY_ERROR, strRejectReason);
    if (state.IsInvalid())
    {
        if (strRejectReason.empty())
            return "rejected";
        return strRejectReason;
    }
    // Should be impossible
    return "valid?";
}

class submitblock_StateCatcher : public CValidationInterface
{
public:
    uint256 hash;
    bool found;
    CValidationState state;

    submitblock_StateCatcher(const uint256 &hashIn) : hash(hashIn), found(false), state() {};

protected:
    virtual void BlockChecked(const CBlock& block, const CValidationState& stateIn) {
        if (block.GetHash() != hash)
            return;
        found = true;
        state = stateIn;
    };
};

bool GetChainDefinition(string &name, CPBaaSChainDefinition &chainDef)
{
    CCcontract_info CC;
    CCcontract_info *cp;

    // make the chain definition output
    cp = CCinit(&CC, EVAL_PBAASDEFINITION);

    CBitcoinAddress bca(CC.unspendableCCaddr);

    CKeyID id;
    bca.GetKeyID(id);

    std::vector<std::pair<CAddressIndexKey, CAmount>> addressIndex;
    bool found = false;

    if (GetAddressIndex(id, 1, addressIndex))
    {
        for (auto txidx : addressIndex)
        {
            CTransaction tx;
            uint256 blkHash;
            if (myGetTransaction(txidx.first.txhash, tx, blkHash))
            {
                chainDef = CPBaaSChainDefinition(tx);
                found = chainDef.IsValid() && chainDef.name == name;
                if (found)
                {
                    break;
                }
            }
        }
    }
    return found;
}

void GetDefinedChains(vector<CPBaaSChainDefinition> &chains, bool includeExpired)
{
    CCcontract_info CC;
    CCcontract_info *cp;

    // make the chain definition output
    cp = CCinit(&CC, EVAL_PBAASDEFINITION);

    CBitcoinAddress bca(CC.unspendableCCaddr);

    CKeyID id;
    bca.GetKeyID(id);

    std::vector<std::pair<CAddressIndexKey, CAmount>> addressIndex;

    if (GetAddressIndex(id, 1, addressIndex))
    {
        for (auto txidx : addressIndex)
        {
            CTransaction tx;
            uint256 blkHash;
            if (myGetTransaction(txidx.first.txhash, tx, blkHash))
            {
                chains.push_back(CPBaaSChainDefinition(tx));
                // remove after to use less storage
                if (!includeExpired && chains.back().endBlock != 0 && chains.back().endBlock < chainActive.Height())
                {
                    chains.pop_back();
                }
            }
        }
    }
}

UniValue getchaindefinition(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
    {
        throw runtime_error(
            "getchaindefinition \"chainname\"\n"
            "\nReturns a complete definition for any given chain if it is registered on the blockchain. If the chain requested\n"
            "\nis NULL, chain definition of the current chain is returned.\n"

            "\nArguments\n"
            "1. \"chainname\"                     (string, optional) name of the chain to look for. no parameter returns current chain in daemon.\n"

            "\nResult:\n"
            "  {\n"
            "    \"version\" : \"n\",             (int) version of this chain definition\n"
            "    \"name\" : \"string\",           (string) name or symbol of the chain, same as passed\n"
            "    \"address\" : \"string\",        (string) cryptocurrency address to send fee and non-converted premine\n"
            "    \"chainid\" : \"hex-string\",    (string) 40 char string that represents the chain ID, calculated from the name\n"
            "    \"premine\" : \"n\",             (int) amount of currency paid out to the premine address in block #1, may be smart distribution\n"
            "    \"convertible\" : \"xxxx\"       (bool) if this currency is a fractional reserve currency of Verus\n"
            "    \"launchfee\" : \"n\",           (int) (launchfee * total converted) / 100000000 sent directly to premine address\n"
            "    \"startblock\" : \"n\",          (int) block # on this chain, which must be notarized into block one of the chain\n"
            "    \"endblock\" : \"n\",            (int) block # after which, this chain's useful life is considered to be over\n"
            "    \"eras\" : \"[obj, ...]\",       (objarray) different chain phases of rewards and convertibility\n"
            "    {\n"
            "      \"reward\" : \"[n, ...]\",     (int) reward start for each era in native coin\n"
            "      \"decay\" : \"[n, ...]\",      (int) exponential or linear decay of rewards during each era\n"
            "      \"halving\" : \"[n, ...]\",    (int) blocks between halvings during each era\n"
            "      \"eraend\" : \"[n, ...]\",     (int) block marking the end of each era\n"
            "      \"eraoptions\" : \"[n, ...]\", (int) options for each era (reserved)\n"
            "    }\n"
            "    \"nodes\"      : \"[obj, ..]\",  (objectarray, optional) up to 2 nodes that can be used to connect to the blockchain"
            "      [{\n"
            "         \"nodeaddress\" : \"txid\", (string,  optional) internet, TOR, or other supported address for node\n"
            "         \"paymentaddress\" : \"n\", (int,     optional) rewards payment address\n"
            "       }, .. ]\n"
            "  }\n"

            "\nExamples:\n"
            + HelpExampleCli("getchaindefinition", "\"chainname\"")
            + HelpExampleRpc("getchaindefinition", "\"chainname\"")
        );
    }
    UniValue ret(UniValue::VOBJ);

    string name = params[0].get_str();

    if (name.size() > KOMODO_ASSETCHAIN_MAXLEN - 1)
    {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid chain name -- must be 64 characters or less");
    }

    CPBaaSChainDefinition chainDef;

    if (GetChainDefinition(name, chainDef))
    {
        return chainDef.ToUniValue();
    }
    else
    {
        return NullUniValue;
    }
}

UniValue getdefinedchains(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
    {
        throw runtime_error(
            "getdefinedchains (includeexpired)\n"
            "\nReturns a complete definition for any given chain if it is registered on the blockchain. If the chain requested\n"
            "\nis NULL, chain definition of the current chain is returned.\n"

            "\nArguments\n"
            "1. \"includeexpired\"                (bool, optional) if true, include chains that are no longer active\n"

            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"version\" : \"n\",             (int) version of this chain definition\n"
            "    \"name\" : \"string\",           (string) name or symbol of the chain, same as passed\n"
            "    \"address\" : \"string\",        (string) cryptocurrency address to send fee and non-converted premine\n"
            "    \"chainid\" : \"hex-string\",    (string) 40 char string that represents the chain ID, calculated from the name\n"
            "    \"premine\" : \"n\",             (int) amount of currency paid out to the premine address in block #1, may be smart distribution\n"
            "    \"convertible\" : \"xxxx\"       (bool) if this currency is a fractional reserve currency of Verus\n"
            "    \"launchfee\" : \"n\",           (int) (launchfee * total converted) / 100000000 sent directly to premine address\n"
            "    \"startblock\" : \"n\",          (int) block # on this chain, which must be notarized into block one of the chain\n"
            "    \"endblock\" : \"n\",            (int) block # after which, this chain's useful life is considered to be over\n"
            "    \"eras\" : \"[obj, ...]\",       (objarray) different chain phases of rewards and convertibility\n"
            "    {\n"
            "      \"reward\" : \"[n, ...]\",     (int) reward start for each era in native coin\n"
            "      \"decay\" : \"[n, ...]\",      (int) exponential or linear decay of rewards during each era\n"
            "      \"halving\" : \"[n, ...]\",    (int) blocks between halvings during each era\n"
            "      \"eraend\" : \"[n, ...]\",     (int) block marking the end of each era\n"
            "      \"eraoptions\" : \"[n, ...]\", (int) options for each era (reserved)\n"
            "    }\n"
            "    \"nodes\"      : \"[obj, ..]\",  (objectarray, optional) up to 2 nodes that can be used to connect to the blockchain"
            "      [{\n"
            "         \"nodeaddress\" : \"txid\", (string,  optional) internet, TOR, or other supported address for node\n"
            "         \"paymentaddress\" : \"n\", (int,     optional) rewards payment address\n"
            "       }, .. ]\n"
            "  }, ...\n"
            "]\n"

            "\nExamples:\n"
            + HelpExampleCli("getdefinedchains", "true")
            + HelpExampleRpc("getdefinedchains", "true")
        );
    }
    UniValue ret(UniValue::VARR);

    bool includeExpired = params[0].isBool() ? params[0].get_bool() : false;

    vector<CPBaaSChainDefinition> chains;
    GetDefinedChains(chains, includeExpired);

    for (auto def : chains)
    {
        ret.push_back(def.ToUniValue());
    }

    return ret;
}

bool GetNotarizationData(uint160 chainID, uint32_t ecode, CChainNotarizationData &notarizationData)
{
    notarizationData.version = PBAAS_VERSION;

    // look for unspent notarization finalization outputs for the requested chain
    CKeyID keyID(CCrossChainRPCData::GetConditionID(chainID, EVAL_FINALIZENOTARIZATION));
    std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue> > unspentOutputs;
    CPBaaSChainDefinition chainDef;

    if (!GetAddressUnspent(keyID, 1, unspentOutputs))
    {
        return false;
    }
    else
    {
        multimap<int32_t, pair<uint256, CPBaaSNotarization>> sorted;

        // filter out all transactions that do not spend from the notarization thread, or originate as the
        // chain definition
        for (auto it = unspentOutputs.begin(); it != unspentOutputs.end(); it++)
        {
            CTransaction ntx;
            uint256 blkHash;

            if (myGetTransaction(it->first.txhash, ntx, blkHash))
            {
                // try to make a chain definition out of each transaction, and keep the first one that is valid
                if (!chainDef.IsValid())
                {
                    chainDef = CPBaaSChainDefinition(ntx);
                }
                CPBaaSNotarization notarization = CPBaaSNotarization(ntx);
                if (notarization.IsValid())
                {
                    auto blkit = mapBlockIndex.find(blkHash);
                    if (blkit != mapBlockIndex.end())
                    {
                        sorted.emplace(blkit->second->GetHeight(), make_pair(blkit->first, notarization));
                    }
                }
            }
            else
            {
                throw JSONRPCError(RPC_TRANSACTION_ERROR, "cannot retrieve transaction");
            }
        }

        if (!sorted.size())
        {
            return false;
        }

        // the first entry must either be a chain definition, which we should have to compare, or must refer to the last
        // confirmed notarization
        notarizationData.lastConfirmed = 0;
        if (!chainDef.IsValid())
        {
            // the first entry of all forks must reference a confirmed transaction
            CTransaction rootTx;
            uint256 blkHash;
            if (!myGetTransaction(sorted.begin()->second.second.prevNotarization, rootTx, blkHash))
            {
                return false;
            }
            // ensure that we have a finalization output
            COptCCParams p;
            CPBaaSNotarization notarization;

            for (auto o : rootTx.vout)
            {
                if (IsPayToCryptoCondition(o.scriptPubKey, p, notarization) && notarization.IsValid())
                {
                    if (p.vKeys.size() && GetDestinationID(p.vKeys[0]) == keyID)
                    {
                        notarizationData.vtx.push_back(make_pair(sorted.begin()->second.second.prevNotarization, notarization));
                        notarizationData.lastConfirmed = notarizationData.vtx.size() - 1;
                    }
                }
            }
        }
        else
        {
            // we still have the chain definition in our forks, so no notarization has been confirmed yet
            notarizationData.lastConfirmed = -1;
        }

        multimap<uint256, pair<int32_t, int32_t>> references;       // associates the txid, the fork index, and the index in the fork

        for (auto it : sorted)
        {
            notarizationData.vtx.push_back(make_pair(it.second.first, it.second.second));
        }

        // we now have all unspent notarizations sorted by block height
        // put them into the notarization data, organize them into
        // forks, then determine best chain of notarizations, and if they refer to a confirmed notarization
        // there must be a common root in the forks or the confirmed notarization to which they refer

        // find roots and create a chain from each
        for (int32_t i = 0; i < notarizationData.vtx.size(); i++)
        {
            auto &nzp = notarizationData.vtx[i];
            auto it = references.find(nzp.second.prevNotarization);

            int32_t chainIdx = 0;
            int32_t posIdx = 0;

            // do we refer to a notarization that is already in a fork?
            if (it != references.end())
            {
                std::vector<int32_t> &fork = notarizationData.forks[it->second.first];

                // if it is the end of the fork, put this entry there, if not the end, copy max once to another fork
                if (it->second.second == (fork.size() - 1))
                {
                    fork.push_back(i);
                    chainIdx = it->second.first;
                    posIdx = fork.size() - 1;
                }
                else
                {
                    notarizationData.forks.push_back(vector<int32_t>(&fork[0], &fork[it->second.second]));
                    fork = notarizationData.forks.back();
                    fork.push_back(i);
                    chainIdx = notarizationData.forks.size() - 1;
                    posIdx = fork.size() - 1;
                }
            }
            else
            {
                // start a new fork
                notarizationData.forks.push_back(vector<int32_t>(0));
                notarizationData.forks.back().push_back(i);
                chainIdx = notarizationData.forks.size() - 1;
                posIdx = notarizationData.forks.back().size() - 1;
            }
            references.emplace(nzp.first, make_pair(chainIdx, posIdx));
        }

        CChainPower best;

        // now, we should have all forks in vectors
        // they should all have roots that point to the same confirmed or initial notarization, which should be enforced by chain rules
        // the best chain should simply be the tip with most power
        for (int i = 0; i < notarizationData.forks.size(); i++)
        {
            CChainPower curPower = ExpandCompactPower(notarizationData.vtx[notarizationData.forks[i].back()].second.compactPower, i);
            if (curPower > best)
            {
                best = curPower;
            }
        }
        notarizationData.bestChain = best.nHeight;
        return true;
    }
}

UniValue getnotarizationdata(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
    {
        throw runtime_error(
            "getnotarizationdata \"chainid\" ( maxcount since )\n"
            "\nReturns the latest PBaaS notarization data for the specifed chainid.\n"

            "\nArguments\n"
            "1. \"chainid\"                     (string, required) the hex-encoded chainid to search for notarizations on\n"
            "2. \"accepted\"                    (bool, optional) accepted, not earned notarizations, default: true if on\n"
            "                                                    VRSC or VRSCTEST, false otherwise\n"

            "\nResult:\n"
            "{\n"
            "  \"version\" : n,                 (numeric) The notarization protocol version\n"
            "}\n"

            "\nExamples:\n"
            + HelpExampleCli("getnotarizationdata", "\"chainid\" true")
            + HelpExampleRpc("getnotarizationdata", "\"chainid\"")
        );
    }
    uint160 chainID;
    CChainNotarizationData nData;
    uint32_t ecode;
    
    if (IsVerusActive())
    {
        ecode = EVAL_ACCEPTEDNOTARIZATION;
    }
    else
    {
        ecode = EVAL_EARNEDNOTARIZATION;
    }

    if (params[0].type() == UniValue::VSTR)
    {
        try
        {
            chainID = uint160(ParseHex(params[0].get_str()));
        }
        catch(const std::exception& e)
        {
        }
    }

    if (chainID.IsNull())
    {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid chainid");
    }

    if (params.size() > 1)
    {
        if (!params[1].get_bool())
        {
            ecode = EVAL_EARNEDNOTARIZATION;
        }
    }
    
    if (GetNotarizationData(chainID, ecode, nData))
    {
        return nData.ToUniValue();
    }
    else
    {
        return UniValue(UniValue::VOBJ);
    }
}

UniValue getcrossnotarization(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 2 || params.size() > 3)
    {
        throw runtime_error(
            "getcrossnotarization \"chainid\" '[\"notarizationtxid1\", \"notarizationtxid2\", ...]'\n"
            "\nReturns the latest PBaaS notarization transaction found in the list of transaction IDs or nothing if not found\n"

            "\nArguments\n"
            "1. \"chainid\"                     (string, required) the hex-encoded chainid to search for notarizations on\n"
            "2. \"txidlist\"                    (stringarray, optional) list of transaction ids to check in preferred order, first found is returned\n"
            "2. \"accepted\"                    (bool, optional) accepted, not earned notarizations, default: true if on\n"
            "                                                    VRSC or VRSCTEST, false otherwise\n"

            "\nResult:\n"
            "{\n"
            "  \"crosstxid\" : \"xxxx\",        (hexstring) cross-transaction id of the notarization that matches, which is one in the arguments\n"
            "  \"txid\" : \"xxxx\",             (hexstring) transaction id of the notarization that was found\n"
            "  \"rawtx\" : \"hexdata\",         (hexstring) entire matching transaction data, serialized\n"
            "  \"newtx\" : \"hexdata\"          (hexstring) the proposed notarization transaction with an opret and opretproof\n"
            "}\n"

            "\nExamples:\n"
            + HelpExampleCli("getcrossnotarization", "\"chainid\" '[\"notarizationtxid1\", \"notarizationtxid2\", ...]'")
            + HelpExampleRpc("getcrossnotarization", "\"chainid\" '[\"notarizationtxid1\", \"notarizationtxid2\", ...]'")
        );
    }

    uint160 chainID;
    uint32_t ecode;
    UniValue ret(UniValue::VOBJ);

    if (IsVerusActive())
    {
        ecode = EVAL_ACCEPTEDNOTARIZATION;
    }
    else
    {
        ecode = EVAL_EARNEDNOTARIZATION;
    }

    if (params[0].type() == UniValue::VSTR)
    {
        try
        {
            chainID = uint160(ParseHex(params[0].get_str()));
        }
        catch(const std::exception& e)
        {
        }
    }

    if (chainID.IsNull())
    {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid chainid");
    }

    if (params.size() > 2)
    {
        if (!params[2].get_bool())
        {
            ecode = EVAL_EARNEDNOTARIZATION;
        }
    }

    if (params[1].type() != UniValue::VARR)
    {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid second parameter object type: " + itostr(params[1].type()));
    }

    vector<UniValue> values = params[1].getValues();
    set<uint256> txids;
    for (int32_t i = 0; i < values.size(); i++)
    {
        auto txid = uint256S(values[i].get_str());
        if (txid.IsNull())
        {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter for notarization ID: " + values[i].get_str());
        }
        txids.insert(txid);
    }

    CChainNotarizationData nData;

    // get notarization data and check all transactions
    if (GetNotarizationData(chainID, ecode, nData))
    {
        CTransaction tx;
        CPBaaSNotarization ourLast;
        uint256 blkHash;
        bool found = false;

        // if we are the first earned notarization on this chain, we don't have to find a match, chain definition is the match
        if (txids.size() == 0 && ecode == EVAL_ACCEPTEDNOTARIZATION && !nData.IsConfirmed() && !nData.vtx.size() != 0)
        {
            if (GetTransaction(nData.vtx[0].first, tx, blkHash, true) && (ourLast = CPBaaSNotarization(tx)).IsValid())
            {
                found = true;
                // we have the first matching transaction, return it
                ret.push_back(Pair("crosstxid", uint256().GetHex()));
                ret.push_back(Pair("txid", nData.vtx[0].first.GetHex()));
                ret.push_back(Pair("rawtx", EncodeHexTx(tx)));
            }
        }
        else
        {
            // loop in reverse through list, as most recent is at end
            for (int32_t i = nData.vtx.size() - 1; i >= 0; i--)
            {
                const pair<uint256, CPBaaSNotarization> &nzp = nData.vtx[i];
                auto nit = txids.find(nzp.second.crossNotarization);
                if (GetTransaction(nzp.first, tx, blkHash, true) && (ourLast = CPBaaSNotarization(tx)).IsValid())
                {
                    found = true;
                    // we have the first matching transaction, return it
                    ret.push_back(Pair("crosstxid", nzp.second.crossNotarization.GetHex()));
                    ret.push_back(Pair("txid", nzp.first.GetHex()));
                    ret.push_back(Pair("rawtx", EncodeHexTx(tx)));
                }
            }
        }

        // now make the basic notarization for this chain that the other chain daemon can complete
        // after it is returned
        if (found)
        {
            // make sure our MMR matches our tip height, etc.
            LOCK(cs_main);

            int32_t proofheight = chainActive.Height();
            ChainMerkleMountainView mmv(chainActive.GetMMR(), proofheight);
            uint256 mmrRoot = mmv.GetRoot();

            CMerkleBranch blockProof;
            chainActive.GetBlockProof(mmv, blockProof, proofheight);

            // prove the last notarization txid with new MMR, which also provides its blockhash and power as part of proof
            CBlock block;
            CBlockIndex *pnindex = mapBlockIndex.find(blkHash)->second;

            if(!pnindex || !ReadBlockFromDisk(block, pnindex, 0))
            {
                throw JSONRPCError(RPC_INTERNAL_ERROR, "Can't read block from disk");
            }

            int32_t prevHeight = pnindex->GetHeight();

            // which transaction are we in this block?
            CKeyID keyID(CCrossChainRPCData::GetConditionID(chainID, EVAL_FINALIZENOTARIZATION));
            std::vector<std::pair<CAddressIndexKey, CAmount>> addressIndex;

            if (!GetAddressIndex(keyID, 1, addressIndex, prevHeight, prevHeight))
            {
                throw JSONRPCError(RPC_INTERNAL_ERROR, "Address index read error - possible corruption in address index");
            }

            uint256 txHash = tx.GetHash();
            int i;
            for (i = 0; i < addressIndex.size(); i++)
            {
                if (addressIndex[i].first.txhash == txHash)
                {
                    break;
                }
            }

            if (i == addressIndex.size())
            {
                throw JSONRPCError(RPC_INTERNAL_ERROR, "Notarization not found in address index - possible corruption");
            }

            // if bock headers are merge mined, keep header refs, not headers

            // create and store the notarization proof of chain
            vector<CBaseChainObject *> chainObjects;
            COpRetProof orp;

            // first, provide the latest block header in the opret...
            CBlockHeader bh = chainActive[proofheight]->GetBlockHeader();
            CChainObject<CBlockHeader> latestHeaderObj(CHAINOBJ_HEADER, bh);
            chainObjects.push_back(&latestHeaderObj);
            orp.AddObject(CHAINOBJ_HEADER, chainActive[proofheight]->GetBlockHash());

            // prove it with the latest MMR root
            CChainObject<CMerkleBranch> latestHeaderProof(CHAINOBJ_PROOF, blockProof);
            chainObjects.push_back(&latestHeaderObj);
            orp.AddObject(bh, chainActive[proofheight]->GetBlockHash());

            // get a proof of the prior notarizaton from the MMR root of this notarization
            CMerkleBranch txProof(i, block.GetMerkleBranch(addressIndex[i].first.txindex));
            chainActive.GetMerkleProof(mmv, txProof, prevHeight);

            // include the last notarization tx, minus its opret in the new notarization's opret
            CMutableTransaction mtx(tx);
            if (mtx.vout[mtx.vout.size() - 1].scriptPubKey.IsOpReturn())
            {
                // remove the opret, which is large and can be reconstructed from the opretproof, solely with data on the other chain
                mtx.vout.pop_back();
            }
            CTransaction strippedTx(mtx);

            // add the cross transaction from this chain to return
            CChainObject<CTransaction> strippedTxObj(CHAINOBJ_TRANSACTION, strippedTx);
            chainObjects.push_back(&strippedTxObj);
            orp.AddObject(CHAINOBJ_TRANSACTION, strippedTx.GetHash());

            // add proof of the transaction
            CChainObject<CMerkleBranch> txProofObj(CHAINOBJ_PROOF, txProof);
            chainObjects.push_back(&txProofObj);
            orp.AddObject(CHAINOBJ_PROOF, txHash);

            // TODO: select one block between the last notarization and one before it at random as a function of the
            // MMR bits of the latest block and the stake power of available PoS blocks. on any matching chain,
            // this selection will return the same selection
            // if we haven't yet, prove a PoS block

            // get node keys and addresses
            vector<CNodeData> nodes;
            const static int MAX_NODES = 2;

            {
                LOCK(cs_vNodes);
                if (!vNodes.empty())
                {
                    for (int j = 0; j < vNodes.size(); i++)
                    {
                        CNodeStats stats;
                        vNodes[i]->copyStats(stats);
                        if (vNodes[i]->fSuccessfullyConnected && !vNodes[i]->fInbound)
                        {
                            CBitcoinAddress bca(CKeyID(vNodes[i]->hashPaymentAddress));
                            nodes.push_back(CNodeData(vNodes[i]->addr.ToString(), bca.ToString()));
                        }
                    }
                }
            }

            // reduce number to max by removing randomly
            while (nodes.size() > MAX_NODES)
            {
                int toErase = GetRandInt(nodes.size() - 1);
                nodes.erase(nodes.begin() + toErase);
            }

            // get the current block's MMR root and proof height
            CPBaaSNotarization notarization = CPBaaSNotarization(CPBaaSNotarization::CURRENT_VERSION, 
                                                                 ASSETCHAINS_CHAINID,
                                                                 ourLast.rewardPerBlock,
                                                                 proofheight,
                                                                 mmrRoot,
                                                                 ArithToUint256(GetCompactPower(pnindex->nNonce, pnindex->nBits, pnindex->nVersion)),
                                                                 uint256(), 0,
                                                                 tx.GetHash(), prevHeight,
                                                                 orp,
                                                                 nodes);

            // we now have the chain objects, all associated proofs, and notarization data
            // make a partial transaction and return it
        }
    }
    return ret;
}

UniValue definechain(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
    {
        throw runtime_error(
            "definechain '{\"name\": \"BAAS\", ... }'\n"
            "\nThis defines a PBaaS chain, provides it with initial notarization fees to support its launch, and prepares it to begin running.\n"

            "\nArguments\n"
            "      {\n"
            "         \"name\"       : \"xxxx\",    (string, required) unique Verus ecosystem-wide name/symbol of this PBaaS chain\n"
            "         \"address\"    : \"Rxxx\",    (string, optional) premine and launch fee recipient\n"
            "         \"premine\"    : \"n\",       (int,    optional) amount of coins that will be premined and distributed to premine address\n"
            "         \"convertible\" : \"n\",      (int,    optional) amount of coins that may be converted from Verus, price determined by total contribution\n"
            "         \"launchfee\"  : \"n\",       (int,    optional) VRSC fee for conversion at startup, multiplied by amount, divided by 100000000\n"
            "         \"startblock\" : \"n\",       (int,    optional) VRSC block must be notarized into block 1 of PBaaS chain, default curheight + 100\n"
            "         \"eras\"       : \"objarray\", (array, optional) data specific to each era, maximum 3\n"
            "         {\n"
            "            \"reward\"      : \"n\",   (int64,  optional) native initial block rewards in each period\n"
            "            \"decay\" : \"n\",         (int64,  optional) reward decay for each era\n"
            "            \"halving\"      : \"n\",  (int,    optional) halving period for each era\n"
            "            \"eraend\"       : \"n\",  (int,    optional) ending block of each era\n"
            "            \"eraoptions\"   : \"n\",  (int,    optional) options for each era\n"
            "         }\n"
            "         \"notarizationreward\" : \"n\", (int,  required) default VRSC notarization reward total for first billing period\n"
            "         \"billingperiod\" : \"n\",    (int,    optional) number of blocks in each billing period\n"
            "         \"nodes\"      : \"[obj, ..]\", (objectarray, optional) up to 2 nodes that can be used to connect to the blockchain"
            "         [{\n"
            "            \"nodeaddress\" : \"txid\", (string,  optional) internet, TOR, or other supported address for node\n"
            "            \"paymentaddress\" : \"n\", (int,     optional) rewards payment address\n"
            "          }, .. ]\n"
            "      }\n"

            "\nResult:\n"
            "{\n"
            "  \"txid\" : \"transactionid\", (string) The transaction id.\n"
            "  \"hex\"  : \"data\"           (string) Raw data for signed transaction\n"
            "}\n"

            "\nExamples:\n"
            + HelpExampleCli("definechain", "jsondefinition")
            + HelpExampleRpc("definechain", "jsondefinition")
        );
    }
    if (!params[0].isObject())
    {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "JSON object required. see help.");
    }
    if (!pwalletMain)
    {
        throw JSONRPCError(RPC_WALLET_ERROR, "must have active wallet to define PBaaS chain");
    }

    CPBaaSChainDefinition newChain(params[0]);

    if (!newChain.startBlock)
    {
        newChain.startBlock = chainActive.Height() + 100;
    }

    if (newChain.billingPeriod < CPBaaSChainDefinition::MIN_BILLING_PERIOD || (newChain.notarizationReward / newChain.billingPeriod) < CPBaaSChainDefinition::MIN_PER_BLOCK_NOTARIZATION)
    {
        throw JSONRPCError(RPC_INVALID_PARAMS, "Billing period of at least " + 
                                               to_string(CPBaaSChainDefinition::MIN_BILLING_PERIOD) + 
                                               " blocks and per-block notary rewards of >= 1000000 are required to define a chain\n");
    }

    vector<CRecipient> outputs;

    // default double fee for miner of chain definition tx
    // one output for definition, one for finalization
    CAmount nReward = newChain.notarizationReward + (DEFAULT_TRANSACTION_FEE * 4);

    CCcontract_info CC;
    CCcontract_info *cp;

    // make the chain definition output
    cp = CCinit(&CC, EVAL_PBAASDEFINITION);
    // need to be able to send this to EVAL_PBAASDEFINITION address as a destination, locked by the default pubkey
    CPubKey pk = CPubKey(std::vector<unsigned char>(CC.CChexstr, CC.CChexstr + strlen(CC.CChexstr)));
    CBitcoinAddress bca(CC.unspendableCCaddr);
    CKeyID id;
    bca.GetKeyID(id);
    std::vector<CTxDestination> dests({id});
    CTxOut defOut = MakeCC1of1Vout(EVAL_PBAASDEFINITION, DEFAULT_TRANSACTION_FEE, pk, dests, newChain);
    outputs.push_back(CRecipient({defOut.scriptPubKey, CPBaaSChainDefinition::DEFAULT_OUTPUT_VALUE, false}));

    // make the first chain notarization output
    cp = CCinit(&CC, EVAL_ACCEPTEDNOTARIZATION);

    // we need to make a notarization, notarize this information and block 0, since we know that will be in the new
    // chain, our authorization will be that we are the chain definition
    uint256 mmvRoot;
    {
        LOCK(cs_main);
        auto mmr = chainActive.GetMMR();
        auto mmv = CMerkleMountainView<CMMRPowerNode, CChunkedLayer<CMMRPowerNode>, COverlayNodeLayer<CMMRPowerNode, CChain>>(mmr, mmr.size());
        mmv.resize(1);
        mmvRoot = mmv.GetRoot();
    }

    CPBaaSNotarization pbn = CPBaaSNotarization(PBAAS_VERSION,
                                                newChain.GetChainID(), 
                                                newChain.notarizationReward,
                                                0, mmvRoot,
                                                ArithToUint256(GetCompactPower(chainActive.Genesis()->nNonce, chainActive.Genesis()->nBits, chainActive.Genesis()->nVersion)),
                                                uint256(), 0,
                                                uint256(), 0,
                                                COpRetProof(),
                                                newChain.nodes);

    pk = CPubKey(std::vector<unsigned char>(CC.CChexstr, CC.CChexstr + strlen(CC.CChexstr)));
    dests = std::vector<CTxDestination>({CKeyID(newChain.GetConditionID(EVAL_ACCEPTEDNOTARIZATION))});
    CTxOut notarizationOut = MakeCC1of1Vout(EVAL_ACCEPTEDNOTARIZATION, nReward, pk, dests, pbn);
    outputs.push_back(CRecipient({notarizationOut.scriptPubKey, newChain.notarizationReward, false}));

    // make the finalization output
    cp = CCinit(&CC, EVAL_FINALIZENOTARIZATION);
    pk = CPubKey(std::vector<unsigned char>(CC.CChexstr, CC.CChexstr + strlen(CC.CChexstr)));
    dests = std::vector<CTxDestination>({CKeyID(newChain.GetConditionID(EVAL_FINALIZENOTARIZATION))});
    CNotarizationFinalization nf(0);
    CTxOut finalizationOut = MakeCC1of1Vout(EVAL_FINALIZENOTARIZATION, DEFAULT_TRANSACTION_FEE, pk, dests, nf);
    outputs.push_back(CRecipient({finalizationOut.scriptPubKey, CPBaaSChainDefinition::DEFAULT_OUTPUT_VALUE, false}));

    // create the transaction
    CWalletTx wtx;
    {
        LOCK2(cs_main, pwalletMain->cs_wallet);

        CReserveKey reserveKey(pwalletMain);
        CAmount fee;
        int nChangePos;
        string failReason;

        pwalletMain->CreateTransaction(outputs, wtx, reserveKey, fee, nChangePos, failReason);
    }

    UniValue uvret(UniValue::VOBJ);
    uvret.push_back(Pair("chaindefinition", CPBaaSChainDefinition(wtx).ToUniValue()));

    uvret.push_back(Pair("basenotarization", CPBaaSNotarization(wtx).ToUniValue()));

    uvret.push_back(Pair("txid", wtx.GetHash().GetHex()));

    string strHex = EncodeHexTx(static_cast<CTransaction>(wtx));
    uvret.push_back(Pair("hex", strHex));

    return uvret;
}

UniValue addmergedblock(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 5)
    {
        throw runtime_error(
            "addmergedblock \"hexdata\" ( \"jsonparametersobject\" )\n"
            "\nAdds a fully prepared block and its header to the current merge mining queue of this daemon.\n"
            "Parameters determine the action to take if adding this block would exceed the available merge mining slots.\n"
            "Default action to take if adding would exceed available space is to replace the choice with the least ROI if this block provides more.\n"

            "\nArguments\n"
            "1. \"hexdata\"                     (string, required) the hex-encoded, complete, unsolved block data to add. nTime, and nSolution are replaced.\n"
            "2. \"name\"                        (string, required) chain name symbol\n"
            "3. \"rpchost\"                     (string, required) host address for RPC connection\n"
            "4. \"rpcport\"                     (int,    required) port address for RPC connection\n"
            "5. \"userpass\"                    (string, required) credentials for login to RPC\n"

            "\nResult:\n"
            "\"deserialize-invalid\" - block could not be deserialized and was rejected as invalid\n"
            "\"blocksfull\"          - block did not exceed others in estimated ROI, and there was no room for an additional merge mined block\n"

            "\nExamples:\n"
            + HelpExampleCli("addmergedblock", "\"hexdata\" \'{\"chainid\" : \"hexstring\", \"rpchost\" : \"127.0.0.1\", \"rpcport\" : portnum}\'")
            + HelpExampleRpc("addmergedblock", "\"hexdata\" \'{\"chainid\" : \"hexstring\", \"rpchost\" : \"127.0.0.1\", \"rpcport\" : portnum, \"estimatedroi\" : (verusreward/hashrate)}\'")
        );
    }

    // check to see if we should replace any existing block or add a new one. if so, add this to the merge mine vector
    string name = params[1].get_str();
    if (name == "")
    {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "must provide chain name to merge mine");
    }

    string rpchost = params[2].get_str();
    int32_t rpcport = params[3].get_int();
    string rpcuserpass = params[4].get_str();

    if (rpchost == "" || rpcport == 0 || rpcuserpass == "")
    {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "must provide valid RPC connection parameters to merge mine");
    }

    ConnectedChains.PruneOldChains(GetAdjustedTime() - 60000);

    uint160 chainID = CCrossChainRPCData::GetChainID(name);

    // confirm data from blockchain
    CRPCChainData chainData;
    CPBaaSChainDefinition chainDef;
    if (ConnectedChains.GetChainInfo(chainID, chainData))
    {
        chainDef = chainData.chainDefinition;
    }

    if (!chainDef.IsValid() && !GetChainDefinition(name, chainDef))
    {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "chain not found");
    }

    CBlock blk;

    if (!DecodeHexBlk(blk, params[0].get_str()))
        return "deserialize-invalid";

    CPBaaSMergeMinedChainData blkData = CPBaaSMergeMinedChainData(chainDef, rpchost, rpcport, rpcuserpass, blk);

    return ConnectedChains.AddMergedBlock(blkData) ? NullUniValue : "blocksfull";
}

UniValue submitmergedblock(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "submitmergedblock \"hexdata\" ( \"jsonparametersobject\" )\n"
            "\nAttempts to submit one more more new blocks to one or more networks.\n"
            "Each merged block submission may be valid for Verus and/or up to 8 merge mined chains.\n"
            "The submitted block consists of a valid block for this chain, along with embedded headers of up to 8 other chains.\n"
            "If the hash for this header meets targets of other chains that have been added with 'addmergedblock', this API will\n"
            "submit those blocks to the specified URL endpoints with an RPC 'submitblock' request."
            "\nAttempts to submit one more more new blocks to one or more networks.\n"
            "The 'jsonparametersobject' parameter is currently ignored.\n"
            "See https://en.bitcoin.it/wiki/BIP_0022 for full specification.\n"

            "\nArguments\n"
            "1. \"hexdata\"    (string, required) the hex-encoded block data to submit\n"
            "2. \"jsonparametersobject\"     (string, optional) object of optional parameters\n"
            "    {\n"
            "      \"workid\" : \"id\"    (string, optional) if the server provided a workid, it MUST be included with submissions\n"
            "    }\n"
            "\nResult:\n"
            "\"duplicate\" - node already has valid copy of block\n"
            "\"duplicate-invalid\" - node already has block, but it is invalid\n"
            "\"duplicate-inconclusive\" - node already has block but has not validated it\n"
            "\"inconclusive\" - node has not validated the block, it may not be on the node's current best chain\n"
            "\"rejected\" - block was rejected as invalid\n"
            "For more information on submitblock parameters and results, see: https://github.com/bitcoin/bips/blob/master/bip-0022.mediawiki#block-submission\n"
            "\nExamples:\n"
            + HelpExampleCli("submitblock", "\"mydata\"")
            + HelpExampleRpc("submitblock", "\"mydata\"")
        );

    CBlock block;
    //LogPrintStr("Hex block submission: " + params[0].get_str());
    if (!DecodeHexBlk(block, params[0].get_str()))
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Block decode failed");

    uint256 hash = block.GetHash();
    bool fBlockPresent = false;
    {
        LOCK(cs_main);
        BlockMap::iterator mi = mapBlockIndex.find(hash);
        if (mi != mapBlockIndex.end()) {
            CBlockIndex *pindex = mi->second;
            if (pindex)
            {
                if (pindex->IsValid(BLOCK_VALID_SCRIPTS))
                    return "duplicate";
                if (pindex->nStatus & BLOCK_FAILED_MASK)
                    return "duplicate-invalid";
                // Otherwise, we might only have the header - process the block before returning
                fBlockPresent = true;
            }
        }
    }

    CValidationState state;
    submitblock_StateCatcher sc(block.GetHash());
    RegisterValidationInterface(&sc);
    //printf("submitblock, height=%d, coinbase sequence: %d, scriptSig: %s\n", chainActive.LastTip()->GetHeight()+1, block.vtx[0].vin[0].nSequence, block.vtx[0].vin[0].scriptSig.ToString().c_str());
    bool fAccepted = ProcessNewBlock(1,chainActive.LastTip()->GetHeight()+1,state, NULL, &block, true, NULL);
    UnregisterValidationInterface(&sc);
    if (fBlockPresent)
    {
        if (fAccepted && !sc.found)
            return "duplicate-inconclusive";
        return "duplicate";
    }
    if (fAccepted)
    {
        if (!sc.found)
            return "inconclusive";
        state = sc.state;
    }
    return BIP22ValidationResult(state);
}

UniValue getmergedblocktemplate(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw runtime_error(
            "getblocktemplate ( \"jsonrequestobject\" )\n"
            "\nIf the request parameters include a 'mode' key, that is used to explicitly select between the default 'template' request or a 'proposal'.\n"
            "It returns data needed to construct a block to work on.\n"
            "See https://en.bitcoin.it/wiki/BIP_0022 for full specification.\n"

            "\nArguments:\n"
            "1. \"jsonrequestobject\"       (string, optional) A json object in the following spec\n"
            "     {\n"
            "       \"mode\":\"template\"    (string, optional) This must be set to \"template\" or omitted\n"
            "       \"capabilities\":[       (array, optional) A list of strings\n"
            "           \"support\"           (string) client side supported feature, 'longpoll', 'coinbasetxn', 'coinbasevalue', 'proposal', 'serverlist', 'workid'\n"
            "           ,...\n"
            "         ]\n"
            "     }\n"
            "\n"

            "\nResult:\n"
            "{\n"
            "  \"version\" : n,                     (numeric) The block version\n"
            "  \"previousblockhash\" : \"xxxx\",    (string) The hash of current highest block\n"
            "  \"finalsaplingroothash\" : \"xxxx\", (string) The hash of the final sapling root\n"
            "  \"transactions\" : [                 (array) contents of non-coinbase transactions that should be included in the next block\n"
            "      {\n"
            "         \"data\" : \"xxxx\",          (string) transaction data encoded in hexadecimal (byte-for-byte)\n"
            "         \"hash\" : \"xxxx\",          (string) hash/id encoded in little-endian hexadecimal\n"
            "         \"depends\" : [              (array) array of numbers \n"
            "             n                        (numeric) transactions before this one (by 1-based index in 'transactions' list) that must be present in the final block if this one is\n"
            "             ,...\n"
            "         ],\n"
            "         \"fee\": n,                   (numeric) difference in value between transaction inputs and outputs (in Satoshis); for coinbase transactions, this is a negative Number of the total collected block fees (ie, not including the block subsidy); if key is not present, fee is unknown and clients MUST NOT assume there isn't one\n"
            "         \"sigops\" : n,               (numeric) total number of SigOps, as counted for purposes of block limits; if key is not present, sigop count is unknown and clients MUST NOT assume there aren't any\n"
            "         \"required\" : true|false     (boolean) if provided and true, this transaction must be in the final block\n"
            "      }\n"
            "      ,...\n"
            "  ],\n"
//            "  \"coinbaseaux\" : {                  (json object) data that should be included in the coinbase's scriptSig content\n"
//            "      \"flags\" : \"flags\"            (string) \n"
//            "  },\n"
//            "  \"coinbasevalue\" : n,               (numeric) maximum allowable input to coinbase transaction, including the generation award and transaction fees (in Satoshis)\n"
            "  \"coinbasetxn\" : { ... },           (json object) information for coinbase transaction\n"
            "  \"target\" : \"xxxx\",               (string) The hash target\n"
            "  \"mintime\" : xxx,                   (numeric) The minimum timestamp appropriate for next block time in seconds since epoch (Jan 1 1970 GMT)\n"
            "  \"mutable\" : [                      (array of string) list of ways the block template may be changed \n"
            "     \"value\"                         (string) A way the block template may be changed, e.g. 'time', 'transactions', 'prevblock'\n"
            "     ,...\n"
            "  ],\n"
            "  \"noncerange\" : \"00000000ffffffff\",   (string) A range of valid nonces\n"
            "  \"sigoplimit\" : n,                 (numeric) limit of sigops in blocks\n"
            "  \"sizelimit\" : n,                  (numeric) limit of block size\n"
            "  \"curtime\" : ttt,                  (numeric) current timestamp in seconds since epoch (Jan 1 1970 GMT)\n"
            "  \"bits\" : \"xxx\",                 (string) compressed target of next block\n"
            "  \"height\" : n                      (numeric) The height of the next block\n"
            "}\n"

            "\nExamples:\n"
            + HelpExampleCli("getblocktemplate", "")
            + HelpExampleRpc("getblocktemplate", "")
         );

    LOCK(cs_main);

    // Wallet or miner address is required because we support coinbasetxn
    if (GetArg("-mineraddress", "").empty()) {
#ifdef ENABLE_WALLET
        if (!pwalletMain) {
            throw JSONRPCError(RPC_METHOD_NOT_FOUND, "Wallet disabled and -mineraddress not set");
        }
#else
        throw JSONRPCError(RPC_METHOD_NOT_FOUND, "komodod compiled without wallet and -mineraddress not set");
#endif
    }

    std::string strMode = "template";
    UniValue lpval = NullUniValue;
    // TODO: Re-enable coinbasevalue once a specification has been written
    bool coinbasetxn = true;
    if (params.size() > 0)
    {
        const UniValue& oparam = params[0].get_obj();
        const UniValue& modeval = find_value(oparam, "mode");
        if (modeval.isStr())
            strMode = modeval.get_str();
        else if (modeval.isNull())
        {
            /* Do nothing */
        }
        else
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid mode");
        lpval = find_value(oparam, "longpollid");

        if (strMode == "proposal")
        {
            const UniValue& dataval = find_value(oparam, "data");
            if (!dataval.isStr())
                throw JSONRPCError(RPC_TYPE_ERROR, "Missing data String key for proposal");

            CBlock block;
            if (!DecodeHexBlk(block, dataval.get_str()))
                throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Block decode failed");

            uint256 hash = block.GetHash();
            BlockMap::iterator mi = mapBlockIndex.find(hash);
            if (mi != mapBlockIndex.end()) {
                CBlockIndex *pindex = mi->second;
                if (pindex)
                {
                    if (pindex->IsValid(BLOCK_VALID_SCRIPTS))
                        return "duplicate";
                    if (pindex->nStatus & BLOCK_FAILED_MASK)
                        return "duplicate-invalid";
                }
                return "duplicate-inconclusive";
            }

            CBlockIndex* const pindexPrev = chainActive.LastTip();
            // TestBlockValidity only supports blocks built on the current Tip
            if (block.hashPrevBlock != pindexPrev->GetBlockHash())
                return "inconclusive-not-best-prevblk";
            CValidationState state;
            TestBlockValidity(state, block, pindexPrev, false, true);
            return BIP22ValidationResult(state);
        }
    }

    if (strMode != "template")
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid mode");

    bool fvNodesEmpty;
    {
        LOCK(cs_vNodes);
        fvNodesEmpty = vNodes.empty();
    }
    if (Params().MiningRequiresPeers() && (IsNotInSync() || fvNodesEmpty))
    {
        throw JSONRPCError(RPC_CLIENT_NOT_CONNECTED, "Cannot get a block template while no peers are connected or chain not in sync!");
    }

    //if (IsInitialBlockDownload())
     //   throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD, "Zcash is downloading blocks...");

    static unsigned int nTransactionsUpdatedLast;

    if (!lpval.isNull())
    {
        // Wait to respond until either the best block changes, OR a minute has passed and there are more transactions
        uint256 hashWatchedChain;
        boost::system_time checktxtime;
        unsigned int nTransactionsUpdatedLastLP;

        if (lpval.isStr())
        {
            // Format: <hashBestChain><nTransactionsUpdatedLast>
            std::string lpstr = lpval.get_str();

            hashWatchedChain.SetHex(lpstr.substr(0, 64));
            nTransactionsUpdatedLastLP = atoi64(lpstr.substr(64));
        }
        else
        {
            // NOTE: Spec does not specify behaviour for non-string longpollid, but this makes testing easier
            hashWatchedChain = chainActive.LastTip()->GetBlockHash();
            nTransactionsUpdatedLastLP = nTransactionsUpdatedLast;
        }

        // Release the wallet and main lock while waiting
        LEAVE_CRITICAL_SECTION(cs_main);
        {
            checktxtime = boost::get_system_time() + boost::posix_time::minutes(1);

            boost::unique_lock<boost::mutex> lock(csBestBlock);
            while (chainActive.LastTip()->GetBlockHash() == hashWatchedChain && IsRPCRunning())
            {
                if (!cvBlockChange.timed_wait(lock, checktxtime))
                {
                    // Timeout: Check transactions for update
                    if (mempool.GetTransactionsUpdated() != nTransactionsUpdatedLastLP)
                        break;
                    checktxtime += boost::posix_time::seconds(10);
                }
            }
        }
        ENTER_CRITICAL_SECTION(cs_main);

        if (!IsRPCRunning())
            throw JSONRPCError(RPC_CLIENT_NOT_CONNECTED, "Shutting down");
        // TODO: Maybe recheck connections/IBD and (if something wrong) send an expires-immediately template to stop miners?
    }

    // Update block
    static CBlockIndex* pindexPrev;
    static int64_t nStart;
    static CBlockTemplate* pblocktemplate;
    if (pindexPrev != chainActive.LastTip() ||
        (mempool.GetTransactionsUpdated() != nTransactionsUpdatedLast && GetTime() - nStart > 5))
    {
        // Clear pindexPrev so future calls make a new block, despite any failures from here on
        pindexPrev = NULL;

        // Store the pindexBest used before CreateNewBlockWithKey, to avoid races
        nTransactionsUpdatedLast = mempool.GetTransactionsUpdated();
        CBlockIndex* pindexPrevNew = chainActive.LastTip();
        nStart = GetTime();

        // Create new block
        if(pblocktemplate)
        {
            delete pblocktemplate;
            pblocktemplate = NULL;
        }
#ifdef ENABLE_WALLET
        CReserveKey reservekey(pwalletMain);
        pblocktemplate = CreateNewBlockWithKey(reservekey,chainActive.LastTip()->GetHeight()+1,KOMODO_MAXGPUCOUNT);
#else
        pblocktemplate = CreateNewBlockWithKey();
#endif
        if (!pblocktemplate)
            throw JSONRPCError(RPC_OUT_OF_MEMORY, "Out of memory or no available utxo for staking");

        // Need to update only after we know CreateNewBlockWithKey succeeded
        pindexPrev = pindexPrevNew;
    }
    CBlock* pblock = &pblocktemplate->block; // pointer for convenience

    // Update nTime
    UpdateTime(pblock, Params().GetConsensus(), pindexPrev);
    pblock->nNonce = uint256();

    UniValue aCaps(UniValue::VARR); aCaps.push_back("proposal");

    UniValue txCoinbase = NullUniValue;
    UniValue transactions(UniValue::VARR);
    map<uint256, int64_t> setTxIndex;
    int i = 0;
    BOOST_FOREACH (const CTransaction& tx, pblock->vtx) {
        uint256 txHash = tx.GetHash();
        setTxIndex[txHash] = i++;

        if (tx.IsCoinBase() && !coinbasetxn)
            continue;

        UniValue entry(UniValue::VOBJ);

        entry.push_back(Pair("data", EncodeHexTx(tx)));

        entry.push_back(Pair("hash", txHash.GetHex()));

        UniValue deps(UniValue::VARR);
        BOOST_FOREACH (const CTxIn &in, tx.vin)
        {
            if (setTxIndex.count(in.prevout.hash))
                deps.push_back(setTxIndex[in.prevout.hash]);
        }
        entry.push_back(Pair("depends", deps));

        int index_in_template = i - 1;
        entry.push_back(Pair("fee", pblocktemplate->vTxFees[index_in_template]));
        entry.push_back(Pair("sigops", pblocktemplate->vTxSigOps[index_in_template]));

        if (tx.IsCoinBase()) {
            // Show founders' reward if it is required
            //if (pblock->vtx[0].vout.size() > 1) {
                // Correct this if GetBlockTemplate changes the order
            //    entry.push_back(Pair("foundersreward", (int64_t)tx.vout[1].nValue));
            //}
            CAmount nReward = GetBlockSubsidy(chainActive.LastTip()->GetHeight()+1, Params().GetConsensus());
            entry.push_back(Pair("coinbasevalue", nReward));
            entry.push_back(Pair("required", true));
            txCoinbase = entry;
        } else
            transactions.push_back(entry);
    }

    UniValue aux(UniValue::VOBJ);
    aux.push_back(Pair("flags", HexStr(COINBASE_FLAGS.begin(), COINBASE_FLAGS.end())));

    arith_uint256 hashTarget = arith_uint256().SetCompact(pblock->nBits);

    static UniValue aMutable(UniValue::VARR);
    if (aMutable.empty())
    {
        aMutable.push_back("time");
        aMutable.push_back("transactions");
        aMutable.push_back("prevblock");
    }

    UniValue result(UniValue::VOBJ);
    result.push_back(Pair("capabilities", aCaps));
    result.push_back(Pair("version", pblock->nVersion));
    result.push_back(Pair("previousblockhash", pblock->hashPrevBlock.GetHex()));
    result.push_back(Pair("finalsaplingroothash", pblock->hashFinalSaplingRoot.GetHex()));
    result.push_back(Pair("transactions", transactions));
    if (coinbasetxn) {
        assert(txCoinbase.isObject());
        result.push_back(Pair("coinbasetxn", txCoinbase));
    } else {
        result.push_back(Pair("coinbaseaux", aux));
        result.push_back(Pair("coinbasevalue", (int64_t)pblock->vtx[0].vout[0].nValue));
    }
    result.push_back(Pair("longpollid", chainActive.LastTip()->GetBlockHash().GetHex() + i64tostr(nTransactionsUpdatedLast)));
    if ( ASSETCHAINS_STAKED != 0 )
    {
        arith_uint256 POWtarget; int32_t PoSperc;
        POWtarget = komodo_PoWtarget(&PoSperc,hashTarget,(int32_t)(pindexPrev->GetHeight()+1),ASSETCHAINS_STAKED);
        result.push_back(Pair("target", POWtarget.GetHex()));
        result.push_back(Pair("PoSperc", (int64_t)PoSperc));
        result.push_back(Pair("ac_staked", (int64_t)ASSETCHAINS_STAKED));
        result.push_back(Pair("origtarget", hashTarget.GetHex()));
    } else result.push_back(Pair("target", hashTarget.GetHex()));
    result.push_back(Pair("mintime", (int64_t)pindexPrev->GetMedianTimePast()+1));
    result.push_back(Pair("mutable", aMutable));
    result.push_back(Pair("noncerange", "00000000ffffffff"));
    result.push_back(Pair("sigoplimit", (int64_t)MAX_BLOCK_SIGOPS));
    result.push_back(Pair("sizelimit", (int64_t)MAX_BLOCK_SIZE));
    result.push_back(Pair("curtime", pblock->GetBlockTime()));
    result.push_back(Pair("bits", strprintf("%08x", pblock->nBits)));
    result.push_back(Pair("height", (int64_t)(pindexPrev->GetHeight()+1)));

    //fprintf(stderr,"return complete template\n");
    return result;
}

static const CRPCCommand commands[] =
{ //  category              name                      actor (function)         okSafeMode
  //  --------------------- ------------------------  -----------------------  ----------
    { "pbaas",        "getchaindefinition",           &getchaindefinition,     true  },
    { "pbaas",        "getdefinedchains",             &getdefinedchains,       true  },
    { "pbaas",        "getmergedblocktemplate",       &getmergedblocktemplate, true  },
    { "pbaas",        "getnotarizationdata",          &getnotarizationdata,    true  },
    { "pbaas",        "getcrossnotarization",         &getcrossnotarization,   true  },
    { "pbaas",        "definechain",                  &definechain,            true  },
    { "pbaas",        "addmergedblock",               &addmergedblock,         true  }
};

void RegisterPBaaSRPCCommands(CRPCTable &tableRPC)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        tableRPC.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
