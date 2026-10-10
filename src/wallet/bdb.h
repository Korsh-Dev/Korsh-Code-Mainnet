// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_BDB_H
#define BITCOIN_WALLET_BDB_H

#include <cstdint>
#include <clientversion.h>
#include <fs.h>
#include <serialize.h>
#include <streams.h>
#include <util/system.h>
#include <wallet/db.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct bilingual_str;

#include <db_cxx.h>

#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <utility>

namespace wallet {
extern RecursiveMutex cs_db;

struct WalletDatabaseFileId {
    uint8_t value[DB_FILE_ID_LEN];
    bool operator==(const WalletDatabaseFileId& rhs) const;
};

class BerkeleyDatabase;

class BerkeleyEnvironment
{
private:
    bool fDbEnvInit;
    bool fMockDb;
    std::atomic<bool> m_recovery_required{false};
    bool m_directory_lock_held{false};
    std::string m_directory_lock_key;
    mutable std::shared_mutex m_db_operation_mutex;
    void UnlockDirectoryLock();
    // Don't change into fs::path, as that can result in
    // shutdown problems/crashes caused by a static initialized internal pointer.
    std::string strPath;

public:
    enum class WalletMigrationResult {
        SUCCESS,
        DATABASE_IN_USE,
        CLOSE_FAILED,
        CONVERSION_FAILED,
        OPEN_FAILED,
    };

    std::unique_ptr<DbEnv> dbenv;
    std::map<fs::path, std::reference_wrapper<BerkeleyDatabase>> m_databases;
    std::unordered_map<std::string, WalletDatabaseFileId> m_fileids;
    std::condition_variable_any m_db_in_use;
    bool m_use_shared_memory;

    explicit BerkeleyEnvironment(const fs::path& env_directory, bool use_shared_memory);
    BerkeleyEnvironment();
    ~BerkeleyEnvironment();
    void Reset();

    bool IsMock() const { return fMockDb; }
    bool IsInitialized() const { return fDbEnvInit; }
    fs::path Directory() const { return fs::PathFromString(strPath); }

    bool Open(bilingual_str& error, bool preserve_directory_lock_on_failure = false);
    bool Close(bool preserve_files = false, bool preserve_directory_lock = false);
    WalletMigrationResult ConvertWalletFile(const std::function<bool()>& converter, bilingual_str& open_error);
    bool Flush(bool fShutdown, bool preserve_directory_lock = false);
    bool CheckpointLSN(const std::string& strFile);
    void MarkRecoveryRequired();
    bool IsRecoveryRequired() const;
    bool IsDirectoryLockHeld() const;

    /** Run one BDB call while admitted against recovery and rewrite promotion. */
    template <typename Function>
    int RunDatabaseOperation(Function&& operation, bool latch_on_any_error = false, bool* operation_ran = nullptr)
    {
        if (operation_ran) *operation_ran = false;
        std::shared_lock<std::shared_mutex> lock{m_db_operation_mutex};
        if (IsRecoveryRequired()) return DB_RUNRECOVERY;
        if (operation_ran) *operation_ran = true;
        const int ret = std::forward<Function>(operation)();
        if (ret == DB_RUNRECOVERY || (latch_on_any_error && ret != 0)) MarkRecoveryRequired();
        return ret;
    }

    /** Run teardown/cleanup BDB calls even after recovery has been latched. */
    template <typename Function>
    int RunCleanupDatabaseOperation(Function&& operation, bool latch_on_any_error = false)
    {
        std::shared_lock<std::shared_mutex> lock{m_db_operation_mutex};
        const int ret = std::forward<Function>(operation)();
        if (ret == DB_RUNRECOVERY || (latch_on_any_error && ret != 0)) MarkRecoveryRequired();
        return ret;
    }

    /** Exclude other BDB operations while atomically replacing a wallet database. */
    template <typename Function>
    bool RunExclusiveDatabaseOperation(Function&& operation)
    {
        std::unique_lock<std::shared_mutex> lock{m_db_operation_mutex};
        if (IsRecoveryRequired()) return false;
        return std::forward<Function>(operation)();
    }

    bool CloseDb(const fs::path& filename);
    void ReloadDbEnv();

    DbTxn* TxnBegin(int flags = DB_TXN_WRITE_NOSYNC)
    {
        DbTxn* ptxn = nullptr;
        const int ret = RunDatabaseOperation([&] { return dbenv->txn_begin(nullptr, &ptxn, flags); });
        if (!ptxn || ret != 0)
            return nullptr;
        return ptxn;
    }
};

/** Get BerkeleyEnvironment for a directory; returns empty while a prior environment is tearing down. */
std::shared_ptr<BerkeleyEnvironment> GetBerkeleyEnv(const fs::path& env_directory, bool use_shared_memory);

class BerkeleyBatch;

/** An instance of this class represents one database.
 * For BerkeleyDB this is just a (env, strFile) tuple.
 **/
class BerkeleyDatabase : public WalletDatabase
{
public:
    BerkeleyDatabase() = delete;

