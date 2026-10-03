// Copyright (c) 2011-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <clientversion.h>
#include <policy/fees.h>
#include <policy/policy.h>
#include <streams.h>
#include <test/util/txmempool.h>
#include <txmempool.h>
#include <uint256.h>
#include <util/system.h>
#include <util/time.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <limits>

BOOST_FIXTURE_TEST_SUITE(policyestimator_tests, ChainTestingSetup)

BOOST_AUTO_TEST_CASE(BlockPolicyEstimates)
{
    CBlockPolicyEstimator& feeEst = *Assert(m_node.fee_estimator);
    CTxMemPool& mpool = *Assert(m_node.mempool);
    LOCK2(cs_main, mpool.cs);
    TestMemPoolEntryHelper entry;
    CAmount basefee(2000);
    CAmount deltaFee(100);
    std::vector<CAmount> feeV;

    // Populate vectors of increasing fees
    for (int j = 0; j < 10; j++) {
        feeV.push_back(basefee * (j+1));
    }

    // Store the hashes of transactions that have been
    // added to the mempool by their associate fee
    // txHashes[j] is populated with transactions either of
    // fee = basefee * (j+1)
    std::vector<uint256> txHashes[10];

    // Create a transaction template
    CScript garbage;
    for (unsigned int i = 0; i < 128; i++)
        garbage.push_back('X');
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vin[0].scriptSig = garbage;
    tx.vout.resize(1);
    tx.vout[0].nValue=0LL;
    CFeeRate baseRate(basefee, GetVirtualTransactionSize(CTransaction(tx)));

    // Create a fake block
    std::vector<CTransactionRef> block;
    int blocknum = 0;

    // Loop through 200 blocks
    // At a decay .9952 and 4 fee transactions per block
    // This makes the tx count about 2.5 per bucket, well above the 0.1 threshold
    while (blocknum < 200) {
        for (int j = 0; j < 10; j++) { // For each fee
            for (int k = 0; k < 4; k++) { // add 4 fee txs
                tx.vin[0].prevout.n = 10000*blocknum+100*j+k; // make transaction unique
                uint256 hash = tx.GetHash();
                mpool.addUnchecked(entry.Fee(feeV[j]).Time(Now<NodeSeconds>()).Height(blocknum).FromTx(tx));
                txHashes[j].push_back(hash);
            }
        }
        //Create blocks where higher fee txs are included more often
        for (int h = 0; h <= blocknum%10; h++) {
            // 10/10 blocks add highest fee transactions
            // 9/10 blocks add 2nd highest and so on until ...
            // 1/10 blocks add lowest fee transactions
            while (txHashes[9-h].size()) {
                CTransactionRef ptx = mpool.get(txHashes[9-h].back());
                if (ptx)
                    block.push_back(ptx);
                txHashes[9-h].pop_back();
            }
        }
        mpool.removeForBlock(block, ++blocknum);
        block.clear();
        // Check after just a few txs that combining buckets works as expected
        if (blocknum == 3) {
            // At this point we should need to combine 3 buckets to get enough data points
            // So estimateFee(1) should fail and estimateFee(2) should return somewhere around
            // 9*baserate.  estimateFee(2) %'s are 100,100,90 = average 97%
            BOOST_CHECK(feeEst.estimateFee(1) == CFeeRate(0));
            BOOST_CHECK(feeEst.estimateFee(2).GetFeePerK() < 9*baseRate.GetFeePerK() + deltaFee);
            BOOST_CHECK(feeEst.estimateFee(2).GetFeePerK() > 9*baseRate.GetFeePerK() - deltaFee);
        }
    }

    std::vector<CAmount> origFeeEst;
    // Highest feerate is 10*baseRate and gets in all blocks,
    // second highest feerate is 9*baseRate and gets in 9/10 blocks = 90%,
    // third highest feerate is 8*base rate, and gets in 8/10 blocks = 80%,
    // so estimateFee(1) would return 10*baseRate but is hardcoded to return failure
    // Second highest feerate has 100% chance of being included by 2 blocks,
    // so estimateFee(2) should return 9*baseRate etc...
    for (int i = 1; i < 10;i++) {
        origFeeEst.push_back(feeEst.estimateFee(i).GetFeePerK());
        if (i > 2) { // Fee estimates should be monotonically decreasing
            BOOST_CHECK(origFeeEst[i-1] <= origFeeEst[i-2]);
        }
        int mult = 11-i;
        if (i % 2 == 0) { //At scale 2, test logic is only correct for even targets
            BOOST_CHECK(origFeeEst[i-1] < mult*baseRate.GetFeePerK() + deltaFee);
            BOOST_CHECK(origFeeEst[i-1] > mult*baseRate.GetFeePerK() - deltaFee);
        }
    }
    // Fill out rest of the original estimates
    for (int i = 10; i <= 48; i++) {
        origFeeEst.push_back(feeEst.estimateFee(i).GetFeePerK());
    }

    // Mine 50 more blocks with no transactions happening, estimates shouldn't change
    // We haven't decayed the moving average enough so we still have enough data points in every bucket
    while (blocknum < 250)
        mpool.removeForBlock(block, ++blocknum);

    BOOST_CHECK(feeEst.estimateFee(1) == CFeeRate(0));
    for (int i = 2; i < 10;i++) {
        BOOST_CHECK(feeEst.estimateFee(i).GetFeePerK() < origFeeEst[i-1] + deltaFee);
        BOOST_CHECK(feeEst.estimateFee(i).GetFeePerK() > origFeeEst[i-1] - deltaFee);
    }


    // Mine 15 more blocks with lots of transactions happening and not getting mined
    // Estimates should go up
    while (blocknum < 265) {
        for (int j = 0; j < 10; j++) { // For each fee multiple
            for (int k = 0; k < 4; k++) { // add 4 fee txs
                tx.vin[0].prevout.n = 10000*blocknum+100*j+k;
                uint256 hash = tx.GetHash();
                mpool.addUnchecked(entry.Fee(feeV[j]).Time(Now<NodeSeconds>()).Height(blocknum).FromTx(tx));
                txHashes[j].push_back(hash);
            }
        }
        mpool.removeForBlock(block, ++blocknum);
    }

    for (int i = 1; i < 10;i++) {
        BOOST_CHECK(feeEst.estimateFee(i) == CFeeRate(0) || feeEst.estimateFee(i).GetFeePerK() > origFeeEst[i-1] - deltaFee);
    }

    // Mine all those transactions
    // Estimates should still not be below original
    for (int j = 0; j < 10; j++) {
        while(txHashes[j].size()) {
            CTransactionRef ptx = mpool.get(txHashes[j].back());
            if (ptx)
                block.push_back(ptx);
            txHashes[j].pop_back();
        }
    }
    mpool.removeForBlock(block, 266);
    block.clear();
    BOOST_CHECK(feeEst.estimateFee(1) == CFeeRate(0));
    for (int i = 2; i < 10;i++) {
        BOOST_CHECK(feeEst.estimateFee(i) == CFeeRate(0) || feeEst.estimateFee(i).GetFeePerK() > origFeeEst[i-1] - deltaFee);
    }

    // Mine 400 more blocks where everything is mined every block
    // Estimates should be below original estimates
    while (blocknum < 665) {
        for (int j = 0; j < 10; j++) { // For each fee multiple
            for (int k = 0; k < 4; k++) { // add 4 fee txs
                tx.vin[0].prevout.n = 10000*blocknum+100*j+k;
                uint256 hash = tx.GetHash();
                mpool.addUnchecked(entry.Fee(feeV[j]).Time(Now<NodeSeconds>()).Height(blocknum).FromTx(tx));
                CTransactionRef ptx = mpool.get(hash);
                if (ptx)
                    block.push_back(ptx);

            }
        }
        mpool.removeForBlock(block, ++blocknum);
        block.clear();
    }
    BOOST_CHECK(feeEst.estimateFee(1) == CFeeRate(0));
    for (int i = 2; i < 9; i++) { // At 9, the original estimate was already at the bottom (b/c scale = 2)
        BOOST_CHECK(feeEst.estimateFee(i).GetFeePerK() < origFeeEst[i-1] - deltaFee);
    }
}

