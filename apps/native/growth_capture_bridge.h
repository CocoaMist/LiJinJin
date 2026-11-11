#pragma once
#include "analysis.h"
#include "growth_observations.h"
#include "automatic_selected_inputs.h"
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <functional>

// Uses native validation results and coverage; never publishes helper payloads.
QJsonObject growthCapturePublicStatus(const QJsonObject &ingestion,
    const QJsonObject &coverage,bool enabled,bool running,const QString &lifecycleStatus={});

// A separate API reader can be slightly ahead of the native snapshot. Queue
// that reading and revalidate the previous one; never admit future values.
QJsonObject joinGrowthObservationSnapshot(const Catalog &,GameState &,const QJsonObject &,
    QJsonObject &previousPayload,qint64 nowMs,const GrowthObservationPolicy &);

// Owns structured telemetry and an optional calibrated visible-tooltip helper.
// Neither sends game input. Each process gets its own sidecar directory.
class GrowthCaptureBridge : public QObject {
public:
    explicit GrowthCaptureBridge(const Catalog &,QObject *parent=nullptr);
    ~GrowthCaptureBridge() override;
    void configure(const QString &python,bool enabled);
    void configureVisible(const QJsonObject &settings);
    void setSelectionRound(int resolved,int round,bool open){expectedSelected=resolved;selectionRound=round;selectionOpen=open;}
    void setCaptureExclusions(const QJsonArray &rectangles){latestStatus[QStringLiteral("capture_exclusions")]=rectangles;}
    void enrich(GameState &);
    QJsonObject report() const;
    std::function<void()> onUpdated;
private:
    const Catalog &catalog;
    QProcess process,visibleProcess;
    QTimer timer;
    QString python,contextFile,outputFile,visibleOutputFile;
    bool enabled=false;
    qint64 lastAttemptMs=0;
    QJsonObject latestStatus,ingestion,nativeCoverage,previousPayload;
    QJsonObject visibleSettings,visiblePrevious,selectionStatus;
    AutomaticSelectedInputs selectedInputs;
    int expectedSelected=-1,selectionRound=-1;bool selectionOpen=false;
    qint64 lastVisibleAttempt=0;
    GrowthObservationPolicy policy;
    void startHelper();
    void writeContext(const GameState &);
};
