// Copyright (c) 2018-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <fs.h>
#include <test/util/setup_common.h>
#include <wallet/bdb.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <util/translation.h>

namespace wallet {
BOOST_FIXTURE_TEST_SUITE(db_tests, BasicTestingSetup)

static std::shared_ptr<BerkeleyEnvironment> GetWalletEnv(const fs::path& path, fs::path& database_filename)
{
    fs::path data_file = BDBDataFile(path);
    database_filename = data_file.filename();
    return GetBerkeleyEnv(data_file.parent_path(), false);
}

static std::string ReadFile(const fs::path& path)
{
    std::ifstream stream{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

class FailedOpenBerkeleyDatabase final : public BerkeleyDatabase
{
public:
    using BerkeleyDatabase::BerkeleyDatabase;

    void Open() override
    {
        throw std::runtime_error("injected Berkeley database open failure");
    }
};

BOOST_AUTO_TEST_CASE(getwalletenv_file)
{
    fs::path test_name = "test_name.dat";
    const fs::path datadir = m_args.GetDataDirNet();
    fs::path file_path = datadir / test_name;
    std::ofstream f{file_path};
    f.close();

    fs::path filename;
    std::shared_ptr<BerkeleyEnvironment> env = GetWalletEnv(file_path, filename);
    BOOST_CHECK_EQUAL(filename, test_name);
    BOOST_CHECK_EQUAL(env->Directory(), fs::weakly_canonical(datadir));
}

BOOST_AUTO_TEST_CASE(getwalletenv_directory)
{
    fs::path expected_name = "wallet.dat";
    const fs::path datadir = m_args.GetDataDirNet();

    fs::path filename;
    std::shared_ptr<BerkeleyEnvironment> env = GetWalletEnv(datadir, filename);
    BOOST_CHECK_EQUAL(filename, expected_name);
    BOOST_CHECK_EQUAL(env->Directory(), fs::weakly_canonical(datadir));
}

BOOST_AUTO_TEST_CASE(getwalletenv_g_dbenvs_multiple)
{
    fs::path datadir = m_args.GetDataDirNet() / "1";
    fs::path datadir_2 = m_args.GetDataDirNet() / "2";
    fs::path filename;

    std::shared_ptr<BerkeleyEnvironment> env_1 = GetWalletEnv(datadir, filename);
    std::shared_ptr<BerkeleyEnvironment> env_2 = GetWalletEnv(datadir, filename);
    std::shared_ptr<BerkeleyEnvironment> env_3 = GetWalletEnv(datadir_2, filename);

    BOOST_CHECK(env_1 == env_2);
    BOOST_CHECK(env_2 != env_3);
}

BOOST_AUTO_TEST_CASE(getwalletenv_g_dbenvs_free_instance)
{
    fs::path datadir = gArgs.GetDataDirNet() / "1";
    fs::path datadir_2 = gArgs.GetDataDirNet() / "2";
    fs::path filename;

    std::shared_ptr <BerkeleyEnvironment> env_1_a = GetWalletEnv(datadir, filename);
    std::shared_ptr <BerkeleyEnvironment> env_2_a = GetWalletEnv(datadir_2, filename);
    env_1_a.reset();

    std::shared_ptr<BerkeleyEnvironment> env_1_b = GetWalletEnv(datadir, filename);
    std::shared_ptr<BerkeleyEnvironment> env_2_b = GetWalletEnv(datadir_2, filename);

    BOOST_CHECK(env_1_a != env_1_b);
    BOOST_CHECK(env_2_a == env_2_b);
}

BOOST_AUTO_TEST_CASE(bdb_batch_releases_reference_when_open_throws)
{
    const fs::path env_dir = m_args.GetDataDirNet() / "bdb-batch-open-failure";
    auto env = std::make_shared<BerkeleyEnvironment>(env_dir, false);
    DatabaseOptions options;
    FailedOpenBerkeleyDatabase database{env, "wallet.dat", options};

    bool threw{false};
    try {
        (void)database.MakeBatch();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    BOOST_CHECK(threw);
    BOOST_CHECK_EQUAL(database.m_refcount.load(), 0);
}

BOOST_AUTO_TEST_CASE(bdb_recovery_failure_preserves_wallet_and_logs)
{
    const fs::path env_dir = m_args.GetDataDirNet() / "bdb-recovery-failure";
    const fs::path log_dir = env_dir / "database";
    BOOST_REQUIRE(!fs::exists(env_dir));
    fs::create_directories(log_dir);

    const fs::path wallet_file = env_dir / "wallet.dat";
    const std::string wallet_bytes{"synthetic wallet bytes"};
    {
        std::ofstream wallet_stream{wallet_file, std::ios::binary};
        wallet_stream.write(wallet_bytes.data(), wallet_bytes.size());
        BOOST_REQUIRE(wallet_stream.good());
    }

    const fs::path log_file = log_dir / "log.0000000001";
    const std::string corrupt_log(4096, 'x');
    {
        std::ofstream log_stream{log_file, std::ios::binary};
        log_stream.write(corrupt_log.data(), corrupt_log.size());
        BOOST_REQUIRE(log_stream.good());
    }
    {
        std::ofstream error_stream{env_dir / "db.log"};
        error_stream << "historical Berkeley DB version warning\n";
        BOOST_REQUIRE(error_stream.good());
    }

    BerkeleyEnvironment env{env_dir, false};
    bilingual_str error;
    BOOST_CHECK_MESSAGE(!env.Open(error), "a corrupted BDB environment must fail closed");
    BOOST_CHECK(!env.IsInitialized());
    BOOST_CHECK(fs::exists(wallet_file));
    BOOST_CHECK_MESSAGE(ReadFile(wallet_file) == wallet_bytes, "failed recovery changed wallet.dat");
    BOOST_CHECK(fs::exists(log_file));
    BOOST_CHECK_MESSAGE(ReadFile(log_file) == corrupt_log, "failed recovery changed the transaction log");

    bool backup_created{false};
    for (const auto& entry : fs::directory_iterator{env_dir}) {
        if (fs::PathToString(entry.path().filename()).rfind("bdb-env-backup-", 0) == 0) {
            backup_created = true;
            break;
        }
    }
    BOOST_CHECK(!backup_created);
}

BOOST_AUTO_TEST_CASE(bdb_recovery_latch_survives_environment_recreation)
{
    const fs::path env_dir = m_args.GetDataDirNet() / "bdb-recovery-latch-recreation";
    BOOST_REQUIRE(!fs::exists(env_dir));
    fs::create_directories(env_dir);

    const fs::path wallet_file = env_dir / "wallet.dat";
    const std::string wallet_bytes{"synthetic wallet bytes"};
    {
        std::ofstream wallet_stream{wallet_file, std::ios::binary};
        wallet_stream.write(wallet_bytes.data(), wallet_bytes.size());
        BOOST_REQUIRE(wallet_stream.good());
    }

    bool first_opened{false};
    {
        auto env = GetBerkeleyEnv(env_dir, false);
        bilingual_str error;
        first_opened = env->Open(error);
        BOOST_CHECK(first_opened);
        if (first_opened) {
            env->MarkRecoveryRequired();
            BOOST_CHECK(env->IsRecoveryRequired());
            BOOST_CHECK(env->IsDirectoryLockHeld());
        }
    }

    if (!first_opened) {
        UnlockDirectory(env_dir, ".walletlock");
        return;
    }

    {
        auto retry_env = GetBerkeleyEnv(env_dir, false);
        BOOST_CHECK_MESSAGE(retry_env->IsRecoveryRequired(), "recovery failure state must survive environment recreation in the same process");

        bilingual_str retry_error;
        const bool reopened = retry_env->Open(retry_error);
        BOOST_CHECK_MESSAGE(!reopened, "a same-process retry must not reopen BDB files after recovery failed");
        BOOST_CHECK(!retry_env->IsInitialized());
        BOOST_CHECK_MESSAGE(retry_error.original.find("recovery previously failed") != std::string::npos,
                            "same-process retry should report the latched recovery failure");
        BOOST_CHECK_MESSAGE(ReadFile(wallet_file) == wallet_bytes, "same-process retry changed wallet.dat after recovery failed");

        if (reopened) retry_env->MarkRecoveryRequired();
    }

    // Release only this test fixture's intentionally retained OS lock.
    UnlockDirectory(env_dir, ".walletlock");
}

BOOST_AUTO_TEST_CASE(bdb_environment_lookup_fails_closed_during_teardown)
{
    const fs::path env_dir = m_args.GetDataDirNet() / "bdb-environment-teardown-race";
    auto environment = GetBerkeleyEnv(env_dir, false);
    std::weak_ptr<BerkeleyEnvironment> environment_weak{environment};
    std::promise<void> dropping_last_reference;
    auto dropping_last_reference_future = dropping_last_reference.get_future();
    std::jthread destroyer;
    std::shared_ptr<BerkeleyEnvironment> overlapping_environment;
    bool observed_expiration{false};

    {
        LOCK(cs_db);
        destroyer = std::jthread([environment = std::move(environment), &dropping_last_reference]() mutable {
            dropping_last_reference.set_value();
            environment.reset();
        });
        dropping_last_reference_future.wait();

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!environment_weak.expired() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        observed_expiration = environment_weak.expired();
        if (observed_expiration) overlapping_environment = GetBerkeleyEnv(env_dir, false);
    }

    destroyer.join();
    BOOST_CHECK_MESSAGE(observed_expiration, "last environment reference did not expire while teardown was serialized");
    BOOST_CHECK_MESSAGE(!overlapping_environment, "a replacement BDB environment must not be created before the previous environment finishes teardown");
    overlapping_environment.reset();
    BOOST_CHECK(GetBerkeleyEnv(env_dir, false));
}

#ifndef WIN32
BOOST_AUTO_TEST_CASE(bdb_recovery_latch_survives_symlinked_directory_alias)
{
    const fs::path env_dir = m_args.GetDataDirNet() / "bdb-recovery-latch-symlink-target";
    const fs::path alias_dir = m_args.GetDataDirNet() / "bdb-recovery-latch-symlink-alias";
    BOOST_REQUIRE(!fs::exists(env_dir));
    BOOST_REQUIRE(!fs::exists(alias_dir));
    fs::create_directories(env_dir);
    fs::create_directory_symlink(env_dir, alias_dir);

    auto env = GetBerkeleyEnv(env_dir, false);
    bilingual_str error;
    BOOST_REQUIRE(env->Open(error));
    env->MarkRecoveryRequired();

    auto alias_env = GetBerkeleyEnv(alias_dir, false);
    const bool same_environment = alias_env == env;
    const bool alias_latched = alias_env->IsRecoveryRequired();
    bool alias_reopened{false};
    if (same_environment) {
        bilingual_str alias_error;
        alias_reopened = alias_env->Open(alias_error);
    }

    // Release only this synthetic fixture's retained lock before its directory is removed.
    UnlockDirectory(env_dir, ".walletlock");
    fs::remove(alias_dir);
    BOOST_CHECK_MESSAGE(same_environment, "symlink aliases must share the same BDB environment identity");
    BOOST_CHECK_MESSAGE(alias_latched, "recovery-required state must follow a wallet-directory symlink alias");
    if (same_environment) BOOST_CHECK_MESSAGE(!alias_reopened, "a symlink alias must not reopen a recovery-latched BDB environment");
}
#endif

BOOST_AUTO_TEST_CASE(bdb_recovery_latch_survives_lexical_directory_alias)
{
    const fs::path env_dir = m_args.GetDataDirNet() / "bdb-recovery-latch-lexical-target";
    std::filesystem::path alias_std{env_dir.parent_path()};
    alias_std /= ".";
    alias_std /= fs::PathToString(env_dir.filename());
    const fs::path alias_dir{alias_std};
    BOOST_REQUIRE(!fs::exists(env_dir));
    fs::create_directories(env_dir);

    auto env = GetBerkeleyEnv(env_dir, false);
    bilingual_str error;
    BOOST_REQUIRE(env->Open(error));
    env->MarkRecoveryRequired();

    auto alias_env = GetBerkeleyEnv(alias_dir, false);
    const bool same_environment = alias_env == env;
    const bool alias_latched = alias_env && alias_env->IsRecoveryRequired();
    bool alias_reopened{false};
    if (alias_env) {
        bilingual_str alias_error;
        alias_reopened = alias_env->Open(alias_error);
    }

    UnlockDirectory(env_dir, ".walletlock");
    BOOST_CHECK_MESSAGE(same_environment, "lexical directory aliases must share one BDB environment identity");
    BOOST_CHECK_MESSAGE(alias_latched, "recovery-required state must follow a lexical directory alias");
    if (alias_env) BOOST_CHECK_MESSAGE(!alias_reopened, "a lexical alias must not reopen a recovery-latched BDB environment");
}

BOOST_AUTO_TEST_CASE(bdb_failed_checkpoint_preserves_recovery_logs)
{
    const fs::path base_dir = m_args.GetDataDirNet() / "bdb-failed-checkpoint";
    const fs::path source_dir = base_dir / "source";
    const fs::path target_dir = base_dir / "target";
    BOOST_REQUIRE(!fs::exists(base_dir));
    fs::create_directories(source_dir);
    fs::create_directories(target_dir);

    DatabaseOptions options;
    bilingual_str error;
    auto source_env = std::make_shared<BerkeleyEnvironment>(source_dir, false);
    BOOST_REQUIRE(source_env->Open(error));
    BerkeleyDatabase source_db{source_env, "wallet.dat", options};
    {
        auto batch = source_db.MakeBatch();
        BOOST_REQUIRE(batch->TxnBegin());
        for (int i = 0; i < 128; ++i) {
            BOOST_REQUIRE(batch->Write(std::string{"synthetic-key-"} + std::to_string(i), std::string{"synthetic-value"}));
        }
        BOOST_REQUIRE(batch->TxnCommit());
    }

    BOOST_REQUIRE_EQUAL(source_env->dbenv->txn_checkpoint(0, 0, 0), 0);
    source_env->CloseDb("wallet.dat");
    const fs::path source_wallet = source_dir / "wallet.dat";
    const fs::path target_wallet = target_dir / "wallet.dat";
    BOOST_REQUIRE(fs::exists(source_wallet));

    auto target_env = std::make_shared<BerkeleyEnvironment>(target_dir, false);
    BOOST_REQUIRE(target_env->Open(error));
    const fs::path target_log = target_dir / "database" / "log.0000000001";
    {
        BerkeleyDatabase seed_db{target_env, "seed.dat", options};
        {
            auto batch = seed_db.MakeBatch();
            BOOST_REQUIRE(batch->TxnBegin());
            BOOST_REQUIRE(batch->Write(std::string{"seed-key"}, std::string{"seed-value"}));
            BOOST_REQUIRE(batch->TxnCommit());
        }
        BOOST_REQUIRE_EQUAL(target_env->dbenv->txn_checkpoint(0, 0, 0), 0);
        target_env->CloseDb("seed.dat");
        BOOST_REQUIRE(fs::exists(target_log));

        fs::copy_file(source_wallet, target_wallet, fs::copy_options::none);
        auto active_batch = seed_db.MakeBatch();
        BOOST_REQUIRE_EQUAL(seed_db.m_refcount.load(), 1);

        BerkeleyDatabase target_db{target_env, "wallet.dat", options};
        try {
            target_db.Open();
        } catch (const std::exception&) {
            // The copied file intentionally has no matching source environment log.
        }

        const bool flush_success = target_env->Flush(true);
        BOOST_CHECK_MESSAGE(!flush_success, "a failed BDB checkpoint must be reported");

        const std::string diagnostics = ReadFile(target_dir / "db.log");
        BOOST_CHECK_MESSAGE(diagnostics.find("DB_RUNRECOVERY") != std::string::npos ||
                                diagnostics.find("past end of log") != std::string::npos,
                            "fixture did not trigger the expected BDB recovery failure");
        BOOST_CHECK(target_env->IsDirectoryLockHeld());
        BOOST_CHECK_MESSAGE(fs::exists(target_log), "failed checkpoint removed the recovery log");
    }

    // Destroying the last database owner must not turn an earlier failure into
    // a clean-shutdown path that removes the recovery logs.
    target_env->Flush(true);
    BOOST_CHECK(target_env->IsDirectoryLockHeld());
    BOOST_CHECK_MESSAGE(fs::exists(target_log), "environment destruction removed recovery logs after an earlier failure");
    BOOST_CHECK(fs::exists(target_wallet));
}

BOOST_AUTO_TEST_CASE(bdb_verify_ignores_historical_unsupported_version_text)
{
    const fs::path env_dir = m_args.GetDataDirNet() / "bdb-verify-historical-version-text";
    BOOST_REQUIRE(!fs::exists(env_dir));
    fs::create_directories(env_dir / "database");

    const fs::path wallet_file = env_dir / "wallet.dat";
    const std::string corrupt_wallet(4096, '\0');
    {
        std::ofstream wallet_stream{wallet_file, std::ios::binary};
        wallet_stream.write(corrupt_wallet.data(), corrupt_wallet.size());
        BOOST_REQUIRE(wallet_stream.good());
    }
    {
        std::ofstream log_stream{env_dir / "db.log"};
        log_stream << "historical diagnostic: unsupported DB version\n";
        BOOST_REQUIRE(log_stream.good());
    }

    DatabaseOptions options;
    auto env = std::make_shared<BerkeleyEnvironment>(env_dir, false);
    BerkeleyDatabase database{env, "wallet.dat", options};
    bilingual_str error;
    BOOST_CHECK(!database.Verify(error));
    BOOST_CHECK_MESSAGE(error.original.find("corrupt") != std::string::npos,
                        "a stale db.log version warning must not classify a distinct current verification failure as a newer wallet format");
    BOOST_CHECK(fs::exists(wallet_file));
    BOOST_CHECK_MESSAGE(ReadFile(wallet_file) == corrupt_wallet, "verification changed the original wallet bytes");

    bool conversion_backup_created{false};
    const std::string backup_prefix = "bdb-verify-historical-version-text-backup-auto-bdb48-";
    for (const auto& entry : fs::directory_iterator{env_dir.parent_path()}) {
        if (fs::PathToString(entry.path().filename()).rfind(backup_prefix, 0) == 0) {
            conversion_backup_created = true;
            break;
        }
    }
    BOOST_CHECK_MESSAGE(!conversion_backup_created, "historical db.log text triggered automatic wallet conversion");
}

BOOST_AUTO_TEST_CASE(bdb_batch_stops_using_environment_after_recovery_is_required)
{
    const fs::path env_dir = m_args.GetDataDirNet() / "bdb-batch-recovery-latch";
    BOOST_REQUIRE(!fs::exists(env_dir));

    DatabaseOptions options;
    auto env = std::make_shared<BerkeleyEnvironment>(env_dir, false);
    bilingual_str error;
    BOOST_REQUIRE(env->Open(error));
    BerkeleyDatabase database{env, "wallet.dat", options};
    auto batch = database.MakeBatch();
    BOOST_REQUIRE(batch->Write(std::string{"existing-key"}, std::string{"existing-value"}));
    std::string value;
    BOOST_REQUIRE(batch->Read(std::string{"existing-key"}, value));
    BOOST_REQUIRE_EQUAL(value, "existing-value");

    env->MarkRecoveryRequired();

    BOOST_CHECK(!batch->Write(std::string{"blocked-key"}, std::string{"must-not-write"}));
    BOOST_CHECK(!batch->Erase(std::string{"existing-key"}));
    BOOST_CHECK(!batch->Read(std::string{"existing-key"}, value));
    BOOST_CHECK(!batch->Exists(std::string{"existing-key"}));
    BOOST_CHECK(!batch->TxnBegin());
}

BOOST_AUTO_TEST_CASE(bdb_checkpoint_skips_when_recovery_is_required)
{
    const fs::path env_dir = m_args.GetDataDirNet() / "bdb-checkpoint-recovery-latch";
    DatabaseOptions options;
    auto env = std::make_shared<BerkeleyEnvironment>(env_dir, false);
    bilingual_str error;
    BOOST_REQUIRE(env->Open(error));
    BerkeleyDatabase database{env, "wallet.dat", options};
    auto batch = database.MakeBatch();
    BOOST_REQUIRE(batch->Write(std::string{"key"}, std::string{"value"}));
    batch.reset();
    database.Close();
    BOOST_REQUIRE(!env->IsRecoveryRequired());

    env->MarkRecoveryRequired();

    BOOST_CHECK(!env->CheckpointLSN("wallet.dat"));
}

BOOST_AUTO_TEST_CASE(bdb_rewrite_preserves_source_when_stale_temporary_exists)
{
    const fs::path env_dir = m_args.GetDataDirNet() / "bdb-rewrite-stale-temporary-strings";
    BOOST_REQUIRE(!fs::exists(env_dir));
    DatabaseOptions options;
    auto env = std::make_shared<BerkeleyEnvironment>(env_dir, false);
    bilingual_str error;
    BOOST_REQUIRE(env->Open(error));

    const std::string conflict_key{"conflict-key"};
    const std::string source_value{"source-value"};
    const std::string source_only_key{"source-only-key"};
    const std::string source_only_value{"source-only-value"};
    const std::string stale_value{"stale-value"};
    const std::string stale_only_key{"stale-only-key"};
    const std::string stale_only_value{"stale-only-value"};

    BerkeleyDatabase database{env, "wallet.dat", options};
    {
        auto batch = database.MakeBatch();
        BOOST_REQUIRE(batch->Write(conflict_key, source_value));
        BOOST_REQUIRE(batch->Write(source_only_key, source_only_value));
    }
    {
        auto batch = database.MakeBatch();
        std::string value;
        BOOST_REQUIRE(batch->Read(conflict_key, value));
        BOOST_REQUIRE_EQUAL(value, source_value);
        BOOST_REQUIRE(batch->Read(source_only_key, value));
        BOOST_REQUIRE_EQUAL(value, source_only_value);
    }
    {
        BerkeleyDatabase stale_rewrite{env, "wallet.dat.rewrite", options};
        auto batch = stale_rewrite.MakeBatch();
        BOOST_REQUIRE(batch->Write(conflict_key, stale_value));
        BOOST_REQUIRE(batch->Write(stale_only_key, stale_only_value));
        batch.reset();
        stale_rewrite.Close();
    }

    BOOST_REQUIRE(fs::exists(env_dir / "wallet.dat.rewrite"));
    BOOST_CHECK_MESSAGE(database.Rewrite(), "rewrite should not promote a stale temporary database over the source wallet");
    BOOST_CHECK(fs::exists(env_dir / "wallet.dat"));

    {
        auto batch = database.MakeBatch();
        std::string value;
        BOOST_CHECK(batch->Read(conflict_key, value));
        BOOST_CHECK_EQUAL(value, source_value);
        BOOST_CHECK(batch->Read(source_only_key, value));
        BOOST_CHECK_EQUAL(value, source_only_value);
        BOOST_CHECK(!batch->Read(stale_only_key, value));
    }

    BerkeleyDatabase stale_rewrite{env, "wallet.dat.rewrite", options};
    auto stale_batch = stale_rewrite.MakeBatch();
    std::string stale_read_value;
    BOOST_CHECK(stale_batch->Read(conflict_key, stale_read_value));
    BOOST_CHECK_EQUAL(stale_read_value, stale_value);
    BOOST_CHECK(stale_batch->Read(stale_only_key, stale_read_value));
    BOOST_CHECK_EQUAL(stale_read_value, stale_only_value);
}

BOOST_AUTO_TEST_CASE(bdb_rewrite_succeeds_for_valid_windows_wallet_path_near_max_path)
{
    fs::path env_dir = m_args.GetDataDirNet() / "bdb-rewrite-near-max-path";
#ifdef WIN32
    constexpr size_t WINDOWS_MAX_PATH = 260;
    constexpr size_t TARGET_WALLET_PATH_LENGTH = 210;
    const size_t base_wallet_path_length = (env_dir / "wallet.dat").native().size();
    if (base_wallet_path_length + 1 < TARGET_WALLET_PATH_LENGTH) {
        env_dir /= fs::PathFromString(std::string(TARGET_WALLET_PATH_LENGTH - base_wallet_path_length - 1, 'p'));
    }
    const size_t wallet_path_length = (env_dir / "wallet.dat").native().size();
    BOOST_REQUIRE_MESSAGE(wallet_path_length < WINDOWS_MAX_PATH, "fixture wallet path itself must remain valid under Windows MAX_PATH");
    // The previous `.rewrite-` plus 64-hex suffix added 73 characters, which
    // overflowed MAX_PATH even though wallet.dat itself remained accessible.
    BOOST_REQUIRE_MESSAGE(wallet_path_length + 73 >= WINDOWS_MAX_PATH, "fixture must exercise the temporary rewrite-path boundary");
#endif
    BOOST_REQUIRE(!fs::exists(env_dir));

    DatabaseOptions options;
    bilingual_str error;
    auto env = GetBerkeleyEnv(env_dir, false);
    BOOST_REQUIRE(env->Open(error));
    const char** unconfigured_data_dirs{nullptr};
    BOOST_REQUIRE_EQUAL(env->dbenv->get_data_dirs(&unconfigured_data_dirs), 0);
    BerkeleyDatabase database{env, "wallet.dat", options};
    {
        auto batch = database.MakeBatch();
        BOOST_REQUIRE(batch->Write(std::string{"source-key"}, std::string{"source-value"}));
    }

    BOOST_REQUIRE_MESSAGE(database.Rewrite(), "wallet rewrite should succeed when wallet.dat itself fits within the Windows path limit");
    {
        auto batch = database.MakeBatch();
        std::string value;
        BOOST_CHECK(batch->Read(std::string{"source-key"}, value));
        BOOST_CHECK_EQUAL(value, "source-value");
    }
}

#ifdef WIN32
BOOST_AUTO_TEST_CASE(bdb_rewrite_refuses_when_fallback_cannot_preserve_transaction_headroom)
{
    fs::path env_dir = m_args.GetDataDirNet() / "bdb-rewrite-no-fallback-headroom";
    constexpr size_t WINDOWS_MAX_PATH = 260;
    constexpr size_t TARGET_WALLET_PATH_LENGTH = 245;
    const size_t base_wallet_path_length = (env_dir / "wallet.dat").native().size();
    if (base_wallet_path_length + 1 < TARGET_WALLET_PATH_LENGTH) {
        env_dir /= fs::PathFromString(std::string(TARGET_WALLET_PATH_LENGTH - base_wallet_path_length - 1, 'p'));
    }
    const size_t wallet_path_length = (env_dir / "wallet.dat").native().size();
    BOOST_REQUIRE_EQUAL(wallet_path_length, TARGET_WALLET_PATH_LENGTH);
    BOOST_REQUIRE_MESSAGE(wallet_path_length < WINDOWS_MAX_PATH, "fixture wallet path itself must remain valid under Windows MAX_PATH");
    BOOST_REQUIRE(!fs::exists(env_dir));

    DatabaseOptions options;
    bilingual_str error;
    auto env = GetBerkeleyEnv(env_dir, false);
    BOOST_REQUIRE(env->Open(error));
    BerkeleyDatabase database{env, "wallet.dat", options};
    {
        auto batch = database.MakeBatch();
        BOOST_REQUIRE(batch->Write(std::string{"source-key"}, std::string{"source-value"}));
    }

    BOOST_CHECK_MESSAGE(!database.Rewrite(), "rewrite must refuse when no fallback filename can preserve BDB transaction headroom");
    BOOST_CHECK_MESSAGE(!env->IsRecoveryRequired(), "refusing an unsafe temporary path must not poison the Berkeley environment");
    if (env->IsRecoveryRequired()) return;
    auto batch = database.MakeBatch();
    std::string value;
    BOOST_CHECK(batch->Read(std::string{"source-key"}, value));
    BOOST_CHECK_EQUAL(value, "source-value");
}
#endif

BOOST_AUTO_TEST_CASE(bdb_rewrite_respects_configured_data_dir_path_budget)
{
    fs::path env_dir = m_args.GetDataDirNet() / "bdb-rewrite-configured-data-dir";
    fs::path data_dir_name{"wallet-data-directory-path-budget"};
    const fs::path later_data_dir_name{"later-wallet-data-directory-path-budget-extension"};
    fs::path data_dir_path = env_dir / data_dir_name;
#ifdef WIN32
    constexpr size_t WINDOWS_MAX_PATH = 260;
    constexpr size_t WINDOWS_REWRITE_PATH_HEADROOM = 16;
    constexpr size_t MAX_SAFE_REWRITE_PATH = WINDOWS_MAX_PATH - 1 - WINDOWS_REWRITE_PATH_HEADROOM;
    // Leave suffix budget after the configured data-directory component too.
    constexpr size_t TARGET_WALLET_PATH_LENGTH = 180;
    bool remove_external_data_dir{false};
    const size_t base_wallet_path_length = (env_dir / "wallet.dat").native().size();
    if (base_wallet_path_length + 1 < TARGET_WALLET_PATH_LENGTH) {
        env_dir /= fs::PathFromString(std::string(TARGET_WALLET_PATH_LENGTH - base_wallet_path_length - 1, 'p'));
    }

    // A root-relative Windows DB_CONFIG path resolves on the process's current
    // drive, not relative to the BDB environment. This runs on single-drive
    // systems too; multi-drive runners additionally exercise different roots.
    const fs::path current_path = fs::current_path();
    const char* user_profile_value = std::getenv("USERPROFILE");
    if (user_profile_value) {
        const fs::path user_profile = fs::PathFromString(user_profile_value);
        const fs::path profile_temp = user_profile / "AppData" / "Local" / "Temp";
        if (user_profile.root_name() == current_path.root_name() && user_profile.native().find(L' ') == std::wstring::npos) {
            const fs::path root_relative_profile_temp = profile_temp.root_directory() / profile_temp.relative_path();
            data_dir_name = root_relative_profile_temp / fs::PathFromString(
                "korsh-bdb-root-relative-" + fs::PathToString(m_args.GetDataDirNet().parent_path().filename()));
            BOOST_REQUIRE(data_dir_name.has_root_directory());
            BOOST_REQUIRE(data_dir_name.root_name().empty());
            BOOST_TEST_MESSAGE("Testing root-relative DB_CONFIG path with environment drive " << env_dir.root_name()
                                                                                               << " and current drive " << current_path.root_name());
            std::error_code absolute_error;
            data_dir_path = std::filesystem::absolute(data_dir_name, absolute_error);
            BOOST_REQUIRE_MESSAGE(!absolute_error, "root-relative fixture directory must resolve on the current drive");
            BOOST_REQUIRE_EQUAL(data_dir_path.root_name().string(), current_path.root_name().string());
            const fs::path resolved_data_dir_parent{data_dir_path.parent_path()};
            BOOST_REQUIRE_EQUAL(fs::PathToString(resolved_data_dir_parent), fs::PathToString(profile_temp));
            remove_external_data_dir = true;
        }
    }
    if (!data_dir_name.has_root_directory()) data_dir_path = env_dir / data_dir_name;
    struct RemoveDirectoryOnExit {
        fs::path path;
        ~RemoveDirectoryOnExit()
        {
            if (!path.empty()) {
                std::error_code error;
                fs::remove_all(path, error);
            }
        }
    } cleanup_external_data_dir{};

    const size_t source_wallet_path_length = (data_dir_path / "wallet.dat").native().size();
    const size_t later_wallet_path_length = (env_dir / later_data_dir_name / "wallet.dat").native().size();
    BOOST_REQUIRE_MESSAGE(source_wallet_path_length < WINDOWS_MAX_PATH, "configured source wallet path must remain valid under Windows MAX_PATH");
    BOOST_REQUIRE_MESSAGE(later_wallet_path_length < WINDOWS_MAX_PATH, "later configured data-directory path must remain valid under Windows MAX_PATH");
    BOOST_REQUIRE_MESSAGE(source_wallet_path_length < MAX_SAFE_REWRITE_PATH, "source wallet path must leave room for transactional rewrite headroom");
    const size_t source_suffix_budget = MAX_SAFE_REWRITE_PATH - source_wallet_path_length;
    BOOST_REQUIRE_MESSAGE(source_suffix_budget >= 25, "the source directory must allow a collision-resistant suffixed temporary name");
    const size_t source_only_suffix_length = 9 + std::min<size_t>(32, source_suffix_budget - 9);
    BOOST_REQUIRE_MESSAGE(later_wallet_path_length + source_only_suffix_length > MAX_SAFE_REWRITE_PATH,
                          "a suffix budgeted only against the source directory must exceed the safe budget in the later configured directory");
    const size_t later_directory_length = (env_dir / later_data_dir_name).native().size();
    BOOST_REQUIRE_MESSAGE(later_directory_length + 1 + 16 <= MAX_SAFE_REWRITE_PATH,
                          "the short sibling fallback must fit while preserving transactional path headroom");
#endif
    BOOST_REQUIRE(!fs::exists(env_dir));
    BOOST_REQUIRE(!fs::exists(data_dir_path));
#ifdef WIN32
    if (remove_external_data_dir) cleanup_external_data_dir.path = data_dir_path;
#endif
    fs::create_directories(data_dir_path);
    fs::create_directories(env_dir / later_data_dir_name);
    {
        std::ofstream config{env_dir / "DB_CONFIG", std::ios::binary};
        config << "set_data_dir " << fs::PathToString(data_dir_name) << '\n'
               << "set_data_dir " << fs::PathToString(later_data_dir_name) << '\n';
        BOOST_REQUIRE(config.good());
    }

    DatabaseOptions options;
    bilingual_str error;
    auto env = GetBerkeleyEnv(env_dir, false);
    BOOST_REQUIRE(env->Open(error));
    BerkeleyDatabase database{env, "wallet.dat", options};
    {
        auto batch = database.MakeBatch();
        BOOST_REQUIRE(batch->Write(std::string{"configured-key"}, std::string{"configured-value"}));
    }

    BOOST_REQUIRE(fs::exists(data_dir_path / "wallet.dat"));
    const char** configured_data_dirs{nullptr};
    BOOST_REQUIRE_EQUAL(env->dbenv->get_data_dirs(&configured_data_dirs), 0);
    BOOST_REQUIRE(configured_data_dirs && configured_data_dirs[0] && configured_data_dirs[1]);
    BOOST_CHECK_EQUAL(fs::PathToString(fs::PathFromString(configured_data_dirs[0]).filename()), fs::PathToString(data_dir_name.filename()));
    BOOST_CHECK_EQUAL(fs::PathToString(fs::PathFromString(configured_data_dirs[1]).filename()), fs::PathToString(later_data_dir_name.filename()));
    const bool rewrite_succeeded = database.Rewrite();
    if (!rewrite_succeeded) BOOST_TEST_MESSAGE("BDB environment log: " << ReadFile(env_dir / "db.log"));
    BOOST_REQUIRE_MESSAGE(rewrite_succeeded, "rewrite should honor the data directory configured by Berkeley DB");
    {
        auto batch = database.MakeBatch();
        std::string value;
        BOOST_CHECK(batch->Read(std::string{"configured-key"}, value));
        BOOST_CHECK_EQUAL(value, "configured-value");
    }
}

BOOST_AUTO_TEST_CASE(bdb_rewrite_refuses_when_recovery_is_required)
{
    const fs::path env_dir = m_args.GetDataDirNet() / "bdb-rewrite-recovery-latch";
    DatabaseOptions options;
    auto env = std::make_shared<BerkeleyEnvironment>(env_dir, false);
    bilingual_str error;
    BOOST_REQUIRE(env->Open(error));
    BerkeleyDatabase database{env, "wallet.dat", options};
    auto batch = database.MakeBatch();
    BOOST_REQUIRE(batch->Write(std::string{"key"}, std::string{"value"}));
    batch.reset();
    database.Close();
    BOOST_REQUIRE(!env->IsRecoveryRequired());

    env->MarkRecoveryRequired();

    bool rewrite_succeeded = true;
    BOOST_CHECK_NO_THROW(rewrite_succeeded = database.Rewrite(nullptr));
    BOOST_CHECK(!rewrite_succeeded);
}

BOOST_AUTO_TEST_CASE(bdb_recovery_error_blocks_exclusive_rewrite_operation)
{
    BerkeleyEnvironment env;
    std::promise<void> operation_entered;
    std::promise<void> allow_operation_to_fail;
    std::promise<void> exclusive_operation_started;
    auto operation_entered_future = operation_entered.get_future();
    auto allow_operation_to_fail_future = allow_operation_to_fail.get_future();
    auto exclusive_operation_started_future = exclusive_operation_started.get_future();

    int operation_result{-1};
    std::thread failing_operation([&] {
        operation_result = env.RunDatabaseOperation([&] {
            operation_entered.set_value();
            allow_operation_to_fail_future.wait();
            return DB_RUNRECOVERY;
        });
    });
    operation_entered_future.wait();

    bool promotion_callback_called{false};
    bool promotion_succeeded{true};
    std::thread promotion([&] {
        exclusive_operation_started.set_value();
        promotion_succeeded = env.RunExclusiveDatabaseOperation([&] {
            promotion_callback_called = true;
            return true;
        });
    });
    exclusive_operation_started_future.wait();

    allow_operation_to_fail.set_value();
    failing_operation.join();
    promotion.join();

    BOOST_CHECK_EQUAL(operation_result, DB_RUNRECOVERY);
    BOOST_CHECK(env.IsRecoveryRequired());
    BOOST_CHECK(!promotion_callback_called);
    BOOST_CHECK(!promotion_succeeded);

    bool later_operation_called{false};
    const int later_result = env.RunDatabaseOperation([&] {
        later_operation_called = true;
        return 0;
    });
    BOOST_CHECK_EQUAL(later_result, DB_RUNRECOVERY);
    BOOST_CHECK(!later_operation_called);
}

BOOST_AUTO_TEST_CASE(bdb_cursor_cleanup_error_latches_recovery)
{
    BerkeleyEnvironment env;

    const int close_result = env.RunCleanupDatabaseOperation([] { return DB_LOCK_DEADLOCK; }, /*latch_on_any_error=*/true);

    BOOST_CHECK_EQUAL(close_result, DB_LOCK_DEADLOCK);
    BOOST_CHECK(env.IsRecoveryRequired());
}

BOOST_AUTO_TEST_CASE(bdb_wallet_file_conversion_closes_resets_and_reopens_environment)
{
    fs::path database_filename;
    auto env = GetWalletEnv(m_args.GetDataDirNet() / "bdb-migration", database_filename);
    bilingual_str error;
    BOOST_REQUIRE(env->Open(error));

    bool converter_called{false};
    bool directory_lock_held_during_conversion{false};
    const auto result = env->ConvertWalletFile([&] {
        converter_called = true;
        directory_lock_held_during_conversion = env->IsDirectoryLockHeld();
        BOOST_CHECK(!env->IsInitialized());
        return true;
    }, error);

    BOOST_CHECK(result == BerkeleyEnvironment::WalletMigrationResult::SUCCESS);
    BOOST_CHECK(converter_called);
    BOOST_CHECK(directory_lock_held_during_conversion);
    BOOST_CHECK(env->IsDirectoryLockHeld());
    BOOST_CHECK(env->IsInitialized());
}

BOOST_AUTO_TEST_CASE(bdb_wallet_file_conversion_refuses_active_database_batch)
{
    fs::path database_filename;
    auto env = GetWalletEnv(m_args.GetDataDirNet() / "bdb-migration-active", database_filename);
    bilingual_str error;
    BOOST_REQUIRE(env->Open(error));

    DatabaseOptions options;
    options.verify = false;
    BerkeleyDatabase database{env, database_filename, options};
    auto batch = database.MakeBatch(/*flush_on_close=*/false);
    BOOST_REQUIRE(batch);

    bool converter_called{false};
    const auto result = env->ConvertWalletFile([&] {
        converter_called = true;
        return true;
    }, error);

    BOOST_CHECK(result == BerkeleyEnvironment::WalletMigrationResult::DATABASE_IN_USE);
    BOOST_CHECK(!converter_called);
    BOOST_CHECK(env->IsInitialized());
    BOOST_CHECK(batch->Write(std::string{"still-open"}, std::string{"ok"}));
}

BOOST_AUTO_TEST_CASE(bdb_reload_db_env_retains_and_releases_directory_lock)
{
    fs::path database_filename;
    auto env = GetWalletEnv(m_args.GetDataDirNet() / "bdb-reload-lock", database_filename);
    bilingual_str error;
    BOOST_REQUIRE(env->Open(error));
    BOOST_REQUIRE(env->IsDirectoryLockHeld());

    env->ReloadDbEnv();

    BOOST_CHECK(env->IsInitialized());
    BOOST_CHECK(env->IsDirectoryLockHeld());
    BOOST_REQUIRE(env->Flush(true));
    BOOST_CHECK(!env->IsDirectoryLockHeld());
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
