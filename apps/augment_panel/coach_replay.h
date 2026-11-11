#pragma once
#include <QMainWindow>
#include <QJsonObject>
#include <QJsonArray>
#include <QByteArray>

class Catalog;
class QLabel;
class QTreeWidget;
class QTextBrowser;
class QPushButton;
class HudAssets;
class DashboardChrome;
class ReplaySummary;
class QSplitter;

// The live timeline and any opened local archive have independent state.
class CoachReplay : public QMainWindow {
public:
    explicit CoachReplay(const Catalog &, QWidget *parent = nullptr, bool embedded = false);
    void observe(const QJsonObject &anonymousSnapshot);
    QJsonObject archiveSnapshot(const QString &expectedContext) const;
    static bool saveArchive(const QString &directory, const QString &verifiedMatchRef, const QJsonObject &captured);
    static bool archiveAvailable(const QString &directory, const QString &verifiedMatchRef);
    bool openArchive(const QString &directory, const QString &verifiedMatchRef);
    void showLive();
    QJsonObject report() const;
protected:
    void paintEvent(QPaintEvent *) override;
private:
    const Catalog &catalog;
    const bool embedded;
    QLabel *stateLabel;
    QTreeWidget *list;
    QTextBrowser *detail;
    HudAssets *assets;
    DashboardChrome *chrome = nullptr;
    ReplaySummary *summary;
    QWidget *filterBar=nullptr,*emptyState=nullptr;
    QSplitter *splitter=nullptr;
    QPushButton *followAction=nullptr;
    QList<QPushButton *> filters;
    QString activeFilter;
    QString context, status, observedPatch;
    bool synthetic = false;
    bool followLatest = true, updatingSelection = false;
    QByteArray displayedRecord;
    double lastTime = -1;
    QJsonArray records;
    QJsonObject archive;
    QByteArray lastPurchase, lastOffer, lastCards, lastSelection, lastCue;
    QJsonArray lastInventory;
    bool inventoryObserved = false;
    void clearMemory();
    void append(const QString &kind, const QJsonObject &payload, double time, int championId);
    void insertRow(const QJsonObject &record);
    void resetPresentation();
    void refreshPresentation();
    void applyFilter();
    void showDetail();
};
