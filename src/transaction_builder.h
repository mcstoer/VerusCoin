// Copyright (c) 2018 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#ifndef TRANSACTION_BUILDER_H
#define TRANSACTION_BUILDER_H

#include "coins.h"
#include "consensus/params.h"
#include "keystore.h"
#include "primitives/transaction.h"
#include "script/script.h"
#include "script/standard.h"
#include "uint256.h"
#include "zcash/Address.hpp"
#include "zcash/IncrementalMerkleTree.hpp"
#include "zcash/JoinSplit.hpp"
#include "zcash/Note.hpp"
#include "zcash/NoteEncryption.hpp"

#include <boost/optional.hpp>

struct SpendDescriptionInfo {
    libzcash::SaplingExpandedSpendingKey expsk;
    libzcash::SaplingNote note;
    uint256 alpha;
    uint256 anchor;
    SaplingWitness witness;

    SpendDescriptionInfo(
        libzcash::SaplingExpandedSpendingKey expsk,
        libzcash::SaplingNote note,
        uint256 anchor,
        SaplingWitness witness);
};

struct OutputDescriptionInfo {
    uint256 ovk;
    libzcash::SaplingNote note;
    std::array<unsigned char, ZC_MEMO_SIZE> memo;

    OutputDescriptionInfo(
        uint256 ovk,
        libzcash::SaplingNote note,
        std::array<unsigned char, ZC_MEMO_SIZE> memo) : ovk(ovk), note(note), memo(memo) {}
};

struct TransparentInputInfo {
    CScript scriptPubKey;
    CAmount value;

    TransparentInputInfo(
        CScript scriptPubKey,
        CAmount value) : scriptPubKey(scriptPubKey), value(value) {}
};

class TransactionBuilderResult {
private:
    boost::optional<CTransaction> maybeTx;
    boost::optional<std::string> maybeError;
public:
    TransactionBuilderResult() = delete;
    TransactionBuilderResult(const CTransaction& tx);
    TransactionBuilderResult(const std::string& error);
    bool IsTx();
    bool IsError();
    bool IsHexTx(CTransaction *pTx=nullptr);
    CTransaction GetTxOrThrow();
    std::string GetError();
};

class TransactionBuilder
{
private:
    Consensus::Params consensusParams;
    int nHeight;
    const CKeyStore* keystore;
    ZCJoinSplit* sproutParams;
    const CCoinsViewCache* coinsView;
    CCriticalSection* cs_coinsView;
    CAmount fee = 10000;
    CCurrencyValueMap reserveFee;

    std::vector<SpendDescriptionInfo> spends;
    std::vector<OutputDescriptionInfo> outputs;
    std::vector<libzcash::JSInput> jsInputs;
    std::vector<libzcash::JSOutput> jsOutputs;
    std::vector<TransparentInputInfo> tIns;

    boost::optional<std::pair<uint256, libzcash::SaplingPaymentAddress>> saplingChangeAddr;
    boost::optional<libzcash::SproutPaymentAddress> sproutChangeAddr;
    boost::optional<CTxDestination> tChangeAddr;
    boost::optional<CScript> opReturn;

    bool AddOpRetLast(CScript &s);

public:
    CMutableTransaction mtx;

    TransactionBuilder() {}
    TransactionBuilder(
        const Consensus::Params& consensusParams,
        int nHeight,
        CKeyStore* keyStore = nullptr,
        ZCJoinSplit* sproutParams = nullptr,
        CCoinsViewCache* coinsView = nullptr,
        CCriticalSection* cs_coinsView = nullptr);

    void SetExpiryHeight(uint32_t nExpiryHeight);

    void SetFee(CAmount fee);
    CAmount GetFee() const
    {
        return fee;
    }

    void SetReserveFee(const CCurrencyValueMap &fees);
    CCurrencyValueMap GetReserveFee() const
    {
        return reserveFee;
    }

    int SpendCount() const
    {
        return spends.size();
    }

    CScript GetOpRet() const
    {
        if (opReturn)
        {
            return opReturn.get();
        }
        return CScript();
    }

    CTxDestination TransparentChangeAddress()
    {
        if (tChangeAddr)
        {
            return tChangeAddr.get();
        }
        return CTxDestination();
    }

    // Throws if the anchor does not match the anchor used by
    // previously-added Sapling spends.
    void AddSaplingSpend(
        libzcash::SaplingExpandedSpendingKey expsk,
        libzcash::SaplingNote note,
        uint256 anchor,
        SaplingWitness witness);

    void AddSaplingOutput(
        uint256 ovk,
        libzcash::SaplingPaymentAddress to,
        CAmount value,
        std::array<unsigned char, ZC_MEMO_SIZE> memo = {{0}});

    // Throws if the anchor does not match the anchor used by
    // previously-added Sprout inputs.
    void AddSproutInput(
        libzcash::SproutSpendingKey sk,
        libzcash::SproutNote note,
        SproutWitness witness);

    void AddSproutOutput(
        libzcash::SproutPaymentAddress to,
        CAmount value,
        std::array<unsigned char, ZC_MEMO_SIZE> memo = {{0xF6}});

    // Assumes that the value correctly corresponds to the provided UTXO.
    void AddTransparentInput(COutPoint utxo, CScript scriptPubKey, CAmount value, uint32_t nSequence = 0xffffffff);

    void AddTransparentOutput(const CTxDestination& to, CAmount value);

    bool AddTransparentOutput(const CScript &scriptPubKey, CAmount value);

    void AddOpRet(const CScript &s);

    bool AddOpRetLast();

    void SendChangeTo(libzcash::SaplingPaymentAddress changeAddr, uint256 ovk);

    void SendChangeTo(libzcash::SproutPaymentAddress);

    void SetLockTime(uint32_t time) { this->mtx.nLockTime = time; }

    void SendChangeTo(const CTxDestination &changeAddr);

    TransactionBuilderResult Build(bool throwTxWithPartialSig=false);

private:
    void CreateJSDescriptions();

    void CreateJSDescription(
        uint64_t vpub_old,
        uint64_t vpub_new,
        std::array<libzcash::JSInput, ZC_NUM_JS_INPUTS> vjsin,
        std::array<libzcash::JSOutput, ZC_NUM_JS_OUTPUTS> vjsout,
        std::array<size_t, ZC_NUM_JS_INPUTS>& inputMap,
        std::array<size_t, ZC_NUM_JS_OUTPUTS>& outputMap);
};

#endif /* TRANSACTION_BUILDER_H */
