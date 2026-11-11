#pragma once
#include <QElapsedTimer>
#include <QJsonObject>
#include <QRect>
#include <QString>
#include <QVector>

namespace GameWindow {
enum class Target { Game, Client };
struct Surface {
    bool valid=false,visible=false,minimized=false,framed=false;
    QRect clientPixels,monitorPixels;
    QString monitorName;
};
struct Candidate {
    quintptr window=0;
    quint32 process=0;
    QString executable;
    Surface surface;
    bool foreground=false;
};
struct Screen {
    QString name;
    QRect geometry;
    qreal scale=1.;
};
struct State {
    bool found=false,visible=false,minimized=false,foreground=false;
    QRect clientPixels,client;
    QString mode=QStringLiteral("unavailable");
    QString positionBasis=QStringLiteral("game_client_area");
    qreal scale=1.;
    bool overlayAllowed()const;
    QJsonObject report()const;
};
bool isGameExecutable(const QString &path);
bool isClientExecutable(const QString &path);
QRect logicalClient(const QRect &pixels,const QRect &nativeMonitor,const QRect &qtScreen,qreal scale);
QString surfaceMode(const Surface &surface);
int selectCandidate(const QVector<Candidate> &candidates,quintptr previous=0);
int selectClientCandidate(const QVector<Candidate> &candidates,quint32 expectedProcess);
int screenIndex(const Surface &surface,const QVector<Screen> &screens);
Surface nativeSurface(quintptr window);

// Read-only window observation. No game hooks, input injection or display changes.
class Tracker {
public:
    explicit Tracker(Target target=Target::Game);
    // Client observations must belong to the UX process of the connected LCU.
    // Clearing/changing it revokes the previously observed window immediately.
    void setExpectedProcess(quint32 process);
    const State &refresh();
    const State &state()const{return current;}
private:
    Target target;
    quint32 expectedProcess=0;
    QElapsedTimer clock;
    qint64 lastScan=-500;
    quintptr trackedWindow=0;
    quint32 trackedProcess=0;
    State current;
};
class ClientTracker : public Tracker {
public:
    ClientTracker():Tracker(Target::Client){}
};
}
