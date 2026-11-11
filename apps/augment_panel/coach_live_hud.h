#pragma once
#include <QWidget>
#include <QJsonObject>
#include <QJsonArray>
#include <functional>

class Catalog;
class HudAssets;
class QPainter;
namespace Kda { class Transition; }

// Small purchase strip; no network or calculation runs in this view.
class CoachItemStrip : public QWidget {
public:
    CoachItemStrip(const Catalog &,HudAssets *,QWidget *parent=nullptr);
    void setComponents(const QJsonArray &,const QString &emptyText);
    // Passive all-player band. Only observed booleans mark death; a unique
    // published self flag supplies the neutral focus rim.
    void setRoster(const QJsonArray &);
    // The owner may suppress repeated empty-state text without hiding icons,
    // component costs, or a published roster.
    bool hasVisualContent() const;
    QString text() const;
    QJsonObject report() const;
protected:
    void paintEvent(QPaintEvent *) override;
private:
    const Catalog &catalog;HudAssets *assets;
    QJsonArray components;QString emptyText;
    QJsonArray roster;
    bool rosterMode=false;
};

// Anonymous snapshots only. The compact strip shows actual counters; the
// expanded roster explicitly labels the separate 8-second model estimates.
class CoachLiveHud : public QWidget {
public:
    CoachLiveHud(const Catalog &,HudAssets *,bool compact,QWidget *parent=nullptr);
    void setSummary(const QJsonObject &);
    // A static, icon-first roster keeps every player in view without paging.
    void setDenseRoster(bool);
    // Paging is opt-in; existing callers keep the full roster by default.
    void setRosterPageSize(int);
    void setRosterPage(int);
    int rosterPage() const;
    int rosterPageCount() const;
    int rosterPlayerCount() const;
    QSize sizeHint() const override;
    QJsonObject report() const;
    std::function<void(int)> onPlayer;
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    bool event(QEvent *) override;
private:
    const Catalog &catalog;HudAssets *assets;bool compact,denseRoster=false;
    QJsonObject summary;
    Kda::Transition *motion=nullptr;
    QString motionKey;
    QString rosterIdentity;
    int pageSize=0,pageIndex=0;
    struct Row {QJsonObject player;QRect rect;int index;};
    QList<Row> orderedRows() const;
    QList<Row> rows() const;
    int rosterContentHeight() const;
    void refreshRosterGeometry();
    void paintDenseRoster(QPainter &);
    QJsonObject denseRosterReport() const;
    QString tooltip(const Row &) const;
};
