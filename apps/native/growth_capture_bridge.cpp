#include "growth_capture_bridge.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>
#include <cmath>
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {
QString k(const char *s){return QString::fromLatin1(s);}
QJsonArray ids(const QList<int>& values){QJsonArray out;for(int id:values)out.append(id);return out;}
}

QJsonObject joinGrowthObservationSnapshot(const Catalog &catalog,GameState &state,const QJsonObject &payload,
        QJsonObject &previousPayload,qint64 nowMs,const GrowthObservationPolicy &policy){
    auto trial=state;const auto result=applyGrowthObservations(catalog,trial,payload,nowMs,policy);
    const auto rejected=result.value(k("rejected")).toArray();const auto clock=payload.value(k("game_time"));
    const bool onlyAhead=rejected.size()==1&&rejected.first().toObject().value(k("reason"))==k("capture_game_time_invalid_or_stale")
        &&clock.isDouble()&&clock.toDouble()>state.gameTime&&clock.toDouble()-state.gameTime<=2.;
    if(onlyAhead&&!previousPayload.isEmpty()){
        // Check all remaining provider/identity/unit guards in a disposable
        // preview. Its future clock and values are never assigned to state.
        auto preview=state;preview.gameTime=clock.toDouble();
        const auto future=applyGrowthObservations(catalog,preview,payload,nowMs,policy);
        if(!future.value(k("accepted")).toArray().isEmpty()&&future.value(k("rejected")).toArray().isEmpty()){
            auto retained=state;auto past=applyGrowthObservations(catalog,retained,previousPayload,nowMs,policy);
            if(!past.value(k("accepted")).toArray().isEmpty()&&past.value(k("rejected")).toArray().isEmpty()){
                past[k("pending_newer_snapshot")]=true;retained.growthObservations=past;state=retained;return past;
            }
        }
    }
    state=trial;
    previousPayload=!result.value(k("accepted")).toArray().isEmpty()&&rejected.isEmpty()?payload:QJsonObject{};
    return result;
}

QJsonObject growthCapturePublicStatus(const QJsonObject &ingestion,const QJsonObject &coverage,bool enabled,bool running,const QString &lifecycleStatus){
    const auto summary=growthObservationPublicReport(enabled?ingestion:QJsonObject{});
    const bool observed=enabled&&!summary.value(k("accepted")).toArray().isEmpty();
    bool visibleFacts=false,structuredFacts=false;for(const auto &v:summary.value(k("accepted")).toArray()){if(v.toObject().value(k("measurement_kind"))==k("visible_tooltip_reading"))visibleFacts=true;else structuredFacts=true;}
    bool expired=false;
    for(const auto &value:summary.value(k("rejected")).toArray()){
        const auto reason=value.toObject().value(k("reason")).toString();
        expired|=reason==k("capture_time_invalid_or_stale")||reason==k("capture_game_time_invalid_or_stale")||reason==k("current_live_snapshot_required");
    }
    QString status=enabled?k("waiting_for_structured_data"):k("disabled_in_synthetic_mode");
    if(enabled){
        if(observed)status=k("observed");
        else if(expired)status=k("data_expired");
        else if(ingestion.contains(k("baseline_game_time"))&&summary.value(k("rejected")).toArray().isEmpty())status=k("layers_unavailable");
        else if(QStringList{k("waiting_for_game"),k("capture_directory_unavailable"),k("provider_unavailable")}.contains(lifecycleStatus))status=lifecycleStatus;
    }
    const auto available=[&](const char *key){const auto v=coverage.value(k(key));return enabled&&(v==QJsonValue(true)||v.toObject().value(k("available")).toBool());};
    QJsonArray unknowns;
    if(!available("heartsteel_permanent_health"))unknowns.append(k("heartsteel_permanent_hp_not_exposed"));
    if(!available("augment_quest_progress"))unknowns.append(k("augment_task_progress_not_exposed"));
    unknowns.append(k("augment_3116_target_stacks_not_provided"));
    return {{k("enabled"),enabled},{k("provider"),visibleFacts?(structuredFacts?k("hybrid"):k("visible_tooltip")):k("structured")},{k("status"),status},{k("connected"),observed},
        {k("source"),observed?(visibleFacts?k("native_validated_visible_or_hybrid_observations"):k("native_validated_structured_observations")):k("structured_provider_unavailable")},
        {k("ocr_enabled"),false},{k("screen_capture_enabled"),false},{k("game_input_sent"),false},{k("memory_read"),false},
        {k("process_running"),running},{k("ingestion"),summary},{k("input_coverage"),enabled?coverage:QJsonObject{}},{k("unknowns"),unknowns},
        {k("metric_capabilities"),enabled?growthObservationMetricCapabilities(ingestion):growthObservationMetricCapabilities({})},
        {k("growth_summary"),QStringLiteral("心钢永久成长、任务进度与目标层数未提供时保留未知；总生命值不能代替层数。")},
        {k("capabilities"),QJsonObject{{k("equipment_growth_counters"),observed},{k("augment_task_progress"),available("augment_quest_progress")},
            {k("equipment_growth_counters_semantics"),k("validated_facts_not_necessarily_numeric_baselines")},
            {k("numeric_equipment_growth_counters"),enabled&&ingestion.value(k("coverage")).toObject().value(k("numeric_baselines_applied")).toInt()>0},
            {k("registered_counter_provider"),enabled&&ingestion.value(k("registered_structured_provider_count")).toInt()>0}}}};
}

