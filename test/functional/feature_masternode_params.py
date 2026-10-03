#!/usr/bin/env python3
# Copyright (c) 2025 The Korsh Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test masternode parameter interactions.

This test verifies that masternode parameters are automatically enabled and a
loaded wallet remains available after restarting with -masternodeblsprivkey.
"""

from test_framework.test_framework import BitcoinTestFramework

# Service flags
NODE_COMPACT_FILTERS = (1 << 6)

# Constants
BASIC_FILTER_INDEX = 'basic block filter index'


class MasternodeParamsTest(BitcoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        self.log.info("Test that regular node has default settings")
        node0 = self.nodes[0]

        # Regular node should have peerblockfilters disabled by default
        services = int(node0.getnetworkinfo()['localservices'], 16)
        assert services & NODE_COMPACT_FILTERS == 0

        # Regular node should not have blockfilterindex enabled
        index_info = node0.getindexinfo()
        assert BASIC_FILTER_INDEX not in index_info

        self.log.info("Test that masternode has blockfilters auto-enabled")
        # Generate a valid BLS key for testing
        bls_info = node0.bls('generate')
        bls_key = bls_info['secret']

        # Start a node with masternode key
        self.restart_node(1, extra_args=[f"-masternodeblsprivkey={bls_key}"])
        node1 = self.nodes[1]
        assert node1.listwallets(), "Wallet support must remain available when masternode mode is enabled"

        # Masternode should have peerblockfilters enabled
        services = int(node1.getnetworkinfo()['localservices'], 16)
        self.log.info(f"Masternode services: {hex(services)}, has COMPACT_FILTERS: {services & NODE_COMPACT_FILTERS != 0}")
        assert services & NODE_COMPACT_FILTERS != 0

        # Check blockfilterindex
        index_info = node1.getindexinfo()
        self.log.info(f"Masternode indexes: {list(index_info.keys())}")

        assert BASIC_FILTER_INDEX in index_info
        self.wait_until(lambda: node1.getindexinfo()[BASIC_FILTER_INDEX]['synced'])
        assert node1.getindexinfo()[BASIC_FILTER_INDEX]['best_block_height'] == node1.getblockcount()

        self.log.info("Test that masternode can explicitly disable blockfilters")
        # Restart masternode with explicit disable
        self.restart_node(1, extra_args=[
            f"-masternodeblsprivkey={bls_key}",
            "-peerblockfilters=0",
            "-blockfilterindex=0"
        ])
        node1 = self.nodes[1]
        assert node1.listwallets(), "Explicit masternode filter settings must not disable the wallet"

        # Should not have COMPACT_FILTERS service
        services = int(node1.getnetworkinfo()['localservices'], 16)
        assert services & NODE_COMPACT_FILTERS == 0

        # Should not have blockfilterindex
        index_info = node1.getindexinfo()
        assert BASIC_FILTER_INDEX not in index_info

        self.log.info("Test that masternode parameter interaction is logged")
        # Stop the node first so we can check the startup logs
        self.stop_node(1)

        # Check debug log for the current masternode parameter interactions.
        with self.nodes[1].assert_debug_log([
            "parameter interaction: -masternodeblsprivkey set -> setting -peerblockfilters=1",
            "parameter interaction: -masternodeblsprivkey set -> setting -blockfilterindex=basic",
        ]):
            self.start_node(1, extra_args=[f"-masternodeblsprivkey={bls_key}"])


if __name__ == '__main__':
    MasternodeParamsTest().main()
