#!/usr/bin/env python3
# Copyright (c) 2021 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test ThreadDNSAddressSeed logic for querying DNS seeds."""

import itertools
import select
import socket
import socketserver
import threading
from datetime import datetime

from test_framework.p2p import P2PInterface
from test_framework.socks5 import recvall
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


class LocalOnlyProxy(socketserver.ThreadingTCPServer):
    """SOCKS5 sink for seed names; forward only numeric loopback P2P peers.

    No hostname is resolved and no non-loopback connection is ever made.
    This exercises the real name-proxy/addr-fetch path without external DNS.
    """
    daemon_threads = True

    def __init__(self):
        self.seed_requests = []
        self.requests_lock = threading.Lock()
        super().__init__(("127.0.0.1", 0), LocalOnlyProxyHandler)

    def seed_count(self):
        with self.requests_lock:
            return len(self.seed_requests)


class LocalOnlyProxyHandler(socketserver.BaseRequestHandler):
    def handle(self):
        conn = self.request
        conn.settimeout(10)
        version, nmethods = recvall(conn, 2)
        assert_equal(version, 5)
        assert 0 in recvall(conn, nmethods)
        conn.sendall(b"\x05\x00")
        assert_equal(recvall(conn, 4), b"\x05\x01\x00\x03")
        host = recvall(conn, recvall(conn, 1)[0]).decode('ascii')
        port = int.from_bytes(recvall(conn, 2), 'big')
        if host != "127.0.0.1":
            if host == "dummySeed.invalid.":
                with self.server.requests_lock:
                    self.server.seed_requests.append((host, port))
            conn.sendall(b"\x05\x04\x00\x01" + bytes(6))
            return
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as peer:
            peer.settimeout(10)
            peer.connect((host, port))
            conn.sendall(b"\x05\x00\x00\x01" + bytes(6))
            while True:
                readable, _, _ = select.select([conn, peer], [], [], 1)
                for source in readable:
                    data = source.recv(65536)
                    if not data:
                        return
                    (peer if source is conn else conn).sendall(data)


