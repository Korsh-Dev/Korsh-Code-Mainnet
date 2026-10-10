// Copyright (c) 2012-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>
#include <clientversion.h>
#include <primitives/block.h>
#include <streams.h>
#include <uint256.h>
#include <util/translation.h>
#include <wallet/dump.h>
#include <wallet/walletdb.h>

#include <boost/test/unit_test.hpp>

#include <memory>

namespace wallet {
BOOST_FIXTURE_TEST_SUITE(walletdb_tests, BasicTestingSetup)

class FailFirstWriteBatch final : public DummyBatch
{
private:
    int& m_write_calls;

public:
    explicit FailFirstWriteBatch(int& write_calls) : m_write_calls(write_calls) {}

private:
    bool WriteKey(CDataStream&&, CDataStream&&, bool = true) override
    {
        return ++m_write_calls != 1;
    }
};

class FailFirstWriteDatabase final : public DummyDatabase
{
public:
    int m_write_calls{0};

    std::unique_ptr<DatabaseBatch> MakeBatch(bool = true) override
    {
        return std::make_unique<FailFirstWriteBatch>(m_write_calls);
    }
};

class FailedCursorCloseBatch final : public DummyBatch
{
private:
    int& m_close_calls;

public:
    explicit FailedCursorCloseBatch(int& close_calls) : m_close_calls(close_calls) {}

    bool ReadAtCursor(CDataStream&, CDataStream&, bool& complete) override
    {
        complete = true;
        return true;
    }

    bool CloseCursor() override
    {
        ++m_close_calls;
        return false;
    }
};

class FailedCursorCloseDatabase final : public DummyDatabase
{
public:
    int m_close_calls{0};

    std::unique_ptr<DatabaseBatch> MakeBatch(bool = true) override
    {
        return std::make_unique<FailedCursorCloseBatch>(m_close_calls);
    }
};

BOOST_AUTO_TEST_CASE(walletdb_readkeyvalue)
{
    /**
     * When ReadKeyValue() reads from either a "key" or "wkey" it first reads the CDataStream steam into a
     * CPrivKey or CWalletKey respectively and then reads a hash of the pubkey and privkey into a uint256.
     * Wallets from 0.8 or before do not store the pubkey/privkey hash, trying to read the hash from old
     * wallets throws an exception, for backwards compatibility this read is wrapped in a try block to
     * silently fail. The test here makes sure the type of exception thrown from CDataStream::read()
     * matches the type we expect, otherwise we need to update the "key"/"wkey" exception type caught.
     */
    CDataStream ssValue(SER_DISK, CLIENT_VERSION);
    uint256 dummy;
    BOOST_CHECK_THROW(ssValue >> dummy, std::ios_base::failure);
}

BOOST_AUTO_TEST_CASE(walletdb_bestblock_stops_after_failed_legacy_locator_write)
{
    FailFirstWriteDatabase database;
    WalletBatch batch{database};
    CBlockLocator locator;

    BOOST_CHECK(!batch.WriteBestBlock(locator));
    BOOST_CHECK_EQUAL(database.m_write_calls, 1);
}

BOOST_AUTO_TEST_CASE(walletdb_find_wallet_tx_hashes_fails_if_cursor_cannot_close)
{
    FailedCursorCloseDatabase database;
    WalletBatch batch{database};
    std::vector<uint256> tx_hashes;

    BOOST_CHECK(batch.FindWalletTxHashes(tx_hashes) == DBErrors::LOAD_FAIL);
    BOOST_CHECK_EQUAL(database.m_close_calls, 1);
}

BOOST_AUTO_TEST_CASE(wallet_dump_fails_if_cursor_cannot_close)
{
    FailedCursorCloseDatabase database;
    bilingual_str error;
    const fs::path dump_path = m_args.GetDataDirNet() / "cursor-close-failure.dump";
    m_args.ForceSetArg("-dumpfile", fs::PathToString(dump_path));

    BOOST_CHECK(!DumpWallet(m_args, database, error));
    BOOST_CHECK(!fs::exists(dump_path));
    BOOST_CHECK_EQUAL(database.m_close_calls, 1);
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
