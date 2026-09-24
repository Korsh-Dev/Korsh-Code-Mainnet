// Copyright (c) 2014-2023 The Korsh Core developers
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
    const auto& consensus = chainParams->GetConsensus();

    static constexpr CAmount kMaxMoney = 10'000'000 * COIN;
    static constexpr CAmount kInitialSubsidy = 2 * COIN;
    static constexpr int kHalvingInterval = 2500000;

    uint32_t nPrevBits;
    int32_t nPrevHeight;
    CAmount nSubsidy;

    BOOST_CHECK_EQUAL(consensus.nSubsidyHalvingInterval, kHalvingInterval);
    BOOST_CHECK_EQUAL(consensus.nMaxMoney, kMaxMoney);
    // Korsh mainnet keeps the treasury/superblock budget disabled.
    BOOST_CHECK_EQUAL(consensus.nBudgetPaymentsStartBlock, 999999999);

    // Mainnet starts at 2 KSH and the treasury is inactive, so the full subsidy
    // reaches the miner + masternode split.
    nPrevBits = 0x1e3fffff;
    nPrevHeight = 1;
    nSubsidy = GetBlockSubsidyInner(nPrevBits, nPrevHeight, consensus, /*fV20Active=*/ false);
    BOOST_CHECK_EQUAL(nSubsidy, kInitialSubsidy);

    // V20 does not affect subsidy scheduling on Korsh mainnet/testnet.
    nPrevBits = 0x1b10d50b;
    nPrevHeight = 4249;
    nSubsidy = GetBlockSubsidyInner(nPrevBits, nPrevHeight, consensus, /*fV20Active=*/ true);
    BOOST_CHECK_EQUAL(nSubsidy, kInitialSubsidy);

    // The last block before the first halving still pays the initial subsidy.
    nPrevHeight = kHalvingInterval - 1;
    nSubsidy = GetBlockSubsidyInner(nPrevBits, nPrevHeight, consensus, /*fV20Active=*/ false);
    BOOST_CHECK_EQUAL(nSubsidy, kInitialSubsidy);

    // First halving: era 2 pays half of the initial subsidy.
    nPrevHeight = kHalvingInterval;
    nSubsidy = GetBlockSubsidyInner(nPrevBits, nPrevHeight, consensus, /*fV20Active=*/ false);
    BOOST_CHECK_EQUAL(nSubsidy, kInitialSubsidy / 2);

    // The treasury share is zero on mainnet at any height.
    BOOST_CHECK_EQUAL(GetSuperblockSubsidyInner(nPrevBits, nPrevHeight, consensus, /*fV20Active=*/ false), 0);
}

BOOST_AUTO_TEST_CASE(korsh_v040_disabled_test)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    const auto& consensus = chainParams->GetConsensus();

    // Korsh mainnet keeps the v0.4.0 timing changes disabled (60 second blocks,
    // no superblock or governance schedule).
    BOOST_CHECK_EQUAL(consensus.nKSHv040Height, 999999999);
    BOOST_CHECK_EQUAL(consensus.PowTargetSpacing(0), 60);
    BOOST_CHECK_EQUAL(consensus.PowTargetSpacing(consensus.nKSHv040Height - 1), 60);
    BOOST_CHECK_EQUAL(consensus.nPowTargetSpacing, 60);
    BOOST_CHECK_EQUAL(consensus.nSuperblockStartBlock, 999999999);
    BOOST_CHECK_EQUAL(consensus.nSuperblockCycle, 21600);
}

BOOST_AUTO_TEST_CASE(masternode_payment_v014_test)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    const int nKSHv014Height = chainParams->GetConsensus().nKSHv014Height;
    const int nKSHv030Height = chainParams->GetConsensus().nKSHv030Height;

    // After the v0.1.4 fork: masternodes get 3/10 of the distributable reward.
    // Korsh mainnet has no treasury, so this is 30% of the block subsidy.
    CAmount blockValue = 2 * COIN; // 2 KSH block subsidy
    CAmount mnPayment = GetMasternodePayment(nKSHv014Height, blockValue, /*fV20Active=*/ true);
    BOOST_CHECK_EQUAL(mnPayment, blockValue * 3 / 10); // 0.6 KSH

    // Miner gets the other 70%
    CAmount minerPayment = blockValue - mnPayment;
    BOOST_CHECK_EQUAL(minerPayment, blockValue * 7 / 10); // 1.4 KSH

    // One block before the v0.3.0 fork still uses the v0.1.4 70/30 split
    CAmount blockValueHalved = COIN; // 1 KSH (pretend subsidy halved)
    mnPayment = GetMasternodePayment(nKSHv030Height - 1, blockValueHalved, /*fV20Active=*/ true);
    BOOST_CHECK_EQUAL(mnPayment, blockValueHalved * 3 / 10); // 0.3 KSH
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