namespace {
// All files and constructor reads are confined to ChainTestingSetup's datadir.
std::vector<unsigned char> WriteEstimates(const CBlockPolicyEstimator& estimator)
{
    AutoFile file{fsbridge::fopen(gArgs.GetDataDirNet() / "estimator-roundtrip.dat", "w+b")};
    BOOST_REQUIRE(!file.IsNull());
    BOOST_REQUIRE(estimator.Write(file));
    BOOST_REQUIRE_EQUAL(std::fflush(file.Get()), 0);
    const long size = std::ftell(file.Get());
    BOOST_REQUIRE(size > 0);
    std::rewind(file.Get());
    std::vector<unsigned char> bytes(size);
    BOOST_REQUIRE_EQUAL(std::fread(bytes.data(), 1, bytes.size(), file.Get()), bytes.size());
    return bytes;
}

bool ReadEstimates(CBlockPolicyEstimator& estimator, const std::vector<unsigned char>& bytes)
{
    AutoFile file{fsbridge::fopen(gArgs.GetDataDirNet() / "estimator-input.dat", "w+b")};
    BOOST_REQUIRE(!file.IsNull());
    BOOST_REQUIRE_EQUAL(std::fwrite(bytes.data(), 1, bytes.size(), file.Get()), bytes.size());
    BOOST_REQUIRE_EQUAL(std::fflush(file.Get()), 0);
    std::rewind(file.Get());
    return estimator.Read(file);
}

std::vector<CAmount> EstimateSnapshot(const CBlockPolicyEstimator& estimator)
{
    std::vector<CAmount> result;
    for (const auto horizon : {FeeEstimateHorizon::SHORT_HALFLIFE, FeeEstimateHorizon::MED_HALFLIFE, FeeEstimateHorizon::LONG_HALFLIFE}) {
        const auto rate = estimator.estimateRawFee(12, 0.95, horizon).GetFeePerK();
        BOOST_REQUIRE(rate > 0);
        result.push_back(rate);
    }
    for (bool conservative : {false, true}) {
        for (int target : {2, 6, 12, 48, 100, 1008}) {
            FeeCalculation calc;
            const auto rate = estimator.estimateSmartFee(target, &calc, conservative).GetFeePerK();
            BOOST_REQUIRE(rate > 0);
            result.push_back(rate);
            result.push_back(calc.returnedTarget);
        }
    }
    return result;
}

void SetEstimateInt(std::vector<unsigned char>& bytes, size_t offset, uint32_t value)
{
    BOOST_REQUIRE(bytes.size() >= offset + 4);
    WriteLE32(bytes.data() + offset, value);
}
} // namespace

