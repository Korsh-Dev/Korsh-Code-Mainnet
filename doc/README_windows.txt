Korsh Core
==========

Intro
-----
Korsh is a free open source peer-to-peer electronic cash system that is
completely decentralized, without the need for a central server or trusted
parties.  Users hold the crypto keys to their own money and transact directly
with each other, with the help of a P2P network to check for double-spending.


Setup
-----
Unpack the archive into a directory and run `bin\korsh-qt.exe`. Command-line tools are in the same `bin` directory.
Sapling parameter files are included in `params`; keep it next to `bin` after extraction. If an operation requires the external files, pass `-paramsdir=.\params` to `korshd.exe` or `korsh-qt.exe`.

Korsh Core is the original Korsh client and it builds the backbone of the network.
However, it downloads and stores the entire history of Korsh transactions;
depending on the speed of your computer and network connection, the synchronization
process can take anywhere from a few hours to a day or more.

See the Korsh project repository for more help and information:
  https://github.com/Korsh-Dev/Korsh-Code-Mainnet
