// Copyright (c) 2026 The Korsh Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef KORSH_QT_KORSHFEATURES_H
#define KORSH_QT_KORSHFEATURES_H

#include <chainparams.h>
#include <consensus/params.h>
#include <interfaces/node.h>

#include <limits>

/**
 * Capability gates for the wallet GUI.
 *
 * Korsh ships with the quorum-based services (InstantSend, ChainLocks,
 * Platform/Evo) inactive and with no treasury, while masternodes and
 * PrivateSend run from the first blocks. The GUI must only offer what the
 * network can actually do, and it must offer a service again automatically
 * once the matching quorum types, sporks and heights are configured.
 */
namespace KorshFeatures {

/** InstantSend needs a configured quorum type and its spork enabled. */
inline bool InstantSendEnabled(interfaces::Node& node)
{
    return node.llmq().isInstantSendEnabled();
}

/** ChainLocks need a configured quorum type, DIP0008 and its spork enabled. */
inline bool ChainLocksEnabled(interfaces::Node& node)
{
    return node.llmq().isChainLocksEnabled();
}

/**
 * Governance proposals are only ever paid out of the superblock budget, so
 * without budget payments scheduled there is nothing governance can do.
 */
inline bool GovernanceEnabled()
{
    return Params().GetConsensus().nBudgetPaymentsStartBlock < std::numeric_limits<int>::max();
}

/** Evo/Platform masternodes need a configured platform quorum type. */
inline bool EvoEnabled()
{
    return Params().GetConsensus().llmqTypePlatform != Consensus::LLMQType::LLMQ_NONE;
}

} // namespace KorshFeatures

#endif // KORSH_QT_KORSHFEATURES_H
