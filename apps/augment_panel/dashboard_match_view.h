#pragma once
#include <QJsonObject>
#include <QString>
class Catalog;

// Pure, anonymous projection. Never queries players, fits models, or turns
// historical performance into a measurement of this match.
QJsonObject buildDashboardMatchView(const Catalog &,const QJsonObject &snapshot);

// The catalog is immutable for this view's lifetime. Runtime clocks, costs and
// diagnostics do not change the roster/history projection; freshness still does.
class DashboardMatchViewCache final {
public:
    explicit DashboardMatchViewCache(const Catalog &value):catalog(value){}
    QJsonObject project(const QJsonObject &snapshot);
    QJsonObject report() const;
private:
    const Catalog &catalog;
    QJsonObject input,view;
    qint64 builds=0,hits=0;
};

// Reads only the bounded aggregate manifest published by the player engine.
// Cached for two seconds; no database or service access.
QJsonObject readDashboardCorpusSummary(const QString &projectRoot);