class P2PDNSSeeds(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [["-dnsseed=1"]]

    def setup_network(self):
        self.proxy = LocalOnlyProxy()
        self.proxy_thread = threading.Thread(target=self.proxy.serve_forever, daemon=True)
        self.proxy_thread.start()
        self.extra_args[0] += [f"-proxy=127.0.0.1:{self.proxy.server_address[1]}", "-proxyrandomize=0"]
        self.add_nodes(self.num_nodes, self.extra_args)
        # Enable the addr-fetch queue, normally suppressed by the framework's
        # connect=0. All automatic attempts remain confined to the local proxy.
        self.nodes[0].replace_in_config([("connect=0", "")])
        self.start_nodes()

    def run_test(self):
        try:
            self.log.info("Check empty-addrman bootstrap contacts the seed through the local sink")
            assert_equal(self.nodes[0].getnodeaddresses(0), [])
            self.wait_until(lambda: self.proxy.seed_count() > 0)
            self.init_arg_tests()
            self.existing_outbound_connections_test()
            self.insufficient_outbound_connections_test()
            self.existing_block_relay_connections_test()
            self.force_dns_test()
            self.wait_time_tests()
        finally:
            self.proxy.shutdown()
            self.proxy.server_close()
            self.proxy_thread.join()

    def isolated_args(self, *args):
        # Explicit restart arguments replace extra_args, so retain isolation.
        return [f"-proxy=127.0.0.1:{self.proxy.server_address[1]}", "-proxyrandomize=0", *args]

    def init_arg_tests(self):
        fakeaddr = "fakenodeaddr.fakedomain.invalid."

        self.log.info("Check that setting -connect disables -dnsseed by default")
        self.nodes[0].stop_node()
        with self.nodes[0].assert_debug_log(expected_msgs=["DNS seeding disabled"]):
            self.start_node(0, self.isolated_args(f"-connect={fakeaddr}"))

        self.log.info("Check that running -connect and -dnsseed means DNS logic runs.")
        with self.nodes[0].assert_debug_log(expected_msgs=["Loading addresses from DNS seed"], timeout=12):
            self.restart_node(0, self.isolated_args(f"-connect={fakeaddr}", "-dnsseed=1"))

        self.log.info("Check that running -forcednsseed and -dnsseed=0 throws an error.")
        self.nodes[0].stop_node()
        self.nodes[0].assert_start_raises_init_error(
            expected_msg="Error: Cannot set -forcednsseed to true when setting -dnsseed to false.",
            extra_args=["-forcednsseed=1", "-dnsseed=0"],
        )

        self.log.info("Check that running -forcednsseed and -connect throws an error.")
        # -connect soft sets -dnsseed to false, so throws the same error
        self.nodes[0].stop_node()
        self.nodes[0].assert_start_raises_init_error(
            expected_msg="Error: Cannot set -forcednsseed to true when setting -dnsseed to false.",
            extra_args=["-forcednsseed=1", f"-connect={fakeaddr}"],
        )

        # Restore default bitcoind settings
        self.restart_node(0)

    def existing_outbound_connections_test(self):
        # Make sure addrman is populated to enter the conditional where we
        # delay and potentially skip DNS seeding.
        self.nodes[0].addpeeraddress("192.0.0.8", 8333)

        self.log.info("Check that we *do not* query DNS seeds if we have 2 outbound connections")

        self.nodes[0].stop_node()
        seed_count = self.proxy.seed_count()
        with self.nodes[0].assert_debug_log(
            expected_msgs=["P2P peers available. Skipped DNS seeding.", "dnsseed thread exit"],
            unexpected_msgs=["Loading addresses from DNS seed"], timeout=12,
        ):
            self.start_node(0)
            for i in range(2):
                self.nodes[0].add_outbound_p2p_connection(P2PInterface(), p2p_idx=i, connection_type="outbound-full-relay")
        assert_equal(self.proxy.seed_count(), seed_count)

    def insufficient_outbound_connections_test(self):
        for count in (0, 1):
            self.log.info(f"Check populated-addrman bootstrap with {count} full-relay peers")
            seed_count = self.proxy.seed_count()
            with self.nodes[0].assert_debug_log(
                expected_msgs=["Loading addresses from DNS seed"],
                unexpected_msgs=["Skipped DNS seeding."], timeout=12,
            ):
                self.restart_node(0)
                for i in range(count):
                    self.nodes[0].add_outbound_p2p_connection(P2PInterface(), p2p_idx=i, connection_type="outbound-full-relay")
            self.wait_until(lambda: self.proxy.seed_count() > seed_count)

    def existing_block_relay_connections_test(self):
        # Make sure addrman is populated to enter the conditional where we
        # delay and potentially skip DNS seeding. No-op when run after
        # existing_outbound_connections_test.
        self.nodes[0].addpeeraddress("192.0.0.8", 8333)

        self.log.info("Check that we *do* query DNS seeds if we only have 2 block-relay-only connections")

        seed_count = self.proxy.seed_count()
        self.restart_node(0)
        with self.nodes[0].assert_debug_log(expected_msgs=["Loading addresses from DNS seed"], timeout=12):
            # This mimics the "anchors" logic where nodes are likely to
            # reconnect to block-relay-only connections on startup.
            # Since we do not participate in addr relay with these connections,
            # we still want to query the DNS seeds.
            for i in range(2):
                self.nodes[0].add_outbound_p2p_connection(P2PInterface(), p2p_idx=i, connection_type="block-relay-only")
        self.wait_until(lambda: self.proxy.seed_count() > seed_count)

    def force_dns_test(self):
        self.log.info("Check that we query DNS seeds if -forcednsseed param is set")

        seed_count = self.proxy.seed_count()
        with self.nodes[0].assert_debug_log(
            expected_msgs=["Loading addresses from DNS seed"],
            unexpected_msgs=["Waiting 11 seconds before querying DNS seeds.", "Skipped DNS seeding."], timeout=12,
        ):
            # -dnsseed defaults to 1 in bitcoind, but 0 in the test framework,
            # so pass it explicitly here
            self.restart_node(0, self.isolated_args("-forcednsseed", "-dnsseed=1"))
        self.wait_until(lambda: self.proxy.seed_count() > seed_count)

        # Restore default for subsequent tests
        self.restart_node(0)

    def wait_time_tests(self):
        self.log.info("Check the delay before querying DNS seeds")

        # Populate addrman with < 1000 addresses
        for i in range(5):
            a = f"192.0.0.{i}"
            self.nodes[0].addpeeraddress(a, 8333)

        # The delay should be 11 seconds
        self.nodes[0].stop_node()
        log_offset = self.nodes[0].debug_log_bytes()
        seed_count = self.proxy.seed_count()
        with self.nodes[0].assert_debug_log(expected_msgs=["Waiting 11 seconds before querying DNS seeds.\n"]):
            self.start_node(0)
        assert 0 < len(self.nodes[0].getnodeaddresses(0)) < 1000
        self.assert_seed_delay(log_offset, seed_count)

        # Populate addrman with > 1000 addresses
        for i in itertools.count():
            first_octet = i % 2 + 1
            second_octet = i % 256
            third_octet = i % 100
            a = f"{first_octet}.{second_octet}.{third_octet}.1"
            self.nodes[0].addpeeraddress(a, 8333)
            if (i > 1000 and i % 100 == 0):
                # The addrman size is non-deterministic because new addresses
                # are sorted into buckets, potentially displacing existing
                # addresses. Periodically check if we have met the desired
                # threshold.
                if len(self.nodes[0].getnodeaddresses(0)) > 1000:
                    break

        # Korsh policy keeps DNSSEEDS_DELAY_MANY_PEERS at 11 seconds, too,
        # rather than the upstream 300 seconds. Still verify the actual wait.
        self.nodes[0].stop_node()
        log_offset = self.nodes[0].debug_log_bytes()
        seed_count = self.proxy.seed_count()
        with self.nodes[0].assert_debug_log(expected_msgs=["Waiting 11 seconds before querying DNS seeds.\n"]):
            self.start_node(0)
        assert len(self.nodes[0].getnodeaddresses(0)) > 1000
        self.assert_seed_delay(log_offset, seed_count)

    def assert_seed_delay(self, log_offset, seed_count):
        # Require a real seed request, not just a message announcing a wait.
        self.wait_until(lambda: self.proxy.seed_count() > seed_count)
        with self.nodes[0].debug_log_path.open(encoding='utf-8') as debug_log:
            debug_log.seek(log_offset)
            lines = debug_log.readlines()
        waiting = [line for line in lines if "Waiting 11 seconds before querying DNS seeds." in line]
        querying = [line for line in lines if "Loading addresses from DNS seed" in line]
        assert_equal(len(waiting), 1)
        assert_equal(len(querying), 1)
        # The leading timestamps are real UTC time, not the logged mocktime.
        timestamp_format = "%Y-%m-%dT%H:%M:%S.%fZ"
        started = datetime.strptime(waiting[0].split()[0], timestamp_format)
        queried = datetime.strptime(querying[0].split()[0], timestamp_format)
        elapsed = (queried - started).total_seconds()
        assert elapsed >= 11, f"DNS seed queried after only {elapsed} seconds"
        self.log.info(f"Verified actual DNS seed delay: {elapsed:.6f} seconds")


if __name__ == '__main__':
    P2PDNSSeeds().main()
