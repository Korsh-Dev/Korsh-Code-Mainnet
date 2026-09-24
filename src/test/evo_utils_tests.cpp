// Copyright (c) 2022-2025 The Korsh Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>

#include <chainparams.h>
#include <llmq/options.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

using node::NodeContext;

/* TODO: rename this file and test to llmq_options_test */
BOOST_AUTO_TEST_SUITE(evo_utils_tests)

void Test(NodeContext& node)
{
    using namespace llmq;
    auto tip = node.chainman->ActiveTip();
    const auto& consensus_params = Params().GetConsensus();
    BOOST_CHECK_EQUAL(node.chainman->IsQuorumTypeEnabled(consensus_params.llmqTypeDIP0024InstantSend, tip,
                                                         /*optDIP0024IsActive=*/false, /*optHaveDIP0024Quorums=*/false),
                      false);
    BOOST_CHECK_EQUAL(node.chainman->IsQuorumTypeEnabled(consensus_params.llmqTypeDIP0024InstantSend, tip,
                                                         /*optDIP0024IsActive=*/true, /*optHaveDIP0024Quorums=*/false),
                      true);
    BOOST_CHECK_EQUAL(node.chainman->IsQuorumTypeEnabled(consensus_params.llmqTypeDIP0024InstantSend, tip,
                                                         /*optDIP0024IsActive=*/true, /*optHaveDIP0024Quorums=*/true),
                      true);
    BOOST_CHECK_EQUAL(node.chainman->IsQuorumTypeEnabled(consensus_params.llmqTypeChainLocks, tip,
                                                         /*optDIP0024IsActive=*/false, /*optHaveDIP0024Quorums=*/false),
                      true);
    BOOST_CHECK_EQUAL(node.chainman->IsQuorumTypeEnabled(consensus_params.llmqTypeChainLocks, tip,
                                                         /*optDIP0024IsActive=*/true, /*optHaveDIP0024Quorums=*/false),
                      true);
    BOOST_CHECK_EQUAL(node.chainman->IsQuorumTypeEnabled(consensus_params.llmqTypeChainLocks, tip,
                                                         /*optDIP0024IsActive=*/true, /*optHaveDIP0024Quorums=*/true),
                      true);
    BOOST_CHECK_EQUAL(node.chainman->IsQuorumTypeEnabled(consensus_params.llmqTypePlatform, tip,
                                                         /*optDIP0024IsActive=*/false, /*optHaveDIP0024Quorums=*/false),
                      Params().IsTestChain());
    BOOST_CHECK_EQUAL(node.chainman->IsQuorumTypeEnabled(consensus_params.llmqTypePlatform, tip,
                                                         /*optDIP0024IsActive=*/true, /*optHaveDIP0024Quorums=*/false),
                      Params().IsTestChain());
    BOOST_CHECK_EQUAL(node.chainman->IsQuorumTypeEnabled(consensus_params.llmqTypePlatform, tip,
                                                         /*optDIP0024IsActive=*/true, /*optHaveDIP0024Quorums=*/true),
                      Params().IsTestChain());
    BOOST_CHECK_EQUAL(node.chainman->IsQuorumTypeEnabled(consensus_params.llmqTypeMnhf, tip,
                                                         /*optDIP0024IsActive=*/false, /*optHaveDIP0024Quorums=*/false),
                      true);
    BOOST_CHECK_EQUAL(node.chainman->IsQuorumTypeEnabled(consensus_params.llmqTypeMnhf, tip,
                                                         /*optDIP0024IsActive=*/true, /*optHaveDIP0024Quorums=*/false),
                      true);
    BOOST_CHECK_EQUAL(node.chainman->IsQuorumTypeEnabled(consensus_params.llmqTypeMnhf, tip,
                                                         /*optDIP0024IsActive=*/true, /*optHaveDIP0024Quorums=*/true),
                      true);
}

BOOST_FIXTURE_TEST_CASE(utils_IsQuorumTypeEnabled_tests_regtest, RegTestingSetup)
{
    Test(m_node);
}

BOOST_FIXTURE_TEST_CASE(utils_IsQuorumTypeEnabled_tests_mainnet, TestingSetup)
{
    // Korsh mainnet disables every quorum-based service, so all quorum roles map to
    // LLMQ_NONE and IsQuorumTypeEnabled() must report them as disabled regardless of
    // the DIP0024 flags.
    using namespace llmq;
    auto tip = m_node.chainman->ActiveTip();
    const auto& consensus_params = Params().GetConsensus();
    for (const auto llmq_type : {consensus_params.llmqTypeDIP0024InstantSend, consensus_params.llmqTypeChainLocks,
                                 consensus_params.llmqTypePlatform, consensus_params.llmqTypeMnhf}) {
        BOOST_CHECK(llmq_type == Consensus::LLMQType::LLMQ_NONE);
        BOOST_CHECK_EQUAL(m_node.chainman->IsQuorumTypeEnabled(llmq_type, tip, /*optDIP0024IsActive=*/false, /*optHaveDIP0024Quorums=*/false), false);
        BOOST_CHECK_EQUAL(m_node.chainman->IsQuorumTypeEnabled(llmq_type, tip, /*optDIP0024IsActive=*/true, /*optHaveDIP0024Quorums=*/false), false);
        BOOST_CHECK_EQUAL(m_node.chainman->IsQuorumTypeEnabled(llmq_type, tip, /*optDIP0024IsActive=*/true, /*optHaveDIP0024Quorums=*/true), false);
    }
}

BOOST_AUTO_TEST_SUITE_END()
