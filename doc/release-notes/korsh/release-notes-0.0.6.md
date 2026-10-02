# Korsh Core version v0.0.6

In one sentence: v0.0.6 lets you update an existing masternode's registered service port from the Qt wallet without registering it again or moving its collateral.

## Highlights

- Added **Update service to the current network port** to the Masternodes context menu when the selected ProTx advertises an outdated endpoint.
- The wallet submits `protx update_service` for the existing ProTx and leaves its payout address unchanged.
- Fixed parsing of UTF-8-BOM-prefixed configuration files, including files whose first line is a comment.
- Updated the active masternode guides to current mainnet settings: **1,500 KSH** collateral, **15 confirmations**, and **9777/tcp**. Evo masternodes remain disabled on the current mainnet.
- Added regression coverage for service endpoint replacement and wallet availability when a node is started with `-masternodeblsprivkey`.

## Updating an existing masternode

Before submitting the service update, configure the masternode host to listen on port `9777` and allow that port through its firewall. In the wallet's **Masternodes** tab, right-click the affected entry and select **Update service to the current network port**. Review the old and new endpoints and confirm the transaction.

The update requires the operator BLS key on the node submitting it, a loaded and unlocked wallet, and a confirmed spendable output to pay the fee. Do not register a second masternode for the same collateral.

## Wallet balance and collateral

The service update changes the registered endpoint only. It does not unlock or move masternode collateral, deregister the masternode, or restore a balance that is absent from the wallet. A local wallet lock can prevent an output from being selected for spending, but does not itself remove the output from the wallet's balance. If collateral is missing from the displayed balance, verify that its outpoint is still unspent and controlled by the loaded, synchronized wallet.

## Wallet database compatibility

The v0.0.6 packages retain the platform-specific Berkeley DB backends used by v0.0.5: 4.8.30 on macOS, 5.3 on Linux, and 6.2 on Windows. Opening the same `wallet.dat` across operating systems or converting it between these backends has not been tested. Back up before upgrading; do not share wallet files, private keys, or seed phrases when asking for balance help.

## Consensus and economics

No consensus, subsidy, or collateral rules changed in v0.0.6. This is a wallet/UI, configuration parsing, and documentation release; it does not require a chain reset or reindex.
