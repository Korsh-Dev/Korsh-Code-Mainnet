// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <compat/compat.h>
#include <fs.h>
#include <wallet/bdb.h>
#include <wallet/db.h>

#include <random.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <util/translation.h>

#include <stdint.h>

#include <stdarg.h>
#include <stdio.h>

#include <fstream>
#include <set>
#include <string>
#include <sys/stat.h>
#include <vector>

#ifdef WIN32
#include <windows.h>
#endif

// Windows may not define S_IRUSR or S_IWUSR. We define both
// here, with the same values as glibc (see stat.h).
#ifdef WIN32
#ifndef S_IRUSR
#define S_IRUSR             0400
#define S_IWUSR             0200
#endif
#endif

namespace wallet {
RecursiveMutex cs_db;

namespace {
Span<const std::byte> SpanFromDbt(const BerkeleyBatch::SafeDbt& dbt)
{
    return {reinterpret_cast<const std::byte*>(dbt.get_data()), dbt.get_size()};
}

fs::path CanonicalWalletDirectory(const fs::path& directory)
{
    std::error_code error;
    fs::path canonical_directory = fs::weakly_canonical(directory, error);
    if (error) {
        throw fs::filesystem_error("Unable to canonicalize Berkeley DB wallet directory", directory, error);
    }
    return canonical_directory;
}

//! Make sure database has a unique fileid within the environment. If it
//! doesn't, throw an error. BDB caches do not work properly when more than one
//! open database has the same fileid (values written to one database may show
//! up in reads to other databases).
//!
//! BerkeleyDB generates unique fileids by default
//! (https://docs.oracle.com/cd/E17275_01/html/programmer_reference/program_copy.html),
//! so bitcoin should never create different databases with the same fileid, but
//! this error can be triggered if users manually copy database files.
void CheckUniqueFileid(BerkeleyEnvironment& env, const std::string& filename, Db& db, WalletDatabaseFileId& fileid)
{
    if (env.IsMock()) return;

    int ret = env.RunDatabaseOperation([&] { return db.get_mpf()->get_fileid(fileid.value); });
    if (ret != 0) {
        throw std::runtime_error(strprintf("BerkeleyDatabase: Can't open database %s (get_fileid failed with %d)", filename, ret));
    }

    for (const auto& item : env.m_fileids) {
        if (fileid == item.second && &fileid != &item.second) {
            throw std::runtime_error(strprintf("BerkeleyDatabase: Can't open database %s (duplicates fileid %s from %s)", filename,
            HexStr(item.second.value), item.first));
        }
    }
}

std::string MakeRewriteTemporaryFilename(BerkeleyEnvironment& env, const fs::path& source_filename)
{
#ifdef WIN32
    // Keep Windows rewrite paths below MAX_PATH. The original wallet filename
    // can be valid even when appending a long random suffix would not be.
    constexpr size_t MAX_WINDOWS_PATH_LENGTH = MAX_PATH - 1;
    // Leave room for Berkeley DB's internal Windows path handling during the
    // transactional rename and commit for both temporary-name strategies.
    constexpr size_t WINDOWS_REWRITE_PATH_HEADROOM = 16;
    constexpr size_t MAX_SAFE_WINDOWS_PATH_LENGTH = MAX_WINDOWS_PATH_LENGTH - WINDOWS_REWRITE_PATH_HEADROOM;
    const char** configured_data_dirs{nullptr};
    const int data_dirs_result = env.RunDatabaseOperation([&] { return env.dbenv->get_data_dirs(&configured_data_dirs); });
    if (data_dirs_result != 0) {
        LogPrintf("BerkeleyBatch::Rewrite: Error %d retrieving BDB data directories: %s\n", data_dirs_result, DbEnv::strerror(data_dirs_result));
        return {};
    }

    std::error_code error;
    std::vector<fs::path> data_directories;
    if (configured_data_dirs) {
        for (const char** configured_dir = configured_data_dirs; *configured_dir; ++configured_dir) {
            fs::path directory = fs::PathFromString(*configured_dir);
            // On Windows, BDB treats a root-relative path (e.g. "\\data") as
            // absolute to the process's current drive, although filesystem::path
            // reports is_absolute() == false when the root name is absent.
            if (!directory.is_absolute() && !directory.has_root_directory()) directory = env.Directory() / directory;
            directory = std::filesystem::absolute(directory, error);
            if (error) return {};
            data_directories.push_back(std::move(directory));
        }
    }
    if (data_directories.empty()) {
        fs::path directory = std::filesystem::absolute(env.Directory(), error);
        if (error) return {};
        data_directories.push_back(std::move(directory));
    }

    // Relative database filenames may resolve through DB_CONFIG set_data_dir.
    // Use the actual source directory and BDB's first directory (the creation
    // target), and precheck every configured directory for a collision.
    fs::path source_directory = data_directories.front();
    if (!source_filename.is_absolute()) {
        bool source_found{false};
        for (const fs::path& directory : data_directories) {
            std::error_code exists_error;
            const bool source_exists = std::filesystem::exists(directory / source_filename, exists_error);
            if (exists_error) return {};
            if (source_exists) {
                source_directory = directory;
                source_found = true;
                break;
            }
        }
        if (!source_found) return {};
    } else {
        source_directory = source_filename.parent_path();
    }
    const fs::path source_parent_directory{source_filename.parent_path()};
    const fs::path creation_directory = source_filename.is_absolute() ? source_parent_directory : data_directories.front();
    const fs::path absolute_source_path = source_directory / source_filename;
    const fs::path absolute_creation_base_path = creation_directory / source_filename;
    const std::wstring source_native = absolute_source_path.native();
    const std::wstring creation_base_native = absolute_creation_base_path.native();
    if (source_native.size() > MAX_WINDOWS_PATH_LENGTH || creation_base_native.size() > MAX_WINDOWS_PATH_LENGTH) return {};
    size_t suffix_budget = std::min(MAX_WINDOWS_PATH_LENGTH - source_native.size(),
                                    MAX_WINDOWS_PATH_LENGTH - creation_base_native.size());
    size_t fallback_name_budget = MAX_SAFE_WINDOWS_PATH_LENGTH;
    const std::vector<fs::path> candidate_directories = source_filename.is_absolute()
                                                             ? std::vector<fs::path>{source_parent_directory}
                                                             : data_directories;
    for (const fs::path& directory : candidate_directories) {
        const fs::path candidate_base_path = directory / source_filename;
        const std::wstring candidate_base_native = candidate_base_path.native();
        suffix_budget = std::min(suffix_budget,
                                 candidate_base_native.size() <= MAX_WINDOWS_PATH_LENGTH
                                     ? MAX_WINDOWS_PATH_LENGTH - candidate_base_native.size()
                                     : size_t{0});

        const fs::path candidate_parent_path = directory / source_parent_directory;
        const std::wstring candidate_parent_native = candidate_parent_path.native();
        const bool parent_has_separator = !candidate_parent_native.empty() && candidate_parent_native.back() != L'\\' &&
                                          candidate_parent_native.back() != L'/';
        const size_t parent_path_prefix_length = candidate_parent_native.size() + (parent_has_separator ? 1 : 0);
        fallback_name_budget = std::min(fallback_name_budget,
                                        parent_path_prefix_length < MAX_SAFE_WINDOWS_PATH_LENGTH
                                            ? MAX_SAFE_WINDOWS_PATH_LENGTH - parent_path_prefix_length
                                            : size_t{0});
    }
    const std::string rewrite_prefix{".rewrite-"};
    const size_t safe_suffix_budget = suffix_budget > rewrite_prefix.size() + WINDOWS_REWRITE_PATH_HEADROOM
                                          ? suffix_budget - rewrite_prefix.size() - WINDOWS_REWRITE_PATH_HEADROOM
                                          : 0;
    const bool can_use_wallet_suffix = safe_suffix_budget >= 16;
    const bool can_use_fallback_name = fallback_name_budget >= 16;

    const size_t random_hex_length = can_use_wallet_suffix
                                         ? std::min<size_t>(32, safe_suffix_budget)
                                         : can_use_fallback_name ? 16 : 0;
    if (random_hex_length == 0) return {};

    for (int attempt = 0; attempt < 32; ++attempt) {
        const std::string random_hex = GetRandHash().GetHex();
        const fs::path relative_candidate = can_use_wallet_suffix
                                                ? fs::PathFromString(fs::PathToString(source_filename) + rewrite_prefix + random_hex.substr(0, random_hex_length))
                                                : source_parent_directory / fs::PathFromString(random_hex.substr(0, random_hex_length));
        bool candidate_exists{false};
        for (const fs::path& directory : candidate_directories) {
            std::error_code exists_error;
            candidate_exists = std::filesystem::exists(directory / relative_candidate, exists_error);
            if (exists_error) return {};
            if (candidate_exists) break;
        }
        if (!candidate_exists) return fs::PathToString(relative_candidate);
    }
    return {};
#else
    return fs::PathToString(source_filename) + ".rewrite-" + GetRandHash().GetHex();
#endif
}

int ReadBerkeleyDbFileVersion(const fs::path& path)
{
    std::ifstream file{path, std::ios::binary};
    if (!file.is_open()) return -1;

    file.seekg(16);
    unsigned char version[4]{};
    file.read(reinterpret_cast<char*>(version), sizeof(version));
    if (file.gcount() != sizeof(version)) return -1;

    return static_cast<int>(version[0]) |
        (static_cast<int>(version[1]) << 8) |
        (static_cast<int>(version[2]) << 16) |
        (static_cast<int>(version[3]) << 24);
}

#ifdef WIN32
static constexpr int BDB62_DUMP_RESOURCE_ID = 401;
static constexpr int BDB48_LOAD_RESOURCE_ID = 402;
static constexpr int BDB62_DLL_RESOURCE_ID = 403;
static constexpr int BDB62_WINPTHREAD_RESOURCE_ID = 404;
static constexpr int BDB62_LOAD_RESOURCE_ID = 405;

std::wstring QuoteCommandArg(const std::wstring& arg)
{
    std::wstring quoted;
    quoted.reserve(arg.size() + 2);
    quoted.push_back(L'"');
    for (const wchar_t ch : arg) {
        if (ch == L'"') quoted.push_back(L'\\');
        quoted.push_back(ch);
    }
    quoted.push_back(L'"');
    return quoted;
}

bool WriteResourceFile(const int resource_id, const fs::path& path)
{
    HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(resource_id), MAKEINTRESOURCEW(10));
    if (resource == nullptr) return false;

