#include "lcu_discovery.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMap>
#include <limits>
#include <vector>
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <tlhelp32.h>
#include <iphlpapi.h>
#include <shellapi.h>
#endif

namespace {
quint32 positiveInteger(const QString &text, quint32 maximum) {
    if (text.isEmpty() || text.size() > 10) return 0;
    for (QChar c : text) if (c < QLatin1Char('0') || c > QLatin1Char('9')) return 0;
    bool ok = false;
    const auto value = text.toULongLong(&ok);
    return ok && value > 0 && value <= maximum ? quint32(value) : 0;
}
bool validToken(const QString &token) {
    if (token.isEmpty() || token.size() > 512) return false;
    for (QChar c : token) if (c.unicode() < 33 || c.unicode() > 126 || c == QLatin1Char(':')) return false;
    return true;
}
QByteArray basicAuth(const QString &token) {
    return QByteArrayLiteral("Basic ") + (QByteArrayLiteral("riot:") + token.toLatin1()).toBase64();
}
QString folderOf(const QString &executable) { return QFileInfo(executable).absolutePath(); }
bool samePath(const QString &a, const QString &b) {
    return QDir::cleanPath(QDir::fromNativeSeparators(a)).compare(QDir::cleanPath(QDir::fromNativeSeparators(b)), Qt::CaseInsensitive) == 0;
}
#ifdef Q_OS_WIN
struct Handle {
    HANDLE value;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct Process { QString name; quint32 parent; };
QHash<quint32,Process> processes() {
    QHash<quint32,Process> result;
    Handle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    if (snapshot.value == INVALID_HANDLE_VALUE) return result;
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot.value, &entry)) do {
        if (_wcsicmp(entry.szExeFile, L"LeagueClientUx.exe") == 0 || _wcsicmp(entry.szExeFile, L"LeagueClient.exe") == 0
            || _wcsicmp(entry.szExeFile, L"RiotClientServices.exe") == 0)
            result.insert(entry.th32ProcessID, {QString::fromWCharArray(entry.szExeFile), entry.th32ParentProcessID});
    } while (Process32NextW(snapshot.value, &entry));
    return result;
}
QString executablePath(HANDLE process) {
    wchar_t path[32768]; DWORD length = 32768;
    return QueryFullProcessImageNameW(process, 0, path, &length) ? QString::fromWCharArray(path, int(length)) : QString();
}
QStringList processArguments(HANDLE process) {
    // OS command-line metadata only. No PROCESS_VM_READ, PEB access or remote memory reads.
    using Query = LONG (NTAPI *)(HANDLE, int, PVOID, ULONG, PULONG);
    const auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    if (!query) return {};
    ULONG required = 0;
    query(process, 60 /* ProcessCommandLineInformation */, nullptr, 0, &required);
    constexpr ULONG maximum = 512 * 1024;
    struct UnicodeString { USHORT length, maximumLength; PWSTR buffer; };
    if (required < sizeof(UnicodeString) || required > maximum) return {};
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::vector<unsigned char> bytes(required);
        ULONG returned = 0;
        const auto status = query(process, 60, bytes.data(), ULONG(bytes.size()), &returned);
        if (status < 0) {
            SecureZeroMemory(bytes.data(), bytes.size());
            if (returned > required && returned <= maximum) { required = returned; continue; }
            return {};
        }
        const auto *value = reinterpret_cast<const UnicodeString *>(bytes.data());
        const auto begin = reinterpret_cast<uintptr_t>(bytes.data());
        const auto pointer = reinterpret_cast<uintptr_t>(value->buffer);
        const bool bounded = returned >= sizeof(UnicodeString) && returned <= bytes.size()
            && value->length != 0 && value->length % sizeof(wchar_t) == 0
            && value->length <= value->maximumLength && pointer >= begin + sizeof(UnicodeString)
            && pointer <= begin + returned && value->length <= begin + returned - pointer;
        QStringList result;
        if (bounded) {
            auto command = QString::fromWCharArray(value->buffer, value->length / sizeof(wchar_t));
            int count = 0;
            auto arguments = CommandLineToArgvW(reinterpret_cast<LPCWSTR>(command.utf16()), &count);
            if (arguments) {
                for (int i = 0; i < count; ++i) result.append(QString::fromWCharArray(arguments[i]));
                LocalFree(arguments);
            }
            command.fill(QChar(0));
        }
        SecureZeroMemory(bytes.data(), bytes.size());
        return result;
    }
    return {};
}
#endif
}