    /** Create DB handle to real database */
    BerkeleyDatabase(std::shared_ptr<BerkeleyEnvironment> env, fs::path filename, const DatabaseOptions& options) :
        WalletDatabase(), env(std::move(env)), m_filename(std::move(filename)), m_max_log_mb(options.max_log_mb)
    {
        auto inserted = this->env->m_databases.emplace(m_filename, std::ref(*this));
        assert(inserted.second);
    }

    ~BerkeleyDatabase() override;

    /** Open the database if it is not already opened. */
    void Open() override;

    /** Rewrite the entire database on disk, with the exception of key pszSkip if non-zero
     */
    bool Rewrite(const char* pszSkip=nullptr) override;

    /** Indicate that a new database user has begun using the database. */
    void AddRef() override;
    /** Indicate that database user has stopped using the database and that it could be flushed or closed. */
    void RemoveRef() override;

    /** Back up the entire database to a file.
     */
    bool Backup(const std::string& strDest) const override;

    /** Make sure all changes are flushed to database file.
     */
    void Flush() override;
    /** Flush to the database file and close idle database handles.
     *  The shared environment remains open until its last owner is destroyed.
     */
    void Close() override;
    /* flush the wallet passively (TRY_LOCK)
       ideal to be called periodically */
    bool PeriodicFlush() override;

    void IncrementUpdateCounter() override;

    void ReloadDbEnv() override;

    /** Verifies the environment and database file */
    bool Verify(bilingual_str& error);

    /** Return path to main database filename */
    std::string Filename() override { return fs::PathToString(env->Directory() / m_filename); }

    std::string Format() override { return "bdb"; }
    /**
     * Pointer to shared database environment.
     *
     * Normally there is only one BerkeleyDatabase object per
     * BerkeleyEnvivonment, but in the special, backwards compatible case where
     * multiple wallet BDB data files are loaded from the same directory, this
     * will point to a shared instance that gets freed when the last data file
     * is closed.
     */
    std::shared_ptr<BerkeleyEnvironment> env;

    /** Database pointer. This is initialized lazily and reset during flushes, so it can be null. */
    std::unique_ptr<Db> m_db;

    fs::path m_filename;
    int64_t m_max_log_mb;

    /** Make a BerkeleyBatch connected to this database */
    std::unique_ptr<DatabaseBatch> MakeBatch(bool flush_on_close = true) override;

    virtual bool SupportsAutoBackup() override { return true; }
};

/** RAII class that provides access to a Berkeley database */
class BerkeleyBatch : public DatabaseBatch
{
public:
    /** RAII class that automatically cleanses its data on destruction */
    class SafeDbt final
    {
        Dbt m_dbt;

    public:
        // construct Dbt with internally-managed data
        SafeDbt();
        // construct Dbt with provided data
        SafeDbt(void* data, size_t size);
        ~SafeDbt();

        // delegate to Dbt
        const void* get_data() const;
        uint32_t get_size() const;

        // conversion operator to access the underlying Dbt
        operator Dbt*();
    };

private:
    bool ReadKey(CDataStream&& key, CDataStream& value) override;
    bool WriteKey(CDataStream&& key, CDataStream&& value, bool overwrite = true) override;
    bool EraseKey(CDataStream&& key) override;
    bool HasKey(CDataStream&& key) override;

protected:
    Db* pdb{nullptr};
    std::string strFile;
    DbTxn* activeTxn{nullptr};
    Dbc* m_cursor{nullptr};
    bool fReadOnly;
    bool fFlushOnClose;
    BerkeleyEnvironment *env;
    BerkeleyDatabase& m_database;

public:
    explicit BerkeleyBatch(BerkeleyDatabase& database, const bool fReadOnly, bool fFlushOnCloseIn=true);
    ~BerkeleyBatch() override;

    BerkeleyBatch(const BerkeleyBatch&) = delete;
    BerkeleyBatch& operator=(const BerkeleyBatch&) = delete;

    void Flush() override;
    void Close() override;

    bool StartCursor() override;
    bool ReadAtCursor(CDataStream& ssKey, CDataStream& ssValue, bool& complete) override;
    bool CloseCursor() override;
    bool TxnBegin() override;
    bool TxnCommit() override;
    bool TxnAbort() override;
};

std::string BerkeleyDatabaseVersion();

/** Perform sanity check of runtime BDB version versus linked BDB version.
 */
bool BerkeleyDatabaseSanityCheck();

//! Return object giving access to Berkeley database at specified path.
std::unique_ptr<BerkeleyDatabase> MakeBerkeleyDatabase(const fs::path& path, const DatabaseOptions& options, DatabaseStatus& status, bilingual_str& error);
} // namespace wallet

#endif // BITCOIN_WALLET_BDB_H