    HGLOBAL resource_handle = LoadResource(nullptr, resource);
    if (resource_handle == nullptr) return false;

    const DWORD resource_size = SizeofResource(nullptr, resource);
    const void* resource_data = LockResource(resource_handle);
    if (resource_size == 0 || resource_data == nullptr) return false;

    try {
        fs::create_directories(path.parent_path());
    } catch (const std::exception&) {
        return false;
    }

    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    if (!file) return false;

    file.write(static_cast<const char*>(resource_data), resource_size);
    return file.good();
}

bool RunRecoveryTool(const fs::path& exe_path, const std::wstring& args, const fs::path& work_dir, int& exit_code)
{
    std::wstring command = QuoteCommandArg(exe_path.native()) + L" " + args;
    std::vector<wchar_t> command_buffer(command.begin(), command.end());
    command_buffer.push_back(L'\0');

    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    PROCESS_INFORMATION process_info{};

    const std::wstring work_dir_native = work_dir.native();
    if (!CreateProcessW(nullptr, command_buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
            work_dir_native.c_str(), &startup_info, &process_info)) {
        return false;
    }

    WaitForSingleObject(process_info.hProcess, INFINITE);
    DWORD process_exit_code = 1;
    const bool got_exit_code = GetExitCodeProcess(process_info.hProcess, &process_exit_code);
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    if (!got_exit_code) return false;

    exit_code = static_cast<int>(process_exit_code);
    return true;
}

fs::path UniquePathWithSuffix(const fs::path& base, const std::string& suffix)
{
    for (int i = 0; i < 1000; ++i) {
        fs::path candidate = i == 0 ? base / fs::PathFromString(suffix) : base / fs::PathFromString(strprintf("%s-%d", suffix, i));
        if (!fs::exists(candidate)) return candidate;
    }
    return base / fs::PathFromString(strprintf("%s-fallback", suffix));
}

bool BackupDirectory(const fs::path& source, const fs::path& dest)
{
    std::error_code error;
    fs::copy(source, dest, fs::copy_options::recursive | fs::copy_options::copy_symlinks, error);
    if (error) {
        LogPrintf("BerkeleyDatabase::Verify: failed to back up wallet directory %s to %s: %s\n",
            fs::PathToString(source), fs::PathToString(dest), error.message());
        return false;
    }
    return true;
}

void RemoveBerkeleyEnvFiles(const fs::path& wallet_dir)
{
    std::error_code error;
    fs::remove_all(wallet_dir / "database", error);
    fs::remove(wallet_dir / "db.log", error);
    if (!fs::exists(wallet_dir) || !fs::is_directory(wallet_dir)) return;

    for (const auto& entry : fs::directory_iterator(wallet_dir)) {
        const std::string filename = fs::PathToString(entry.path().filename());
        if (filename.rfind("__db.", 0) == 0) {
            fs::remove(entry.path(), error);
        }
    }
}

bool TryAutoConvertBerkeleyWallet(const fs::path& wallet_dir, const fs::path& wallet_file, const int load_resource_id,
    const char* load_tool_name, const char* target_label, const char* original_label, fs::path& backup_dir_out)
{
    if (!fs::exists(wallet_file)) return false;

    wchar_t temp_path_buffer[MAX_PATH];
    DWORD temp_path_len = GetTempPathW(MAX_PATH, temp_path_buffer);
    if (temp_path_len == 0 || temp_path_len >= MAX_PATH) return false;

    const fs::path temp_root = fs::path(std::wstring(temp_path_buffer, temp_path_len)) /
        fs::PathFromString(strprintf("KorshBdbRecovery-%d-%lu", GetTime(), static_cast<unsigned long>(GetCurrentProcessId())));
    TryCreateDirectories(temp_root);

    const fs::path db_dump62 = temp_root / "db_dump62.exe";
    const fs::path db_load = temp_root / load_tool_name;
    const fs::path libdb62 = temp_root / "libdb-6.2.dll";
    const fs::path libwinpthread = temp_root / "libwinpthread-1.dll";
    const fs::path dump_file = temp_root / "wallet-bdb.dump";
    const fs::path new_wallet_file = temp_root / fs::PathFromString(strprintf("wallet.dat.%s-new", target_label));

    if (!WriteResourceFile(BDB62_DUMP_RESOURCE_ID, db_dump62) ||
        !WriteResourceFile(load_resource_id, db_load) ||
        !WriteResourceFile(BDB62_DLL_RESOURCE_ID, libdb62) ||
        !WriteResourceFile(BDB62_WINPTHREAD_RESOURCE_ID, libwinpthread)) {
        LogPrintf("BerkeleyDatabase::Verify: BDB recovery resources are not embedded in this executable\n");
        std::error_code error;
        fs::remove_all(temp_root, error);
        return false;
    }

    int exit_code = 1;
    const std::wstring dump_args = L"-f " + QuoteCommandArg(dump_file.native()) + L" " + QuoteCommandArg(wallet_file.native());
    if (!RunRecoveryTool(db_dump62, dump_args, temp_root, exit_code) || exit_code != 0) {
        LogPrintf("BerkeleyDatabase::Verify: BDB 6.2 dump did not recover %s (exit code %d)\n",
            fs::PathToString(wallet_file), exit_code);
        std::error_code error;
        fs::remove_all(temp_root, error);
        return false;
    }

    const std::wstring load_args = L"-f " + QuoteCommandArg(dump_file.native()) + L" " + QuoteCommandArg(new_wallet_file.native());
    if (!RunRecoveryTool(db_load, load_args, temp_root, exit_code) || exit_code != 0 || !fs::exists(new_wallet_file)) {
        LogPrintf("BerkeleyDatabase::Verify: BDB %s load failed for %s (exit code %d)\n",
            target_label, fs::PathToString(wallet_file), exit_code);
        std::error_code error;
        fs::remove_all(temp_root, error);
        return false;
    }

    const int64_t now = GetTime();
    const fs::path parent_dir = wallet_dir.parent_path();
    const std::string backup_name = fs::PathToString(wallet_dir.filename()) + strprintf("-backup-auto-%s-%d", target_label, now);
    const fs::path backup_dir = UniquePathWithSuffix(parent_dir, backup_name);
    if (!BackupDirectory(wallet_dir, backup_dir)) {
        std::error_code error;
        fs::remove_all(temp_root, error);
        return false;
    }

    const fs::path original_wallet_file = UniquePathWithSuffix(wallet_dir, strprintf("wallet.dat.%s-original-%d", original_label, now));
    std::error_code error;
    fs::rename(wallet_file, original_wallet_file, error);
    if (error) {
        LogPrintf("BerkeleyDatabase::Verify: failed to move original wallet.dat to %s: %s\n",
            fs::PathToString(original_wallet_file), error.message());
        fs::remove_all(temp_root, error);
        return false;
    }

    fs::rename(new_wallet_file, wallet_file, error);
    if (error) {
        LogPrintf("BerkeleyDatabase::Verify: failed to install converted BDB %s wallet.dat: %s\n", target_label, error.message());
        fs::rename(original_wallet_file, wallet_file, error);
        fs::remove_all(temp_root, error);
        return false;
    }

    RemoveBerkeleyEnvFiles(wallet_dir);
    backup_dir_out = backup_dir;
    fs::remove_all(temp_root, error);
    LogPrintf("BerkeleyDatabase::Verify: auto-converted wallet.dat to Berkeley DB %s. Full backup: %s. Original wallet: %s\n",
        target_label, fs::PathToString(backup_dir), fs::PathToString(original_wallet_file));
    return true;
}

bool TryAutoConvertBerkeleyDb62Wallet(const fs::path& wallet_dir, const fs::path& wallet_file, fs::path& backup_dir_out)
{
    return TryAutoConvertBerkeleyWallet(wallet_dir, wallet_file, BDB48_LOAD_RESOURCE_ID, "db_load48.exe", "bdb48", "bdb62", backup_dir_out);
}

#if DB_VERSION_MAJOR >= 6
bool TryAutoConvertBerkeleyDb48Wallet(const fs::path& wallet_dir, const fs::path& wallet_file, fs::path& backup_dir_out)
{
    return TryAutoConvertBerkeleyWallet(wallet_dir, wallet_file, BDB62_LOAD_RESOURCE_ID, "db_load62.exe", "bdb62", "bdb48", backup_dir_out);
}
#endif
#endif

std::map<std::string, std::weak_ptr<BerkeleyEnvironment>> g_dbenvs GUARDED_BY(cs_db); //!< Map from directory name to db environment.
// Keep recovery failures sticky after an environment object is destroyed. The
// process-global directory lock is idempotent, so a new object could otherwise
// mistake the old object's retained lock for permission to retry BDB recovery.
std::mutex g_recovery_required_paths_mutex;
std::set<std::string> g_recovery_required_paths;

bool IsRecoveryRequiredPath(const std::string& path)
{
    if (path.empty()) return false;
    std::lock_guard<std::mutex> lock{g_recovery_required_paths_mutex};
    return g_recovery_required_paths.count(path) != 0;
}

void RememberRecoveryRequiredPath(const std::string& path)
{
    if (path.empty()) return;
    std::lock_guard<std::mutex> lock{g_recovery_required_paths_mutex};
    g_recovery_required_paths.insert(path);
}
} // namespace

