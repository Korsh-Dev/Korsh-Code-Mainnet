// Copyright (c) 2014-2023 The Smartiecoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <governance/classes.h>
#include <validation.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(subsidy_tests, TestingSetup)

BOOST_AUTO_TEST_CASE(block_subsidy_test)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);

    static constexpr CAmount kMaxMoney = 10'000'000 * COIN;
    static constexpr CAmount kInitialSubsidy = 50 * COIN;
    static constexpr int kOldHalvingInterval = 1030596;

    uint32_t nPrevBits;
    int32_t nPrevHeight;
    CAmount nSubsidy;

    // chainparams keeps the old interval; the new one activates at nKSHv014Height
    BOOST_CHECK_EQUAL(chainParams->GetConsensus().nSubsidyHalvingInterval, kOldHalvingInterval);
    BOOST_CHECK_EQUAL(chainParams->GetConsensus().nMaxMoney, kMaxMoney);

    // Mainnet starts at 50 KSH subsidy.
    nPrevBits = 0x1e3fffff;
    nPrevHeight = 1;
    nSubsidy = GetBlockSubsidyInner(nPrevBits, nPrevHeight, chainParams->GetConsensus(), /*fV20Active=*/ false);
    BOOST_CHECK_EQUAL(nSubsidy, kInitialSubsidy);

    // V20 does not affect subsidy scheduling on Korsh mainnet/testnet.
    nPrevBits = 0x1b10d50b;
    nPrevHeight = 4249;
    nSubsidy = GetBlockSubsidyInner(nPrevBits, nPrevHeight, chainParams->GetConsensus(), /*fV20Active=*/ true);
    BOOST_CHECK_EQUAL(nSubsidy, kInitialSubsidy);

    // Korsh reaches its 10M KSH cap before the first halving boundary.
    nPrevBits = 0x1b10d50b;
    nPrevHeight = 199999;
    nSubsidy = GetBlockSubsidyInner(nPrevBits, nPrevHeight, chainParams->GetConsensus(), /*fV20Active=*/ false);
    BOOST_CHECK_EQUAL(nSubsidy, 45 * COIN); // 50 KSH - 10% treasury

    nPrevHeight = 200000;
    nSubsidy = GetBlockSubsidyInner(nPrevBits, nPrevHeight, chainParams->GetConsensus(), /*fV20Active=*/ false);
    BOOST_CHECK_EQUAL(nSubsidy, 0);
    BOOST_CHECK_EQUAL(GetSuperblockSubsidyInner(nPrevBits, nPrevHeight, chainParams->GetConsensus(), /*fV20Active=*/ false), 0);
}

BOOST_AUTO_TEST_CASE(smt_v040_timing_and_superblock_cycle_test)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    const auto& consensus = chainParams->GetConsensus();

    BOOST_CHECK_EQUAL(consensus.nKSHv040Height, 172800);
    BOOST_CHECK_EQUAL(consensus.PowTargetSpacing(consensus.nKSHv040Height - 1), 60);
    BOOST_CHECK_EQUAL(consensus.PowTargetSpacing(consensus.nKSHv040Height), 120);

    int last_superblock{0};
    int next_superblock{0};

    CSuperblock::GetNearestSuperblocksHeights(consensus.nKSHv040Height - 1, last_superblock, next_superblock);
    BOOST_CHECK_EQUAL(last_superblock, 151200);
    BOOST_CHECK_EQUAL(next_superblock, 172800);
    BOOST_CHECK(CSuperblock::IsValidBlockHeight(172800));
    BOOST_CHECK_EQUAL(CSuperblock::GetPaymentCycle(172800), 10800);

    CSuperblock::GetNearestSuperblocksHeights(consensus.nKSHv040Height, last_superblock, next_superblock);
    BOOST_CHECK_EQUAL(last_superblock, 172800);
    BOOST_CHECK_EQUAL(next_superblock, 183600);
    BOOST_CHECK(CSuperblock::IsValidBlockHeight(183600));
    BOOST_CHECK(!CSuperblock::IsValidBlockHeight(194399));
    BOOST_CHECK(CSuperblock::IsValidBlockHeight(194400));
}

BOOST_AUTO_TEST_CASE(masternode_payment_v014_test)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    const int nKSHv014Height = chainParams->GetConsensus().nKSHv014Height;
    const int nKSHv030Height = chainParams->GetConsensus().nKSHv030Height;

    // After v0.1.4 fork: MN gets 50% of distributable (= 45% of total subsidy)
    // blockValue passed to GetMasternodePayment is already after treasury deduction
    CAmount blockValue = 45 * COIN; // 50 KSH - 10% treasury = 45 KSH
    CAmount mnPayment = GetMasternodePayment(nKSHv014Height, blockValue, /*fV20Active=*/ true);
    BOOST_CHECK_EQUAL(mnPayment, blockValue / 2); // 22.5 KSH

    // Miner gets the other half
    CAmount minerPayment = blockValue - mnPayment;
    BOOST_CHECK_EQUAL(minerPayment, blockValue / 2); // 22.5 KSH

    // One block before v0.3.0 fork still uses v0.1.4 50/50 split
    CAmount blockValueHalved = 2250000000; // 22.5 KSH (pretend subsidy halved)
    mnPayment = GetMasternodePayment(nKSHv030Height - 1, blockValueHalved, /*fV20Active=*/ true);
    BOOST_CHECK_EQUAL(mnPayment, blockValueHalved / 2); // 11.25 KSH
}

BOOST_AUTO_TEST_CASE(masternode_payment_v030_test)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    const int nKSHv030Height = chainParams->GetConsensus().nKSHv030Height;

    // After v0.3.0 fork: 18/72/10 split.
    // Treasury (10%) is already deducted from blockValue, so blockValue = 90% of subsidy.
    // MN = blockValue * 4/5 = 80% of blockValue = 72% of subsidy.
    // Miner = blockValue * 1/5 = 20% of blockValue = 18% of subsidy.
    CAmount blockValue = 45 * COIN; // 50 KSH - 10% treasury = 45 KSH
    CAmount mnPayment = GetMasternodePayment(nKSHv030Height, blockValue, /*fV20Active=*/ true);
    BOOST_CHECK_EQUAL(mnPayment, 36 * COIN); // 72% of 50 KSH
    BOOST_CHECK_EQUAL(mnPayment, blockValue * 4 / 5);

    CAmount minerPayment = blockValue - mnPayment;
    BOOST_CHECK_EQUAL(minerPayment, 9 * COIN); // 18% of 50 KSH

    // Same ratios apply at any height >= nKSHv030Height (subsidy halving is independent).
    CAmount blockValueHalved = 2250000000; // 22.5 KSH post-halving distributable
    mnPayment = GetMasternodePayment(nKSHv030Height + 1000000, blockValueHalved, /*fV20Active=*/ true);
    BOOST_CHECK_EQUAL(mnPayment, 1800000000); // 18 KSH = 72% of 25 KSH base
}

BOOST_AUTO_TEST_SUITE_END()
