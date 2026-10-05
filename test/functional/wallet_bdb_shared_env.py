#!/usr/bin/env python3
# Copyright (c) 2026 The Korsh Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Closing one legacy wallet must not invalidate another wallet's environment."""

from io import BytesIO
from pathlib import Path
import shutil

from test_framework.bdb import dump_bdb_kv
from test_framework.messages import CBlockLocator, ser_string
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


class SharedEnvironmentTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.wallet_names = []
        self.extra_args = [["-nowallet", "-flushwallet=0"]]

    def add_options(self, parser):
        self.add_wallet_options(parser)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_bdb()

    def run_test(self):
        node = self.nodes[0]
        wallet_dir = Path(node.datadir) / self.chain / "wallets"
        for name in ("", "sibling"):
            node.createwallet(name, descriptors=False, load_on_startup=False)
        self.stop_node(0)
        # Plain files in the same directory share a BerkeleyEnvironment.
        (wallet_dir / "sibling" / "wallet.dat").rename(wallet_dir / "second.dat")
        shutil.rmtree(wallet_dir / "sibling")
        self.start_node(0)
        names = ["", "second.dat"]
        addresses = {}
        txids = {}
        for name in names:
            node.loadwallet(name)
            wallet = node.get_wallet_rpc(name)
            addresses[name] = wallet.getnewaddress("persist-" + name)
            block = self.generatetoaddress(node, 1, addresses[name])[0]
            txids[name] = node.getblock(block)["tx"][0]
        tip = node.getbestblockhash()
        balances = {name: node.get_wallet_rpc(name).getbalances() for name in names}

        # StopWallets writes each locator immediately before closing its DB.
        # The second write must still have a live environment, even with no
        # BerkeleyBatch active when the first database is closed.
        self.stop_node(0)
        for name, filename in (("", "wallet.dat"), ("second.dat", "second.dat")):
            records = dump_bdb_kv(wallet_dir / filename)
            locator = CBlockLocator()
            locator.deserialize(BytesIO(records[ser_string(b"bestblock_nomerkle")]))
            assert_equal(locator.vHave[0], int(tip, 16))
        assert not (wallet_dir / "database").exists()

        # Reopen in reverse order and prove keys, labels and transactions were
        # checkpointed, not just that shutdown avoided crashing.
        self.start_node(0)
        for name in reversed(names):
            node.loadwallet(name)
            wallet = node.get_wallet_rpc(name)
            info = wallet.getaddressinfo(addresses[name])
            assert info["ismine"]
            assert_equal(info["labels"], ["persist-" + name])
            assert_equal(wallet.getbalances(), balances[name])
            assert wallet.gettransaction(txids[name])["confirmations"] > 0
            signature = wallet.signmessage(addresses[name], "persisted key")
            assert node.verifymessage(addresses[name], signature, "persisted key")

        # Releasing an individual owner must leave the sibling writable, and
        # releasing the last owner must allow a fresh environment to be loaded.
        node.unloadwallet("")
        node.get_wallet_rpc("second.dat").setlabel(addresses["second.dat"], "after-unload")
        node.unloadwallet("second.dat")
        assert not (wallet_dir / "database").exists()
        for name in names:
            node.loadwallet(name)
        assert_equal(node.get_wallet_rpc("second.dat").getaddressinfo(addresses["second.dat"])["labels"], ["after-unload"])
        self.stop_node(0)


if __name__ == '__main__':
    SharedEnvironmentTest().main()