bool WalletDatabaseFileId::operator==(const WalletDatabaseFileId& rhs) const
{
    return memcmp(value, &rhs.value, sizeof(value)) == 0;
}

/**
 * @param[in] env_directory Path to environment directory
 * @return A shared pointer to the BerkeleyEnvironment object for the wallet directory, or an empty pointer while a prior
 * environment for the same directory is still being destroyed.
 * @post A new BerkeleyEnvironment weak pointer is inserted into g_dbenvs if the directory path key was not already in the map.
 */
std::shared_ptr<BerkeleyEnvironment> GetBerkeleyEnv(const fs::path& env_directory, bool use_shared_memory)
{
    LOCK(cs_db);
    const fs::path canonical_directory = CanonicalWalletDirectory(env_directory);
    const std::string path_key = fs::PathToString(canonical_directory);
    auto existing = g_dbenvs.find(path_key);
    if (existing != g_dbenvs.end()) {
        if (auto env = existing->second.lock()) return env;
        // The last owner has started destruction but has not yet acquired
        // cs_db to close the old BDB handle and erase this entry. Fail closed;
        // replacing it here could open a second environment before teardown.
        return {};
    }
    auto env = std::make_shared<BerkeleyEnvironment>(canonical_directory, use_shared_memory);
    g_dbenvs.emplace(path_key, env);
    return env;
}

//
// BerkeleyBatch
//

void BerkeleyEnvironment::UnlockDirectoryLock()
{
    if (!m_directory_lock_held) return;
    UnlockDirectoryByKey(m_directory_lock_key);
    m_directory_lock_key.clear();
    m_directory_lock_held = false;
}

bool BerkeleyEnvironment::Close(bool preserve_files, bool preserve_directory_lock)
{
    LOCK(cs_db);
    std::unique_lock<std::shared_mutex> operation_lock{m_db_operation_mutex};
    if (!fDbEnvInit) {
        const bool clean = !preserve_files && !IsRecoveryRequired();
        if (clean && !preserve_directory_lock && m_directory_lock_held) {
            UnlockDirectoryLock();
        }
        return clean;
    }

    fDbEnvInit = false;
    bool success = true;

    for (auto& db : m_databases) {
        BerkeleyDatabase& database = db.second.get();
        assert(database.m_refcount <= 0);
        if (database.m_db) {
            const int ret = database.m_db->close(0);
            database.m_db.reset();
            if (ret != 0) {
                LogPrintf("BerkeleyEnvironment::Close: Error %d closing database %s: %s\n", ret, fs::PathToString(db.first), DbEnv::strerror(ret));
                success = false;
                MarkRecoveryRequired();
            }
        }
    }

    FILE* error_file = nullptr;
    dbenv->get_errfile(&error_file);

    int ret = dbenv->close(0);
    if (ret != 0) {
        LogPrintf("BerkeleyEnvironment::Close: Error %d closing database environment: %s\n", ret, DbEnv::strerror(ret));
        success = false;
        MarkRecoveryRequired();
    }
    const bool keep_files = preserve_files || IsRecoveryRequired();
    if (success && !keep_files && !fMockDb) {
        ret = DbEnv(uint32_t{0}).remove(strPath.c_str(), 0);
        if (ret != 0) {
            LogPrintf("BerkeleyEnvironment::Close: Error %d removing database environment: %s\n", ret, DbEnv::strerror(ret));
            success = false;
            MarkRecoveryRequired();
        }
    }

    if (error_file) fclose(error_file);

    if (!preserve_files && !preserve_directory_lock && success && !IsRecoveryRequired() && m_directory_lock_held) {
        UnlockDirectoryLock();
    }
    return success && !keep_files;
}

void BerkeleyEnvironment::Reset()
{
    dbenv.reset(new DbEnv(DB_CXX_NO_EXCEPTIONS));
    fDbEnvInit = false;
    fMockDb = false;
}

BerkeleyEnvironment::WalletMigrationResult BerkeleyEnvironment::ConvertWalletFile(const std::function<bool()>& converter, bilingual_str& open_error)
{
    // Keep database creation/opening out of the Close -> Reset -> convert -> Open
    // sequence. Otherwise another database sharing this environment can reopen
    // the old handle while the converter is replacing the wallet file.
    LOCK(cs_db);
    for (const auto& db : m_databases) {
        if (db.second.get().m_refcount.load() > 0) {
            LogPrintf("BerkeleyEnvironment::ConvertWalletFile: refusing conversion while database %s is in use\n", fs::PathToString(db.first));
            return WalletMigrationResult::DATABASE_IN_USE;
        }
    }
    if (!Close(/*preserve_files=*/false, /*preserve_directory_lock=*/true)) return WalletMigrationResult::CLOSE_FAILED;
    Reset();
    if (!converter()) {
        MarkRecoveryRequired();
        return WalletMigrationResult::CONVERSION_FAILED;
    }
    if (!Open(open_error, /*preserve_directory_lock_on_failure=*/true)) {
        MarkRecoveryRequired();
        return WalletMigrationResult::OPEN_FAILED;
    }
    return WalletMigrationResult::SUCCESS;
}