LcuConnection parseLcuArguments(const QStringList &arguments, quint32 uxPid, quint32 parentPid, const QString &executable) {
    LcuConnection result;
    if (!uxPid || !parentPid || arguments.isEmpty()
        || QFileInfo(executable).fileName().compare(QStringLiteral("LeagueClientUx.exe"), Qt::CaseInsensitive) != 0
        || !samePath(arguments.first(), executable)) return result;
    QMap<QString,QString> flags;
    const QStringList wanted{QStringLiteral("--app-port"), QStringLiteral("--remoting-auth-token"), QStringLiteral("--app-pid"),
        QStringLiteral("--riotclient-app-port"), QStringLiteral("--riotclient-auth-token"), QStringLiteral("--rso_platform_id")};
    for (int i = 1; i < arguments.size(); ++i) {
        const auto &arg = arguments[i];
        if (arg == QStringLiteral("--enable-lockfile")) result.lockfileEnabled = true;
        const int equal = arg.indexOf(QLatin1Char('='));
        if (equal < 0) continue;
        const auto key = arg.left(equal);
        if (!wanted.contains(key)) continue;
        if (flags.contains(key)) return {};
        flags.insert(key, arg.mid(equal + 1));
    }
    const auto pid = positiveInteger(flags.value(QStringLiteral("--app-pid")), std::numeric_limits<quint32>::max());
    const auto port = positiveInteger(flags.value(QStringLiteral("--app-port")), 65535);
    const auto token = flags.value(QStringLiteral("--remoting-auth-token"));
    if (pid != parentPid || !port || !validToken(token)) return {};
    result.processId = pid; result.uxProcessId = uxPid; result.port = quint16(port);
    result.authorization = basicAuth(token); result.folder = folderOf(executable);
    const auto riotPort = positiveInteger(flags.value(QStringLiteral("--riotclient-app-port")), 65535);
    const auto riotToken = flags.value(QStringLiteral("--riotclient-auth-token"));
    if (riotPort && validToken(riotToken)) {
        result.riotPort = quint16(riotPort); result.riotAuthorization = basicAuth(riotToken);
    }
    const auto platform = flags.value(QStringLiteral("--rso_platform_id")).toUpper();
    if (platform.size() <= 16) {
        bool valid = !platform.isEmpty();
        for (const auto c : platform) valid = valid && ((c >= QLatin1Char('A') && c <= QLatin1Char('Z'))
            || (c >= QLatin1Char('0') && c <= QLatin1Char('9')) || c == QLatin1Char('_'));
        if (valid) result.rsoPlatformId = platform;
    }
    result.source = QStringLiteral("windows_process_command_line");
    return result;
}

LcuConnection parseLcuLockfile(const QByteArray &content) {
    LcuConnection result;
    if (content.size() > 2048) return result;
    const auto parts = content.trimmed().split(':');
    if (parts.size() != 5 || parts[0] != QByteArrayLiteral("LeagueClient") || parts[4] != QByteArrayLiteral("https")) return result;
    const auto pid = positiveInteger(QString::fromLatin1(parts[1]), std::numeric_limits<quint32>::max());
    const auto port = positiveInteger(QString::fromLatin1(parts[2]), 65535);
    const auto token = QString::fromLatin1(parts[3]);
    if (!pid || !port || !validToken(token)) return result;
    result.processId = pid; result.port = quint16(port); result.authorization = basicAuth(token);
    result.source = QStringLiteral("validated_lockfile");
    return result;
}

