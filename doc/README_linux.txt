Korsh Core v0.0.6 - Linux x86_64 (Ubuntu 22.04+ / glibc 2.35+)

Runtime requirements (Ubuntu 22.04):
  sudo apt-get install libdb5.3++ libdb5.3 libevent-2.1-7 libevent-pthreads-2.1-7 \
    libsodium23 libzmq5 libboost-filesystem1.74.0 libboost-thread1.74.0 \
    libboost-chrono1.74.0 libboost-program-options1.74.0 libminiupnpc17 \
    libnatpmp1 libsqlite3-0 libqt5core5a libqt5gui5 libqt5widgets5 \
    libqt5network5 libqt5dbus5 libgmp10 zlib1g libssl3

Usage (from the extracted directory):
  ./bin/korshd -daemon                Start the full node
  ./bin/korsh-qt                      Start the graphical wallet (desktop required)
  ./bin/korsh-cli getblockcount       Query the local node
  ./bin/korsh-cli getblockchaininfo   Show synchronization status

The default data and configuration directory is ~/.korshcore. Mainnet ports are
P2P 9777 and RPC 9776. Sapling parameter files are included under params/; keep
that directory alongside bin/ after extraction. If an operation requires the
external files, pass -paramsdir=./params to korshd or korsh-qt. Keep the six
executables together in bin/; they were built for Ubuntu 22.04 or newer systems
with glibc 2.35 or newer.