BerkeleyEnvironment::BerkeleyEnvironment(const fs::path& dir_path, bool use_shared_memory)
    : m_use_shared_memory(use_shared_memory)
{
    strPath = fs::PathToString(CanonicalWalletDirectory(dir_path));
    m_recovery_required = IsRecoveryRequiredPath(strPath);
    Reset();
}

BerkeleyEnvironment::~BerkeleyEnvironment()
{
    LOCK(cs_db);
    auto environment = g_dbenvs.find(strPath);
    if (environment != g_dbenvs.end() && environment->second.expired()) g_dbenvs.erase(environment);
    // Database owners keep the shared environment alive through their final
    // writes. Only the last owner's destruction may remove its logs and close it.
    Flush(true);
}

bool BerkeleyEnvironment::Open(bilingual_str& err, bool preserve_directory_lock_on_failure)
{
    LOCK(cs_db);
    if (IsRecoveryRequired()) {
        err = strprintf(_("Error initializing wallet database environment %s!"), fs::quoted(fs::PathToString(Directory()))) +
              Untranslated(" ") + _("Berkeley DB recovery previously failed. Wallet and transaction-log files were preserved; restart only after restoring a matching backup or recovering a copy.");
        return false;
    }
    if (fDbEnvInit) {
        return true;
    }

    fs::path pathIn = fs::PathFromString(strPath);
    TryCreateDirectories(pathIn);
    if (!LockDirectory(pathIn, ".walletlock", /*probe_only=*/false, &m_directory_lock_key)) {
        m_directory_lock_key.clear();
        m_directory_lock_held = false;
        LogPrintf("Cannot obtain a lock on wallet directory %s. Another instance may be using it.\n", strPath);
        err = strprintf(_("Error initializing wallet database environment %s!"), fs::quoted(fs::PathToString(Directory())));
        return false;
    }
    m_directory_lock_held = true;

    unsigned int nEnvFlags = 0;
    if (!m_use_shared_memory) {
        nEnvFlags |= DB_PRIVATE;
    }

    {
        fs::path pathLogDir = pathIn / "database";
        TryCreateDirectories(pathLogDir);
        fs::path pathErrorFile = pathIn / "db.log";
        LogPrintf("BerkeleyEnvironment::Open: LogDir=%s ErrorFile=%s\n", fs::PathToString(pathLogDir), fs::PathToString(pathErrorFile));

        dbenv->set_lg_dir(fs::PathToString(pathLogDir).c_str());
        dbenv->set_cachesize(0, 0x100000, 1); // 1 MiB should be enough for just the wallet
        dbenv->set_lg_bsize(0x10000);
        dbenv->set_lg_max(1048576);
        dbenv->set_lk_max_locks(40000);
        dbenv->set_lk_max_objects(40000);
        dbenv->set_errfile(fsbridge::fopen(pathErrorFile, "a")); /// debug
        dbenv->set_flags(DB_AUTO_COMMIT, 1);
        dbenv->set_flags(DB_TXN_WRITE_NOSYNC, 1);
        dbenv->log_set_config(DB_LOG_AUTO_REMOVE, 1);
        int ret = RunDatabaseOperation([&] {
            return dbenv->open(strPath.c_str(),
                               DB_CREATE |
                                   DB_INIT_LOCK |
                                   DB_INIT_LOG |
                                   DB_INIT_MPOOL |
                                   DB_INIT_TXN |
                                   DB_THREAD |
                                   DB_RECOVER |
                                   nEnvFlags,
                               S_IRUSR | S_IWUSR);
        });
        if (ret == 0) {
            fDbEnvInit = true;
            fMockDb = false;
            return true;
        }

        LogPrintf("BerkeleyEnvironment::Open: Error %d opening database environment: %s\n", ret, DbEnv::strerror(ret));
        FILE* error_file = nullptr;
        dbenv->get_errfile(&error_file);
        int ret2 = RunCleanupDatabaseOperation([&] { return dbenv->close(0); }, /*latch_on_any_error=*/true);
        if (ret2 != 0) {
            LogPrintf("BerkeleyEnvironment::Open: Error %d closing failed database environment: %s\n", ret2, DbEnv::strerror(ret2));
        }
        if (error_file) fclose(error_file);
        Reset();

        err = strprintf(_("Error initializing wallet database environment %s!"), fs::quoted(fs::PathToString(Directory())));
        if (ret == DB_RUNRECOVERY) {
            err += Untranslated(" ") + _("Berkeley DB requires recovery. Existing wallet and transaction-log files were preserved. Restore a matching backup or use korsh-wallet salvage on a copy; do not delete the database directory.");
        }
        if (!preserve_directory_lock_on_failure && ret2 == 0 && !IsRecoveryRequired()) {
            UnlockDirectoryLock();
        }
        return false;
    }
}

//! Construct an in-memory mock Berkeley environment for testing
BerkeleyEnvironment::BerkeleyEnvironment() : m_use_shared_memory(false)
{
    Reset();

    LogPrint(BCLog::WALLETDB, "BerkeleyEnvironment::MakeMock\n");

    dbenv->set_cachesize(1, 0, 1);
    dbenv->set_lg_bsize(10485760 * 4);
    dbenv->set_lg_max(10485760);
    dbenv->set_lk_max_locks(10000);
    dbenv->set_lk_max_objects(10000);
    dbenv->set_flags(DB_AUTO_COMMIT, 1);
    dbenv->log_set_config(DB_LOG_IN_MEMORY, 1);
    int ret = RunDatabaseOperation([&] {
        return dbenv->open(nullptr,
                           DB_CREATE |
                               DB_INIT_LOCK |
                               DB_INIT_LOG |
                               DB_INIT_MPOOL |
                               DB_INIT_TXN |
                               DB_THREAD |
                               DB_PRIVATE,
                           S_IRUSR | S_IWUSR);
    });
    if (ret > 0) {
        throw std::runtime_error(strprintf("BerkeleyEnvironment::MakeMock: Error %d opening database environment.", ret));
    }

    fDbEnvInit = true;
    fMockDb = true;
}

BerkeleyBatch::SafeDbt::SafeDbt()
{
    m_dbt.set_flags(DB_DBT_MALLOC);
}

BerkeleyBatch::SafeDbt::SafeDbt(void* data, size_t size)
    : m_dbt(data, size)
{
}

BerkeleyBatch::SafeDbt::~SafeDbt()
{
    if (m_dbt.get_data() != nullptr) {
        // Clear memory, e.g. in case it was a private key
        memory_cleanse(m_dbt.get_data(), m_dbt.get_size());
        // under DB_DBT_MALLOC, data is malloced by the Dbt, but must be
        // freed by the caller.
        // https://docs.oracle.com/cd/E17275_01/html/api_reference/C/dbt.html
        if (m_dbt.get_flags() & DB_DBT_MALLOC) {
            free(m_dbt.get_data());
        }
    }
}

const void* BerkeleyBatch::SafeDbt::get_data() const
{
    return m_dbt.get_data();
}

uint32_t BerkeleyBatch::SafeDbt::get_size() const
{
    return m_dbt.get_size();
}

BerkeleyBatch::SafeDbt::operator Dbt*()
{
    return &m_dbt;
}

