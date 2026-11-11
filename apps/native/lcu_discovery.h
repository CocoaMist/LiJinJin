#pragma once
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QUrl>

// Session credentials stay in process memory. Never serialize this structure.
struct LcuConnection {
    QString folder, source, diagnostic;
    QByteArray authorization;
    QByteArray riotAuthorization;
    QString rsoPlatformId;
    quint32 processId = 0, uxProcessId = 0;
    quint32 riotProcessId = 0;
    quint16 port = 0;
    quint16 riotPort = 0;
    bool lockfileEnabled = false;
    bool valid() const { return processId != 0 && port != 0 && !authorization.isEmpty(); }
};

LcuConnection parseLcuArguments(const QStringList &, quint32 uxPid, quint32 parentPid, const QString &executable);
LcuConnection parseLcuLockfile(const QByteArray &);
LcuConnection discoverLcu(const QString &preferredFolder = {});
bool lcuPortOwnedBy(quint16 port, quint32 processId);
bool allowedLcuRequest(const QUrl &, const LcuConnection &);
