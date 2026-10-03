// Copyright (c) 2016-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>
#include <bench/data.h>

#include <arith_uint256.h>
#include <chainparams.h>
#include <consensus/validation.h>
#include <pow.h>
#include <stats/client.h>
#include <streams.h>
#include <util/system.h>
#include <validation.h>

#include <memory>

// These are the two major time-sinks which happen after we have fully received
// a block off the wire, but before we can relay the block on to peers using
// compact block relay.

static void DeserializeBlockTest(benchmark::Bench& bench)
{
    CDataStream stream(benchmark::data::block813851, SER_NETWORK, PROTOCOL_VERSION);
    std::byte a{0};
    stream.write({&a, 1}); // Prevent compaction

    bench.unit("block").run([&] {
        CBlock block;
        stream >> block;
        bool rewound = stream.Rewind(benchmark::data::block813851.size());
        assert(rewound);
    });
}

static void DeserializeAndCheckBlockTest(benchmark::Bench& bench)
{
    ArgsManager bench_args;
    const auto chainParams = CreateChainParams(bench_args, CBaseChainParams::REGTEST);

    // The historical fixture does not satisfy Korsh's proof of work. Retain
    // its transaction workload, but solve a regtest header before timing so
    // CheckBlock still verifies both proof of work and the merkle root.
    CDataStream fixture(benchmark::data::block813851, SER_NETWORK, PROTOCOL_VERSION);
    CBlock templateBlock;
    fixture >> templateBlock;
    templateBlock.nBits = UintToArith256(chainParams->GetConsensus().powLimit).GetCompact();
    templateBlock.nNonce = 0;
    while (!CheckProofOfWork(templateBlock.GetHash(), templateBlock.nBits, chainParams->GetConsensus())) {
        ++templateBlock.nNonce;
    }
    CDataStream stream(SER_NETWORK, PROTOCOL_VERSION);
    stream << templateBlock;
    const auto blockSize = stream.size();
    std::byte a{0};
    stream.write({&a, 1}); // Prevent compaction
    // CheckBlock calls g_stats_client internally, we aren't using a testing setup
    // so we need to do this manually. We can use the stub interface for this.
    ::g_stats_client = std::make_unique<StatsdClient>();

    bench.unit("block").run([&] {
        CBlock block; // Note that CBlock caches its checked state, so we need to recreate it here
        stream >> block;
        bool rewound = stream.Rewind(blockSize);
        assert(rewound);

        BlockValidationState validationState;
        bool checked = CheckBlock(block, validationState, chainParams->GetConsensus());
        assert(checked);
    });
}

BENCHMARK(DeserializeBlockTest);
BENCHMARK(DeserializeAndCheckBlockTest);