bool BerkeleyDatabase::Verify(bilingual_str& errorStr)
{
    fs::path walletDir = env->Directory();
    fs::path file_path = walletDir / m_filename;

    LogPrintf("Using BerkeleyDB version %s\n", BerkeleyDatabaseVersion());
    LogPrintf("Using wallet %s\n", fs::PathToString(file_path));

    if (!env->Open(errorStr)) {
        return false;
    }

    if (fs::exists(file_path))
    {
        assert(m_refcount == 0);

        const int bdb_file_version = ReadBerkeleyDbFileVersion(file_path);
#if defined(WIN32) && DB_VERSION_MAJOR >= 6
        if (bdb_file_version > 0 && bdb_file_version < 10) {
            fs::path auto_migration_backup;
            bilingual_str open_error;
            const auto migration_result = env->ConvertWalletFile([&] {
                return TryAutoConvertBerkeleyDb48Wallet(walletDir, file_path, auto_migration_backup);
            }, open_error);
            if (migration_result == BerkeleyEnvironment::WalletMigrationResult::DATABASE_IN_USE) {
                errorStr = strprintf(_("%s could not be migrated safely because another wallet database operation is active. The original wallet and recovery logs were preserved."), fs::quoted(fs::PathToString(file_path)));
                return false;
            }
            if (migration_result == BerkeleyEnvironment::WalletMigrationResult::CLOSE_FAILED) {
                errorStr = strprintf(_("%s could not be safely closed for automatic migration. The original wallet and recovery logs were preserved."), fs::quoted(fs::PathToString(file_path)));
                return false;
            }
            if (migration_result == BerkeleyEnvironment::WalletMigrationResult::CONVERSION_FAILED) {
                errorStr = strprintf(_("%s could not be automatically migrated to Berkeley DB 6.2. The original wallet.dat was not replaced. Restore from backup or retry after closing all wallet processes."), fs::quoted(fs::PathToString(file_path)));
                return false;
            }
            if (migration_result == BerkeleyEnvironment::WalletMigrationResult::OPEN_FAILED) {
                errorStr = open_error;
                return false;
            }
        }
#endif

        const std::string strFile = fs::PathToString(m_filename);
        int result;
        {
            Db db(env->dbenv.get(), 0);
            result = env->RunDatabaseOperation([&] { return db.verify(strFile.c_str(), nullptr, nullptr, 0); });
        }
        if (result != 0) {
            const bool newer_bdb_file = bdb_file_version >= 10;
#ifdef WIN32
            if (newer_bdb_file) {
                fs::path auto_recovery_backup;
                bilingual_str open_error;
                const auto migration_result = env->ConvertWalletFile([&] {
                    return TryAutoConvertBerkeleyDb62Wallet(walletDir, file_path, auto_recovery_backup);
                }, open_error);
                if (migration_result == BerkeleyEnvironment::WalletMigrationResult::DATABASE_IN_USE ||
                    migration_result == BerkeleyEnvironment::WalletMigrationResult::CLOSE_FAILED) {
                    errorStr = strprintf(_("%s could not be safely closed for automatic recovery. The original wallet and recovery logs were preserved."), fs::quoted(fs::PathToString(file_path)));
                    return false;
                }
                if (migration_result == BerkeleyEnvironment::WalletMigrationResult::OPEN_FAILED) {
                    errorStr = open_error;
                    return false;
                }
                if (migration_result == BerkeleyEnvironment::WalletMigrationResult::SUCCESS) {
                    Db db(env->dbenv.get(), 0);
                    result = env->RunDatabaseOperation([&] { return db.verify(strFile.c_str(), nullptr, nullptr, 0); });
                    if (result == 0) {
                        return true;
                    }
                    LogPrintf("BerkeleyDatabase::Verify: converted wallet still failed verification after automatic recovery. Backup: %s\n",
                        fs::PathToString(auto_recovery_backup));
                }
            }
#endif
            if (newer_bdb_file) {
                errorStr = strprintf(_("%s was written by a newer Berkeley DB file format. Convert this wallet back to Berkeley DB 4.8 or restore a pre-upgrade backup before opening it with this release."), fs::quoted(fs::PathToString(file_path)));
            } else {
                errorStr = strprintf(_("%s corrupt. Try using the wallet tool korsh-wallet to salvage or restoring a backup."), fs::quoted(fs::PathToString(file_path)));
            }
            return false;
        }
    }
    // also return true if files does not exists
    return true;
}

bool BerkeleyEnvironment::CheckpointLSN(const std::string& strFile)
{
    LOCK(cs_db);
    if (IsRecoveryRequired()) {
        LogPrintf("BerkeleyEnvironment::CheckpointLSN: skipping checkpoint for %s because recovery is required\n", strFile);
        return false;
    }
    return RunExclusiveDatabaseOperation([&] {
        int ret = dbenv->txn_checkpoint(0, 0, 0);
        if (ret != 0) {
            LogPrintf("BerkeleyEnvironment::CheckpointLSN: Error %d checkpointing database %s: %s\n", ret, strFile, DbEnv::strerror(ret));
            MarkRecoveryRequired();
            return false;
        }
        if (fMockDb) return true;
        ret = dbenv->lsn_reset(strFile.c_str(), 0);
        if (ret != 0) {
            LogPrintf("BerkeleyEnvironment::CheckpointLSN: Error %d resetting LSN for database %s: %s\n", ret, strFile, DbEnv::strerror(ret));
            MarkRecoveryRequired();
            return false;
        }
        return true;
    });
}

BerkeleyDatabase::~BerkeleyDatabase()
{
    if (env) {
        LOCK(cs_db);
        env->CloseDb(m_filename);
        assert(!m_db);
        size_t erased = env->m_databases.erase(m_filename);
        assert(erased == 1);
        env->m_fileids.erase(fs::PathToString(m_filename));
    }
}

BerkeleyBatch::BerkeleyBatch(BerkeleyDatabase& database, const bool read_only, bool fFlushOnCloseIn) : m_database(database)
{
    database.AddRef();
    try {
        database.Open();
    } catch (...) {
        database.RemoveRef();
        throw;
    }
    fReadOnly = read_only;
    fFlushOnClose = fFlushOnCloseIn;
    env = database.env.get();
    pdb = database.m_db.get();
    strFile = fs::PathToString(database.m_filename);
}

void BerkeleyDatabase::Open()
{
    unsigned int nFlags = DB_THREAD | DB_CREATE;

    {
        LOCK(cs_db);
        bilingual_str open_err;
        if (!env->Open(open_err))
            throw std::runtime_error("BerkeleyDatabase: Failed to open database environment.");

        if (m_db == nullptr) {
            int ret;
            std::unique_ptr<Db> pdb_temp = std::make_unique<Db>(env->dbenv.get(), 0);
            const std::string strFile = fs::PathToString(m_filename);

            bool fMockDb = env->IsMock();
            if (fMockDb) {
                DbMpoolFile* mpf = pdb_temp->get_mpf();
                ret = mpf->set_flags(DB_MPOOL_NOFILE, 1);
                if (ret != 0) {
                    throw std::runtime_error(strprintf("BerkeleyDatabase: Failed to configure for no temp file backing for database %s", strFile));
                }
            }

            ret = env->RunDatabaseOperation([&] {
                return pdb_temp->open(nullptr,                        // Txn pointer
                                       fMockDb ? nullptr : strFile.c_str(), // Filename
                                       fMockDb ? strFile.c_str() : "main", // Logical db name
                                       DB_BTREE,                    // Database type
                                       nFlags,                      // Flags
                                       0);
            });

            if (ret != 0) {
                throw std::runtime_error(strprintf("BerkeleyDatabase: Error %d, can't open database %s", ret, strFile));
            }

            // Call CheckUniqueFileid on the containing BDB environment to
            // avoid BDB data consistency bugs that happen when different data
            // files in the same environment have the same fileid.
            CheckUniqueFileid(*env, strFile, *pdb_temp, this->env->m_fileids[strFile]);

            m_db.reset(pdb_temp.release());

        }
    }
}

void BerkeleyBatch::Flush()
{
    if (activeTxn)
        return;


    // Flush database activity from memory pool to disk log
    unsigned int nMinutes = 0;
    if (fReadOnly)
        nMinutes = 1;

    if (env) { // env is nullptr for dummy databases (i.e. in tests). Don't actually flush if env is nullptr so we don't segfault
        const int ret = env->RunDatabaseOperation([&] {
            return env->dbenv->txn_checkpoint(nMinutes ? m_database.m_max_log_mb * 1024 : 0, nMinutes, 0);
        }, /*latch_on_any_error=*/true);
        if (ret != 0) {
            LogPrintf("BerkeleyBatch::Flush: Error %d checkpointing database %s: %s\n", ret, strFile, DbEnv::strerror(ret));
        }
    }
}