bool lcuPortOwnedBy(quint16 port, quint32 processId) {
#ifdef Q_OS_WIN
    if (!port || !processId) return false;
    ULONG size = 0;
    if (GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != ERROR_INSUFFICIENT_BUFFER || size > 4 * 1024 * 1024) return false;
    std::vector<unsigned char> bytes(size);
    if (GetExtendedTcpTable(bytes.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR) return false;
    const auto *table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID *>(bytes.data());
    const size_t offset = offsetof(MIB_TCPTABLE_OWNER_PID, table);
    if (bytes.size() < offset || table->dwNumEntries > (bytes.size() - offset) / sizeof(MIB_TCPROW_OWNER_PID)) return false;
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto &row = table->table[i];
        if (row.dwOwningPid == processId && ntohs(u_short(row.dwLocalPort)) == port
            && (row.dwLocalAddr == 0 || ntohl(row.dwLocalAddr) == INADDR_LOOPBACK)) return true;
    }
#else
    Q_UNUSED(port); Q_UNUSED(processId);
#endif
    return false;
}

bool allowedLcuRequest(const QUrl &url, const LcuConnection &connection) {
    return connection.valid() && url.isValid() && url.scheme() == QStringLiteral("https")
        && url.host() == QStringLiteral("127.0.0.1") && url.port() == connection.port
        && url.userInfo().isEmpty() && !url.hasFragment();
}

LcuConnection discoverLcu(const QString &preferredFolder) {
    LcuConnection failure;
#ifdef Q_OS_WIN
    const auto entries = processes();
    QStringList folders;
    if (!preferredFolder.isEmpty()) folders.append(QDir(preferredFolder).absolutePath());
    for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
        if (it->name.compare(QStringLiteral("LeagueClientUx.exe"), Qt::CaseInsensitive) != 0) continue;
        const auto parent = entries.constFind(it->parent);
        if (parent == entries.cend() || parent->name.compare(QStringLiteral("LeagueClient.exe"), Qt::CaseInsensitive) != 0) continue;
        Handle ux{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, it.key())};
        Handle client{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, it->parent)};
        if (!ux.value || !client.value) continue;
        const auto path = executablePath(ux.value), backend = executablePath(client.value);
        if (path.isEmpty() || backend.isEmpty() || !samePath(folderOf(path), folderOf(backend))) continue;
        folders.append(folderOf(path));
        auto arguments = processArguments(ux.value);
        auto found = parseLcuArguments(arguments, it.key(), it->parent, path);
        for (auto &argument : arguments) argument.fill(QChar(0));
        if (found.valid() && lcuPortOwnedBy(found.port, found.processId)) {
            if (found.riotPort) {
                if (lcuPortOwnedBy(found.riotPort, found.processId)) found.riotProcessId = found.processId;
                else for (auto candidate = entries.cbegin(); candidate != entries.cend(); ++candidate) {
                    if (candidate->name.compare(QStringLiteral("RiotClientServices.exe"), Qt::CaseInsensitive) == 0
                        && lcuPortOwnedBy(found.riotPort, candidate.key())) { found.riotProcessId = candidate.key(); break; }
                }
                if (!found.riotProcessId) { found.riotPort = 0; found.riotAuthorization.clear(); }
            }
            return found;
        }
    }
    folders.removeDuplicates();
    for (const auto &folder : folders) {
        QFile lock(folder + QStringLiteral("/lockfile"));
        if (!lock.open(QIODevice::ReadOnly)) continue;
        auto bytes = lock.read(2049);
        auto found = parseLcuLockfile(bytes); bytes.fill('\0');
        if (!found.valid()) continue;
        const auto process = entries.constFind(found.processId);
        if (process == entries.cend() || process->name.compare(QStringLiteral("LeagueClient.exe"), Qt::CaseInsensitive) != 0) continue;
        Handle handle{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, found.processId)};
        if (!handle.value || !samePath(folderOf(executablePath(handle.value)), folder)) continue;
        if (lcuPortOwnedBy(found.port, found.processId)) { found.folder = folder; return found; }
    }
    failure.diagnostic = entries.isEmpty() ? QStringLiteral("未发现运行中的客户端") : QStringLiteral("客户端认证信息暂不可用，稍后自动重试");
#else
    Q_UNUSED(preferredFolder);
    failure.diagnostic = QStringLiteral("本机客户端发现仅支持 Windows");
#endif
    return failure;
}
