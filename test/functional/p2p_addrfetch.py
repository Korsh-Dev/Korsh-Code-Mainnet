#!/usr/bin/env python3
# Copyright (c) 2021 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""
Test p2p addr-fetch connections
"""

import time

from test_framework.messages import (
    CAddress,
    NODE_HEADERS_COMPRESSED,
    msg_addr,
)
from test_framework.p2p import (
    P2PInterface,
    p2p_lock,
    P2P_SERVICES,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

ADDR = CAddress()
ADDR.time = int(time.time())
ADDR.nServices = P2P_SERVICES
ADDR.ip = "192.0.0.8"
ADDR.port = 18444


class P2PAddrFetch(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1

    def assert_getpeerinfo(self, *, peer_ids):
        num_peers = len(peer_ids)
        info = self.nodes[0].getpeerinfo()
        assert_equal(len(info), num_peers)
        for n in range(0, num_peers):
            assert_equal(info[n]['id'], peer_ids[n])
            assert_equal(info[n]['connection_type'], 'addr-fetch')

    def run_test(self):
        node = self.nodes[0]
        # Exercise the recent-best-header alternative even on a clean chain.
        node.setmocktime(node.getblockheader(node.getbestblockhash())["time"])
        self.log.info("Connect to an addr-fetch peer")
        peer_id = 0
        peer = node.add_outbound_p2p_connection(P2PInterface(), p2p_idx=peer_id, connection_type="addr-fetch")
        self.assert_getpeerinfo(peer_ids=[peer_id])

        self.log.info("Check that we send getaddr but don't try to sync headers with the addr-fetch peer")
        peer.sync_send_with_ping()
        with p2p_lock:
            assert peer.message_count['getaddr'] == 1
            assert peer.message_count['getheaders'] == 0
            assert peer.message_count['getheaders2'] == 0

        self.log.info("Check that answering the getaddr with a single address does not lead to disconnect")
        # This prevents disconnecting on self-announcements
        msg = msg_addr()
        msg.addrs = [ADDR]
        peer.send_and_ping(msg)
        self.assert_getpeerinfo(peer_ids=[peer_id])

        self.log.info("Check that answering with larger addr messages leads to disconnect")
        msg.addrs = [ADDR] * 2
        peer.send_message(msg)
        peer.wait_for_disconnect(timeout=5)

        self.log.info("Check timeout for addr-fetch peer that does not send addrs")
        peer_id = 1
        peer = node.add_outbound_p2p_connection(P2PInterface(), p2p_idx=peer_id, connection_type="addr-fetch")

        time_now = int(time.time())
        self.assert_getpeerinfo(peer_ids=[peer_id])

        # Expect addr-fetch peer connection to be maintained up to 5 minutes.
        node.setmocktime(time_now + 295)
        self.assert_getpeerinfo(peer_ids=[peer_id])

        # Expect addr-fetch peer connection to be disconnected after 5 minutes.
        node.setmocktime(time_now + 301)
        peer.wait_for_disconnect(timeout=5)
        self.assert_getpeerinfo(peer_ids=[])
        node.disconnect_p2ps()

        # Neither the recent-tip OR arm nor the ordinary IBD path may start
        # sync with address-only peers. Full/block relay must still sync when
        # the peer does or does not advertise compressed-header support.
        tip_time = node.getblockheader(node.getbestblockhash())["time"]
        for age in (0, 2 * 24 * 60 * 60):
            node.setmocktime(tip_time + age)
            for compressed in (False, True):
                services = (P2P_SERVICES & ~NODE_HEADERS_COMPRESSED) | (NODE_HEADERS_COMPRESSED if compressed else 0)
                for connection_type in ("addr-fetch", "outbound-full-relay", "block-relay-only"):
                    self.log.info(f"Check initial headers: age={age}, compressed={compressed}, role={connection_type}")
                    peer = node.add_outbound_p2p_connection(
                        P2PInterface(), p2p_idx=0, connection_type=connection_type, services=services,
                    )
                    assert_equal(node.getpeerinfo()[0]['connection_type'], connection_type)
                    peer.sync_send_with_ping()
                    if connection_type == "addr-fetch":
                        with p2p_lock:
                            assert_equal(peer.message_count['getaddr'], 1)
                            assert_equal(peer.message_count['getheaders'], 0)
                            assert_equal(peer.message_count['getheaders2'], 0)
                    else:
                        # Compressed headers require support from both sides.
                        negotiated = bool(services & peer.nServices & NODE_HEADERS_COMPRESSED)
                        header_message = 'getheaders2' if negotiated else 'getheaders'
                        peer.wait_until(lambda: peer.message_count[header_message] > 0)
                        with p2p_lock:
                            assert_equal(peer.message_count['getheaders' if negotiated else 'getheaders2'], 0)
                    node.disconnect_p2ps()


if __name__ == '__main__':
    P2PAddrFetch().main()