void BerkeleyDatabase::IncrementUpdateCounter()
{
    ++nUpdateCounter;
}

BerkeleyBatch::~BerkeleyBatch()
{
    Close();
    m_database.RemoveRef();
}

void BerkeleyBatch::Close()
{
    if (!pdb)
        return;
    if (activeTxn) {
        const int ret = env->RunCleanupDatabaseOperation([&] { return activeTxn->abort(); }, /*latch_on_any_error=*/true);
        if (ret != 0) {
            LogPrintf("BerkeleyBatch::Close: Error %d aborting transaction for %s: %s\n", ret, strFile, DbEnv::strerror(ret));
        }
    }
    activeTxn = nullptr;
    pdb = nullptr;
    if (!CloseCursor()) {
        LogPrintf("BerkeleyBatch::Close: failed to close cursor for %s\n", strFile);
    }

    if (fFlushOnClose)
        Flush();
}

bool BerkeleyEnvironment::CloseDb(const fs::path& filename)
{
    bool success = true;
    {
        LOCK(cs_db);
        std::unique_lock<std::shared_mutex> operation_lock{m_db_operation_mutex};
        auto it = m_databases.find(filename);
        assert(it != m_databases.end());
        BerkeleyDatabase& database = it->second.get();
        if (database.m_db) {
            // Close the database handle
            const int ret = database.m_db->close(0);
            database.m_db.reset();
            if (ret != 0) {
                LogPrintf("BerkeleyEnvironment::CloseDb: Error %d closing database %s: %s\n", ret, fs::PathToString(filename), DbEnv::strerror(ret));
                success = false;
                MarkRecoveryRequired();
            }
        }
    }
    return success;
}

void BerkeleyEnvironment::MarkRecoveryRequired()
{
    m_recovery_required.store(true, std::memory_order_release);
    RememberRecoveryRequiredPath(strPath);
}

bool BerkeleyEnvironment::IsRecoveryRequired() const
{
    return m_recovery_required.load(std::memory_order_acquire);
}

bool BerkeleyEnvironment::IsDirectoryLockHeld() const
{
    LOCK(cs_db);
    return m_directory_lock_held;
}

void BerkeleyEnvironment::ReloadDbEnv()
{
    // Make sure that no Db's are in use
    AssertLockNotHeld(cs_db);
    std::unique_lock<RecursiveMutex> lock(cs_db);
    m_db_in_use.wait(lock, [this](){
        for (auto& db : m_databases) {
            if (db.second.get().m_refcount > 0) return false;
        }
        return true;
    });

    std::vector<fs::path> filenames;
    for (const auto& it : m_databases) {
        filenames.push_back(it.first);
    }
    // Close the individual Db's
    for (const fs::path& filename : filenames) {
        if (!CloseDb(filename)) {
            LogPrintf("BerkeleyEnvironment::ReloadDbEnv: failed to close database %s; preserving the environment\n", fs::PathToString(filename));
            return;
        }
    }
    // Reset the environment
    if (!Flush(true, /*preserve_directory_lock=*/true)) {
        LogPrintf("BerkeleyEnvironment::ReloadDbEnv: failed to flush database environment; preserving it\n");
        MarkRecoveryRequired();
        return;
    }
    Reset();
    bilingual_str open_err;
    if (!Open(open_err, /*preserve_directory_lock_on_failure=*/true)) {
        MarkRecoveryRequired();
        LogPrintf("BerkeleyEnvironment::ReloadDbEnv: %s\n", open_err.original);
    }
}

bool BerkeleyDatabase::Rewrite(const char* pszSkip)
{
    while (true) {
        {
            LOCK(cs_db);
            if (env->IsRecoveryRequired()) {
                LogPrintf("BerkeleyBatch::Rewrite: recovery is required; preserving %s\n", fs::PathToString(m_filename));
                return false;
            }
            const std::string strFile = fs::PathToString(m_filename);
            if (m_refcount <= 0) {
                const std::string strFileRes = MakeRewriteTemporaryFilename(*env, m_filename);
                if (strFileRes.empty()) {
                    LogPrintf("BerkeleyBatch::Rewrite: unable to choose a safe temporary filename for %s; preserving the original database\n", strFile);
                    return false;
                }
                // Flush log data to the dat file
                if (!env->CloseDb(m_filename) || !env->CheckpointLSN(strFile)) {
                    LogPrintf("BerkeleyBatch::Rewrite: unable to checkpoint %s; preserving the original database\n", strFile);
                    return false;
                }
                m_refcount = -1;

                bool fSuccess = true;
                LogPrintf("BerkeleyBatch::Rewrite: Rewriting %s...\n", strFile);
                {
                    BerkeleyBatch db(*this, true);
                    std::unique_ptr<Db> pdbCopy = std::make_unique<Db>(env->dbenv.get(), 0);

                    int ret = env->RunDatabaseOperation([&] {
                        return pdbCopy->open(nullptr,               // Txn pointer
                                             strFileRes.c_str(), // Filename
                                             "main",             // Logical db name
                                             DB_BTREE,           // Database type
                                             DB_CREATE | DB_EXCL | DB_AUTO_COMMIT,
                                             0);
                    });
                    if (ret != 0) {
                        LogPrintf("BerkeleyBatch::Rewrite: Can't create database file %s: %s\n", strFileRes, DbEnv::strerror(ret));
                        fSuccess = false;
                    }

                    if (fSuccess && !db.StartCursor()) fSuccess = false;
                    while (fSuccess) {
                        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
                        CDataStream ssValue(SER_DISK, CLIENT_VERSION);
                        bool complete;
                        const bool ret1 = db.ReadAtCursor(ssKey, ssValue, complete);
                        if (complete) break;
                        if (!ret1) {
                            fSuccess = false;
                            break;
                        }
                        if (pszSkip &&
                            strncmp((const char*)ssKey.data(), pszSkip, std::min(ssKey.size(), strlen(pszSkip))) == 0) {
                            continue;
                        }
                        if (strncmp((const char*)ssKey.data(), "\x07version", 8) == 0) {
                            ssValue.clear();
                            ssValue << CLIENT_VERSION;
                        }
                        Dbt datKey(ssKey.data(), ssKey.size());
                        Dbt datValue(ssValue.data(), ssValue.size());
                        const int put_ret = env->RunDatabaseOperation([&] {
                            return pdbCopy->put(nullptr, &datKey, &datValue, DB_NOOVERWRITE);
                        });
                        if (put_ret != 0) {
                            LogPrintf("BerkeleyBatch::Rewrite: Error %d copying record to %s: %s\n", put_ret, strFileRes, DbEnv::strerror(put_ret));
                            fSuccess = false;
                        }
                    }
                    if (!db.CloseCursor()) fSuccess = false;
                    if (env->IsRecoveryRequired()) fSuccess = false;
                    db.Close();
                    if (env->IsRecoveryRequired()) fSuccess = false;
                    if (!env->CloseDb(m_filename)) fSuccess = false;
                    const int close_ret = env->RunCleanupDatabaseOperation([&] { return pdbCopy->close(0); }, /*latch_on_any_error=*/true);
                    if (close_ret != 0) {
                        LogPrintf("BerkeleyBatch::Rewrite: Error %d closing temporary database %s: %s\n", close_ret, strFileRes, DbEnv::strerror(close_ret));
                        fSuccess = false;
                    }
                }

                if (fSuccess && !env->CheckpointLSN(strFileRes)) {
                    LogPrintf("BerkeleyBatch::Rewrite: unable to checkpoint temporary database %s; preserving the original\n", strFileRes);
                    fSuccess = false;
                }
                if (fSuccess) {
                    fSuccess = env->RunExclusiveDatabaseOperation([&] {
                        DbTxn* replacement = nullptr;
                        int ret = env->dbenv->txn_begin(nullptr, &replacement, DB_TXN_WRITE_NOSYNC);
                        if (ret != 0 || !replacement) {
                            if (ret == DB_RUNRECOVERY) env->MarkRecoveryRequired();
                            LogPrintf("BerkeleyBatch::Rewrite: Error %d beginning wallet replacement transaction: %s\n", ret, DbEnv::strerror(ret));
                            if (replacement) {
                                const int abort_ret = replacement->abort();
                                if (abort_ret != 0) {
                                    env->MarkRecoveryRequired();
                                    LogPrintf("BerkeleyBatch::Rewrite: Error %d aborting wallet replacement: %s\n", abort_ret, DbEnv::strerror(abort_ret));
                                }
                            }
                            return false;
                        }

                        ret = env->dbenv->dbremove(replacement, strFile.c_str(), nullptr, 0);
                        if (ret == 0) {
                            ret = env->dbenv->dbrename(replacement, strFileRes.c_str(), nullptr, strFile.c_str(), 0);
                        }
                        if (ret != 0) {
                            if (ret == DB_RUNRECOVERY) env->MarkRecoveryRequired();
                            LogPrintf("BerkeleyBatch::Rewrite: Error %d replacing %s: %s\n", ret, strFile, DbEnv::strerror(ret));
                            const int abort_ret = replacement->abort();
                            if (abort_ret != 0) {
                                env->MarkRecoveryRequired();
                                LogPrintf("BerkeleyBatch::Rewrite: Error %d aborting wallet replacement: %s\n", abort_ret, DbEnv::strerror(abort_ret));
                            }
                            return false;
                        }

                        const int commit_ret = replacement->commit(DB_TXN_SYNC);
                        if (commit_ret != 0) {
                            env->MarkRecoveryRequired();
                            LogPrintf("BerkeleyBatch::Rewrite: Error %d committing wallet replacement: %s\n", commit_ret, DbEnv::strerror(commit_ret));
                            return false;
                        }
                        env->m_fileids.erase(strFile);
                        return true;
                    });
                }
                if (!fSuccess)
                    LogPrintf("BerkeleyBatch::Rewrite: Failed to rewrite database file %s\n", strFileRes);
                return fSuccess;
            }
        }
        UninterruptibleSleep(std::chrono::milliseconds{100});
    }
}