GrowthCaptureBridge::GrowthCaptureBridge(const Catalog &c,QObject *parent):QObject(parent),catalog(c){
    policy.structuredProviders=QJsonArray{QJsonObject{{k("provider_id"),k("riot-liveclient-glory-ledger")},
        {k("source"),k("official_event_derived")},{k("schema_revision"),k("glory-event-ledger-v1")},
        {k("counter_keys"),QJsonArray{k("item:1082:glory_stacks"),k("item:3041:glory_stacks")}}}};
    timer.setInterval(1000);
    connect(&timer,&QTimer::timeout,this,[this]{if(onUpdated)onUpdated();});
    connect(&process,&QProcess::readyReadStandardOutput,this,[this]{process.readAllStandardOutput();});
    connect(&process,&QProcess::readyReadStandardError,this,[this]{process.readAllStandardError();});
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args){args->flags|=CREATE_NO_WINDOW;});
    visibleProcess.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args){args->flags|=CREATE_NO_WINDOW;});
#endif
}
GrowthCaptureBridge::~GrowthCaptureBridge(){
    timer.stop();
    if(visibleProcess.state()!=QProcess::NotRunning){visibleProcess.terminate();if(!visibleProcess.waitForFinished(500)){visibleProcess.kill();visibleProcess.waitForFinished(500);}}
    if(process.state()!=QProcess::NotRunning){process.terminate();if(!process.waitForFinished(500)){process.kill();process.waitForFinished(500);}}
}
void GrowthCaptureBridge::configure(const QString &candidate,bool active){
    timer.stop();enabled=active;
    if(process.state()!=QProcess::NotRunning){process.terminate();if(!process.waitForFinished(500)){process.kill();process.waitForFinished(500);}}
    latestStatus={};ingestion={};nativeCoverage={};previousPayload={};
    if(!enabled){if(!contextFile.isEmpty())writeContext(GameState{});return;}
    python=candidate;
    if(!QFile::exists(python))python=QStandardPaths::findExecutable(k("python.exe"));
    if(python.isEmpty())python=QStandardPaths::findExecutable(k("python"));
    const auto folder=catalog.root+k("/.local/augment-panel/growth-capture/run-")+QString::number(QCoreApplication::applicationPid())+k("-")+QUuid::createUuid().toString(QUuid::Id128);
    if(!QDir().mkpath(folder)){latestStatus={{k("status"),k("capture_directory_unavailable")}};return;}
    contextFile=folder+k("/context.json");outputFile=folder+k("/observations.json");visibleOutputFile=folder+k("/visible.json");timer.start();
}
void GrowthCaptureBridge::configureVisible(const QJsonObject &settings){
    if(visibleProcess.state()!=QProcess::NotRunning){visibleProcess.terminate();if(!visibleProcess.waitForFinished(500)){visibleProcess.kill();visibleProcess.waitForFinished(500);}}
    visibleSettings=settings;visiblePrevious={};selectionStatus={};selectedInputs=AutomaticSelectedInputs{};lastVisibleAttempt=0;
    policy.allowTooltipOcr=enabled&&settings.value(k("enabled")).toBool()&&AutomaticSelectedInputs::validBinding(settings.value(k("capture_binding")).toObject());
}
void GrowthCaptureBridge::writeContext(const GameState &s){
    const bool active=enabled&&s.source==GameState::Source::Live&&s.fresh()&&s.championId>0&&s.matchContext.value(k("active")).toBool();
    QJsonObject context{{k("schema_version"),1},{k("active"),active},{k("context_id"),active?s.matchContext.value(k("context_id")):QJsonValue()},
        {k("champion_id"),active?s.championId:0},{k("game_time"),active?s.gameTime:0},{k("observed_at_utc"),s.received.toUTC().toString(Qt::ISODateWithMs)},
        {k("patch"),s.observedPatch},{k("mode"),s.gameMode},{k("item_ids"),active?ids(s.owned):QJsonArray{}},{k("augment_ids"),active?ids(s.selected):QJsonArray{}},
        {k("provider"),k("structured")},{k("ocr_enabled"),false},{k("screen_capture_enabled"),false}};
    if(policy.allowTooltipOcr){context[k("capture_binding")]=visibleSettings.value(k("capture_binding"));context[k("selection_round")]=selectionRound;context[k("capture_exclusions")]=latestStatus.value(k("capture_exclusions")).toArray();}
    QSaveFile file(contextFile);if(file.open(QIODevice::WriteOnly)){file.write(QJsonDocument(context).toJson(QJsonDocument::Compact));file.commit();}
}
void GrowthCaptureBridge::startHelper(){
    if(!enabled||python.isEmpty()||contextFile.isEmpty()||process.state()!=QProcess::NotRunning)return;
    const auto now=QDateTime::currentMSecsSinceEpoch();if(now-lastAttemptMs<15000)return;lastAttemptMs=now;
    const auto script=catalog.root+k("/scripts/growth_observation_capture.py");if(!QFile::exists(script)){latestStatus={{k("status"),k("provider_unavailable")}};return;}
    process.setWorkingDirectory(catalog.root);process.setProgram(python);
    process.setArguments({k("-X"),k("utf8"),script,k("--provider"),k("structured"),k("--root"),catalog.root,k("--context-file"),contextFile,k("--output"),outputFile,k("--follow"),k("--owner-pid"),QString::number(QCoreApplication::applicationPid())});
    process.start(QIODevice::ReadOnly);
}
void GrowthCaptureBridge::enrich(GameState &s){
    if(!enabled)return;
    const bool live=s.source==GameState::Source::Live&&s.fresh();
    const auto panelAvailable=[&](const QString &key){const auto v=s.liveStats.value(key);return live&&v.isDouble()&&std::isfinite(v.toDouble())&&v.toDouble()>=0;};
    nativeCoverage={{k("current_health"),QJsonObject{{k("available"),panelAvailable(k("currentHealth"))},{k("source_kind"),k("liveclient_public_api")}}},
        {k("max_health"),QJsonObject{{k("available"),panelAvailable(k("maxHealth"))},{k("source_kind"),k("liveclient_public_api")}}},
        {k("owned_items"),QJsonObject{{k("available"),false},{k("source_kind"),k("liveclient_public_api")}}},
        {k("selected_augments"),QJsonObject{{k("available"),false}}},
        {k("heartsteel_permanent_health"),false},{k("augment_quest_progress"),false},{k("glory_stacks"),false},{k("augment_3116_target_stacks"),false}};
    for(const auto &value:s.players){const auto p=value.toObject();if(!p.value(k("self")).toBool())continue;
        nativeCoverage[k("owned_items")]=QJsonObject{{k("available"),live&&p.value(k("items_complete")).toBool()},{k("source_kind"),k("liveclient_public_api")}};
        nativeCoverage[k("selected_augments")]=QJsonObject{{k("available"),live&&p.value(k("augments_complete")).toBool()},
            {k("source_kind"),p.value(k("augment_source"))}};
    }
    QJsonObject visual;
    if(policy.allowTooltipOcr){QFile file(visibleOutputFile);if(file.open(QIODevice::ReadOnly)&&file.size()<=256*1024)visual=QJsonDocument::fromJson(file.readAll()).object();}
    latestStatus[k("visible_status")]=QJsonObject{{k("status"),visual.value(k("status"))},{k("unknowns"),visual.value(k("unknowns"))},{k("screen_capture_used"),visual.value(k("screen_capture_used"))},{k("captured_at_utc"),visual.value(k("captured_at_utc"))}};
    selectionStatus=selectedInputs.apply(catalog,s,visual,policy.allowTooltipOcr?visibleSettings.value(k("capture_binding")).toObject():QJsonObject{},expectedSelected,selectionRound,selectionOpen);
    for(const auto &v:s.players){const auto p=v.toObject();if(p.value(k("self")).toBool())nativeCoverage[k("selected_augments")]=QJsonObject{{k("available"),live&&p.value(k("augments_complete")).toBool()},{k("source_kind"),p.value(k("augment_source"))}};}
    writeContext(s);
    if(s.source!=GameState::Source::Live||!s.fresh()){
        ingestion=joinGrowthObservationSnapshot(catalog,s,{},previousPayload,0,policy);latestStatus={{k("status"),k("waiting_for_game")}};return;
    }
    startHelper();QJsonObject payload;
    if(policy.allowTooltipOcr&&visibleProcess.state()==QProcess::NotRunning&&QDateTime::currentMSecsSinceEpoch()-lastVisibleAttempt>=15000){
        lastVisibleAttempt=QDateTime::currentMSecsSinceEpoch();visibleProcess.setWorkingDirectory(catalog.root);visibleProcess.setProgram(python);
        visibleProcess.setArguments({k("-X"),k("utf8"),catalog.root+k("/scripts/visible_match_inputs.py"),k("--root"),catalog.root,k("--context-file"),contextFile,k("--output"),visibleOutputFile,k("--owner-pid"),QString::number(QCoreApplication::applicationPid())});
        visibleProcess.start(QIODevice::ReadOnly);
    }
    QFile file(outputFile);if(file.open(QIODevice::ReadOnly)&&file.size()>0&&file.size()<=256*1024)payload=QJsonDocument::fromJson(file.readAll()).object();
    latestStatus[k("status")]=process.state()==QProcess::NotRunning?k("provider_unavailable"):k("waiting_for_structured_data");
    ingestion=joinGrowthObservationSnapshot(catalog,s,payload,previousPayload,0,policy);
    if(policy.allowTooltipOcr){
        visual.remove(k("selected_observations"));visual.remove(k("selection_round"));visual.remove(k("capture_binding"));
        auto trial=s;trial.growthObservations={};
        const auto primaryKeys=ingestion.value(k("applied_counter_keys")).toArray();QJsonArray values;
        for(const auto &v:visual.value(k("observations")).toArray())if(!primaryKeys.contains(v.toObject().value(k("counter_key"))))values.append(v);
        visual[k("observations")]=values;
        const auto extra=joinGrowthObservationSnapshot(catalog,trial,visual,visiblePrevious,0,policy);
        if(!extra.value(k("accepted")).toArray().isEmpty()){
            s.effectCounters=trial.effectCounters;auto facts=ingestion.value(k("accepted")).toArray(),keys=primaryKeys;
            for(const auto &v:extra.value(k("accepted")).toArray())facts.append(v);for(const auto &v:extra.value(k("applied_counter_keys")).toArray())keys.append(v);
            ingestion[k("accepted")]=facts;ingestion[k("applied_counter_keys")]=keys;ingestion[k("status")]=k("observed");
            ingestion[k("expires_at_ms")]=std::min(ingestion.value(k("expires_at_ms")).toDouble(extra.value(k("expires_at_ms")).toDouble()),extra.value(k("expires_at_ms")).toDouble());
            auto coverage=ingestion.value(k("coverage")).toObject();coverage[k("numeric_baselines_applied")]=keys.size();coverage[k("accepted_observations")]=facts.size();coverage[k("observation_only_count")]=facts.size()-keys.size();ingestion[k("coverage")]=coverage;ingestion[k("metric_capabilities")]=growthObservationMetricCapabilities(ingestion);s.growthObservations=ingestion;
        }
    }
    for(const auto &value:ingestion.value(k("accepted")).toArray()){const auto row=value.toObject();const auto key=row.value(k("counter_key")).toString();
        if(key==k("item:3084:permanent_hp"))nativeCoverage[k("heartsteel_permanent_health")]=QJsonObject{{k("available"),true},{k("source_kind"),row.value(k("measurement_kind"))}};
        if(key==k("augment:1152:heartsteel_progress"))nativeCoverage[k("augment_quest_progress")]=QJsonObject{{k("available"),true},{k("source_kind"),row.value(k("measurement_kind"))}};
        if(key==k("item:1082:glory_stacks")||key==k("item:3041:glory_stacks"))nativeCoverage[k("glory_stacks")]=QJsonObject{{k("available"),true},{k("source_kind"),row.value(k("measurement_kind"))}};
    }
    // HP alone cannot establish a cutoff for every mechanic (AD, haste, etc.).
    // Preserve that ledger; the existing fresh-panel anchor recalibrates its
    // residual after replacing counters, rather than replaying HP as a delta.
}
QJsonObject GrowthCaptureBridge::report() const{
    auto result=growthCapturePublicStatus(ingestion,nativeCoverage,enabled,process.state()!=QProcess::NotRunning,latestStatus.value(k("status")).toString());
    result[k("visible_reader")]=QJsonObject{{k("enabled"),policy.allowTooltipOcr},{k("process_running"),visibleProcess.state()!=QProcess::NotRunning},{k("settings"),visibleSettings},{k("selected_inputs"),selectionStatus},{k("last_capture"),latestStatus.value(k("visible_status"))},{k("requires_self_tooltip_hover"),true},{k("real_game_validated"),false}};
    result[k("ocr_enabled")]=policy.allowTooltipOcr;result[k("screen_capture_enabled")]=policy.allowTooltipOcr;return result;
}