BOOST_AUTO_TEST_CASE(PopulatedFeeEstimatePersistence)
{
    CBlockPolicyEstimator source;
    TestMemPoolEntryHelper helper;
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vout.resize(1);
    tx.vin[0].scriptSig = CScript() << std::vector<unsigned char>(128, 'X');
    // Every observation is confirmed; no unserialized mempool counters remain.
    for (unsigned int height = 0; height < 200; ++height) {
        std::vector<CTxMemPoolEntry> entries;
        entries.reserve(40);
        for (unsigned int i = 0; i < 40; ++i) {
            tx.vin[0].prevout.n = height * 40 + i;
            entries.push_back(helper.Fee(2000 + 100 * i).Height(height).FromTx(tx));
            source.processTransaction(entries.back(), true);
        }
        std::vector<const CTxMemPoolEntry*> block;
        for (const auto& entry : entries) block.push_back(&entry);
        source.processBlock(height + 1, block);
    }
    source.FlushUnconfirmed();
    const auto expected = EstimateSnapshot(source);
    const auto original = WriteEstimates(source);
    BOOST_REQUIRE_EQUAL(ReadLE32(original.data()), 140100U);
    BOOST_CHECK_EQUAL(ReadLE32(original.data() + 4), CLIENT_VERSION);
    CBlockPolicyEstimator restored;
    BOOST_REQUIRE(ReadEstimates(restored, original));
    BOOST_CHECK(EstimateSnapshot(restored) == expected);
    // Newly collected history and reserialized historical history take opposite
    // Write branches, but must preserve identical bytes/target availability.
    BOOST_CHECK(WriteEstimates(restored) == original);
    CBlockPolicyEstimator twice;
    BOOST_REQUIRE(ReadEstimates(twice, WriteEstimates(restored)));
    BOOST_CHECK(EstimateSnapshot(twice) == expected);

    // Synthetic historical/future writer metadata, not foreign-chain fixtures.
    // The required format, not the writer's release number, governs acceptance.
    for (int writer : {5, 6, 140100, 220103, std::numeric_limits<int>::max()}) {
        auto bytes = original;
        SetEstimateInt(bytes, 4, writer);
        CBlockPolicyEstimator target;
        BOOST_REQUIRE(ReadEstimates(target, bytes));
        BOOST_CHECK(EstimateSnapshot(target) == expected);
        BOOST_CHECK(WriteEstimates(target) == original);
    }
    const auto assert_unchanged = [&](const std::vector<unsigned char>& bytes, bool accepted) {
        BOOST_CHECK_EQUAL(ReadEstimates(restored, bytes), accepted);
        BOOST_CHECK(WriteEstimates(restored) == original);
        BOOST_CHECK(EstimateSnapshot(restored) == expected);
    };
    for (int required : {140101, std::numeric_limits<int>::max()}) {
        auto bytes = original;
        SetEstimateInt(bytes, 0, required);
        assert_unchanged(bytes, false);
        bytes.resize(8); // Reject even before the common height/payload is read.
        assert_unchanged(bytes, false);
    }
    for (int required : {6, 140099}) {
        auto bytes = original;
        SetEstimateInt(bytes, 0, required);
        bytes.resize(12); // Existing unsupported-old behavior: true, no import.
        assert_unchanged(bytes, true);
        bytes.pop_back();
        assert_unchanged(bytes, false);
    }
    for (size_t length : {size_t{0}, size_t{7}, size_t{11}, size_t{19}, original.size() / 2, original.size() - 1}) {
        assert_unchanged({original.begin(), original.begin() + length}, false);
    }
    auto corrupt = original;
    SetEstimateInt(corrupt, 12, 201); // historical first > historical best
    assert_unchanged(corrupt, false);
    corrupt = original;
    SetEstimateInt(corrupt, 16, 201); // historical best > current best
    assert_unchanged(corrupt, false);
    corrupt = original;
    corrupt[20] = 1; // invalid bucket count
    assert_unchanged(corrupt, false);

    // Locate each horizon in the unchanged layout using integer representations
    // of the encoded doubles. Corruption late in the file must not import the
    // earlier, already parsed horizons either.
    DataStream stream{original};
    stream.ignore(20);
    std::vector<uint64_t> buckets;
    stream >> buckets;
    for (int horizon = 0; horizon < 3; ++horizon) {
        BOOST_TEST_CONTEXT("serialized horizon " << horizon) {
            const size_t start = original.size() - stream.size();
            uint64_t decay;
            unsigned int scale;
            std::vector<uint64_t> averages, counts;
            std::vector<std::vector<uint64_t>> confirms, failures;
            stream >> decay >> scale >> averages >> counts;
            const size_t matrix = original.size() - stream.size();
            stream >> confirms >> failures;
            corrupt = original;
            WriteLE64(corrupt.data() + start, 0); // invalid zero decay
            assert_unchanged(corrupt, false);
            corrupt = original;
            SetEstimateInt(corrupt, start + 8, 0); // invalid zero scale
            assert_unchanged(corrupt, false);
            corrupt = original;
            // The existing horizons have one-byte CompactSize row counts.
            BOOST_REQUIRE(confirms.size() < 253);
            BOOST_REQUIRE(original[matrix] == confirms.size());
            corrupt[matrix + 1] = 1; // mismatched first confirmation row
            assert_unchanged(corrupt, false);
            assert_unchanged({original.begin(), original.begin() + start + 12}, false);
        }
    }
    BOOST_CHECK(stream.empty());
}

BOOST_AUTO_TEST_SUITE_END()
