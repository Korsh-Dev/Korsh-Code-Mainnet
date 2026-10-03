// Copyright (c) 2026 The Korsh Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef BITCOIN_QT_MASTERNODEWIZARDCONFIG_H
#define BITCOIN_QT_MASTERNODEWIZARDCONFIG_H

#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QSaveFile>
#include <QString>
#include <QTemporaryDir>

#include <filesystem>
#ifdef Q_OS_WIN
#include <QDir>
#include <QUuid>
#include <windows.h>
#include <aclapi.h>
#include <vector>
#endif

namespace MasternodeWizardConfig {
#ifdef Q_OS_WIN
// Qt's Windows setPermissions does not establish a private NTFS DACL.
// New objects must be private at creation (hardening after writing is too late).
// Only TokenUser is granted access. SYSTEM/administrators retain their OS
// privilege powers, but are not ordinary DACL readers. Existing BA-owned files
// are accepted only when BA is this process token's default owner (elevation).
namespace WindowsPrivate {
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { close(); }
    void close() { if (value != INVALID_HANDLE_VALUE && value != nullptr) CloseHandle(value); value = INVALID_HANDLE_VALUE; }
    bool valid() const { return value != INVALID_HANDLE_VALUE && value != nullptr; }
};
inline bool Fail(QString& error, const char* operation, DWORD code = GetLastError())
{
    error = QStringLiteral("Windows private-file security failed (%1, error %2). No new secret was published.")
                .arg(QString::fromLatin1(operation)).arg(code);
    return false;
}
struct Security {
    std::vector<unsigned char> user, owner, acl;
    SECURITY_DESCRIPTOR descriptor{};
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), &descriptor, FALSE};
    PSID sid() const { return reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid; }
    bool init(QString& error)
    {
        Handle thread;
        if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &thread.value) || GetLastError() != ERROR_NO_TOKEN)
            return Fail(error, "impersonation is unsupported", ERROR_NOT_SUPPORTED);
        Handle token;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)) return Fail(error, "process token");
        auto get = [&](TOKEN_INFORMATION_CLASS type, std::vector<unsigned char>& data) {
            DWORD size = 0;
            GetTokenInformation(token.value, type, nullptr, 0, &size);
            if (!size) return false;
            data.resize(size);
            return GetTokenInformation(token.value, type, data.data(), size, &size) != FALSE;
        };
        if (!get(TokenUser, user) || !get(TokenOwner, owner) || !IsValidSid(sid())) return Fail(error, "token identity");
        acl.resize(sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) + GetLengthSid(sid()));
        auto* dacl = reinterpret_cast<PACL>(acl.data());
        if (!InitializeAcl(dacl, static_cast<DWORD>(acl.size()), ACL_REVISION) ||
            !AddAccessAllowedAceEx(dacl, ACL_REVISION, 0, FILE_ALL_ACCESS, sid()) ||
            !InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
            !SetSecurityDescriptorOwner(&descriptor, sid(), FALSE) ||
            !SetSecurityDescriptorDacl(&descriptor, TRUE, dacl, FALSE) ||
            !SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED))
            return Fail(error, "private security descriptor");
        return true;
    }
    bool trustedOwner(PSID actual) const
    {
        if (!actual || !IsValidSid(actual)) return false;
        if (EqualSid(actual, sid())) return true;
        unsigned char admins[SECURITY_MAX_SID_SIZE]; DWORD size = sizeof(admins);
        return CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admins, &size) &&
            EqualSid(actual, admins) && EqualSid(actual, reinterpret_cast<const TOKEN_OWNER*>(owner.data())->Owner);
    }
    bool inspect(HANDLE file, bool directory, bool harden, QString& error)
    {
        BY_HANDLE_FILE_INFORMATION info{};
        DWORD flags = 0; wchar_t fs[32]{};
        if (GetFileType(file) != FILE_TYPE_DISK || !GetFileInformationByHandle(file, &info) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            bool(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory || (!directory && info.nNumberOfLinks != 1))
            return Fail(error, "non-regular, reparse, or multiply-linked object", ERROR_NOT_SUPPORTED);
        if (!GetVolumeInformationByHandleW(file, nullptr, 0, nullptr, nullptr, &flags, fs, 32) ||
            !(flags & FILE_PERSISTENT_ACLS) || wcscmp(fs, L"NTFS") != 0)
            return Fail(error, "requires local NTFS with persistent ACLs", ERROR_NOT_SUPPORTED);
        auto verify = [&](bool exact) {
            PSECURITY_DESCRIPTOR sd = nullptr; PSID actual = nullptr; PACL dacl = nullptr;
            const DWORD rc = GetSecurityInfo(file, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                            &actual, nullptr, &dacl, nullptr, &sd);
            if (rc != ERROR_SUCCESS) return Fail(error, "read security descriptor", rc);
            bool ok = trustedOwner(actual);
            if (ok && exact) {
                SECURITY_DESCRIPTOR_CONTROL control{}; DWORD revision = 0; void* raw = nullptr;
                ok = GetSecurityDescriptorControl(sd, &control, &revision) && (control & SE_DACL_PROTECTED) &&
                    dacl && IsValidAcl(dacl) && dacl->AceCount == 1 && GetAce(dacl, 0, &raw);
                if (ok) {
                    const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
                    ok = ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE && ace->Header.AceFlags == 0 &&
                        ace->Mask == FILE_ALL_ACCESS && EqualSid(const_cast<DWORD*>(&ace->SidStart), sid());
                }
            }
            LocalFree(sd);
            return ok || Fail(error, "owner or effective DACL is not private", ERROR_ACCESS_DENIED);
        };
        if (!verify(false)) return false;
        if (harden) {
            const DWORD rc = SetSecurityInfo(file, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                            nullptr, nullptr, reinterpret_cast<PACL>(acl.data()), nullptr);
            if (rc != ERROR_SUCCESS) return Fail(error, "protect DACL", rc);
        }
        return verify(true);
    }
};
// Pin all parent directories against rename/deletion, rejecting reparse points
// and remote/device/ADS paths. Never change any parent directory's ACL.
struct Parents {
    std::vector<HANDLE> handles;
    ~Parents() { for (auto h : handles) CloseHandle(h); }
    bool pin(const QString& path, QString& error)
    {
        const QString absolute = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
        if (absolute.size() < 4 || !absolute[0].isLetter() || absolute.mid(1, 2) != QStringLiteral(":/") ||
            absolute.mid(2).contains(':') || GetDriveTypeW(absolute.left(3).toStdWString().c_str()) == DRIVE_REMOTE)
            return Fail(error, "unsupported path", ERROR_NOT_SUPPORTED);
        QString parent = QFileInfo(absolute).absolutePath();
        std::vector<QString> names;
        while (true) {
            names.push_back(parent);
            if (parent.size() <= 3) break;
            const QString next = QFileInfo(parent).absolutePath();
            if (next == parent) return Fail(error, "invalid parent", ERROR_INVALID_NAME);
            parent = next;
        }
        for (auto it = names.rbegin(); it != names.rend(); ++it) {
            const DWORD access = FILE_READ_ATTRIBUTES | (*it == names.front() ? FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY : 0);
            HANDLE h = CreateFileW(it->toStdWString().c_str(), access,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (h == INVALID_HANDLE_VALUE) return Fail(error, "open parent");
            handles.push_back(h);
            BY_HANDLE_FILE_INFORMATION info{}; DWORD flags = 0; wchar_t fs[32]{};
            if (!GetFileInformationByHandle(h, &info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                !GetVolumeInformationByHandleW(h, nullptr, 0, nullptr, nullptr, &flags, fs, 32) ||
                !(flags & FILE_PERSISTENT_ACLS) || wcscmp(fs, L"NTFS") != 0)
                return Fail(error, "unsafe parent or unsupported filesystem", ERROR_NOT_SUPPORTED);
        }
        return true;
    }
};
struct Stage {
    QString directory, file;
    Handle pin;
    ~Stage() {
        // Delete only our known file/directory, never recurse through a path.
        if (!file.isEmpty()) DeleteFileW(file.toStdWString().c_str());
        pin.close();
        if (!directory.isEmpty()) RemoveDirectoryW(directory.toStdWString().c_str());
    }
    bool write(const QString& target, const QByteArray& bytes, Security& security, QString& error)
    {
        const QString candidate = target + QStringLiteral(".staging-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!CreateDirectoryW(candidate.toStdWString().c_str(), &security.attributes)) return Fail(error, "create private staging directory");
        directory = candidate;
        pin.value = CreateFileW(directory.toStdWString().c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (!pin.valid()) return Fail(error, "open private staging directory");
        if (!security.inspect(pin.value, true, false, error)) return false;
        file = directory + QStringLiteral("/payload");
        Handle out(CreateFileW(file.toStdWString().c_str(), GENERIC_WRITE | READ_CONTROL,
                               0, &security.attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!out.valid()) return Fail(error, "create private staging file");
        if (!security.inspect(out.value, false, false, error)) return false;
        // Verify before the very first secret byte. No publicly inherited
        // handle can have been opened on either new object before hardening.
        DWORD written = 0;
        if (!WriteFile(out.value, bytes.constData(), static_cast<DWORD>(bytes.size()), &written, nullptr) ||
            written != static_cast<DWORD>(bytes.size()) || !FlushFileBuffers(out.value)) return Fail(error, "write/flush private file");
        return security.inspect(out.value, false, false, error);
    }
};
} // namespace WindowsPrivate
#endif

// Publish a complete private journal with a single no-replace filesystem
// operation, not a check followed by a replacing rename. The staging directory
// is private and on the destination filesystem. Hard links work on POSIX and
// Windows/NTFS; unsupported filesystems fail closed (never fall back to copy).
inline bool SaveRecovery(const QString& requestedPath, const QByteArray& bytes, QString& error)
{
#ifdef Q_OS_WIN
    const QString path = QDir::cleanPath(QFileInfo(requestedPath).absoluteFilePath());
    WindowsPrivate::Parents parents;
    WindowsPrivate::Security security;
    WindowsPrivate::Stage staging;
    if (!parents.pin(path, error) || !security.init(error) || !staging.write(path, bytes, security, error)) return false;
    const QString staged = staging.file;
#else
    const QString& path = requestedPath;
    QTemporaryDir staging(path + QStringLiteral(".staging-XXXXXX"));
    if (!staging.isValid()) { error = staging.errorString(); return false; }
    const QString staged = staging.filePath(QStringLiteral("journal"));
    QSaveFile out(staged);
    out.setDirectWriteFallback(false);
    // Check buffered-write flush separately: never publish a short journal
    // even if commit's internal flush does not propagate a partial write.
    if (!out.open(QIODevice::WriteOnly) ||
        !out.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        out.write(bytes) != bytes.size() || !out.flush() || !out.commit()) {
        error = out.errorString();
        return false;
    }
#endif
    const auto nativePath = [](const QString& name) -> std::filesystem::path {
#ifdef Q_OS_WIN
        return std::filesystem::path(name.toStdWString());
#else
        return std::filesystem::path(QFile::encodeName(name).constData());
#endif
    };
    std::error_code ec;
    // EEXIST includes dangling symlinks. There is no overwrite window here.
    std::filesystem::create_hard_link(nativePath(staged), nativePath(path), ec);
    if (ec) {
        error = QStringLiteral("Could not exclusively publish recovery data (existing target or unsupported filesystem): %1")
                    .arg(QString::fromStdString(ec.message()));
        return false;
    }
    return true;
}

// Fail closed: the wizard never rotates an existing operator identity. A
// deliberate rotation belongs in the operator's backed-up configuration flow.
inline bool Prepare(const QByteArray& original, const QString& network,
                    const QString& secret, QByteArray& updated, QString& error)
{
    if (secret.isEmpty() || secret.contains('\n') || secret.contains('\r')) {
        error = QStringLiteral("Invalid empty or multiline operator key.");
        return false;
    }
    QByteArray parsed = original;
    if (parsed.startsWith("\xEF\xBB\xBF")) parsed.remove(0, 3);
    QString prefix;
    bool same = false;
    for (const auto& bytes : parsed.split('\n')) {
        const QString line = QString::fromUtf8(bytes).section('#', 0, 0).trimmed();
        if (line.isEmpty()) continue;
        if (line.startsWith('[') && line.endsWith(']')) {
            prefix = line.mid(1, line.size() - 2) + '.';
            continue;
        }
        const int eq = line.indexOf('=');
        if (eq < 0 || line.startsWith('-')) {
            error = QStringLiteral("Invalid configuration syntax; fix it before registration.");
            return false;
        }
        const QString name = prefix + line.left(eq).trimmed();
        const QString value = line.mid(eq + 1).trimmed();
        const QString base = name.section('.', -1);
        if (base == QStringLiteral("includeconf")) {
            error = QStringLiteral("Included configurations require manual operator-key setup; no transaction was sent.");
            return false;
        }
        const bool applicable = !name.contains('.') || name.startsWith(network + '.');
        if (!applicable) continue;
        if (base == QStringLiteral("nomasternodeblsprivkey") ||
            (base == QStringLiteral("masternodeblsprivkey") && value != secret)) {
            error = QStringLiteral("An existing operator key would be replaced or shadowed. Back up and configure the intended masternode manually; the wizard will not rotate it.");
            return false;
        }
        if (base == QStringLiteral("masternodeblsprivkey")) same = true;
    }
    updated = original;
    if (!same) {
        if (!updated.isEmpty() && !updated.endsWith('\n')) updated += '\n';
        updated += '[' + network.toUtf8() + "]\nmasternodeblsprivkey=" + secret.toUtf8() + '\n';
    }
    return true;
}

inline bool Read(const QString& path, QByteArray& bytes, QString& error)
{
    if (!QFileInfo::exists(path)) { bytes.clear(); return true; }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { error = file.errorString(); return false; }
    bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) { error = file.errorString(); return false; }
    return true;
}

// Shared admission for preflight and save. Keep the lock and Windows handles
// alive through parsing/staging; save must not rely on a prior preflight result.
struct ConfigTarget {
    const QString path;
#ifdef Q_OS_WIN
    WindowsPrivate::Parents parents;
    WindowsPrivate::Security security;
#endif
    QLockFile lock;
#ifdef Q_OS_WIN
    WindowsPrivate::Handle existing;
    bool existed{false};
#endif
    explicit ConfigTarget(const QString& requestedPath)
#ifdef Q_OS_WIN
        : path(QDir::cleanPath(QFileInfo(requestedPath).absoluteFilePath())),
#else
        : path(requestedPath),
#endif
          lock(path + QStringLiteral(".mnsetup.lock")) {}

    bool open(QString& error)
    {
#ifdef Q_OS_WIN
        if (!parents.pin(path, error) || !security.init(error)) return false;
#endif
        // Do not follow symlinks or overwrite a non-regular target.
        const QFileInfo info(path);
        if (info.isSymLink() || (info.exists() && !info.isFile())) {
            error = QStringLiteral("Configuration target is not a regular file.");
            return false;
        }
        if (!lock.tryLock(0)) { error = QStringLiteral("Configuration is busy."); return false; }
#ifdef Q_OS_WIN
        existing.value = CreateFileW(path.toStdWString().c_str(), GENERIC_READ | WRITE_DAC,
                                    FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        existed = existing.valid();
        if (!existed && GetLastError() != ERROR_FILE_NOT_FOUND) return WindowsPrivate::Fail(error, "open existing config");
        if (existed && !security.inspect(existing.value, false, true, error)) return false;
#endif
        return true;
    }
};

// Non-publishing: may harden an existing Windows DACL, but never writes an
// operator identity or replaces the config. Exercise real private creation,
// descriptor readback and flush using an EMPTY payload, not the prepared bytes.
// The probe is not a reservation: final save repeats admission and staging.
inline bool Preflight(const QString& path, const QString& network, const QString& secret, QString& error)
{
    ConfigTarget target(path);
    if (!target.open(error)) return false;
    QByteArray original, updated;
    if (!Read(target.path, original, error) || !Prepare(original, network, secret, updated, error)) return false;
#ifdef Q_OS_WIN
    WindowsPrivate::Stage staging;
    if (!staging.write(target.path, QByteArray(), target.security, error)) return false;
#endif
    return true;
}

inline bool Save(const QString& requestedPath, const QString& network, const QString& secret,
                 bool& changed, QString& error)
{
    changed = false;
    ConfigTarget target(requestedPath);
    if (!target.open(error)) return false;
    const QString& path = target.path;
    QByteArray original, updated;
    if (!Read(path, original, error) || !Prepare(original, network, secret, updated, error)) return false;
    if (original == updated) {
        // A content no-op is not a security no-op: imported/restored configs
        // may still be world-readable. Do not claim success if hardening fails.
#ifndef Q_OS_WIN
        QFile existing(path);
        if (!existing.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
            error = QStringLiteral("Could not restrict configuration permissions: %1").arg(existing.errorString());
            return false;
        }
#endif
        return true;
    }
#ifdef Q_OS_WIN
    WindowsPrivate::Stage staging;
    if (!staging.write(path, updated, target.security, error)) return false;
    QByteArray current;
    if (!Read(path, current, error) || current != original) {
        error = QStringLiteral("Configuration changed while saving; retry without overwriting it.");
        return false;
    }
    // Keep the original pinned against writes/replacement until publication.
    // MoveFileEx preserves the private source DACL on this same-volume move;
    // ReplaceFile would instead preserve the potentially public target DACL.
    target.existing.close();
    if (!MoveFileExW(staging.file.toStdWString().c_str(), path.toStdWString().c_str(),
                     (target.existed ? MOVEFILE_REPLACE_EXISTING : 0) | MOVEFILE_WRITE_THROUGH))
        return WindowsPrivate::Fail(error, "atomic config publication");
#else
    QSaveFile out(path);
    out.setDirectWriteFallback(false);
    if (!out.open(QIODevice::WriteOnly) ||
        !out.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        out.write(updated) != updated.size() || !out.flush()) {
        error = out.errorString();
        out.cancelWriting();
        return false;
    }
    QByteArray current;
    if (!Read(path, current, error) || current != original) {
        error = QStringLiteral("Configuration changed while saving; retry without overwriting it.");
        out.cancelWriting();
        return false;
    }
    if (!out.commit()) { error = out.errorString(); return false; }
#endif
    changed = true;
    return true;
}
} // namespace MasternodeWizardConfig
#endif // BITCOIN_QT_MASTERNODEWIZARDCONFIG_H