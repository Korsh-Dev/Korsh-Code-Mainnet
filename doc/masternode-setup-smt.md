# Korsh Masternode Guide (Simple / Old-School Style)

This is the short version most miners want: send collateral to yourself, register, done.

Windows users:

- Use this guide: `doc/masternode-setup-smt-windows.md`
- Direct link: https://github.com/Korsh-Dev/Korsh-Code-Mainnet/blob/main/doc/masternode-setup-smt-windows.md
- Qt-first guide (minimal commands): `doc/masternode-setup-smt-windows-qt.md`
- Direct link: https://github.com/Korsh-Dev/Korsh-Code-Mainnet/blob/main/doc/masternode-setup-smt-windows-qt.md

Network values:

- Regular MN collateral: `1,500 KSH`
- Evo masternodes: disabled on the current mainnet
- Mainnet P2P port: `9777`
- Collateral confirmations: `15`

## 1. Quick Reality Check

- You **can** run MN + wallet in the same node process (single-wallet mode).
- You **do not need** a VPS if your machine is reachable from the internet (`9777/tcp` open + stable public IP).
- Korsh uses deterministic masternodes (ProTx), so one registration tx is still required.

## 2. Minimal Config (Single Wallet)

Put this in `korsh.conf` on the machine that will run the masternode:

```ini
server=1
daemon=1
listen=1
port=9777
externalip=YOUR_PUBLIC_IP:9777

txindex=1
prune=0

masternodeblsprivkey=PASTE_OPERATOR_SECRET_HERE
disablewallet=0
```

Then restart node/wallet.

## 3. Old-School Fast Flow (Regular MN)

Use wallet CLI:

```bash
CLI=korsh-cli
WALLET=main
```

If needed, create wallet once:

```bash
$CLI createwallet "$WALLET" || true
```

### Step A: Generate keys and addresses

```bash
BLS_JSON=$($CLI bls generate)
OPERATOR_SECRET=$(echo "$BLS_JSON" | jq -r '.secret')
OPERATOR_PUB=$(echo "$BLS_JSON" | jq -r '.public')

COLLATERAL_ADDR=$($CLI -rpcwallet=$WALLET getnewaddress "mn_collateral")
OWNER_ADDR=$($CLI -rpcwallet=$WALLET getnewaddress "mn_owner")
VOTING_ADDR=$($CLI -rpcwallet=$WALLET getnewaddress "mn_voting")
PAYOUT_ADDR=$($CLI -rpcwallet=$WALLET getnewaddress "mn_payout")
FEE_ADDR=$($CLI -rpcwallet=$WALLET getnewaddress "mn_fee")
```

Put `OPERATOR_SECRET` into `korsh.conf` as `masternodeblsprivkey=...` and restart once.

### Step B: Send collateral to yourself

```bash
TXID=$($CLI -rpcwallet=$WALLET sendtoaddress "$COLLATERAL_ADDR" 1500)
echo "$TXID"
```

Wait for at least `15` confirmations.

### Step C: Get collateral outpoint

```bash
OUTPOINT=$($CLI -rpcwallet=$WALLET masternode outputs | jq -r '.[0]')
COLL_TXID="${OUTPOINT%-*}"
COLL_VOUT="${OUTPOINT##*-}"
echo "$COLL_TXID $COLL_VOUT"
```

### Step D: Register masternode

```bash
MN_IP="YOUR_PUBLIC_IP"

PROTX_HASH=$($CLI -rpcwallet=$WALLET protx register \
"$COLL_TXID" "$COLL_VOUT" "[\"$MN_IP:9777\"]" \
"$OWNER_ADDR" "$OPERATOR_PUB" "$VOTING_ADDR" \
0 "$PAYOUT_ADDR" "$FEE_ADDR" true)

echo "$PROTX_HASH"
```

### Step E: Verify

```bash
$CLI protx info "$PROTX_HASH"
$CLI masternode list status
$CLI masternode status
```

If status is not valid yet, give it a few blocks.

## 4. Even Shorter Alternative (No Manual `masternode outputs`)

If you prefer one-call funding + registration, use:

```bash
$CLI -rpcwallet=$WALLET protx register_fund \
"$COLLATERAL_ADDR" "[\"$MN_IP:9777\"]" \
"$OWNER_ADDR" "$OPERATOR_PUB" "$VOTING_ADDR" \
0 "$PAYOUT_ADDR" "$FEE_ADDR" true
```

This auto-creates collateral in the same operation.

## 5. Fix an existing registration still using port 8383

With Korsh Core `v0.0.6`, an existing ProTx can be updated without registering a second masternode or sending the collateral again. In the **Masternodes** tab, right-click the affected row and choose **Update service to the current network port**. The wallet submits the service update; make sure the masternode host is listening on port `9777` first. This requires its operator BLS key and a confirmed spendable wallet output for the fee. It does not unlock collateral or restore a missing wallet balance.

## 6. Evo MN

Evo masternodes are disabled on the current mainnet; use the regular masternode flow above.

## 7. Top 5 Mistakes

- Collateral is not exactly `1500 KSH`.
- Less than `15` confirmations.
- `masternodeblsprivkey` does not match operator public key used in ProTx.
- Port `9777` is closed.
- Node not reachable at `externalip`.