bool BerkeleyEnvironment::Flush(bool fShutdown, bool preserve_directory_lock)
{
    const auto start{SteadyClock::now()};
    // Flush log data to the actual data file on all files that are not in use
    LogPrint(BCLog::WALLETDB, "BerkeleyEnvironment::Flush: [%s] Flush(%s)%s\n", strPath, fShutdown ? "true" : "false", fDbEnvInit ? "" : " database not started");
    {
        LOCK(cs_db);
        const auto databases_are_idle = [this] {
            for (const auto& entry : m_databases) {
                if (entry.second.get().m_refcount > 0) return false;
            }
            return true;
        };
        if (IsRecoveryRequired()) {
            if (fShutdown && fDbEnvInit) {
                if (databases_are_idle()) {
                    LogPrintf("BerkeleyEnvironment::Flush: closing failed environment while preserving wallet recovery files\n");
                    Close(/*preserve_files=*/true, /*preserve_directory_lock=*/true);
                } else {
                    LogPrintf("BerkeleyEnvironment::Flush: database still in use; deferring close and preserving wallet recovery files\n");
                }
            }
            return false;
        }
        if (!fDbEnvInit) {
            if (fShutdown && !preserve_directory_lock && !IsRecoveryRequired() && m_directory_lock_held) {
                UnlockDirectoryLock();
            }
            return true;
        }
        bool no_dbs_accessed = true;
        bool success = true;
        for (auto& db_it : m_databases) {
            const fs::path& filename = db_it.first;
            BerkeleyDatabase& database = db_it.second.get();
            int nRefCount = database.m_refcount;
            if (nRefCount < 0) continue;
            const std::string strFile = fs::PathToString(filename);
            LogPrint(BCLog::WALLETDB, "BerkeleyEnvironment::Flush: Flushing %s (refcount = %d)...\n", strFile, nRefCount);
            if (nRefCount == 0) {
                // Move log data to the dat file
                if (!CloseDb(filename)) {
                    success = false;
                    no_dbs_accessed = false;
                    continue;
                }
                LogPrint(BCLog::WALLETDB, "BerkeleyEnvironment::Flush: %s checkpoint\n", strFile);
                if (!CheckpointLSN(strFile)) {
                    success = false;
                    no_dbs_accessed = false;
                    continue;
                }
                database.m_refcount = -1;
                LogPrint(BCLog::WALLETDB, "BerkeleyEnvironment::Flush: %s detach\n", strFile);
                LogPrint(BCLog::WALLETDB, "BerkeleyEnvironment::Flush: %s closed\n", strFile);
            } else {
                no_dbs_accessed = false;
            }
        }
        LogPrint(BCLog::WALLETDB, "BerkeleyEnvironment::Flush: Flush(%s)%s took %15dms\n", fShutdown ? "true" : "false", fDbEnvInit ? "" : " database not started", Ticks<std::chrono::milliseconds>(SteadyClock::now() - start));
        if (!success) {
            MarkRecoveryRequired();
            if (fShutdown && databases_are_idle()) {
                Close(/*preserve_files=*/true, /*preserve_directory_lock=*/true);
            }
            return false;
        }
        if (fShutdown) {
            if (!no_dbs_accessed) {
                return false;
            }
            if (!Close(/*preserve_files=*/false, /*preserve_directory_lock=*/true)) {
                return false;
            }
            if (!fMockDb) {
                std::error_code error;
                fs::remove_all(fs::PathFromString(strPath) / "database", error);
                if (error) {
                    LogPrintf("BerkeleyEnvironment::Flush: failed to remove database log directory %s: %s\n", strPath, error.message());
                    MarkRecoveryRequired();
                    return false;
                }
            }
            if (!preserve_directory_lock && m_directory_lock_held) {
                UnlockDirectoryLock();
            }
        }
    }
    return true;
}

bool BerkeleyDatabase::PeriodicFlush()
{
    // Don't flush if we can't acquire the lock.
    TRY_LOCK(cs_db, lockDb);
    if (!lockDb) return false;

    // Don't flush if any databases are in use
    for (auto& it : env->m_databases) {
        if (it.second.get().m_refcount > 0) return false;
    }

    // Don't flush if there haven't been any batch writes for this database.
    if (m_refcount < 0) return false;

    const std::string strFile = fs::PathToString(m_filename);
    LogPrint(BCLog::WALLETDB, "Flushing %s\n", strFile);
    const auto start{SteadyClock::now()};

    // Flush wallet file so it's self contained
    if (!env->CloseDb(m_filename) || !env->CheckpointLSN(strFile))
        return false;
    m_refcount = -1;

    LogPrint(BCLog::WALLETDB, "Flushed %s %dms\n", strFile, Ticks<std::chrono::milliseconds>(SteadyClock::now() - start));

    return true;
}

bool BerkeleyDatabase::Backup(const std::string& strDest) const
{
    const std::string strFile = fs::PathToString(m_filename);
    while (true)
    {
        {
            LOCK(cs_db);
            if (m_refcount <= 0)
            {
                // Flush log data to the dat file
                if (!env->CloseDb(m_filename) || !env->CheckpointLSN(strFile))
                    return false;

                // Copy wallet file
                fs::path pathSrc = env->Directory() / m_filename;
                fs::path pathDest(fs::PathFromString(strDest));
                if (fs::is_directory(pathDest))
                    pathDest /= m_filename;

                try {
                    if (fs::exists(pathDest) && fs::equivalent(pathSrc, pathDest)) {
                        LogPrintf("cannot backup to wallet source file %s\n", fs::PathToString(pathDest));
                        return false;
                    }

                    fs::copy_file(pathSrc, pathDest, fs::copy_options::overwrite_existing);
                    LogPrintf("copied %s to %s\n", strFile, fs::PathToString(pathDest));
                    return true;
                } catch (const fs::filesystem_error& e) {
                    LogPrintf("error copying %s to %s - %s\n", strFile, fs::PathToString(pathDest), fsbridge::get_filesystem_error_message(e));
                    return false;
                }
            }
        }
        UninterruptibleSleep(std::chrono::milliseconds{100});
    }
}

void BerkeleyDatabase::Flush()
{
    if (!env->Flush(false)) {
        throw std::runtime_error(strprintf("BerkeleyDatabase: failed to flush wallet database %s; recovery logs were preserved", fs::PathToString(m_filename)));
    }
}

void BerkeleyDatabase::Close()
{
    // A zero batch refcount does not mean the other database owners are done
    // with this environment (for example, StopWallets still writes locators).
    // Checkpoint and detach idle databases, but leave environment teardown to
    // its shared ownership lifetime.
    if (!env->Flush(false)) {
        LogPrintf("BerkeleyDatabase::Close: failed to checkpoint wallet database %s; recovery logs were preserved\n", fs::PathToString(m_filename));
    }
}

void BerkeleyDatabase::ReloadDbEnv()
{
    env->ReloadDbEnv();
}

bool BerkeleyBatch::StartCursor()
{
    assert(!m_cursor);
    if (!pdb) return false;
    const int ret = env->RunDatabaseOperation([&] { return pdb->cursor(nullptr, &m_cursor, 0); });
    return ret == 0;
}

bool BerkeleyBatch::ReadAtCursor(CDataStream& ssKey, CDataStream& ssValue, bool& complete)
{
    complete = false;
    if (m_cursor == nullptr) return false;
    // Read at cursor
    SafeDbt datKey;
    SafeDbt datValue;
    const int ret = env->RunDatabaseOperation([&] { return m_cursor->get(datKey, datValue, DB_NEXT); });
    if (ret == DB_NOTFOUND) {
        complete = true;
    }
    if (ret != 0)
        return false;
    else if (datKey.get_data() == nullptr || datValue.get_data() == nullptr)
        return false;

    // Convert to streams
    ssKey.SetType(SER_DISK);
    ssKey.clear();
    ssKey.write(SpanFromDbt(datKey));
    ssValue.SetType(SER_DISK);
    ssValue.clear();
    ssValue.write(SpanFromDbt(datValue));
    return true;
}

bool BerkeleyBatch::CloseCursor()
{
    if (!m_cursor) return true;
    Dbc* cursor = m_cursor;
    const int ret = env->RunCleanupDatabaseOperation([&] { return cursor->close(); }, /*latch_on_any_error=*/true);
    m_cursor = nullptr;
    if (ret != 0) {
        LogPrintf("BerkeleyBatch::CloseCursor: Error %d closing cursor for %s: %s\n", ret, strFile, DbEnv::strerror(ret));
    }
    return ret == 0;
}

bool BerkeleyBatch::TxnBegin()
{
    if (!pdb || activeTxn) return false;
    DbTxn* ptxn = env->TxnBegin();
    if (!ptxn)
        return false;
    activeTxn = ptxn;
    return true;
}

bool BerkeleyBatch::TxnCommit()
{
    if (!pdb || !activeTxn) return false;
    bool operation_ran = false;
    const int ret = env->RunDatabaseOperation([&] { return activeTxn->commit(0); }, /*latch_on_any_error=*/false, &operation_ran);
    if (!operation_ran) return false;
    activeTxn = nullptr;
    return (ret == 0);
}

bool BerkeleyBatch::TxnAbort()
{
    if (!pdb || !activeTxn)
        return false;
    const int ret = env->RunCleanupDatabaseOperation([&] { return activeTxn->abort(); }, /*latch_on_any_error=*/true);
    activeTxn = nullptr;
    return (ret == 0);
}

bool BerkeleyDatabaseSanityCheck()
{
    int major, minor;
    DbEnv::version(&major, &minor, nullptr);

    /* If the major version differs, or the minor version of library is *older*
     * than the header that was compiled against, flag an error.
     */
    if (major != DB_VERSION_MAJOR || minor < DB_VERSION_MINOR) {
        LogPrintf("BerkeleyDB database version conflict: header version is %d.%d, library version is %d.%d\n",
            DB_VERSION_MAJOR, DB_VERSION_MINOR, major, minor);
        return false;
    }

    return true;
}

std::string BerkeleyDatabaseVersion()
{
    return DbEnv::version(nullptr, nullptr, nullptr);
}

bool BerkeleyBatch::ReadKey(CDataStream&& key, CDataStream& value)
{
    if (!pdb) return false;

    SafeDbt datKey(key.data(), key.size());

    SafeDbt datValue;
    const int ret = env->RunDatabaseOperation([&] { return pdb->get(activeTxn, datKey, datValue, 0); });
    if (ret == 0 && datValue.get_data() != nullptr) {
        value.clear();
        value.write(SpanFromDbt(datValue));
        return true;
    }
    return false;
}

bool BerkeleyBatch::WriteKey(CDataStream&& key, CDataStream&& value, bool overwrite)
{
    if (!pdb) return false;
    if (fReadOnly)
        assert(!"Write called on database in read-only mode");

    SafeDbt datKey(key.data(), key.size());

    SafeDbt datValue(value.data(), value.size());

    const int ret = env->RunDatabaseOperation([&] { return pdb->put(activeTxn, datKey, datValue, (overwrite ? 0 : DB_NOOVERWRITE)); });
    return (ret == 0);
}

bool BerkeleyBatch::EraseKey(CDataStream&& key)
{
    if (!pdb) return false;
    if (fReadOnly)
        assert(!"Erase called on database in read-only mode");

    SafeDbt datKey(key.data(), key.size());

    const int ret = env->RunDatabaseOperation([&] { return pdb->del(activeTxn, datKey, 0); });
    return (ret == 0 || ret == DB_NOTFOUND);
}

bool BerkeleyBatch::HasKey(CDataStream&& key)
{
    if (!pdb) return false;

    SafeDbt datKey(key.data(), key.size());

    const int ret = env->RunDatabaseOperation([&] { return pdb->exists(activeTxn, datKey, 0); });
    return ret == 0;
}

void BerkeleyDatabase::AddRef()
{
    LOCK(cs_db);
    if (m_refcount < 0) {
        m_refcount = 1;
    } else {
        m_refcount++;
    }
}

void BerkeleyDatabase::RemoveRef()
{
    LOCK(cs_db);
    m_refcount--;
    if (env) env->m_db_in_use.notify_all();
}

std::unique_ptr<DatabaseBatch> BerkeleyDatabase::MakeBatch(bool flush_on_close)
{
    return std::make_unique<BerkeleyBatch>(*this, false, flush_on_close);
}

std::unique_ptr<BerkeleyDatabase> MakeBerkeleyDatabase(const fs::path& path, const DatabaseOptions& options, DatabaseStatus& status, bilingual_str& error)
{
    fs::path data_file = BDBDataFile(path);
    std::unique_ptr<BerkeleyDatabase> db;
    {
        LOCK(cs_db); // Lock env.m_databases until insert in BerkeleyDatabase constructor
        fs::path data_filename = data_file.filename();
        std::shared_ptr<BerkeleyEnvironment> env;
        try {
            env = GetBerkeleyEnv(data_file.parent_path(), options.use_shared_memory);
        } catch (const fs::filesystem_error& e) {
            error = Untranslated(strprintf("Failed to resolve Berkeley DB wallet directory '%s': %s",
                fs::PathToString(data_file.parent_path()), fsbridge::get_filesystem_error_message(e)));
            status = DatabaseStatus::FAILED_BAD_PATH;
            return nullptr;
        }
        if (!env) {
            error = Untranslated("The previous Berkeley DB environment is still shutting down; retry wallet loading.");
            status = DatabaseStatus::FAILED_CREATE;
            return nullptr;
        }
        if (env->m_databases.count(data_filename)) {
            error = Untranslated(strprintf("Refusing to load database. Data file '%s' is already loaded.", fs::PathToString(env->Directory() / data_filename)));
            status = DatabaseStatus::FAILED_ALREADY_LOADED;
            return nullptr;
        }
        db = std::make_unique<BerkeleyDatabase>(std::move(env), std::move(data_filename), options);
    }

    if (options.verify && !db->Verify(error)) {
        status = DatabaseStatus::FAILED_VERIFY;
        return nullptr;
    }

    status = DatabaseStatus::SUCCESS;
    return db;
}
} // namespace wallet
