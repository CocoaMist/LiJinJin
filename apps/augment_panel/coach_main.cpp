#include "purchase_checklist.h"
#include "coach_purchase_list.h"
#include "match_outlook.h"
#include "panel_state.h"
#include "app_version.h"
#include "offer_match_binding.h"
#include "overlay_layout.h"
#include "game_window.h"
#include "overlay_window.h"
#include "hud_layout.h"
#include "hud_layout_editor.h"
#include <QMessageBox>
#include "coach_model.h"
#include "combat.h"
#include "combat_sandbox.h"
#include "purchase_planner.h"
#include "coach_refresh_policy.h"
#include "coach_recommendation_quality.h"
#include "local_choice_evidence.h"
#include "recommendation_history.h"
#include "loading_preparation.h"
#include "decision_context.h"
#include "ai_context.h"
#include "deepseek.h"
#include "ds_forecast.h"
#include "ds_usage_meter.h"
#include "ds_usage_session.h"
#include "live_client.h"
#include "growth_capture_bridge.h"
#include "manual_match_inputs.h"
#include "growth_capture_settings.h"
#include "settings.h"
#include "history_pane.h"
#include "auto_advice.h"
#include "card_advice.h"
#include "card_hint_window.h"
#include "corner_combat_hud.h"
#include "death_tips_window.h"
#include "death_tips_content.h"
#include "death_insights.h"
#include "death_tip_visual.h"
#include "learned_state_model.h"
#include "augment_strategy.h"
#include "headliner.h"
#include "match_context.h"
#include "roster_analysis_bridge.h"
#include "roster_history_basis.h"
#include "coach_knowledge.h"
#include "augment_advisory.h"
#include "card_reference_cache.h"
#include "external_strategy_reference.h"
#include "coach_dashboard.h"
#include "champ_select_view.h"
#include "shop_recommendations.h"
#include "shop_recommendation_projection.h"
#include "coach_action_hud.h"
#include "coach_live_hud.h"
#include "hud_assets.h"
#include "live_combat_summary.h"
#include "live_summary_worker.h"
#include "execution_reference_worker.h"
#include "diagnostic_report_writer.h"
#include "runtime_log.h"
#include "future_completion.h"
#include "recommendation_trace.h"
#include "role_guidance.h"
#include "live_equipment_cues.h"
#include "coach_presentation.h"
#include "coach_replay.h"
#include "ui_browser.h"
#include "hud_paged_text.h"
#include "kda_theme.h"
#include "kda_tray_menu.h"
#include "kda_hud_style.h"
#include "kda_notice.h"
#include "startup_notice.h"
#include "kda_controls.h"
#include "preference_switch.h"
#include "hud_symbol_button.h"
#include "coach_hud_details.h"
#include "coach_engine_strip.h"
#include "coach_roster_insights.h"
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QStandardPaths>
#include <QCryptographicHash>
#include <QIcon>
#include <QTabBar>
#include <QToolButton>
#include <QSystemTrayIcon>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCommandLineParser>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFontDatabase>
#include <QFrame>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QImage>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScreen>
#include <QAbstractScrollArea>
#include <QScrollBar>
#include <QTabWidget>
#include <QTextBrowser>
#include <QToolTip>
#include <QCursor>
#include <QTextDocument>
#include <QTextOption>
#include <QTimer>
#include <QScopeGuard>
#include <QThread>
#include <QThreadPool>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <algorithm>
#include <optional>
#include <utility>
#include <cmath>
#include <cstdio>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif

namespace {
constexpr int compactHudHeight=OverlayMetrics::titlebarHeight+252;
constexpr int expandedHudHeight=392;
constexpr int pregameHudHeight=OverlayMetrics::titlebarHeight+76;
ModelConnection runtimeModelConnection(const QString &root){
    QString error;auto connection=loadLocalSettings(root,&error).modelConnection();
    if(!error.isEmpty()){connection.slotId.clear();connection.apiKey.clear();return connection;}
    if(connection.isDeepSeek()&&connection.apiKey.isEmpty())connection.apiKey=qEnvironmentVariable("DEEPSEEK_API_KEY").trimmed();
    return connection;
}
QString h(const QString &v){return v.toHtmlEscaped();}
QString growthPlanSummary(const QJsonObject &plan){
    for(const auto &key:{QStringLiteral("growth_reasoning"),QStringLiteral("growth_objective")}){
        const auto summary=plan.value(key).toObject().value(QStringLiteral("summary")).toString().trimmed();
        if(!summary.isEmpty())return summary;
    }
    return QStringLiteral("英雄与海克斯成长联动");
}
bool growthCandidatePlan(const QJsonObject &plan){
    return plan.value("growth_conditioned_candidates").toBool()
        ||plan.value("recommendation_kind")==QStringLiteral("growth_conditioned_candidates");
}
QStringList growthCandidateNotes(const QJsonObject &plan){
    const auto objective=plan.value("growth_objective").toObject();
    const auto held=objective.value("held_growth_inputs").toObject();
    bool heartsteel=false,heartsteelGiven=false,otherMissing=false,progressUnknown=false,utilityUnknown=false;
    const auto inspect=[&](const QString &name,const QJsonObject &supplied){
        const auto value=supplied.value(name);const bool given=value.isDouble()&&std::isfinite(value.toDouble())&&value.toDouble()>=0;
        if((name==QStringLiteral("heartsteel_stacks")||name==QStringLiteral("item:3084:permanent_hp"))){heartsteel=true;heartsteelGiven|=given;}else if(!given)otherMissing=true;
    };
    for(const auto &v:objective.value("equipment_growth_inputs").toArray()){
        const auto row=v.toObject();for(const auto &input:row.value("required_inputs").toArray())inspect(input.toString(),row.value("supplied_baseline_inputs").toObject());
    }
    for(const auto &v:objective.value("selected_augments").toArray())for(const auto &input:v.toObject().value("growth_stack_inputs").toArray())inspect(input.toString(),held);
    for(const auto &v:objective.value("unknowns").toArray()){
        const auto reason=v.toString();if(reason.startsWith(QStringLiteral("growth_input_unobserved:")))inspect(reason.mid(24),held);
        progressUnknown|=reason.startsWith(QStringLiteral("growth_dependency_does_not_verify_stacks_or_quest_progress:"));
        utilityUnknown|=reason==QStringLiteral("ally_heal_shield_control_utility_not_fully_quantified");
    }
    QStringList notes;if(heartsteel)notes.append(heartsteelGiven?QStringLiteral("心钢层数按给定基线计算，未自动采集"):QStringLiteral("心钢层数未自动采集，当前未知"));
    if(otherMissing)notes.append(QStringLiteral("部分成长层数或触发计数待补充"));
    if(progressUnknown)notes.append(QStringLiteral("装备前置可核对，层数与任务进度未核实"));
    if(utilityUnknown)notes.append(QStringLiteral("治疗、护盾与控制价值未完整量化"));
    notes.append(QStringLiteral("未来层数、命中与成长用时未预测"));return notes;
}
QString growthMetricLabel(const QString &metric){
    if(metric==QStringLiteral("modeled_output"))return QStringLiteral("模型输出");
    if(metric==QStringLiteral("mixed_effective_hp"))return QStringLiteral("混合有效生命");
    if(metric==QStringLiteral("skill_cycle_capacity"))return QStringLiteral("技能循环容量");
    if(metric==QStringLiteral("ally_protection_reference"))return QStringLiteral("队友保护场景");
    if(metric==QStringLiteral("normalized_tradeoff"))return QStringLiteral("指标折中");
    return QStringLiteral("成长指标比较");
}
QString num(double v,int decimals=0){return QString::number(v,'f',decimals);}
QString localChoiceComponentText(const QJsonObject &row){
    if(!row.value("available").toBool())return {};
    const auto parts=row.value("predicted_components").toObject();
    const auto attack=parts.value("attackDamage"),skill=parts.value("skillDamage"),survival=parts.value("survival");
    for(const auto &value:{attack,skill,survival})if(!value.isDouble()||!std::isfinite(value.toDouble()))return {};
    return QStringLiteral("8 秒局部估计 · 普攻 %1 · 技能 %2 · 生存值 %3")
        .arg(num(attack.toDouble()),num(skill.toDouble()),num(survival.toDouble()));
}
QString score(const QJsonObject &v,const QString &key){return v.value(key).isDouble()?num(v.value(key).toDouble()):QStringLiteral("?");}
QString money(int v){return (v>0?QStringLiteral("+"):QString())+QString::number(v)+QStringLiteral(" G");}
QJsonArray jsonIds(const QList<int> &ids){QJsonArray a;for(int id:ids)a.append(id);return a;}
bool visibleChildrenFit(QWidget *body){
    QList<QRect> occupied;for(auto child:body->findChildren<QWidget*>(QString(),Qt::FindDirectChildrenOnly))if(child->isVisibleTo(body)){
        const auto rect=child->geometry();if(!body->rect().contains(rect))return false;
        for(const auto &prior:occupied)if(prior.intersects(rect))return false;occupied.append(rect);
    }return true;
}
QList<int> listIds(const QJsonArray &a){QList<int> ids;for(const auto &v:a)ids.append(v.toInt());return ids;}
QIcon assistantIcon(){
    static const QIcon icon(QStringLiteral(":/lixin/kda/brand/lijinjin-app.ico"));
    return icon;
}
HudPagedText *browser(){auto view=new HudPagedText;view->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Ignored);view->setMinimumSize(0,0);return view;}
void putHtml(HudPagedText *view,const QString &text){
    if(view->property("lastHtml").toString()==text)return;
    view->setHtml(QStringLiteral("<style>body{color:#dce1ea;font-family:'Inter','Noto Sans SC';font-size:12px;font-weight:400;}p{margin:4px 0}h3{color:#f2f5fb;font-size:14px;font-weight:600;margin:6px 0 4px;}small{color:#aab3c0;font-size:12px;}b{color:#f6f7fb}a{color:#8dbdff;}td{padding:4px 2px;}</style>")+text);
    view->setProperty("lastHtml",text);
}
QJsonValue normalizedLoadoutEntries(const QJsonValue &value){
    if(!value.isArray())return value;
    QList<QJsonValue> entries;for(const auto &v:value.toArray())entries.append(v);
    // Preserve invalid values and duplicates: an invalid/incomplete inventory
    // must not share a cache key with an empty, valid one.
    std::sort(entries.begin(),entries.end(),[](const QJsonValue &a,const QJsonValue &b){return QJsonDocument(QJsonArray{a}).toJson(QJsonDocument::Compact)<QJsonDocument(QJsonArray{b}).toJson(QJsonDocument::Compact);});
    QJsonArray out;for(const auto &v:entries)out.append(v);return out;
}
QJsonArray normalizedOpponentLoadoutBasis(const GameState &state){
    int selfCount=0;QString ownTeam;QJsonArray selfTeams;
    for(const auto &v:state.players){const auto p=v.toObject();if(p.value("self").toBool()){++selfCount;ownTeam=p.value("team").toString();selfTeams.append(QJsonObject{{"present",p.contains("team")},{"value",p.value("team")}});}}
    const bool teamKnown=selfCount==1&&(ownTeam==QStringLiteral("ORDER")||ownTeam==QStringLiteral("CHAOS"));QList<QJsonObject> opponents;
    for(const auto &v:state.players){const auto p=v.toObject();if(teamKnown&&p.value("team").toString()==ownTeam)continue;
        QJsonObject row;for(const auto &key:{QStringLiteral("champion_id"),QStringLiteral("team"),QStringLiteral("self"),QStringLiteral("level"),QStringLiteral("items"),QStringLiteral("items_complete"),QStringLiteral("augment_ids"),QStringLiteral("augments_complete")}){
            const auto value=p.value(key);row[key]=QJsonObject{{"present",p.contains(key)},{"value",key==QStringLiteral("items")||key==QStringLiteral("augment_ids")?normalizedLoadoutEntries(value):value}};
        }opponents.append(row);
    }
    std::sort(opponents.begin(),opponents.end(),[](const QJsonObject &a,const QJsonObject &b){return QJsonDocument(a).toJson(QJsonDocument::Compact)<QJsonDocument(b).toJson(QJsonDocument::Compact);});
    QJsonArray out{QJsonObject{{"self_team_marker",true},{"self_count",selfCount},{"teams",normalizedLoadoutEntries(selfTeams)}}};for(const auto &row:opponents)out.append(row);return out;
}

}

static RuntimeLog::Outcome inferenceLogOutcome(bool accepted, bool resultAvailable, bool complete, const QString &stopReason) {
    using Outcome = RuntimeLog::Outcome;
    if(accepted)return Outcome::Accepted;
    if(stopReason==QStringLiteral("cancelled"))return Outcome::Cancelled;
    if(stopReason==QStringLiteral("time_budget_exceeded"))return Outcome::TimedOut;
    return resultAvailable&&complete?Outcome::Discarded:Outcome::Failed;
}

class DualPanels : public QObject {
public:
    DualPanels(const Catalog &c,const QCommandLineParser &args,QSettings &launchPreferences,CoachDashboard *initialDashboard,RuntimeLog *log):catalog(c),parser(args),runtimeLog(log),
        replay(args.isSet("replay")||args.isSet("demo")),demo(args.isSet("demo")),state(replay),
        preferences(launchPreferences),live(c),ai(this),rosterAi(this),forecast(c,this),rosterBridge(this),growthCapture(c,this){
        startupDashboard=initialDashboard;
        hudLayout=HudLayout::load(preferences);
        Kda::HudStyle::setStrength(preferences.value("appearance/hudSkinStrength",.65).toDouble());
        Kda::setGlassEnabled(preferences.value("appearance/frostedGlass",true).toBool());
        Kda::setSkin(preferences.value("appearance/skin",QStringLiteral("kda")).toString());Kda::setDepthEnabled(preferences.value("appearance/parallax",true).toBool());Kda::setReducedMotion(preferences.value("appearance/reducedMotion",false).toBool());
        autoClock.start();cardPool.setMaxThreadCount(1);loadingPool.setMaxThreadCount(1);modelPool.setMaxThreadCount(1);modelPool.setThreadPriority(QThread::LowPriority);loadingPool.setThreadPriority(QThread::LowPriority);input=parser.value(replay?"replay":"events");reportFile=parser.value("report");
        usageLiveSession=!demo&&!replay&&!parser.isSet("ui-browser")&&!parser.isSet("fixture-feed")&&!parser.isSet("inspection-window")&&!parser.isSet("diagnostic-ds")&&!parser.isSet("test-ds")&&!parser.isSet("self-check-ui")&&!parser.isSet("self-check-dashboard")&&!parser.isSet("self-check-browser")&&!parser.isSet("self-check-assistance");
        if(usageLiveSession)dsUsageMeter().configure(catalog.root);
        cardPool.setThreadPriority(QThread::LowPriority);
        if(!reportFile.isEmpty())reportWriter=std::make_unique<DiagnosticReportWriter>(reportFile,reportCollectionIntervalMs);
        reportRefreshTimer.setSingleShot(true);reportRefreshTimer.setTimerType(Qt::PreciseTimer);
        connect(&reportRefreshTimer,&QTimer::timeout,this,[this]{reportRefreshPending=false;if(!finishing&&!reportDrainStarted)writeReportNow();});
        executionReference.onUpdated=[this]{if(!finishing&&!coachResult.isEmpty()&&executionReference.apply(catalog,game,style->currentData().toString(),coachResult)){renderCoach();writeReport();}};
        liveSummaryEngine.setSynchronousInspection(demo||replay||parser.isSet("self-check-ui")||parser.isSet("self-check-dashboard")||parser.isSet("self-check-browser"));
        liveSummaryEngine.onUpdated=[this]{if(!finishing){refreshLiveHud();publishRuntimeSnapshot();}};
        if(!demo&&!replay&&!parser.isSet("ui-browser")){
#ifdef LIXIN_DISTRIBUTION_BUILD
            recommendationTrace=std::make_unique<RecommendationTrace>(catalog.root,RecommendationTrace::Detail::CardSurfaceOnly);
#else
            recommendationTrace=std::make_unique<RecommendationTrace>(catalog.root);
#endif
        }
        const auto localPython=qEnvironmentVariable("LIXIN_PYTHON",QDir::homePath()+QStringLiteral("/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe"));
        rosterBridge.configure(catalog.root,localPython);rosterAi.configureWebEvidence(catalog.root,localPython);
        ai.configureWebEvidence(catalog.root,localPython);forecast.configureWebEvidence(catalog.root,localPython);
        growthCapture.configure(localPython,!demo&&!replay&&!parser.isSet("ui-browser"));
        growthCapture.configureVisible(QJsonDocument::fromJson(preferences.value("visibleMatchInputs").toByteArray()).object());
        growthCapture.onUpdated=[this]{
            if(finishing||demo||replay||game.source!=GameState::Source::Live)return;
            const auto expiry=game.growthObservations.value("expires_at_ms").toDouble(-1);
            // New helper records join the next fresh native snapshot. Polling
            // them against the previous frame would intermittently reject a
            // newer game clock and invalidate a still-current recommendation.
            if(!game.fresh()||(expiry>=0&&QDateTime::currentMSecsSinceEpoch()>expiry))acceptLiveSnapshot(game);
        };
        rosterSettings=runtimeModelConnection(catalog.root);automaticSettings=rosterSettings;rosterDsConfigured=rosterSettings.isConfigured();
        rosterAi.configureModelConnection(rosterSettings);ai.configureModelConnection(automaticSettings);forecast.configureModelConnection(rosterSettings);
        settingsLoaded=true;settingsChecked=autoClock.elapsed();lastRosterDsEnabled=preferences.value("coach/rosterDsEnabled",true).toBool();
        right=new OverlayWindow(QStringLiteral("对局教练"),"right",preferences,parser.isSet("inspection-window"));
        for(int i=0;i<3;++i)cardHints.append(new CardHintWindow(i));
        deathTips=new DeathTipsWindow;
        auto coach=new QVBoxLayout(right->body);coach->setContentsMargins(8,4,8,4);coach->setSpacing(2);coach->setSizeConstraint(QLayout::SetNoConstraint);
        style=new QComboBox(right);style->setObjectName(QStringLiteral("growthModeCompatibility"));style->addItem(QStringLiteral("成长联动"),QStringLiteral("成长联动"));style->setEnabled(false);style->hide();
        assets=new HudAssets(catalog,this);
        shopStore=std::make_unique<ShopRecommendations::Store>(QDir(catalog.root).absoluteFilePath(QStringLiteral(".local/augment-panel/shop-recommendations-registration.json")));
        combatCorner=new CornerCombatHud(assets);
        deathTips->setIconResolver([this](const QString &kind,int id){return kind==QStringLiteral("champion")?assets->champion(id):assets->item(id);});
        phasePlate=new HudPhasePlate;coach->addWidget(phasePlate);
        champSelectView=new ChampSelectView(catalog,assets);coach->addWidget(champSelectView);champSelectView->hide();
        automaticHint=new CoachActionHud;coach->addWidget(automaticHint);purchaseList=new CoachPurchaseList(catalog,assets);coach->addWidget(purchaseList);
        componentsLabel=new CoachItemStrip(catalog,assets);coach->addWidget(componentsLabel);
        liveCounters=new CoachLiveHud(catalog,assets,true);coach->addWidget(liveCounters);
        rosterSummary=new CoachItemStrip(catalog,assets);rosterSummary->setObjectName(QStringLiteral("passiveRosterSummary"));coach->addWidget(rosterSummary);
        routeLabel=new CoachContextStrip;routeLabel->setObjectName("route");routeLabel->setMinimumWidth(0);QSizePolicy routePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);routePolicy.setRetainSizeWhenHidden(true);routeLabel->setSizePolicy(routePolicy);routeLabel->setFixedHeight(18);
        details=new QWidget;details->setMinimumSize(0,0);details->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Ignored);auto detailLayout=new QVBoxLayout(details);detailLayout->setContentsMargins(0,0,0,0);detailLayout->setSpacing(3);detailLayout->setSizeConstraint(QLayout::SetNoConstraint);
        compactEngine=new CoachEngineStrip(catalog,assets,true);compactEngine->setObjectName("compactEngine");coach->insertWidget(coach->indexOf(componentsLabel),compactEngine);compactEngine->hide();
        engineDetails=new CoachEngineStrip(catalog,assets,false);engineDetails->setObjectName("engineDetails");detailLayout->addWidget(engineDetails);
        summary=new CoachEconomyStrip;summary->setObjectName("summary");detailLayout->addWidget(summary);
        // Full reasoning remains available to reports and the match record.
        // The in-game HUD presents a single passive reading surface.
        advice=browser();advice->setParent(right);advice->hide();roster=browser();roster->setParent(right);roster->hide();
        rosterPage=new QWidget;rosterPage->setObjectName("rosterBody");rosterPage->setMinimumSize(0,0);rosterPage->installEventFilter(this);
        rosterInsights=new QLabel(rosterPage);rosterInsights->setObjectName("rosterInsights");rosterInsights->setFont(Kda::font(Kda::FontRole::Body,12));rosterInsights->setTextFormat(Qt::RichText);rosterInsights->setAlignment(Qt::AlignLeft|Qt::AlignVCenter);rosterInsights->setWordWrap(false);rosterInsights->setFixedHeight(36);rosterInsights->setMinimumWidth(0);
        liveRoster=new CoachLiveHud(catalog,assets,false,rosterPage);liveRoster->setDenseRoster(true);
        roster->setParent(rosterPage);roster->hide();liveRoster->onPlayer=[this](int row){if(game.fresh()&&row>=0&&row<game.players.size())editAugments(row);};
        assets->onChanged=[this]{champSelectView->update();automaticHint->update();componentsLabel->update();liveCounters->update();liveRoster->update();compactEngine->update();engineDetails->update();if(deathTips->isVisible())deathTips->update();if(combatCorner->isVisible())combatCorner->update();refreshPurchaseArtwork();if(cueIcon&&cueIcon->isVisible()){const int id=equipmentCueResult.value("latest").toObject().value("item_id").toInt();cueIcon->setPixmap(Kda::roundedPixmap(assets->item(id),QSize(16,16),Kda::radius("tag")-1,cueIcon->devicePixelRatioF()));}};
        detailLayout->addWidget(rosterPage,1);coach->addWidget(details,1);details->hide();
        auto footer=new QWidget;footer->setObjectName("hudFooter");footer->setFixedHeight(18);auto footerLayout=new QHBoxLayout(footer);footerLayout->setContentsMargins(0,0,0,0);footerLayout->setSpacing(4);cueIcon=new QLabel;cueIcon->setFixedSize(16,16);cueIcon->hide();footerLayout->addWidget(cueIcon);footerLayout->addWidget(routeLabel,1);
        coach->addWidget(footer);
        aiStatus=new QLabel(right->body);aiStatus->setObjectName("muted");aiStatus->hide();
        sourceStatus=new QLabel;sourceStatus->setObjectName("muted");sourceStatus->setMinimumWidth(0);sourceStatus->setWordWrap(false);detailLayout->addWidget(sourceStatus);
        advice->onHighlighted=[this](const QUrl &url){if(url.scheme()=="hud-item"){const int id=url.path().mid(1).toInt();QToolTip::showText(QCursor::pos(),catalog.itemName(id),advice);}else QToolTip::hideText();};
        roster->onAnchor=[this](const QUrl &url){if(url.scheme()=="player")editAugments(url.path().toInt());};
        connect(style,&QComboBox::currentTextChanged,this,[this]{invalidateAi();scheduleCardModel();scheduleModel(true);renderCardHints();});
        connect(&ai,&DeepSeek::status,this,[this](const QString &text){aiStatus->setText(text);writeReport();});
        connect(&ai,&DeepSeek::result,this,[this](const QJsonObject &plan,quint64 revision){if(revision!=aiRevision||!game.fresh()||requestBasis!=adviceBasis()||!overlayAdviceMatchesAction(plan,coachResult))return;aiPlan=plan;aiPending=false;aiStatus->setText(QStringLiteral("建议已自动更新 · %1:%2").arg(int(aiSnapshotTime)/60).arg(int(aiSnapshotTime)%60,2,10,QChar(u'0')));renderCoach();writeReport();});
        connect(&forecast,&DsForecast::changed,this,[this]{if(!finishing){renderCoach();writeReport();}});
        connect(&ai,&DeepSeek::settled,this,[this](quint64 revision,bool success,bool retryable){if(revision!=aiRevision)return;aiPending=false;autoAdvice.complete(success,retryable,autoClock.elapsed());if(!success){lastAiFailure=aiStatus->text();aiConfigurationBlocked=ai.configurationBlocked();aiStatus->setText(retryable?QStringLiteral("本地建议已更新 · 稍后自动补充分析"):QStringLiteral("DS 暂不可用 · 本地建议自动更新"));if(!demo&&!replay&&!runningUiChecks)Kda::postNotice({QStringLiteral("analysis-availability"),QStringLiteral("扩展分析暂不可用"),QStringLiteral("本地建议继续更新；扩展分析稍后重试。"),Kda::NoticeKind::Warning,6000});}renderCoach();writeReport();if(parser.isSet("test-ds")&&(success||!retryable||autoAdvice.dispatches()>=2))QTimer::singleShot(1200,this,[this]{finish();});});
        connect(&rosterBridge,&RosterAnalysisBridge::result,this,[this](const QJsonObject &result){
            if(result.value("context_id")!=game.matchContext.value("context_id"))return;
            const bool changed=rosterHistoryEvidenceBasis(result)!=rosterHistoryEvidenceBasis(anonymousHistory);
            anonymousHistory=result;
            if(changed){++rosterHistoryRevision;game.recommendationEvidence=buildRecommendationHistory(catalog,game,anonymousHistory);scheduleCardModel();scheduleModel(true);}
            updateRosterAdvice();renderCoach();writeReport();
        });
        connect(&live,&LiveClient::privateRosterChanged,this,[this](QJsonObject payload){
            if(payload.isEmpty()){state.bindLiveMatch({},{});invalidateOfferResults();rosterBridge.invalidate();privateHistoryBasis.clear();anonymousHistory={};game.recommendationEvidence={};loadingPreparation={};loadingComputedBasis.clear();scheduleCardModel();scheduleModel(true);renderCards();renderCoach();writeReport();return;}
            if(replay||demo||payload.value("context_id")!=game.matchContext.value("context_id")||game.gameMode!=QStringLiteral("KIWI"))return;
            bindDecisionArchive(payload);
            if(state.bindLiveMatch(payload.value("context_id").toString(),offerMatchRefForGameId(payload.value("game_id"))))invalidateOfferResults();
            auto stable=payload;stable.remove("as_of_ms");const auto basis=QJsonDocument(stable).toJson(QJsonDocument::Compact);if(basis==privateHistoryBasis)return;privateHistoryBasis=basis;anonymousHistory={};game.recommendationEvidence={};loadingPreparation={};loadingComputedBasis.clear();++rosterHistoryRevision;rosterAi.invalidate();++rosterAiRevision;rosterAiPending=false;rosterAiPlan={};rosterBridge.request(payload);scheduleCardModel();scheduleModel(true);updateRosterAdvice();renderCoach();
        });
        connect(&rosterAi,&DeepSeek::matchAnalysis,this,[this](const QJsonObject &plan,quint64 revision){if(revision!=rosterAiRevision||rosterRequestBasis!=rosterBasis()||plan.value("context_id")!=game.matchContext.value("context_id"))return;rosterAiPlan=plan;renderCoach();writeReport();});
        connect(&rosterAi,&DeepSeek::matchSettled,this,[this](quint64 revision,bool success,bool retryable){if(revision!=rosterAiRevision)return;rosterAiPending=false;rosterConfigurationBlocked=rosterAi.configurationBlocked();rosterAiState=success?QStringLiteral("ready"):QStringLiteral("local_only");if(!success&&retryable&&rosterAiDispatches<3){rosterRetryAt=autoClock.elapsed()+30000;rosterAttemptedBasis.clear();}renderCoach();writeReport();});
        right->onClose=[this]{finish();};right->onReset=[this]{resetHudLayout();};right->onThrough=[this](bool value){setThrough(value);};
        right->onHotkey=[this](int id){if(id==0x4c59)setThrough(!through);else if(id==0x4c58)setUserHidden(!hiddenByUser);else if(id==0x4c57)resetHudLayout();else if(id==0x4c56)showDashboard();else if(id==0x4c5a){if(!dashboard)createDashboard();dashboard->showReplay();showDashboard();}};
        connect(&loadingWorker,&QFutureWatcher<QJsonObject>::finished,this,[this]{
            auto completion=loadingCompletion.consume();if(!completion)return;
            const auto result=completedJsonFuture(loadingWorker);
            const bool reusable=result.available()&&!result.interrupted();
            const auto basis=loadingPreparationBasis(catalog,game,style->currentData().toString());
            if(reusable&&!basis.isEmpty()&&basis==loadingWorkerBasis){loadingPreparation=result.value;loadingComputedBasis=basis;failedLoadingBasis.clear();loadingRetryAt=0;++loadingCompleted;}
            else{++loadingDiscarded;loadingPreparation={};loadingComputedBasis.clear();if(!reusable){failedLoadingBasis=loadingWorkerBasis;loadingRetryAt=autoClock.elapsed()+futureFailureRetryMs;}}
            renderCoach();writeReport();completion.release();if(!basis.isEmpty()&&basis!=loadingComputedBasis){
                const auto delay=basis==failedLoadingBasis?std::max(qint64(0),loadingRetryAt-autoClock.elapsed()):0;
                if(delay>0)QTimer::singleShot(int(delay),this,[this]{if(!finishing)scheduleLoadingPreparation();});else scheduleLoadingPreparation();
            }
        });
        connect(&worker,&QFutureWatcher<QJsonObject>::finished,this,[this]{
            auto completion=modelCompletion.consume();if(!completion)return;
            lastModelElapsedMs=std::max(qint64(0),autoClock.elapsed()-workerStartedMs);maxModelElapsedMs=std::max(maxModelElapsedMs,lastModelElapsedMs);++modelCompletions;lastModelCompletedAt=QDateTime::currentDateTimeUtc();
            const auto completed=completedJsonFuture(worker);const auto result=completed.value;auto resultCoach=result.value("coach").toObject();
            if(!completed.available())resultCoach=QJsonObject{{"status","interrupted"},{"computation_complete",false},{"stop_reason",completed.error}};
            const auto resultPlan=resultCoach.value("plan").toObject();
            lastModelStopReason=resultCoach.value("stop_reason").toString();lastModelStageTimings=resultCoach.value("stage_timings_ms").toObject();modelWork.reset();
            auto publication=CoachRefreshPolicy::publication(workerStrategy,modelBasis(),workerSoftBasis,coachSoftModelBasis(game),workerStartedMs,autoClock.elapsed()+workerInputAgeMs,game.fresh(),modelRulesUsable(catalog,game),workerSnapshotGameTime,game.gameTime,resultPlan.value("mechanic_valid_until").toDouble(-1));
            if(resultPlan.value("mechanic_snapshot_time").toDouble(workerSnapshotGameTime)>game.gameTime){publication["accepted"]=false;publication["reason"]="mechanic_clock_rewound";}
            if(workerTicket!=modelTicket&&publication.value("accepted").toBool()){publication["accepted"]=false;publication["reason"]="request_invalidated";}
            if(game.source==GameState::Source::Live&&workerInputAgeMs+lastModelElapsedMs>=4000){publication["accepted"]=false;publication["reason"]="worker_snapshot_expired";}
            const bool incomplete=!resultCoach.value("computation_complete").toBool()||resultCoach.value("status")==QStringLiteral("interrupted");
            if(incomplete){publication["accepted"]=false;publication["reason"]=lastModelStopReason.isEmpty()?QStringLiteral("incomplete_computation"):lastModelStopReason;}
            if(runtimeLog)runtimeLog->record(RuntimeLog::Event::ModelJob,inferenceLogOutcome(publication.value("accepted").toBool(),completed.available(),!incomplete,lastModelStopReason),lastModelElapsedMs);
            if(publication.value("accepted").toBool()){
                purchaseChoiceEvidence=resultCoach.take("local_choice_evidence").toObject();
                coachResult=resultCoach;coachResultContext=modelWorkerContext;context=result.value("context").toObject();computedStrategy=workerStrategy;computedSoftBasis=workerSoftBasis;publishedSnapshotMs=workerStartedMs;publishedInputAgeMs=workerInputAgeMs;publishedSnapshotGameTime=workerSnapshotGameTime;
                publication["snapshot_taken_at"]=workerSnapshotAt.toString(Qt::ISODateWithMs);publication["source_received_at"]=workerSourceReceivedAt.toString(Qt::ISODateWithMs);publication["maximum_retained_age_ms"]=CoachRefreshPolicy::maximumRetainedAgeMs;coachResult["model_refresh"]=publication;
                refreshCoachSnapshot(catalog,game,coachResult);coachResult["requested_combat_style"]=style->currentData().toString();annotateCoachRecommendationQuality(catalog,game,coachResult);refreshAiEconomy();allowedItems=listIds(result.value("items").toArray());allowedAugments=listIds(result.value("augments").toArray());modelState=coachResult.value("status")==QStringLiteral("input_required")?QStringLiteral("input_required"):QStringLiteral("ready");consecutiveModelTimeouts=0;failedModelBasis.clear();modelRetryAt=0;dirty=dirty||publication.value("soft_inputs_changed").toBool();renderCards();renderCoach();writeReport();updateAutomaticAdvice();
            }else{++modelDiscarded;lastModelRejection=publication.value("reason").toString();modelRejections[lastModelRejection]=modelRejections.value(lastModelRejection).toInt()+1;dirty=true;
                if(incomplete){coachResult={};context={};allowedItems.clear();allowedAugments.clear();computedStrategy.clear();computedSoftBasis.clear();publishedSnapshotMs=-1;invalidateAi();modelState=lastModelStopReason==QStringLiteral("time_budget_exceeded")?QStringLiteral("time_budget_exceeded"):QStringLiteral("waiting");if(lastModelStopReason==QStringLiteral("time_budget_exceeded")){
                    consecutiveModelTimeouts=failedModelBasis==workerStrategy?consecutiveModelTimeouts+1:1;
                    failedModelBasis=workerStrategy;modelRetryAt=autoClock.elapsed()+std::min(8000,1000*(1<<std::min(3,consecutiveModelTimeouts-1)));
                }else{failedModelBasis=workerStrategy;modelRetryAt=autoClock.elapsed()+250;}renderCoach();writeReport();}}
            if(recommendationTrace&&!demo&&!replay)recommendationTrace->recordAttempt(modelWorkerContext,QJsonObject{{"request_id",qint64(workerTicket)},{"game_time",workerSnapshotGameTime},{"input_age_ms",workerInputAgeMs},{"elapsed_ms",lastModelElapsedMs},{"stop_reason",lastModelStopReason},{"rejection_reason",publication.value("reason")},{"status",resultCoach.value("status")},{"publication_accepted",publication.value("accepted")},{"stage_timings_ms",lastModelStageTimings}});
            // A newer panel snapshot coalesces into one subsequent job. It does
            // not revoke the just-published matching equipment decision.
            completion.release();
            if(dirty){dirty=false;if(autoClock.elapsed()<modelRetryAt)QTimer::singleShot(int(modelRetryAt-autoClock.elapsed()),this,[this]{if(!finishing)scheduleModel(false);});else scheduleModel(false);}
        });
        connect(&cardWorker,&QFutureWatcher<QJsonObject>::finished,this,[this]{
            auto completion=cardCompletion.consume();if(!completion)return;
            if(finishing){cardWork.reset();completion.release();return;}
            lastCardElapsedMs=std::max(qint64(0),autoClock.elapsed()-cardStartedMs);maxCardElapsedMs=std::max(maxCardElapsedMs,lastCardElapsedMs);
            const auto completed=completedJsonFuture(cardWorker);const auto result=completed.value;++cardCompletions;
            const bool cancelled=cardWork&&cardWork->stopReason()==QStringLiteral("cancelled");
            cardWork.reset();lastCardStopReason=cancelled?QStringLiteral("cancelled"):result.value("stop_reason").toString();
            const bool computationComplete=result.value("computation_complete").toBool(false);
            const bool clockCurrent=coachCardClockCurrent(result.value("mechanic_cache_validity").toObject(),game.gameTime);
            const bool accepted=completed.available()&&!cancelled&&computationComplete&&cardWorkerContext==game.matchContext.value("context_id").toString()&&cardWorkerBasis==cardBasis()&&clockCurrent&&game.fresh()&&modelRulesUsable(catalog,game)&&state.status=="open"&&offersBoundTo(game)&&(game.source!=GameState::Source::Live||cardInputAgeMs+lastCardElapsedMs<4000);
            if(runtimeLog)runtimeLog->record(RuntimeLog::Event::CardJob,inferenceLogOutcome(accepted,completed.available(),computationComplete,lastCardStopReason),lastCardElapsedMs);
            if(accepted){
                cardDecision=result;augmentChoiceEvidence=cardDecision.take("local_choice_evidence").toObject();cardResultContext=cardWorkerContext;computedCardBasis=cardWorkerBasis;failedCardBasis.clear();cardRetryAt=0;cardCompletedAt=QDateTime::currentDateTimeUtc();renderCardHints();cardRenderedAtMs=autoClock.elapsed();writeReport();
            }else{++cardDiscarded;failedCardBasis=cardWorkerBasis;cardRetryAt=autoClock.elapsed()+futureFailureRetryMs;}
            completion.release();
            const auto delay=cardBasis()==failedCardBasis?std::max(qint64(0),cardRetryAt-autoClock.elapsed()):0;
            if(delay>0)QTimer::singleShot(int(delay),this,[this]{if(!finishing)scheduleCardModel();});else scheduleCardModel();
        });
        connect(&live,&LiveClient::snapshotChanged,this,[this](const GameState &snapshot){
            acceptLiveSnapshot(snapshot);
        });
        connect(&live,&LiveClient::connectionChanged,this,[this](const QString &text){connection=text;if(!game.fresh())renderCoach();publishRuntimeSnapshot();});
        if(demo){game=makeCoachDemo(catalog);QJsonArray cards;for(int id:game.offers)cards.append(QJsonObject{{"candidate_index",cards.size()},{"augment_id",id},{"augment_name",catalog.augments.value(id).value("name_zh_cn")},{"rerollable",true}});
            state.apply(QJsonObject{{"schema_version",1},{"source","wegame_local_log"},{"capture_mode","replay"},{"observed_at",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},{"session_ref","synthetic-demo"},{"kind","offer"},{"game_mode","KIWI"},{"pick_count",2},{"candidates",cards}});
        }else if(replay){QFile file(input);if(file.open(QIODevice::ReadOnly)&&file.size()<16*1024*1024)for(const auto &line:file.readAll().split('\n')){const auto event=QJsonDocument::fromJson(line).object();PanelState check(true);if(event.value("kind")=="offer"&&check.apply(event))samples.append(event);}nextReplay();}
        else live.start();
        if(parser.isSet("dashboard")||parser.isSet("self-check-dashboard")||startupDashboard)createDashboard();
        if(initialDashboard&&!parser.isSet("dashboard")&&!parser.isSet("self-check-dashboard")&&!parser.isSet("self-check-startup")&&dashboard)dashboard->hide();
        refreshSourceStatus();rememberOffers();dock(false);registerKeys();renderCards();renderCoach();if(!parser.isSet("self-check-assistance")){scheduleCardModel();scheduleModel(true);}writeReport();
        if(parser.isSet("ui-browser"))createUiBrowser();
        if(parser.isSet("query-summoner"))QTimer::singleShot(100,this,[this]{showHistoryQuery();dashboard->querySummoner(parser.value("query-summoner"));});
        connect(&pollTimer,&QTimer::timeout,this,[this]{++pollCallbacks;if(!demo&&!replay&&!uiBrowser&&!runningUiChecks)dock(false);poll();if(!uiBrowser&&state.expire()){invalidateOfferResults();scheduleModel(true);renderCards();}
            if(!demo&&!replay&&!game.fresh()&&coachResult.value("fresh").toBool()){if(modelWork)modelWork->cancel();++modelTicket;coachResult={};context={};invalidateAi();renderCoach();}
            updateAutomaticAdvice();updateRosterAdvice();if(autoClock.elapsed()-lastMaintenanceMs>=1000){lastMaintenanceMs=autoClock.elapsed();refreshPublishedCoach();refreshShopRecommendations();
                const bool planningEnabled=deepSeekPlanningEnabled();
                forecast.observe(game,rosterSettings.apiKey,rosterSettings.model,planningEnabled,
                                 planningEnabled?currentLocalChoicePacket():QJsonObject{});
                scheduleModel(false);refreshSourceStatus();renderSourceLabels();refreshLiveHud();applyPresentation();renderAutomaticAdvice();dock(false);maybeMinimizeDashboard();
                if(needsFastPoll()||lastMaintenanceMs-lastPeriodicReportMs>=3000){lastPeriodicReportMs=lastMaintenanceMs;writeReport();}
                else publishRuntimeSnapshot();
            }
            renderCardHints();syncPollCadence();
        });
        syncPollCadence();pollTimer.start();
        if(parser.isSet("self-check-assistance")){pollTimer.stop();QTimer::singleShot(200,this,[this]{assistanceSimulation();});}
        if(parser.isSet("snapshot"))QTimer::singleShot(1800,this,[this]{snapshot(parser.value("snapshot"));writeReport();});
        if(parser.isSet("self-check-ui"))QTimer::singleShot(2200,this,[this]{waitForUiFixture();});
        if(parser.isSet("self-check-dashboard"))QTimer::singleShot(2600,this,[this]{inspectDashboard();});
    }
    ~DualPanels()override{if(shopStore&&!demo&&!replay&&!uiBrowser)shopStore->cleanup();if(modelWork)modelWork->cancel();if(cardWork)cardWork->cancel();executionReference.invalidate();unregisterKeys();forecast.invalidate();ai.invalidate();rosterAi.invalidate();rosterBridge.invalidate();if(worker.isRunning())worker.waitForFinished();if(cardWorker.isRunning())cardWorker.waitForFinished();if(loadingWorker.isRunning())loadingWorker.waitForFinished();delete uiBrowser;delete tray;delete dashboard;qDeleteAll(cardHints);delete deathTips;delete combatCorner;delete aiStatus;delete right;}
    void drainReports(){
        if(reportDrainStarted)return;
        reportDrainStarted=true;
        const bool collectLatest=reportRefreshPending||!finishing;
        reportRefreshTimer.stop();reportRefreshPending=false;
        // app.quit() can bypass finish(), and its timer will no longer run.
        // Collect while the widgets still exist, before closing the writer.
        if(collectLatest){publishRuntimeSnapshot();writeReportNow();}
        if(reportWriter){reportWriter->shutdown(true);if(!reportWriter->waitForFinished(2000))std::fprintf(stderr,"Diagnostic report drain exceeded 2000 ms; final snapshot is not confirmed written.\n");}
    }
    void showDashboard(){
        if(finishing)return;
        if(uiBrowser){reloadUiBrowserAppearance();uiBrowser->showNormal();uiBrowser->raise();uiBrowser->activateWindow();return;}
        if(!dashboard)createDashboard();
        if(dashboard->isMinimized())dashboard->setWindowState(dashboard->windowState()&~Qt::WindowMinimized);
        dashboard->show();dashboard->raise();dashboard->activateWindow();
    }
    void finish(){if(dashboardInspectionRunning){dashboardInspectionExitPending=true;return;}if(finishing)return;finishing=true;if(modelWork)modelWork->cancel();if(cardWork)cardWork->cancel();executionReference.invalidate();if(parser.isSet("snapshot")){if(parser.isSet("query-summoner")){showHistoryQuery();}QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);snapshot(parser.value("snapshot"));}writeReport();preferences.setValue("right/position",right->pos());preferences.setValue("right/anchor",QStringLiteral("bottom-left"));if(compactAutoFold)preferences.setValue("right/folded",rightUserFolded);preferences.sync();right->onClose={};for(auto hint:cardHints)hint->close();deathTips->close();combatCorner->close();right->close();qApp->quit();}
    void writeReport(){
        if(reportDrainStarted)return;
        publishRuntimeSnapshot();
        // Coalesce before expensive GUI collection, using the writer's cadence.
        // Finishing and inspection fixtures must capture their latest state now.
        if(!finishing&&!runningUiChecks&&!demo&&!replay&&!uiBrowser){
            if(reportRefreshPending)return;
            reportRefreshPending=true;
            reportRefreshTimer.start(int(std::max(qint64(0),reportNextCollectionAtMs-autoClock.elapsed())));
            return;
        }
        reportRefreshTimer.stop();reportRefreshPending=false;
        writeReportNow();
    }
    QJsonObject runtimePresentationSnapshot(){
        // Anonymous runtime facts only. Presentation/replay publication must not
        // wait for diagnostic geometry collection, serialization or disk cadence.
        auto value=state.json();
        value["connection"]=connection;value["live_phase"]=game.phase;value["live_data_fresh"]=game.fresh();
        value["current_champion_id"]=game.championId;value["current_level"]=game.level;value["game_time"]=game.gameTime;
        value["current_players"]=game.players;value["current_game_mode"]=game.gameMode;value["current_queue_id"]=game.queueId;
        value["match_context"]=game.matchContext;value["coach"]=coachResult;value["model_state"]=modelState;
        value["champ_select"]=champSelectInput();value["shop_recommendations"]=shopStatus;
        value["live_combat_summary"]=liveSummary;value["anonymous_player_history"]=anonymousHistory;
        value["observed_patch"]=game.observedPatch;value["version_match"]=modelPatchMatches(catalog,game);
        value["rule_basis"]=modelRuleBasis(catalog,game);
        value["synthetic_game_state"]=demo;value["synthetic_file_feed_test"]=parser.isSet("fixture-feed");
        value["augment_source"]=sourceHealth;value["growth_capture"]=growthCapture.report();
        value["observed_purchase_facts"]=game.fresh()&&purchaseFacts.value("context_id")==game.matchContext.value("context_id")?purchaseFacts:QJsonObject{};
        value["adaptive_purchase_presentation"]=automaticHint->property("adaptivePurchasePresentation").value<QJsonObject>();
        value["purchase_matchup"]=automaticHint->property("purchaseMatchup").value<QJsonObject>();
        value["purchase_checklist"]=checklistState;value["match_outlook"]=observedMatchOutlook(catalog,game);
        value["equipment_cues"]=equipmentCueResult;value["deepseek_usage"]=dsUsageMeter().summary(usageDisplayContext);
        value["automatic_match_analysis"]=QJsonObject{{"analysis",rosterAiPlan}};
        value["selection_overlay"]=QJsonObject{{"current_model",cardModelCurrent()},
            {"awaiting_complete_offer",incompleteOfferCount>=0},{"received_card_count",incompleteOfferCount<0?QJsonValue():QJsonValue(incompleteOfferCount)}};
        value["offer_revision"]=qint64(offerBindingRevision);
        value["offer_is_reroll"]=offerIsReroll;
        value["shown_purchase_item_id"]=right->isVisible()&&automaticHint->isVisibleTo(right)
            ?automaticHint->property("shownItemId").toInt():0;
        QJsonArray hints;for(auto hint:cardHints){if(!hint->isVisible())continue;const auto source=hint->report();QJsonObject row;
            for(const auto key:{"augment_id","partial","metric","numeric_gain_available","numeric_source","recommendation","recommended"})
                if(source.contains(key))row[key]=source.value(key);
            row["metric"]=source.value("displayed_metric");
            if(source.value("explanation_visible").toBool()){
                row["explanation"]=source.value("displayed_explanation");
                row["explanation_source"]=QStringLiteral("local_displayed_mechanism");
            }
            hints.append(row);
        }value["card_hints"]=hints;
        if(game.source==GameState::Source::Live&&game.fresh()){
            if(const auto items=observedOwnInventory(game))value["own_inventory"]=jsonIds(*items);
            value["selected_complete"]=coachPurchaseInputReadiness(catalog,game).value("ready");
            QJsonArray selected;for(int id:game.selected)selected.append(id);value["confirmed_selected"]=selected;
            for(const auto &entry:game.players)if(entry.toObject().value("self")==QJsonValue(true)){
                const auto origin=entry.toObject().value("augment_source").toString();
                if(origin==QStringLiteral("manual_confirmation")||origin==QStringLiteral("visible_self_hud_tooltip"))value["selection_source"]=origin;
                break;
            }
        }
        return value;
    }
    void publishRuntimeSnapshot(){
        if(!dashboard&&!decisionReplay)return;
        const auto value=runtimePresentationSnapshot();
        if(value==lastRuntimeSnapshot)return;
        lastRuntimeSnapshot=value;
        if(decisionReplay)decisionReplay->observe(value);
        if(dashboard)dashboard->setSnapshot(value);
    }
    void writeReportNow(){
        reportNextCollectionAtMs=autoClock.elapsed()+reportCollectionIntervalMs;
        const bool hadCoach=!coachResult.isEmpty();
        refreshPublishedCoach();
        if((hadCoach&&coachResult.isEmpty())||(!game.fresh()&&(!checklistState.isEmpty()||!purchaseFacts.isEmpty())))
            renderAutomaticAdvice(false);
        updateUiBrowserExplanation();
        // The dashboard and replay consume the independently published runtime
        // snapshot. Diagnostic geometry has no consumer without a writer.
        if(!reportWriter)return;
#ifdef LIXIN_DISTRIBUTION_BUILD
        const QJsonObject supportReport{
            {"report_kind", "distribution_support"},
            {"catalog_patch", catalog.patch},
            {"synthetic_game_state", demo},
            {"visible", right->isVisible()},
            {"live_data_fresh", game.fresh()},
            {"live_phase", game.phase},
            {"card_hint_count", cardHints.size()},
            {"automatic_match_analysis", QJsonObject{{"configured", rosterDsConfigured}}},
            {"local_model_timing", QJsonObject{{"completed", modelCompletions}, {"last_elapsed_ms", lastModelElapsedMs}}},
        };
        reportWriter->submit(++reportSequence, supportReport);
#else
        if(right->body->layout())right->body->layout()->activate();auto out=state.json();out["visible"]=right->isVisible();out["window_count"]=3+cardHints.size();out["combat_corner_hud"]=combatCorner->report();out["death_tips"]=deathTips->report();out["death_tips_rotation_ms"]=8000;out["death_tips_inline"]=QJsonObject{{"placement","existing_hud_footer"},{"standalone_live_window",false},{"insight",inlineDeathInsight}};out["match_outlook"]=observedMatchOutlook(catalog,game);out["purchase_checklist"]=checklistState;out["purchase_checklist_view"]=purchaseList->report();out["hud_phase"]=phasePlate->report();out["shop_capability"]=QJsonObject{{"live_inventory",true},{"live_gold",true},{"live_shop_catalog",false},{"actual_shop_prices",false},{"adapter","documented_Riot_LiveClientData"},{"custom_item_sets_supported_by_game",true},{"live_item_set_hot_reload_verified",false},{"shop_write_implemented",true}};out["observed_purchase_facts"]=game.fresh()&&purchaseFacts.value("context_id")==game.matchContext.value("context_id")?purchaseFacts:QJsonObject{};out["shop_recommendations"]=shopStatus;out["champ_select"]=champSelectView->report();out["client_window"]=clientWindowTracker.state().report();out["selection_observation_fresh"]=championSelectCurrent();out["main_panel_count"]=1;out["left_panel_removed"]=true;out["card_hint_count"]=cardHints.size();out["hidden_by_user"]=hiddenByUser;out["right_panel"]=right->report();out["safe_layout"]=overlayPlacementJson(placement);auto actual=placement;actual.right=right->geometry();QList<QRect> visibleWindows;if(right->isVisible())visibleWindows.append(right->geometry());if(deathTips->isVisible())visibleWindows.append(deathTips->geometry());if(combatCorner->isVisible())visibleWindows.append(combatCorner->geometry());const auto gameplayWindows=visibleWindows;bool selectionGeometrySafe=true;for(int i=0;i<cardHints.size();++i)if(cardHints[i]->isVisible()){const auto geometry=cardHints[i]->geometry();visibleWindows.append(geometry);selectionGeometrySafe=selectionGeometrySafe&&selectionActive&&cardPlacement.valid&&geometry==cardPlacement.hints[i]&&viewport.contains(geometry);}out["actual_windows_avoid_protected_regions"]=visibleOverlaysAvoidAreas(gameplayWindows,actual.protectedAreas)&&selectionGeometrySafe;
        out["runtime_poll"]=QJsonObject{{"elapsed_ms",autoClock.elapsed()},{"interval_ms",pollTimer.interval()},{"callbacks",pollCallbacks},{"event_file_open_attempts",eventFileOpenAttempts},{"card_layout_builds",cardLayoutBuilds},{"card_hint_builds",cardHintBuilds}};
        bool dead=false;for(const auto &v:game.players)if(v.toObject().value("self").toBool())dead=v.toObject().value("is_dead").toBool();
        const auto shop=shopReferenceArea(viewport);out["overlay_safety"]=QJsonObject{{"all_visible_windows_checked",true},{"selection_uses_dedicated_card_region_policy",selectionActive},{"selection_hint_geometry_valid",selectionGeometrySafe},{"visible_window_count",visibleWindows.size()},{"shop_reference",QJsonArray{shop.x(),shop.y(),shop.width(),shop.height()}},{"shop_open_detection_available",false},{"shop_reference_avoided",visibleOverlaysAvoidAreas(gameplayWindows,{shop})},{"death_shop_protection_active",game.fresh()&&dead&&!selectionActive},{"shop_reserved_while_alive",true},{"scoreboard_hold_suppressed",scoreboardHeld},{"scope","reference_layout_with_default_tab_hold_custom_positions_unobserved"}};
        QJsonArray labels;for(auto hint:cardHints)labels.append(hint->report());out["card_hints"]=labels;auto cardReport=selectionCardLayoutJson(cardPlacement);cardReport["active"]=selectionActive;cardReport["current_model"]=cardModelCurrent();cardReport["game_foreground"]=gameForeground;cardReport["temporary_right_hidden"]=rightAvoidingCards;cardReport["side_panels_avoid_reference_cards"]=sidePanelsAvoidCards();cardReport["automatic"]=true;cardReport["numeric_source"]="current_local_model";cardReport["ds_changes_card_labels"]=false;
        cardReport["awaiting_complete_offer"]=incompleteOfferCount>=0;
        cardReport["received_card_count"]=incompleteOfferCount<0?QJsonValue():QJsonValue(incompleteOfferCount);
        out["selection_overlay"]=cardReport;
        auto layoutReport=hudLayout.json();layoutReport["editing"]=hudEditorOpen;out["hud_layout"]=layoutReport;
        out["rule_basis"]=modelRuleBasis(catalog,game);
        if(!hudLayout.empty()){
            auto safety=out["overlay_safety"].toObject();safety["custom_layout_active"]=true;safety["scope"]="explicit_user_layout_clamped_to_game_viewport";
            const bool customCards=hudLayout.contains("card1")||hudLayout.contains("card2")||hudLayout.contains("card3");
            safety["selection_uses_dedicated_card_region_policy"]=selectionActive&&!customCards;
            safety["shop_reserved_while_alive"]=!hudLayout.contains("coach")&&!hudLayout.contains("combat");out["overlay_safety"]=safety;
            auto layout=out["safe_layout"].toObject();layout["custom_coach_bounds"]=hudLayout.contains("coach");layout["scope"]="user_overrides_are_explicit_and_viewport_bounded";if(hudLayout.contains("coach"))layout["anchor"]="custom";out["safe_layout"]=layout;
        }
        out["right_tabs"]=QJsonArray{};out["passive_hud"]=passiveHudReport();
        out["passive_living_hud"]=livingCompactHud();out["automatic_click_through"]=livingCompactHud()&&!parser.isSet("inspection-window");out["details_expanded"]=detailsExpanded;out["default_action_card"]=true;out["unvalidated_probability_visible"]=false;out["action_card"]=QJsonObject{{"route",growthPlanSummary(coachResult.value("plan").toObject())},{"footer_text",routeLabel->property("fullRoute").toString()},{"route_icon_only",routeLabel->property("iconOnly").toBool()},{"footer_live_kda_and_clock",routeLabel->property("liveContext").toBool()},{"details_controls_visible",false},{"buyable_components",componentsLabel->text()},{"execution_control_visible",false},{"hud",automaticHint->report()},{"components",componentsLabel->report()},{"live_counters",liveCounters->report()}};
        out["adaptive_purchase_presentation"]=automaticHint->property("adaptivePurchasePresentation").value<QJsonObject>();
        out["purchase_matchup"]=automaticHint->property("purchaseMatchup").value<QJsonObject>();
        out["synthetic_adaptive_purchase_ui_states"]=automaticHint->property("adaptivePurchaseUiStates").value<QJsonArray>();
        out["canonical_events_accepted"]=accepted;out["replay_samples_available"]=samples.size();out["independent_renderer"]=true;out["network_client_linked"]=true;out["ocr_used"]=false;out["vendor_sdk_used"]=false;out["ui_fixture_model_evidence"]=uiFixtureModelEvidence;
        out["augment_source"]=sourceHealth;out["card_overlay_recent_states"]=cardTransitions;out["card_overlay_trace_limit"]=128;
        out["card_model_timing"]=QJsonObject{{"completed",cardCompletions},{"discarded",cardDiscarded},{"last_elapsed_ms",lastCardElapsedMs},{"maximum_elapsed_ms",maxCardElapsedMs},{"worker_running",cardWorker.isRunning()},{"completion_pending",cardCompletion.pending()},{"last_completed_at",cardCompletedAt.toString(Qt::ISODateWithMs)},{"waits_for_build_search",false},{"dedicated_worker",true},{"event_poll_ms",50},{"offer_received_at",offerReceivedAt.toString(Qt::ISODateWithMs)},{"source_collected_at",sourceCollectedAt.toString(Qt::ISODateWithMs)}};
        out["card_decision"]=cardDecision;
        out["card_reference_cache"]=cardReferenceCache.statistics();
        out["known_model_ui_control"]=knownModelUiControl;out["recommendation_history"]=game.recommendationEvidence;out["loading_preparation"]=loadingPreparation;out["loading_preparation_worker"]=QJsonObject{{"running",loadingWorker.isRunning()},{"completion_pending",loadingCompletion.pending()},{"completed",loadingCompleted},{"discarded",loadingDiscarded},{"live_result_reuse_allowed",false}};
        if(uiBrowser){out["ui_browser"]=uiBrowser->report();out["ui_browser_checks"]=browserChecks;out["ui_browser_states"]=browserStates;out["ui_browser_isolation"]=QJsonObject{{"synthetic_only",demo},{"live_client_started",false},{"paid_analysis_allowed",false},{"global_hotkeys_registered",hotkey},{"settings_file",preferences.fileName()},{"separate_process",true}};}
        out["local_model_timing"]=QJsonObject{{"completed",modelCompletions},{"discarded",modelDiscarded},{"last_elapsed_ms",lastModelElapsedMs},{"maximum_elapsed_ms",maxModelElapsedMs},{"last_completed_at",lastModelCompletedAt.toString(Qt::ISODateWithMs)},{"worker_running",worker.isRunning()},{"completion_pending",modelCompletion.pending()},{"last_rejection",lastModelRejection},{"rejections_by_reason",modelRejections},{"pending_latest_snapshot",dirty},{"minimum_soft_dispatch_ms",CoachRefreshPolicy::minimumSoftDispatchMs},{"periodic_refresh_ms",CoachRefreshPolicy::periodicRefreshMs},{"maximum_publish_age_ms",CoachRefreshPolicy::maximumPublishAgeMs},{"maximum_retained_age_ms",CoachRefreshPolicy::maximumRetainedAgeMs},{"published_snapshot_age_ms",coachResult.value("model_refresh").toObject().value("snapshot_age_ms")},{"work_budget_ms",modelAssignedBudgetMs},{"maximum_work_budget_ms",modelBudgetMs},{"last_stop_reason",lastModelStopReason},{"same_basis_timeouts",consecutiveModelTimeouts},{"stopped_job_restarted",false},{"last_stage_timings_ms",lastModelStageTimings},{"dedicated_worker",true},{"thread_priority","low"},{"computation_scope","actual_inventory_and_legal_next_purchase_only"},{"retry_wait_ms",std::max(qint64(0),modelRetryAt-autoClock.elapsed())}};
        out["live_data_requires_wegame_coach_logs"]=!state.replay;out["synthetic_file_feed_test"]=parser.isSet("fixture-feed");out["synthetic_game_state"]=demo;out["inspection_window"]=parser.isSet("inspection-window");out["own_process_id"]=qint64(QCoreApplication::applicationPid());
        out["game_window_found"]=gameWindowFound;out["game_window"]=gameWindowTracker.state().report();out["game_window_tracking_ms"]=50;out["in_game_overlay_verified"]=false;out["model_state"]=modelState;out["coach"]=coachResult;out["history_query"]=dashboard?dashboard->playerReport():QJsonObject{{"surface","main_window"},{"state","idle"},{"sent_to_ai",false}};out["ai_plan"]=aiPlan;out["deepseek_test_requested"]=dsTestStarted;out["deepseek_test_passed"]=dsTestStarted&&!aiPlan.isEmpty();out["ui_checks"]=uiChecks;out["credential_exported"]=false;out["report_writer"]=reportWriter?reportWriter->report():QJsonObject{{"available",false},{"atomic",true},{"failures",0}};out["execution_reference_worker"]=executionReference.report();
        out["observed_patch"]=game.observedPatch;out["catalog_patch"]=catalog.patch;out["version_match"]=modelPatchMatches(catalog,game);out["headliner"]=evaluateHeadliner(catalog,game);out["historical_state_prediction"]=learnedPrediction;out["historical_purchase_reference"]=purchaseHistory;
        out["offer_match_binding"]=QJsonObject{{"bound_to_current_match",offersBoundTo(game)},{"context_id",state.liveContextId},{"match_ref",state.liveMatchRef.isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(state.liveMatchRef)},{"revision",qint64(offerBindingRevision)},{"event_revision",qint64(state.offerRevision)},{"surface_revision",qint64(cardSurfaceRevision)},{"last_rejection",lastOfferBindingRejection},{"rejections_by_reason",offerBindingRejections},{"scope","provider_game_id_not_global_platform_identity"}};
        if(recommendationTrace)out["recommendation_trace"]=recommendationTrace->report();out["hero_execution_plan"]=coachResult.value("hero_execution_plan");out["hero_execution_reference"]=coachResult.value("hero_execution_reference");out["role_aware_recommendation"]=coachResult.value("role_aware_recommendation");
        out["live_phase"]=game.phase;out["live_data_fresh"]=game.fresh();out["game_time"]=game.gameTime;out["input_readiness"]=coachPurchaseInputReadiness(catalog,game);out["manual_growth_scenario"]=game.growthObservations.value("manual_scenario");out["current_champion_id"]=game.championId;out["current_level"]=game.level;out["current_game_mode"]=game.gameMode;out["current_queue_id"]=game.queueId;out["current_player_count"]=game.players.size();out["hud_pagination"]=hudPaginationReport();out["live_combat_summary"]=liveSummary;out["observed_score"]=observedCoachScore(game);out["roster_insights"]=QJsonObject{{"html",rosterInsights->property("lastHtml").toString()},{"visible",rosterInsights->isVisibleTo(right)},{"conditional_model",true},{"actual_damage_shares_known",false}};out["live_combat_pipeline"]=liveSummaryEngine.report();out["live_roster_hud"]=liveRoster->report();out["hud_assets"]=assets->report();out["equipment_cues"]=equipmentCueResult;out["automatic_presentation"]=presentationState;out["page_indicator"]=QJsonObject{{"visible",false},{"interactive",false}};out["economy_summary"]=summary->report();out["engine_insights"]=engineDetails->report();out["compact_engine_insights"]=compactEngine->report();if(!presentationUiStates.isEmpty())out["synthetic_presentation_ui_states"]=presentationUiStates;
        out["match_context"]=game.matchContext;out["anonymous_player_history"]=anonymousHistory;out["roster_history_bridge"]=rosterBridge.report();
        out["growth_capture"]=growthCapture.report();out["growth_observations"]=growthObservationPublicReport(game.growthObservations);
        out["ocr_used"]=out.value("growth_capture").toObject().value("visible_reader").toObject().value("last_capture").toObject().value("screen_capture_used").toBool();
        if(game.source==GameState::Source::Live&&game.fresh()){
            out["selected_complete"]=coachPurchaseInputReadiness(catalog,game).value("ready");QJsonArray observed;for(int id:game.selected)observed.append(id);out["confirmed_selected"]=observed;
            out["selected_input_source"]="current_self_roster_observation";
        }
        QJsonObject strategyData;
        const auto activeStrategy=currentExternalStrategyEvidence(catalog);
        for(const auto &key:{"available","status","reason","patch","mode","revision","generated_at","record_count","rejected_records","runtime_refresh"})
            if(activeStrategy.contains(key))strategyData[key]=activeStrategy.value(key);
        strategyData["reference_only"]=true;strategyData["numeric_override"]=false;strategyData["live_refresh_enabled"]=true;out["external_strategy_data"]=strategyData;
        out["automatic_match_analysis"]=QJsonObject{{"enabled",rosterDsAllowed()&&rosterDsConfigured},{"configured",rosterDsConfigured},{"mode","visible_roster_only"},{"manual_trigger_required",false},{"state",rosterAiState},{"pending",rosterAiPending},{"dispatches_this_match",rosterAiDispatches},{"maximum_dispatches_per_match",3},{"maximum_champ_select_dispatches",2},{"context_id",game.matchContext.value("context_id")},{"context_scope","one_match_latest_snapshot_no_prior_messages"},{"analysis",rosterAiPlan},{"deepseek",rosterAi.report()},{"local_analysis_available",game.matchContext.value("active").toBool()},{"decision_authority","local_algorithm"},{"identity_exported",false}};
        out["global_notice"]=Kda::noticeReport();out["hud_skin_finish"]=Kda::HudStyle::identity();out["hud_skin_strength"]=Kda::HudStyle::strength();
        out["future_planning"]=forecast.report();out["deepseek_status"]=aiStatus->text();out["deepseek_pending"]=aiPending;
        out["local_choice"]=QJsonObject{{"model","lixin-component-selector-v2"},{"parameters",45027},
            {"backend","embedded_cpu"},{"planning_mode",deepSeekPlanningEnabled()?"local_plus_deepseek":"local_only"},
            {"evidence",currentLocalChoicePacket()},{"analysis_only",true},{"execution_eligible",false}};
        out["model_connection"]=QJsonObject{{"slot_id",rosterSettings.slotId},{"configured",rosterSettings.isConfigured()},{"deepseek_pricing",rosterSettings.isDeepSeek()},{"endpoint_exported",false},{"api_key_exported",false}};
        out["deepseek_usage"]=dsUsageMeter().summary(usageDisplayContext);
        out["automatic_advice"]=QJsonObject{{"enabled",true},{"manual_trigger_required",false},{"rule_hint_visible",automaticHint->isVisible()},{"ds_enabled",diagnosticDsEnabled()},{"ds_mode",diagnosticDsEnabled()?"explicit_diagnostic":"off"},{"dispatches",autoAdvice.dispatches()},{"retries",autoAdvice.retries()},{"settle_ms",800},{"minimum_request_interval_ms",12000},{"retry_delay_ms",30000},{"last_failure",lastAiFailure}};
        auto automatic=out["automatic_advice"].toObject();automatic["purchase_status"]=automaticHint->report().value("status");automatic["ai_context_available"]=!context.isEmpty();automatic["ds_hint_visible"]=false;automatic["ds_diagnostic_available"]=!aiPending&&requestBasis==adviceBasis()&&overlayAdviceMatchesAction(aiPlan,coachResult);automatic["displayed_html"]=automaticHint->text();out["automatic_advice"]=automatic;
        if(decisionReplay)out["decision_replay"]=decisionReplay->report();
        if(dashboard)out["dashboard"]=dashboard->report();
        auto cardTiming=out.value("card_model_timing").toObject();cardTiming["work_budget_ms"]=cardAssignedBudgetMs;cardTiming["maximum_work_budget_ms"]=cardBudgetMs;cardTiming["last_stop_reason"]=lastCardStopReason;cardTiming["cooperative_cancellation"]=true;out["card_model_timing"]=cardTiming;
        if(reportWriter)reportWriter->submit(++reportSequence,out);
#endif
    }
private:
    const Catalog &catalog;const QCommandLineParser &parser;RuntimeLog *const runtimeLog;bool replay,demo;PanelState state;QSettings &preferences;LiveClient live;DeepSeek ai,rosterAi;DsForecast forecast;RosterAnalysisBridge rosterBridge;GrowthCaptureBridge growthCapture;
    RuntimeLog::Outcome lastLiveLogOutcome=RuntimeLog::Outcome::None,lastFeedLogOutcome=RuntimeLog::Outcome::None;
    CoachDashboard *dashboard=nullptr,*startupDashboard=nullptr;CoachReplay *decisionReplay=nullptr;QJsonObject lastRuntimeSnapshot;QSystemTrayIcon *tray=nullptr;bool overlayPreview=false,lastRosterDsEnabled=true;QString lastAutoMinimizedContext;
    QString decisionArchiveContext,decisionArchiveMatchRef,decisionArchiveAccount;
    bool usageLiveSession=false;QString usageDisplayContext;DsUsageSessionTracker usageSession;
    UiBrowserWindow *uiBrowser=nullptr;QString browserScene;QSize browserViewport;QJsonObject browserChecks;QJsonArray browserStates;int browserMotionRevision=0;
    QJsonObject anonymousHistory,rosterAiPlan;QByteArray privateHistoryBasis;QString rosterObservedBasis,rosterRequestBasis,rosterAttemptedBasis,rosterAiState=QStringLiteral("idle");quint64 rosterAiRevision=0,rosterHistoryRevision=0;int rosterAiDispatches=0,rosterPreDispatches=0;bool rosterAiPending=false,rosterDsConfigured=false,rosterConfigurationBlocked=false;qint64 rosterStableSince=0,rosterRetryAt=0;ModelConnection rosterSettings;
    OverlayWindow *right=nullptr;QLabel *sourceStatus=nullptr,*aiStatus=nullptr,*cueIcon=nullptr,*rosterInsights=nullptr;CoachContextStrip *routeLabel=nullptr;CoachEconomyStrip *summary=nullptr;CoachEngineStrip *compactEngine=nullptr,*engineDetails=nullptr;CoachItemStrip *componentsLabel=nullptr,*rosterSummary=nullptr;HudAssets *assets=nullptr;CoachLiveHud *liveCounters=nullptr,*liveRoster=nullptr;QWidget *rosterPage=nullptr;bool dashboardInspectionRunning=false,dashboardInspectionExitPending=false;bool applyingPresentation=false,runningUiChecks=false,manualPresentationExpanded=false,manualPresentationPinned=false,manualPresentationPending=false;QString manualPresentationPage,manualPresentationContext; qint64 manualPresentationUntil=0;CoachPresentation presentation;QJsonObject presentationState;AsyncLiveCombatSummary liveSummaryEngine;std::unique_ptr<RecommendationTrace> recommendationTrace;QString coachResultContext,cardResultContext,cardWorkerContext,modelWorkerContext;QJsonObject liveSummary;LiveEquipmentCues equipmentCues;QJsonObject equipmentCueResult;QWidget *details=nullptr;bool detailsExpanded=false;
    QList<CardHintWindow*> cardHints;DeathTipsWindow *deathTips=nullptr;CornerCombatHud *combatCorner=nullptr;qint64 deathTipsSinceMs=-1;QJsonObject inlineDeathInsight,purchaseFacts;QByteArray purchaseFactsBasis;SelectionCardLayout cardPlacement;bool hiddenByUser=false,selectionActive=false,gameForeground=false,scoreboardHeld=false,rightAvoidingCards=false,compactAutoFold=false,rightUserFolded=false;
    QComboBox *style=nullptr;HudPagedText *advice=nullptr,*roster=nullptr;CoachActionHud *automaticHint=nullptr;
    QSet<int> seenOffers;QString offerSession,computedStrategy,workerStrategy,computedSoftBasis,workerSoftBasis,lastModelRejection;QJsonObject modelRejections,offerBindingRejections;QString lastOfferBindingRejection,lastCardSurface;quint64 offerBindingRevision=0,cardSurfaceRevision=0;
    int incompleteOfferCount=-1;
    bool offerIsReroll=false;
    CoachPurchaseList *purchaseList=nullptr;HudPhasePlate *phasePlate=nullptr;QJsonObject checklistState;QByteArray checklistBasis;QString lastActiveHudContext,endHudContext;qint64 endHudUntil=0;
    QString purchaseChangeReason;
    qint64 purchaseChangeUntil=0;
    AutoAdviceGate autoAdvice;QElapsedTimer autoClock;ModelConnection automaticSettings;QString autoBasis,requestBasis,lastAiFailure;bool settingsLoaded=false,aiConfigurationBlocked=false;qint64 settingsChecked=-1000;
    GameWindow::Tracker gameWindowTracker;
    GameWindow::ClientTracker clientWindowTracker;
    ChampSelectView *champSelectView=nullptr;
    std::unique_ptr<ShopRecommendations::Store> shopStore;
    QJsonObject shopStatus;
    QString shopProjectionKey,warmedChampionsKey;
    QSet<int> shopVerifiedItems;
    bool usingClientViewport=false,shopCleanupPending=true;
    QRect dockedClient;
    QTimer pollTimer;QThreadPool modelPool;QFutureWatcher<QJsonObject> worker;PendingFutureCompletion modelCompletion;RecommendationWorkPtr modelWork;static constexpr qint64 modelBudgetMs=2800;qint64 modelAssignedBudgetMs=0,modelRetryAt=0;QString lastModelStopReason,failedModelBasis;int consecutiveModelTimeouts=0;QJsonObject lastModelStageTimings;GameState game;CoachSimulationOptions options;QJsonObject coachResult,context,aiPlan,uiChecks;QJsonArray presentationUiStates;qint64 presentationCheckTick=-1;QList<int> allowedItems,allowedAugments;QMap<int,QJsonObject> manual;
    static constexpr int reportCollectionIntervalMs=500;
    std::unique_ptr<DiagnosticReportWriter> reportWriter;quint64 reportSequence=0;QTimer reportRefreshTimer;bool reportRefreshPending=false,reportDrainStarted=false;qint64 reportNextCollectionAtMs=0;AsyncExecutionReference executionReference;
    QString input,reportFile,connection,lastStrategy,modelState=QStringLiteral("waiting");QList<QJsonObject> samples;QByteArray pending;QRect viewport;OverlayPlacement placement;
    HudLayout hudLayout,cardPlacementLayout;bool hudEditorOpen=false;
    QRect cardPlacementViewport;
    qint64 cardLayoutBuilds=0,cardHintBuilds=0;
    qint64 offset=0,lastMaintenanceMs=0;int sample=-1,accepted=0;quint64 modelTicket=0,workerTicket=0,aiRevision=0;double aiSnapshotTime=0;bool dirty=false,through=false,hotkey=false,gameWindowFound=false,finishing=false,dsTestStarted=false,aiPending=false;
    QJsonObject sourceHealth,learnedPrediction,purchaseHistory,knownModelUiControl,uiFixtureModelEvidence,cardTraceKey;QJsonArray cardTransitions;qint64 workerStartedMs=0,lastModelElapsedMs=-1,maxModelElapsedMs=0;int modelCompletions=0,modelDiscarded=0;QDateTime lastModelCompletedAt;
    qint64 pollCallbacks=0,eventFileOpenAttempts=0,lastPeriodicReportMs=0;
    QJsonObject purchaseChoiceEvidence,augmentChoiceEvidence;
    qint64 publishedSnapshotMs=-1,workerInputAgeMs=0,publishedInputAgeMs=0;double workerSnapshotGameTime=0,publishedSnapshotGameTime=0;QDateTime workerSnapshotAt,workerSourceReceivedAt;
    static constexpr qint64 futureFailureRetryMs=1000;
    QThreadPool cardPool;QFutureWatcher<QJsonObject> cardWorker;PendingFutureCompletion cardCompletion;RecommendationWorkPtr cardWork;static constexpr qint64 cardBudgetMs=2000;qint64 cardAssignedBudgetMs=0,cardInputAgeMs=0;QString cardWorkerHardBasis,lastCardStopReason;QJsonObject cardDecision;QString cardWorkerBasis,computedCardBasis,failedCardBasis;qint64 cardStartedMs=0,lastCardElapsedMs=-1,maxCardElapsedMs=0,cardRenderedAtMs=-1,cardRetryAt=0;int cardCompletions=0,cardDiscarded=0;QDateTime cardCompletedAt,offerReceivedAt,sourceCollectedAt;CardReferenceCache cardReferenceCache;
    QThreadPool loadingPool;QFutureWatcher<QJsonObject> loadingWorker;PendingFutureCompletion loadingCompletion;QJsonObject loadingPreparation;QString loadingWorkerBasis,loadingComputedBasis,failedLoadingBasis;qint64 loadingRetryAt=0;int loadingCompleted=0,loadingDiscarded=0;
#ifdef Q_OS_WIN
    HWND keyWindow=nullptr;
#endif
    QDateTime combatCornerReceived;
    QString combatCornerContext;
    QString rosterKey(const GameState &s)const{QString result;for(const auto &v:s.players){const auto p=v.toObject();result+=p.value("team").toString()+":"+QString::number(p.value("champion_id").toInt())+";";}return result;}
    static std::optional<QList<int>> observedOwnInventory(const GameState &snapshot){
        std::optional<QList<int>> items;
        for(const auto &value:snapshot.players){
            const auto player=value.toObject();
            if(player.value("self")!=QJsonValue(true))continue;
            if(items||player.value("items_complete")!=QJsonValue(true)||!player.value("items").isArray()
                ||player.value("champion_id").toInt()!=snapshot.championId)return std::nullopt;
            items.emplace();
            for(const auto &item:player.value("items").toArray()){
                if(item.toInt()<=0||item!=QJsonValue(item.toInt()))return std::nullopt;
                items->append(item.toInt());
            }
        }
        if(!items)return std::nullopt;
        auto owned=snapshot.owned;
        std::sort(items->begin(),items->end());
        std::sort(owned.begin(),owned.end());
        if(*items!=owned)return std::nullopt;
        return items;
    }
    void observePurchaseChange(const GameState &next){
        const auto active=[](const GameState &s){
            return s.source==GameState::Source::Live&&s.fresh()&&s.gameMode==QStringLiteral("KIWI")
                &&s.matchContext.value("active").toBool()&&(s.phase==QStringLiteral("InProgress")||s.phase==QStringLiteral("Reconnect"));
        };
        const auto contextId=next.matchContext.value("context_id").toString();
        if(!active(next)||!active(game)||contextId.isEmpty()||contextId!=game.matchContext.value("context_id").toString()
            ||next.championId!=game.championId||next.gameTime+10<game.gameTime){
            purchaseChangeReason.clear();purchaseChangeUntil=0;return;
        }
        const auto before=observedOwnInventory(game),after=observedOwnInventory(next);
        const bool equipment=before&&after&&*before!=*after;
        const bool augment=std::any_of(next.selected.cbegin(),next.selected.cend(),[this](int id){return id>0&&!game.selected.contains(id);});
        if(!equipment&&!augment)return;
        purchaseChangeReason=equipment&&augment?QStringLiteral("装备/强化变化"):
            equipment?QStringLiteral("装备变化"):QStringLiteral("已选强化变化");
        purchaseChangeUntil=autoClock.elapsed()+8000;
    }
    void bindDecisionArchive(const QJsonObject &payload){
        if(!usageLiveSession||!decisionReplay||payload.value("mode")!=QStringLiteral("KIWI")
            ||payload.value("queue_id").toInt()!=2400||payload.value("stage")!=QStringLiteral("in_game"))return;
        const auto account=live.localAccountScope();
        const auto platform=payload.value("platform").toString(),gameId=payload.value("game_id").toString();
        static const QRegularExpression platformPattern(QStringLiteral("^[A-Za-z0-9_-]{2,32}$"));
        static const QRegularExpression puuidPattern(QStringLiteral("^[A-Za-z0-9_-]{8,128}$"));
        if(account.isEmpty()||!platformPattern.match(platform).hasMatch()||offerMatchRefForGameId(gameId).isEmpty())return;
        QString self;
        for(const auto &entry:payload.value("players").toArray()){
            const auto player=entry.toObject();
            if(player.value("self")!=QJsonValue(true))continue;
            if(!self.isEmpty()||player.value("identity_merge_status")==QStringLiteral("conflict"))return;
            self=player.value("puuid").toString();
            if(!puuidPattern.match(self).hasMatch())return;
        }
        const auto boundScope=QCryptographicHash::hash(QByteArrayLiteral("lixin.local-account-scope.v1")+'\0'+self.toUtf8(),QCryptographicHash::Sha256).toHex();
        if(self.isEmpty()||account.toLatin1()!=boundScope)return;
        decisionArchiveAccount=account;
        decisionArchiveContext=payload.value("context_id").toString();
        // Keep the same platform/game reference as the authenticated history bridge.
        decisionArchiveMatchRef=QString::fromLatin1(QCryptographicHash::hash((platform+QChar(u':')+gameId).toUtf8(),QCryptographicHash::Sha256).toHex());
    }
    void settleDecisionArchive(const GameState &next){
        if(decisionArchiveContext.isEmpty())return;
        const auto nextContext=next.matchContext.value("context_id").toString();
        const bool ended=next.phase==QStringLiteral("WaitingForStats")||next.phase==QStringLiteral("PreEndOfGame")||next.phase==QStringLiteral("EndOfGame");
        if(!ended&&nextContext==decisionArchiveContext)return;
        if(ended&&(nextContext.isEmpty()||nextContext==decisionArchiveContext)
            &&decisionReplay&&dashboard&&game.matchContext.value("context_id")==decisionArchiveContext
            &&live.localAccountScope()==decisionArchiveAccount){
            const auto captured=decisionReplay->archiveSnapshot(decisionArchiveContext);
            if(CoachReplay::saveArchive(CoachDashboard::decisionArchiveDirectory(catalog.root,decisionArchiveAccount),decisionArchiveMatchRef,captured))
                dashboard->decisionArchiveChanged();
        }
        decisionArchiveContext.clear();decisionArchiveMatchRef.clear();decisionArchiveAccount.clear();
    }
    void acceptLiveSnapshot(const GameState &snapshot){
        settleDecisionArchive(snapshot);
        if(usageLiveSession){
            const auto oldId=game.matchContext.value("context_id").toString(),nextId=snapshot.matchContext.value("context_id").toString();
            const bool active=snapshot.matchContext.value("active").toBool()&&!nextId.isEmpty();
            const auto usagePhase=active?snapshot.matchContext.value("phase").toString(snapshot.phase):snapshot.phase;
            const auto completed=usageSession.observe(nextId,active,snapshot.matchContext.value("stage").toString(),usagePhase);
            if(!oldId.isEmpty()&&oldId!=nextId){
                dsUsageMeter().observeContext(oldId,false,completed==oldId);
            }
            if(active){usageDisplayContext=nextId;dsUsageMeter().observeContext(nextId,true);}
        }
        if(game.matchContext.value("context_id")!=snapshot.matchContext.value("context_id")){resetMatchAnalysis();manual.clear();seenOffers.clear();state=PanelState(false);invalidateOfferResults();}
        if(rosterKey(game)!=rosterKey(snapshot)||snapshot.gameTime+10<game.gameTime){manual.clear();seenOffers.clear();if(snapshot.gameTime+10<game.gameTime){state=PanelState(false);invalidateOfferResults();}invalidateAi();}
        if(snapshot.source==GameState::Source::Offline&&game.source==GameState::Source::Live){state=PanelState(false);invalidateOfferResults();}
        auto next=snapshot;next.offers.clear();if(next.source!=GameState::Source::Offline&&next.gameMode==QStringLiteral("KIWI")){if(offersBoundTo(next)){next.headlinerContext=state.headlinerContext;if(state.status=="open"&&!state.replay)next.offers=offerIds();}}
        const bool bound=offersBoundTo(next);growthCapture.setSelectionRound(bound?state.resolvedRound:-1,bound?state.round:-1,bound&&(state.status=="open"||state.round>state.resolvedRound));
        QJsonArray excluded;if(!demo&&!replay&&!uiBrowser){bool found=false;const auto area=actualViewport(&found);if(found){const double dpr=gameWindowTracker.state().scale;QList<QWidget*> overlays{right,deathTips,combatCorner};for(auto card:cardHints)overlays.append(card);for(auto widget:overlays)if(widget&&widget->isVisible()){const auto r=widget->geometry().translated(-area.topLeft());excluded.append(QJsonArray{qRound(r.x()*dpr),qRound(r.y()*dpr),qRound(r.width()*dpr),qRound(r.height()*dpr)});}}}growthCapture.setCaptureExclusions(excluded);
        applyConfirmedMatchInputs(catalog,next,{},state.round); // Remove expired scenario provenance before joining automatic facts.
        growthCapture.enrich(next);if(next.source!=GameState::Source::Offline&&next.gameMode==QStringLiteral("KIWI"))applyManual(next);
        next.recommendationEvidence=buildRecommendationHistory(catalog,next,anonymousHistory);
        const auto key=strategyKey(next);if(key!=lastStrategy){if(automaticOverlayAdviceBasis(next,coachResult,style->currentData().toString(),options.allyExecution,offerSession)!=adviceBasis())invalidateAi();lastStrategy=key;coachResult={};context={};if(modelWork)modelWork->cancel();++modelTicket;if(worker.isRunning())dirty=true;}
        observePurchaseChange(next);
        game=next;warmSelectionArtwork();if(!demo&&!replay&&!uiBrowser)dock(false);syncPollCadence();rosterBridge.setGamePhase(next.phase);if(!coachResult.isEmpty()){refreshCoachSnapshot(catalog,game,coachResult);coachResult["requested_combat_style"]=style->currentData().toString();annotateCoachRecommendationQuality(catalog,game,coachResult);refreshAiEconomy();}scheduleCardModel();scheduleModel(false);updateAutomaticAdvice();updateRosterAdvice();renderCards();renderCoach();publishRuntimeSnapshot();
    }
    QString strategyKey(const GameState &s)const{return coachStructuralBasis(s);}
    int confirmedOfferSlotCount()const{
        if(state.status!=QStringLiteral("open")||state.candidates.size()>3)return 0;
        QSet<int> positions,ids;
        for(const auto &value:state.candidates){
            const auto card=value.toObject();
            const auto slot=card.value("candidate_index");
            const int index=slot.toInt(-1),id=card.value("augment_id").toInt();
            if(index<0||index>2||slot.toDouble()!=index||positions.contains(index)||ids.contains(id))return 0;
            positions.insert(index);ids.insert(id);
        }
        return std::count_if(ids.cbegin(),ids.cend(),[this](int id){return catalog.ordinaryOfferEligible(id);});
    }
    QList<int> offerIds()const{
        QList<int> ids;
        if(confirmedOfferSlotCount()!=3||!catalog.combat)return ids;
        for(const auto &v:state.candidates){const int id=v.toObject().value("augment_id").toInt();if(catalog.combat->augments.contains(id))ids.append(id);}
        return ids;
    }
    void rememberOffers(){offerSession=state.session;seenOffers=state.seenCandidates;}
    bool offersBoundTo(const GameState &snapshot)const{
        if((demo||replay||uiBrowser||runningUiChecks)&&snapshot.source==GameState::Source::Replay)return true;
        return state.liveBoundTo(snapshot.matchContext.value("context_id").toString())
            &&liveOfferContextReady(snapshot,state.liveContextId,state.liveMatchRef,catalog.patch);
    }
    void invalidateOfferResults(){
        if(cardWork)cardWork->cancel();
        ++offerBindingRevision;game.offers.clear();game.headlinerContext={};cardDecision={};computedCardBasis.clear();cardResultContext.clear();
        offerIsReroll=false;
        rememberOffers();coachResult={};context={};computedStrategy.clear();computedSoftBasis.clear();
        if(modelWork)modelWork->cancel();++modelTicket;invalidateAi();
    }
    void refreshAiEconomy(){if(context.isEmpty())return;context["observed_game_time"]=game.gameTime;context["coach_economy"]=coachResult.value("economy");context["visible_players_no_identity"]=game.players;}
    QString externalReferenceRevision()const{return currentExternalStrategyEvidence(catalog).value("revision").toString();}
    QString adviceBasis()const{return automaticOverlayAdviceBasis(game,coachResult,style->currentData().toString(),options.allyExecution,offerSession)+game.matchContext.value("context_id").toString()+"|"+externalReferenceRevision();}
    void openUiBrowser(){
        const auto folder=catalog.root+QStringLiteral("/.local/ui-browser");QDir().mkpath(folder);
        // Copy presentation only. Preview keeps its own window and scene settings.
        QSettings preview(folder+QStringLiteral("/appearance.ini"),QSettings::IniFormat);
        preview.setValue("appearance/skin",Kda::skin());
        preview.setValue("appearance/hudSkinStrength",Kda::HudStyle::strength());
        preview.setValue("appearance/frostedGlass",Kda::glassEnabled());
        preview.setValue("appearance/overlayBlur",Kda::overlayBlurEnabled());
        preview.setValue("appearance/overlayBackdropOpacity",Kda::overlayBackdropOpacity());
        preview.setValue("appearance/parallax",Kda::depthEnabled());
        preview.setValue("appearance/reducedMotion",Kda::reducedMotion());
        preview.setValue("right/opacity",right->configuredOpacity());preview.sync();
        if(preview.status()!=QSettings::NoError){Kda::postNotice({QStringLiteral("ui-browser-open"),QStringLiteral("暂时无法打开界面浏览"),{},Kda::NoticeKind::Error});return;}
        if(!QProcess::startDetached(QCoreApplication::applicationFilePath(),{QStringLiteral("--root"),catalog.root,QStringLiteral("--demo"),QStringLiteral("--ui-browser"),QStringLiteral("--inspection-window"),QStringLiteral("--settings"),folder+QStringLiteral("/appearance.ini"),QStringLiteral("--report"),folder+QStringLiteral("/window-report.json")},catalog.root))
            Kda::postNotice({QStringLiteral("ui-browser-open"),QStringLiteral("暂时无法打开界面浏览"),{},Kda::NoticeKind::Error});
    }
    void reloadUiBrowserAppearance(){
        if(!uiBrowser)return;preferences.sync();
        Kda::HudStyle::setStrength(preferences.value("appearance/hudSkinStrength",.65).toDouble());
        Kda::setSkin(preferences.value("appearance/skin",QStringLiteral("kda")).toString());
        Kda::setGlassEnabled(preferences.value("appearance/frostedGlass",true).toBool());
        Kda::setOverlayBlurEnabled(preferences.value("appearance/overlayBlur",true).toBool());
        Kda::setOverlayBackdropOpacity(preferences.value("appearance/overlayBackdropOpacity",.58).toDouble());
        Kda::setDepthEnabled(preferences.value("appearance/parallax",true).toBool());
        Kda::setReducedMotion(preferences.value("appearance/reducedMotion",false).toBool());
        right->setWindowOpacity(std::clamp(preferences.value("right/opacity",.88).toDouble(),.45,1.));
        renderCards();renderCoach();writeReport();
    }
    void positionUiBrowser(){
        if(!uiBrowser)return;const auto area=QGuiApplication::primaryScreen()->availableGeometry();
        uiBrowser->resize(area.width()<1280?QSize(360,420):QSize(380,476));
        const bool cards=browserScene.startsWith(QStringLiteral("cards"));
        int belowHints=area.top()+8;for(auto hint:cardHints)if(hint->geometry().isValid())belowHints=std::max(belowHints,hint->geometry().bottom()+12);
        const QPoint point=cards?QPoint(area.left()+8,std::clamp(belowHints,area.top()+8,std::max(area.top()+8,area.bottom()-uiBrowser->height()-8))):QPoint(area.right()-uiBrowser->width()-8,area.bottom()-uiBrowser->height()-8);
        uiBrowser->move(point);
    }
    void createUiBrowser(){
        options.samples=256;uiBrowser=new UiBrowserWindow;uiBrowser->setWindowIcon(assistantIcon());
        uiBrowser->onScene=[this](const QString &scene){applyUiBrowserScene(scene);};
        uiBrowser->onViewport=[this](QSize size){browserViewport=size;dock(true);positionUiBrowser();renderCoach();writeReport();};
        uiBrowser->onReplay=[this]{applyUiBrowserScene(browserScene);};uiBrowser->onQuit=[this]{finish();};
        uiBrowser->show();uiBrowser->selectScene(QStringLiteral("loading_ready"));positionUiBrowser();
        if(parser.isSet("self-check-browser"))QTimer::singleShot(150,this,[this]{browserCheckNext(0);});
    }
    void applyUiBrowserScene(const QString &scene){
        if(!demo||!uiBrowser)return;bool recognized=false;for(const auto &s:uiBrowserScenes())recognized|=s.id==scene;if(!recognized)return;
        browserScene=scene;++browserMotionRevision;if(modelWork)modelWork->cancel();++modelTicket;dirty=worker.isRunning();invalidateAi();
        coachResult={};context={};cardDecision={};computedCardBasis.clear();computedStrategy.clear();computedSoftBasis.clear();loadingPreparation={};loadingComputedBasis.clear();
        right->setPresentedVisible(false,false);combatCorner->hide();
        for(auto hint:cardHints)hint->hide();deathTips->hide();anonymousHistory={};rosterAiPlan={};
        state=PanelState(true);state.status=QStringLiteral("closed");game=makeCoachDemo(catalog);game.selected={};game.offers={};game.effectCounters.clear();game.effectConditions.clear();
        for(int i=0;i<game.players.size();++i){auto p=game.players[i].toObject();p["augment_ids"]=QJsonArray{};p["augments_complete"]=true;p["augment_source"]=QStringLiteral("synthetic_ui_browser");p["is_dead"]=p.value("self").toBool()&&scene.startsWith(QStringLiteral("dead"));game.players[i]=p;}
        if(scene==QStringLiteral("death_tips")){auto p=game.players[0].toObject();p["is_dead"]=true;game.players[0]=p;}
        if(scene.startsWith(QStringLiteral("cards"))){
            game.offers=scene==QStringLiteral("cards")?QList<int>{1084,1103,1129}:QList<int>{1028,1087,1333};
            if(scene==QStringLiteral("cards_incomplete")){game.championId=412;auto p=game.players[0].toObject();p["champion_id"]=412;p["augments_complete"]=false;game.players[0]=p;}
            QJsonArray cards;for(int id:game.offers)cards.append(QJsonObject{{"candidate_index",cards.size()},{"augment_id",id},{"augment_name",catalog.augments.value(id).value("name_zh_cn")},{"rerollable",true}});
            state.apply(QJsonObject{{"schema_version",1},{"source","wegame_local_log"},{"capture_mode","replay"},{"observed_at",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},{"session_ref",QStringLiteral("synthetic-ui-browser-%1").arg(browserMotionRevision)},{"kind","offer"},{"game_mode","KIWI"},{"pick_count",1},{"candidates",cards}});
        }
        QJsonArray publicRoster;for(const auto &v:game.players){const auto p=v.toObject();publicRoster.append(QJsonObject{{"champion_id",p.value("champion_id")},{"team",p.value("team")==QStringLiteral("ORDER")?QStringLiteral("ALLY"):QStringLiteral("ENEMY")},{"self",p.value("self")}});}
        const bool pregame=scene==QStringLiteral("champ_select")||scene.startsWith(QStringLiteral("loading"));
        if(scene==QStringLiteral("champ_select"))for(int i=5;i<publicRoster.size();++i){auto p=publicRoster[i].toObject();p["champion_id"]=0;publicRoster[i]=p;}
        if(scene==QStringLiteral("loading_pending")&&!publicRoster.isEmpty()){
            auto pendingSeat=publicRoster.last().toObject();pendingSeat["champion_id"]=0;publicRoster.replace(publicRoster.size()-1,pendingSeat);
        }
        game.matchContext=QJsonObject{{"active",scene!=QStringLiteral("idle")},{"context_id",QStringLiteral("synthetic-ui-browser:")+scene},{"stage",scene==QStringLiteral("champ_select")?QStringLiteral("champ_select"):pregame?QStringLiteral("loading"):QStringLiteral("in_game")},{"roster",publicRoster}};
        int visibleAllies=0,visibleEnemies=0;for(const auto &v:publicRoster){const auto p=v.toObject();if(p.value("champion_id").toInt()<=0)continue;if(p.value("team")==QStringLiteral("ALLY"))++visibleAllies;else if(p.value("team")==QStringLiteral("ENEMY"))++visibleEnemies;}
        game.matchContext["visibility"]=QJsonObject{{"allies",QJsonObject{{"visible",visibleAllies},{"total",5}}},{"enemies",QJsonObject{{"visible",visibleEnemies},{"total",5}}}};
        if(scene==QStringLiteral("champ_select")){game.bench={516,51,25,223};game.benchKnown=true;game.championSelectReceived=QDateTime::currentDateTimeUtc();}
        if(pregame){game.source=GameState::Source::Offline;game.phase=scene==QStringLiteral("champ_select")?QStringLiteral("ChampSelect"):QStringLiteral("GameStart");game.players={};game.owned={};game.abilities={};game.selected={};game.level=0;game.goldKnown=false;}
        if(scene==QStringLiteral("idle")){game={};game.matchContext={{"active",false}};}
        if(scene==QStringLiteral("stale_live")){game.source=GameState::Source::Live;game.received=QDateTime::currentDateTimeUtc().addSecs(-90);}
        game.matchContext["composition"]=analyzeMatchComposition(game.matchContext,catalog.champions);rememberOffers();
        manualPresentationContext=presentationContextId();manualPresentationPage=scene.contains(QStringLiteral("roster"))||pregame?QStringLiteral("roster"):QStringLiteral("purchase");
        manualPresentationExpanded=pregame||scene.startsWith(QStringLiteral("dead"))||scene==QStringLiteral("death_tips");manualPresentationPinned=true;manualPresentationPending=true;
        hiddenByUser=false;through=false;dock(true);positionUiBrowser();scheduleCardModel();scheduleModel(true);renderCards();renderCoach();writeReport();
    }
    void updateUiBrowserExplanation(){
        if(!uiBrowser)return;QStringList lines;for(const auto &s:uiBrowserScenes())if(s.id==browserScene)lines.append(s.description);
        if(browserScene.startsWith(QStringLiteral("cards")))for(auto hint:cardHints){const auto h=hint->report();lines.append(h.value("title").toString()+QStringLiteral(" · ")+h.value("metric").toString());lines.append(h.value("display_evaluation_reason").toString(h.value("evaluation_reason").toString()));if(!h.value("comparison_reason").toString().isEmpty())lines.append(h.value("comparison_reason").toString());}
        else if(browserScene.startsWith(QStringLiteral("loading"))){QTextDocument doc;doc.setHtml(loadingPreparationHtml());lines.append(doc.toPlainText());if(loadingPreparation.value("available").toBool()){lines.append(QStringLiteral("预演按五名对手、3/8/20秒窗口比较；以下均为成长候选，进场按实际装备与海克斯重算。"));for(const auto &v:loadingPreparation.value("candidate_directions").toArray())lines.append(v.toObject().value("name").toString());}}
        else if(!coachResult.value("recommendation_quality").toObject().value("summary").toString().isEmpty())lines.append(coachResult.value("recommendation_quality").toObject().value("summary").toString());
        uiBrowser->setExplanation(lines.join(QStringLiteral("\n\n")));
    }
    void browserCheckNext(int index,int attempt=0){
        if(index>=uiBrowserScenes().size()){
            browserChecks["separate_synthetic_instance_no_paid_dispatch_or_hotkeys"]=demo&&!rosterDsAllowed()&&!diagnosticDsEnabled()&&rosterAiDispatches==0&&!hotkey;
            browserChecks["all_scenes_visited"]=browserStates.size()==uiBrowserScenes().size();writeReport();finish();return;
        }
        const auto scene=uiBrowserScenes().at(index).id;if(attempt==0)uiBrowser->selectScene(scene);
        const bool cards=scene.startsWith(QStringLiteral("cards"));
        const bool ready=!game.fresh()||(!coachResult.isEmpty()&&(!cards||cardModelCurrent()));
        if(!ready&&attempt<80){QTimer::singleShot(100,this,[this,index,attempt]{browserCheckNext(index,attempt+1);});return;}
        browserChecks[scene+QStringLiteral("_model_settled")]=ready;
        QTimer::singleShot(320,this,[this,index,scene]{
            renderCoach();updateUiBrowserExplanation();QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
            const auto control=uiBrowser->report(),hud=automaticHint->report();bool content=true;
            if(scene==QStringLiteral("champ_select"))content=champSelectView->isVisibleTo(right)&&champSelectView->report().value("content_fits").toBool()&&champSelectView->model().value("visible_ally_count").toInt()==5;
            const auto rosterStrip=rosterSummary->report();
            const bool tenSeats=right->isVisible()&&rosterSummary->isVisibleTo(right)
                &&rosterStrip.value("roster_count").toInt()==10&&rosterStrip.value("all_content_visible").toBool();
            if(scene.startsWith(QStringLiteral("loading")))content=tenSeats&&phasePlate->isVisibleTo(right)&&!automaticHint->isVisibleTo(right)&&phasePlate->report().value("pending").toBool()==(scene==QStringLiteral("loading_pending"));
            if(scene.startsWith(QStringLiteral("alive")))content=tenSeats&&automaticHint->isVisibleTo(right);
            if(scene.startsWith(QStringLiteral("dead")))content=right->isVisible()&&automaticHint->isVisibleTo(right)&&detailsExpanded
                &&(liveRoster->isVisibleTo(right)||combatCorner->isVisible()||(!right->property("passiveRosterDetailFits").toBool()&&tenSeats));
            if(scene.startsWith(QStringLiteral("cards"))){content=selectionActive;for(auto hint:cardHints)content&=hint->isVisible()&&hint->report().value("metric_fits").toBool()&&!hint->geometry().intersects(uiBrowser->geometry());content&=!control.value("full_explanation").toString().isEmpty();}
            if(scene==QStringLiteral("death_tips"))content=deathTips->isVisible()&&deathTips->report().value("text_fits").toBool();
            if(scene==QStringLiteral("scoreboard"))content=scoreboardHeld&&!right->isVisible()&&!combatCorner->isVisible()&&!deathTips->isVisible()&&!selectionActive;
            if(scene==QStringLiteral("stale_live"))content=!game.fresh()&&hud.value("title")==QStringLiteral("等待实时数据");
            browserChecks[scene+QStringLiteral("_visible_content")]=content;
            browserChecks[scene+QStringLiteral("_layout_fits")]=control.value("all_controls_contained").toBool()&&control.value("controls_non_overlapping").toBool()&&control.value("description_text_fits").toBool()&&hud.value("text_fits").toBool()&&right->report().value("heading_fits").toBool();
            QJsonArray cardReports;for(auto hint:cardHints)cardReports.append(hint->report());
            browserStates.append(QJsonObject{{"card_hints",cardReports},{"scene",scene},{"synthetic",true},{"controls",control},{"hud",hud},{"hud_phase",phasePlate->report()},{"coach_window",right->report()},{"scoreboard_held",scoreboardHeld},{"expanded",detailsExpanded},{"page",presentationPage()},{"loading_preparation_available",loadingPreparation.value("available")},{"roster_insights_visible",rosterInsights->isVisibleTo(right)},{"roster_insights",rosterInsights->property("lastHtml").toString()},{"roster_hud",liveRoster->report()},{"hud_pagination",hudPaginationReport()},{"passive_hud",passiveHudReport()},{"death_tips",deathTips->report()},{"combat_corner_hud",combatCorner->report()}});
            if(parser.isSet("snapshot")){const auto file=QFileInfo(parser.value("snapshot"));const auto base=file.absolutePath()+QStringLiteral("/browser-%1-%2").arg(index,2,10,QChar(u'0')).arg(scene);snapshot(base+QStringLiteral(".png"));uiBrowser->grab().save(base+QStringLiteral("-controls.png"));}
            writeReport();browserCheckNext(index+1);
        });
    }
    void createDashboard(){
        qApp->setQuitOnLastWindowClosed(false);lastRosterDsEnabled=preferences.value("coach/rosterDsEnabled",true).toBool();
        dashboard=startupDashboard?std::exchange(startupDashboard,nullptr):new CoachDashboard(catalog,preferences);dashboard->setLocalAccountScope(live.localAccountScope());dashboard->setLocalPlayerDisplayName(live.localPlayerDisplayName());
        connect(&live,&LiveClient::localAccountScopeChanged,dashboard,[this](const QString &scope){dashboard->setLocalAccountScope(scope);});
        connect(&live,&LiveClient::localPlayerDisplayNameChanged,dashboard,[this](const QString &name){dashboard->setLocalPlayerDisplayName(name);});
        dashboard->setLocalPlayerBackdrop(live.localPlayerBackdrop());connect(&live,&LiveClient::localPlayerBackdropChanged,dashboard,[this](const QByteArray &bytes){dashboard->setLocalPlayerBackdrop(bytes);});
        decisionReplay=new CoachReplay(catalog,dashboard,true);dashboard->setReplayWidget(decisionReplay);dashboard->onOpenReplay=[this]{dashboard->showReplay();};qApp->setWindowIcon(assistantIcon());dashboard->setWindowIcon(assistantIcon());dashboard->onQuit=[this]{finish();};
        dashboard->onVisibilityToggle=[this]{
            const bool enable=!preferences.value("coach/overlayEnabled",true).toBool();
            if(enable){preferences.setValue("coach/overlayEnabled",true);preferences.sync();hiddenByUser=false;overlayPreview=true;}
            else if(hiddenByUser){hiddenByUser=false;overlayPreview=true;}
            else if(!right->isVisible()&&!selectionActive){overlayPreview=true;}
            else{hiddenByUser=true;overlayPreview=false;}
            renderCardHints();writeReport();
        };
        dashboard->onResetOverlay=[this]{resetHudLayout();};
        dashboard->onOpenHudEditor=[this]{openHudEditor();};
        dashboard->onGrowthCaptureSettings=[this]{const auto &gameSurface=gameWindowTracker.refresh();const bool found=gameSurface.found&&gameSurface.visible&&!gameSurface.minimized&&!gameSurface.client.isEmpty();const auto gameArea=gameSurface.client;const auto settings=editGrowthCaptureSettings(dashboard,growthCapture.report(),found?gameArea:QRect{},found?gameSurface.scale:0);if(!settings.isEmpty()){preferences.setValue("visibleMatchInputs",QJsonDocument(settings).toJson(QJsonDocument::Compact));growthCapture.configureVisible(settings);acceptLiveSnapshot(game);}};
        dashboard->onCompleteMatchInputs=[this]{for(int i=0;i<game.players.size();++i)if(game.players[i].toObject().value("self").toBool()){editAugments(i);break;}};
        dashboard->onRefreshMatchData=[this]{
            if(finishing)return QStringLiteral("正在退出，刷新已取消");
            if(demo||replay||uiBrowser||runningUiChecks)return QStringLiteral("演示模式不读取客户端");
            return LiveClient::refreshResultText(live.requestRefresh());
        };
        connect(&live,&LiveClient::manualRefreshStatusChanged,dashboard,[this](const QString &status){dashboard->setMatchRefreshStatus(status);});
        dashboard->onOpenUiBrowser=[this]{openUiBrowser();};
        dashboard->onPreferencesChanged=[this]{
            right->setWindowOpacity(std::clamp(preferences.value("right/opacity",.88).toDouble(),.45,1.));
            refreshModelConfiguration(true);
            if(!preferences.value("coach/overlayEnabled",true).toBool())overlayPreview=false;
            updateAutomaticAdvice();updateRosterAdvice();renderCards();renderCoach();writeReport();
        };
        dashboard->onPreferenceChanged=[this](const QString &key){
            if(key==QStringLiteral("coach/shopRecommendationsEnabled")){refreshShopRecommendations();writeReport();return;}
            if(key==QStringLiteral("coach/closeToTray"))return;
            // Skin, parallax and reduced motion already propagate through Kda.
            // Keep inference, history projections and report I/O off this click.
            if(key.startsWith(QStringLiteral("appearance/")))return;
            if(dashboard->onPreferencesChanged)dashboard->onPreferencesChanged();
        };
        right->onOpenDashboard=[this]{showDashboard();};right->onClose=[this]{setUserHidden(true);};
        if(QSystemTrayIcon::isSystemTrayAvailable()){
            tray=new QSystemTrayIcon(assistantIcon(),this);tray->setToolTip(QStringLiteral("李金金 · 对局助手"));auto menu=new Kda::TrayMenu(dashboard);
            connect(menu->addAction(QStringLiteral("打开李金金")),&QAction::triggered,this,[this]{showDashboard();});
            connect(menu->addAction(QStringLiteral("界面浏览")),&QAction::triggered,this,[this]{openUiBrowser();});
            connect(menu->addAction(QStringLiteral("HUD 管理")),&QAction::triggered,this,[this]{openHudEditor();});
            connect(menu->addAction(QStringLiteral("显示 / 隐藏浮层")),&QAction::triggered,this,[this]{if(dashboard&&dashboard->onVisibilityToggle)dashboard->onVisibilityToggle();});
            connect(menu->addAction(QStringLiteral("展开 / 收起详情 · Ctrl+Alt+F10")),&QAction::triggered,this,[this]{through=false;setDetailsExpanded(!detailsExpanded);});
            connect(menu->addAction(QStringLiteral("恢复自动切换")),&QAction::triggered,this,[this]{resumeAutomaticPresentation();});
            connect(menu->addAction(QStringLiteral("浮层归位")),&QAction::triggered,this,[this]{resetHudLayout();});menu->addSeparator();connect(menu->addAction(QStringLiteral("退出李金金")),&QAction::triggered,this,[this]{finish();});tray->setContextMenu(menu);
            connect(tray,&QSystemTrayIcon::activated,this,[this](QSystemTrayIcon::ActivationReason reason){if(reason==QSystemTrayIcon::Trigger||reason==QSystemTrayIcon::DoubleClick)showDashboard();});tray->show();
        }
        dashboard->show();
    }
    void maybeMinimizeDashboard(){
        if(hudEditorOpen)return;
        if(!dashboard||replay||!preferences.value("coach/autoMinimize",true).toBool())return;
        const auto match=game.matchContext;const auto id=match.value("context_id").toString();
        if(!championSelectStage()&&!id.isEmpty()&&id!=lastAutoMinimizedContext&&gameWindowFound&&gameHasForeground()&&match.value("active").toBool()){
            lastAutoMinimizedContext=id;overlayPreview=false;if(dashboard->isVisible())dashboard->showMinimized();
        }
    }
    void resetMatchAnalysis(){
        forecast.invalidate();
        game.recommendationEvidence={};loadingPreparation={};loadingComputedBasis.clear();
        rosterAi.invalidate();rosterBridge.invalidate();++rosterAiRevision;rosterAiPending=false;rosterAiPlan={};anonymousHistory={};privateHistoryBasis.clear();rosterObservedBasis.clear();rosterRequestBasis.clear();rosterAttemptedBasis.clear();rosterAiDispatches=0;rosterPreDispatches=0;rosterHistoryRevision=0;rosterRetryAt=0;rosterAiState=QStringLiteral("idle");
    }
    QJsonObject usableRosterHistory()const{
        if(anonymousHistory.value("context_id")!=game.matchContext.value("context_id"))return {};
        bool available=false;for(const auto &v:anonymousHistory.value("players").toArray())for(const auto &key:{"history","same_patch","same_champion","same_patch_champion"})available|=v.toObject().value(key).toObject().value("available").toBool();
        if(!available)return {};auto out=anonymousHistory;out.remove("as_of_ms");return out;
    }
    QString rosterBasis()const{
        const auto match=game.matchContext;if(!match.value("active").toBool())return {};
        QJsonArray rows;for(const auto &v:match.value("roster").toArray()){auto p=v.toObject();p.remove("cell");rows.append(p);}
        rows=normalizedLoadoutEntries(rows).toArray();
        const QJsonObject basis{{"context_id",match.value("context_id")},{"stage",match.value("stage")==QStringLiteral("champ_select")?QStringLiteral("champ_select"):QStringLiteral("revealed")},{"roster",rows},{"observed_patch",game.observedPatch},{"history_revision",qint64(rosterHistoryRevision)}};
        return QString::fromUtf8(QJsonDocument(basis).toJson(QJsonDocument::Compact));
    }
    bool rosterDsAllowed()const{return !demo&&!replay&&!parser.isSet("self-check-ui")&&!parser.isSet("self-check-dashboard")&&!parser.isSet("fixture-feed")&&preferences.value("coach/rosterDsEnabled",true).toBool();}
    bool deepSeekPlanningEnabled()const{
        return rosterDsAllowed()&&!runningUiChecks&&!finishing&&rosterSettings.isDeepSeek()&&rosterSettings.isConfigured();
    }
    void refreshModelConfiguration(bool force=false){
        const auto now=autoClock.elapsed();if(!force&&now-settingsChecked<1000)return;
        const auto next=runtimeModelConnection(catalog.root);
        const bool enabled=preferences.value("coach/rosterDsEnabled",true).toBool();
        if(enabled!=lastRosterDsEnabled||next!=rosterSettings){
            rosterAi.invalidate();++rosterAiRevision;rosterAiPending=false;rosterAiPlan={};rosterAttemptedBasis.clear();rosterConfigurationBlocked=false;
            rosterAiState=enabled?QStringLiteral("idle"):QStringLiteral("disabled_by_user");
        }
        if(settingsLoaded&&next!=automaticSettings){invalidateAi();aiConfigurationBlocked=false;}
        rosterSettings=automaticSettings=next;lastRosterDsEnabled=enabled;settingsLoaded=true;settingsChecked=now;
        rosterDsConfigured=next.isConfigured();
        ai.configureModelConnection(next);rosterAi.configureModelConnection(next);forecast.configureModelConnection(next);
    }
    void updateRosterAdvice(){
        if(finishing)return;refreshModelConfiguration();if(!rosterDsAllowed())return;
        const auto match=game.matchContext;const auto basis=rosterBasis();if(basis.isEmpty())return;
        const auto now=autoClock.elapsed();if(basis!=rosterObservedBasis){rosterAi.invalidate();++rosterAiRevision;rosterAiPending=false;rosterAiPlan={};rosterObservedBasis=basis;rosterStableSince=now;rosterRetryAt=0;rosterAiState=QStringLiteral("settling");}
        if(!rosterDsConfigured){rosterAiState=QStringLiteral("model_not_configured");return;}
        if(game.gameMode!=QStringLiteral("KIWI")||!match.value("active").toBool()||match.value("visibility").toObject().value("allies").toObject().value("visible").toInt()==0){rosterAiState=QStringLiteral("waiting_visible_kiwi_roster");return;}
        if(rosterBridge.report().value("pending").toBool()){rosterAiState=QStringLiteral("waiting_local_history");return;}
        const bool pregame=match.value("stage")==QStringLiteral("champ_select");
        if(rosterAiPending||rosterAiDispatches>=3||(pregame&&rosterPreDispatches>=2)||basis==rosterAttemptedBasis||now-rosterStableSince<1200||now<rosterRetryAt||rosterConfigurationBlocked)return;
        rosterAiPending=true;rosterAiState=QStringLiteral("analyzing");rosterRequestBasis=basis;rosterAttemptedBasis=basis;++rosterAiDispatches;++rosterAiRevision;
        if(pregame)++rosterPreDispatches;
        rosterAi.analyzeMatch(makeMatchAiContext(catalog,game,usableRosterHistory()),rosterSettings.apiKey,rosterSettings.model,rosterAiRevision);writeReport();
    }
    bool diagnosticDsEnabled()const{return parser.isSet("diagnostic-ds")||parser.isSet("test-ds");}
    void scheduleLoadingPreparation(){
        if(uiBrowser&&browserScene==QStringLiteral("loading_pending")){loadingPreparation={};loadingComputedBasis.clear();return;}
        const auto basis=loadingPreparationBasis(catalog,game,style->currentData().toString());
        if(basis.isEmpty()){loadingPreparation={};loadingComputedBasis.clear();return;}
        if(loadingCompletion.pending()||basis==loadingComputedBasis)return;
        if(basis==failedLoadingBasis&&autoClock.elapsed()<loadingRetryAt)return;
        if(basis!=failedLoadingBasis){failedLoadingBasis.clear();loadingRetryAt=0;}
        if(!loadingCompletion.tryStart())return;
        loadingPreparation={};
        loadingWorkerBasis=basis;const auto copy=catalog;const auto snapshot=game;const auto preference=style->currentData().toString();
        loadingWorker.setFuture(QtConcurrent::run(&loadingPool,[copy,snapshot,preference]{return runLoadingPreparation(copy,snapshot,preference);}));
    }
    QString modelBasis()const{return coachHardModelBasis(catalog,game,style->currentData().toString(),options.allyExecution)+"|"+externalReferenceRevision();}
    QString cardSurfaceBasis()const{
        const auto &surface=gameWindowTracker.state();
        const auto r=surface.clientPixels;
        return QString::number(r.x())+","+QString::number(r.y())+","+QString::number(r.width())+","+QString::number(r.height())
            +","+QString::number(surface.scale,'g',12);
    }
    QString cardOfferBasis()const{return state.decisionBinding()+"|binding:"+QString::number(offerBindingRevision)
        +"|surface-revision:"+QString::number(cardSurfaceRevision)+"|surface:"+cardSurfaceBasis();}
    QString cardBasis()const{return coachCardModelBasis(catalog,game,style->currentData().toString())+"|"+externalReferenceRevision()+"|"+cardOfferBasis();}
    bool cardModelCurrent()const{return confirmedOfferSlotCount()==3&&offersBoundTo(game)&&game.fresh()&&modelRulesUsable(catalog,game)&&game.gameMode==QStringLiteral("KIWI")&&computedCardBasis==cardBasis()&&!cardDecision.isEmpty()&&coachCardClockCurrent(cardDecision.value("mechanic_cache_validity").toObject(),game.gameTime);}
    QJsonObject currentPurchaseChoiceEvidence()const{
        if(coachResult.isEmpty()||!localChoiceEvidenceCurrent(catalog,game,purchaseChoiceEvidence,false)||computedStrategy!=modelBasis())return {};
        const auto plan=coachResult.value("plan").toObject();
        const double deadline=plan.value("mechanic_valid_until").toDouble(-1);
        if(!std::isfinite(deadline)||(deadline>=0&&game.gameTime>=deadline))return {};
        return purchaseChoiceEvidence;
    }
    QJsonObject currentLocalChoicePacket()const{
        QJsonObject packet;
        const auto purchase=currentPurchaseChoiceEvidence();
        if(!purchase.isEmpty())packet["purchase"]=purchase;
        if(!augmentChoiceEvidence.isEmpty()&&cardModelCurrent()&&localChoiceEvidenceCurrent(catalog,game,augmentChoiceEvidence,true))
            packet["augment"]=augmentChoiceEvidence;
        return packet;
    }
    void scheduleCardModel(){
        if(finishing||!offersBoundTo(game)||!game.fresh()||!modelRulesUsable(catalog,game)||game.gameMode!="KIWI"||confirmedOfferSlotCount()!=3||game.offers.isEmpty()
            ||!catalog.combat||!catalog.combat->champions.contains(game.championId)||game.level<1||game.level>18||!std::isfinite(game.gameTime)||game.gameTime<0){if(cardWork)cardWork->cancel();cardDecision={};computedCardBasis.clear();return;}
        const auto basis=cardBasis();
        const auto hard=coachStructuralBasis(game)+"|"+externalReferenceRevision()+"|"+style->currentData().toString()+"|"+cardOfferBasis();
        // Soft panel ticks coalesce without repeatedly preempting valid work.
        // A different offer, inventory or match cancels the obsolete job.
        if(cardCompletion.pending()){if(cardWorkerHardBasis!=hard&&cardWork)cardWork->cancel();return;}
        if(cardModelCurrent())return;
        if(basis==failedCardBasis&&autoClock.elapsed()<cardRetryAt)return;
        if(basis!=failedCardBasis){failedCardBasis.clear();cardRetryAt=0;}
        cardInputAgeMs=game.source==GameState::Source::Live?std::max(qint64(0),game.received.msecsTo(QDateTime::currentDateTimeUtc())):0;
        cardAssignedBudgetMs=std::min(cardBudgetMs,std::max(qint64(0),qint64(3900)-cardInputAgeMs));
        if(cardAssignedBudgetMs<=0)return;
        if(!cardCompletion.tryStart())return;
        cardWorkerBasis=basis;cardWorkerHardBasis=hard;cardWorkerContext=game.matchContext.value("context_id").toString();cardStartedMs=autoClock.elapsed();const auto copy=catalog;const auto snapshot=game;const auto preference=style->currentData().toString();
        cardWork=std::make_shared<RecommendationWork>(cardAssignedBudgetMs);const auto work=cardWork;
        cardWorker.setFuture(QtConcurrent::run(&cardPool,[copy,snapshot,preference,work]{
            auto decision=buildAugmentDecision(copy,snapshot,{},preference,{},true,work);
            decision["mechanic_cache_validity"]=coachCardMechanicValidity(copy,snapshot);
            if(decision.value("computation_complete").toBool()&&!work->stopped())
                decision["local_choice_evidence"]=buildLocalChoiceEvidence(copy,snapshot,decision,true,work);
            return decision;
        }));
    }
    bool intersectsSelection(const QRect &rect)const{for(const auto &r:cardPlacement.cards)if(rect.intersects(r))return true;for(const auto &r:cardPlacement.hints)if(rect.intersects(r))return true;return false;}
    bool sidePanelsAvoidCards()const{return !selectionActive||!right->isVisible()||!intersectsSelection(right->geometry());}
    void refreshSourceStatus(){
        if(replay){sourceHealth=QJsonObject{{"status",demo?"synthetic_demo":"historical_replay"},{"selection_confirmation_available",false}};return;}
        QJsonObject next{{"status","not_reported"},{"source","wegame_local_log"},{"selection_confirmation_available",false},{"selected_complete",false}};
        const QFileInfo events(input);QFile file(events.dir().filePath(events.completeBaseName()+QStringLiteral(".source.json")));
        if(file.open(QIODevice::ReadOnly)&&file.size()<=32*1024){const auto raw=QJsonDocument::fromJson(file.readAll()).object();const auto checked=QDateTime::fromString(raw.value("checked_at").toString(),Qt::ISODateWithMs);const qint64 age=checked.msecsTo(QDateTime::currentDateTimeUtc());
            const QStringList statuses{"logs_active","logs_idle","directory_missing","logs_missing","read_error","stopped"};const auto status=raw.value("status").toString();
            if(raw.value("schema_version").toInt()==1&&raw.value("source")=="wegame_local_log"&&raw.value("capture_mode")=="live"&&checked.isValid()&&age>=-5000&&statuses.contains(status)){
                next["status"]=raw.value("running").toBool()&&age>5000?QStringLiteral("collector_unresponsive"):status;next["checked_at"]=checked.toUTC().toString(Qt::ISODateWithMs);next["running"]=raw.value("running").toBool();
                for(const auto &key:{"log_file_count","read_errors","recent_read_errors","event_count"})if(raw.value(key).isDouble()&&raw.value(key).toDouble()>=0)next[key]=raw.value(key);
                const auto capture=raw.value("card_capture").toObject();QJsonObject cardCounts;
                for(const auto &key:{"reroll_events","short_offer_events","malformed_log_events","stale_log_lines","pick_signals_without_offer"}){
                    const auto count=capture.value(key);if(count.isDouble()&&std::isfinite(count.toDouble())&&count.toDouble()>=0)cardCounts[key]=count;
                }
                if(!cardCounts.isEmpty())next["card_capture"]=cardCounts;
                QJsonObject counts,at;for(const auto &kind:{"offer","clear","pick_signal","selected_hint","overlay_visibility"}){const auto count=raw.value("event_counts").toObject().value(kind);if(count.isDouble()&&count.toDouble()>=0)counts[kind]=count;const auto stamp=QDateTime::fromString(raw.value("last_event_at").toObject().value(kind).toString(),Qt::ISODateWithMs);if(stamp.isValid())at[kind]=stamp.toUTC().toString(Qt::ISODateWithMs);}next["event_counts"]=counts;next["last_event_at"]=at;
            }
        }
        sourceHealth=next;
        if(!runtimeLog)return;
        using Event = RuntimeLog::Event;
        using Outcome = RuntimeLog::Outcome;
        const auto liveOutcome=game.fresh()?Outcome::Ready:
            (lastLiveLogOutcome==Outcome::Ready||lastLiveLogOutcome==Outcome::Stale?Outcome::Stale:Outcome::Waiting);
        if(liveOutcome!=lastLiveLogOutcome){runtimeLog->record(Event::LiveClientState,liveOutcome);lastLiveLogOutcome=liveOutcome;}
        const auto status=next.value("status").toString();
        auto feedOutcome=Outcome::Waiting;
        if(status==QStringLiteral("logs_active"))feedOutcome=Outcome::Ready;
        else if(status==QStringLiteral("directory_missing")||status==QStringLiteral("logs_missing"))feedOutcome=Outcome::Missing;
        else if(status==QStringLiteral("collector_unresponsive"))feedOutcome=Outcome::Stale;
        else if(status==QStringLiteral("stopped"))feedOutcome=Outcome::Stopped;
        else if(status==QStringLiteral("read_error"))feedOutcome=Outcome::Failed;
        if(feedOutcome!=lastFeedLogOutcome){runtimeLog->record(Event::OfferFeedState,feedOutcome);lastFeedLogOutcome=feedOutcome;}
    }
    void renderSourceLabels(){
        QString text;
        if(demo)text=QStringLiteral("演示");else if(replay)text=QStringLiteral("回放");else if(state.status=="open")text=QStringLiteral("选牌中");else{
            const auto status=sourceHealth.value("status").toString();
            if(status=="logs_active")text=QStringLiteral("候选已连接");else if(status=="logs_idle")text=QStringLiteral("候选等待中");else if(status=="read_error")text=QStringLiteral("候选读取重试");else if(status=="stopped"||status=="collector_unresponsive")text=QStringLiteral("候选已断开");else text=QStringLiteral("候选待连接");
        }
        sourceStatus->setToolTip(QStringLiteral("对局 %1 / 本地规则 %2%3").arg(game.observedPatch.isEmpty()?QStringLiteral("未知"):game.observedPatch,catalog.patch,modelPatchMatches(catalog,game)?QString():QStringLiteral(" · 沿用本地规则估算")));
        text+=QStringLiteral(" · ")+(game.selected.isEmpty()?QStringLiteral("已选未知"):QStringLiteral("已选 %1").arg(game.selected.size()));
        if(through)text+=QStringLiteral(" · 穿透");
        sourceStatus->setText(sourceStatus->fontMetrics().elidedText(text,Qt::ElideRight,std::max(120,sourceStatus->width())));
    }
    static QString cardDisplayMode(const QJsonObject &row){
        const auto semantics=row.value("metric_semantics");
        if(semantics==QStringLiteral("conditional_model_gain_not_win_rate"))
            return row.value("compared_candidate_count").toInt()==3?QStringLiteral("comparison"):QStringLiteral("conditional");
        if(semantics==QStringLiteral("public_reference_preference_not_model_gain"))return QStringLiteral("reference");
        if(semantics==QStringLiteral("conditional_equipment_reference_not_model_gain")
            ||semantics==QStringLiteral("conditional_option_not_numeric_gain"))return QStringLiteral("conditional");
        if(semantics==QStringLiteral("mechanism_reference_not_numeric_gain"))return QStringLiteral("mechanism");
        return QStringLiteral("unknown");
    }
    void recordCardState(bool current,const QString &hideReason){
        QJsonArray ids;for(const auto &candidate:state.candidates){const auto c=candidate.toObject();ids.append(QJsonObject{{"candidate_index",c.value("candidate_index")},{"augment_id",c.value("augment_id")}});}
        QJsonObject entry{{"active",selectionActive},{"round",state.round},{"offer_status",state.status},{"candidates",ids},{"current_model",current},{"model_state",modelState},{"game_foreground",gameForeground},{"hidden_by_user",hiddenByUser}};
        auto stable=entry;stable.remove("model_state");if(state.status!="open")stable.remove("current_model");
        entry["awaiting_complete_offer"]=incompleteOfferCount>=0;
        const QJsonValue nil(QJsonValue::Null);
        QJsonArray offerSlots{nil,nil,nil},displaySlots{nil,nil,nil};
        QSet<int> ambiguousSlots;
        for(const auto &value:ids){
            const auto row=value.toObject();const auto slot=row.value("candidate_index");
            const int index=slot.toInt(-1);
            if(index<0||index>=3||slot.toDouble()!=index)continue;
            if(!offerSlots.at(index).isNull())ambiguousSlots.insert(index);
            offerSlots.replace(index,ambiguousSlots.contains(index)?nil:row.value("augment_id"));
        }
        QJsonArray metrics;
        for(int i=0;i<cardHints.size();++i){
            auto hint=cardHints[i];if(!hint->isVisible())continue;
            const auto row=hint->report();
            const auto mode=cardDisplayMode(row);
            QJsonObject display{{"augment_id",row.value("augment_id")},{"mode",mode}};
            const auto captionRect=row.value("caption_rect").toArray();
            if(mode==QStringLiteral("comparison")&&captionRect.size()==4
                &&captionRect.at(2).toInt()>0&&captionRect.at(3).toInt()>0){
                const auto caption=row.value("displayed_metric_caption").toString();
                if(caption.startsWith(QStringLiteral("3秒领先")))display["leading_window_seconds"]=3;
                if(caption.startsWith(QStringLiteral("20秒领先")))display["leading_window_seconds"]=20;
            }
            displaySlots.replace(i,display);
            metrics.append(QJsonObject{{"augment_id",row.value("augment_id")},{"metric",row.value("metric")},{"recommendation",row.value("recommendation")},{"recommendation_basis",row.value("recommendation_basis")},{"evaluation_status",row.value("evaluation_status")},{"evaluation_reason",row.value("evaluation_reason")},{"evaluation_scope",row.value("evaluation_scope")},{"displayed_explanation",row.value("displayed_explanation")},{"font_bounds_fit",row.value("title_fits").toBool()&&row.value("metric_fits").toBool()&&row.value("line_heights_fit").toBool()&&row.value("badge_fits").toBool()}});
        }
        if(state.status=="open"){entry["labels"]=metrics;stable["labels"]=metrics;}
        if(state.offerRevision>0&&state.round>0&&offersBoundTo(game)){
            const bool waiting=hideReason==QStringLiteral("reroll_pending")||hideReason==QStringLiteral("waiting_offer")
                ||hideReason==QStringLiteral("incomplete_offer");
            QJsonObject surface{{"offer_revision",qint64(state.offerRevision)},{"round",state.providerOfferRound()},
                {"offer_slots",offerSlots},{"display_slots",displaySlots},
                {"disposition",!metrics.isEmpty()?QStringLiteral("shown"):waiting?QStringLiteral("waiting"):QStringLiteral("hidden")},
                {"hide_reason",metrics.isEmpty()?QJsonValue(hideReason):nil}};
            if(state.observed.isValid()&&(state.status==QStringLiteral("open")||state.awaitingReroll))
                surface["offer_observed_at_ms"]=state.observed.toMSecsSinceEpoch();
            entry["card_surface"]=surface;stable["card_surface"]=surface;
            if(recommendationTrace){
                if(!state.offerSourceRef.isEmpty())surface["offer_source_ref"]=state.offerSourceRef;
                recommendationTrace->record(catalog,game,coachResult,current?cardDecision:QJsonObject{},
                    QJsonObject{{"card_surface",surface}},RecommendationTraceBinding{
                        coachResultContext,cardResultContext,game.matchContext.value("context_id").toString()});
            }
        }
        if(stable==cardTraceKey)return;cardTraceKey=stable;
        entry["observed_at"]=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);entry["elapsed_ms"]=autoClock.elapsed();entry["last_model_elapsed_ms"]=lastModelElapsedMs;
        if(state.observed.isValid())entry["since_offer_observed_ms"]=state.observed.msecsTo(QDateTime::currentDateTimeUtc());
        if(offerReceivedAt.isValid())entry["since_offer_received_ms"]=offerReceivedAt.msecsTo(QDateTime::currentDateTimeUtc());
        if(sourceCollectedAt.isValid()&&offerReceivedAt.isValid())entry["source_to_panel_ms"]=sourceCollectedAt.msecsTo(offerReceivedAt);
        cardTransitions.append(entry);while(cardTransitions.size()>128)cardTransitions.removeAt(0);
    }
    void setUserHidden(bool hidden){hiddenByUser=hidden;renderCardHints();writeReport();}
    void renderDeathTips(){
        if(!deathTips)return;
        inlineDeathInsight={};
        if(uiBrowser){
            if(browserScene==QStringLiteral("death_tips")){
                const int index=int(autoClock.elapsed()/8000+browserMotionRevision)%4;
                auto tip=DeathTipVisual::previewCard(index);tip["reduced_motion"]=Kda::reducedMotion();tip["deck_index"]=index;tip["deck_count"]=4;
                deathTips->present(viewport,tip,right->configuredOpacity());
            }
            else deathTips->dismiss();return;
        }
        const bool allowed=preferences.value("coach/overlayEnabled",true).toBool()&&!hiddenByUser&&!selectionActive&&gameForeground;
        const auto deck=allowed?deathInsightsForState(catalog,game,coachResult,liveSummary,equipmentCueResult):QJsonArray{};
        if(deck.isEmpty()){deathTipsSinceMs=-1;if(!gameForeground)deathTips->hide();else deathTips->dismiss();return;}
        const auto now=autoClock.elapsed();if(deathTipsSinceMs<0)deathTipsSinceMs=now;
        const int index=int((now-deathTipsSinceMs)/8000)%deck.size();auto tip=deck.at(index).toObject();tip["deck_index"]=index;tip["deck_count"]=deck.size();
        // Reuse the HUD footer: a second window in the upper-left obscures
        // the game's death recap. The full evidence stays in this report.
        tip["hud_headline"]=deathInsightHeadline(tip);inlineDeathInsight=tip;deathTips->hide();
    }
    void deathTipsUiChecks(){
        const auto savedGame=game;const auto savedSummary=liveSummary;const bool savedForeground=gameForeground,savedHidden=hiddenByUser,savedSelection=selectionActive;
        game.source=GameState::Source::Live;game.received=QDateTime::currentDateTimeUtc();game.phase=QStringLiteral("InProgress");game.gameMode=QStringLiteral("KIWI");game.observedPatch=catalog.patch;
        game.players=QJsonArray{QJsonObject{{"self",true},{"champion_id",game.championId},{"team","ORDER"},{"is_dead",true}}};
        gameForeground=true;hiddenByUser=false;selectionActive=false;for(auto hint:cardHints)hint->hide();renderDeathTips();
        uiChecks["death_tip_no_generic_reminders_without_evidence"]=!deathTips->isVisible();
        QJsonArray observedRows,modelRows;QList<int> ids{game.championId};for(int id:catalog.champions.keys())if(!ids.contains(id)&&ids.size()<10)ids.append(id);
        for(int i=0;i<10;++i){observedRows.append(QJsonObject{{"champion_id",ids[i]},{"self",i==0},{"team",i<5?"ORDER":"CHAOS"},{"is_dead",i==0}});modelRows.append(QJsonObject{{"row_index",i},{"champion_id",ids[i]},{"team",i<5?"ALLY":"ENEMY"},{"model_available",true},{"items_complete",true},{"output_8s_estimate",100.+i*100.},{"effective_health_estimate",1000.+i*100.}});}
        game.players=observedRows;game.gameTime=100;game.matchContext=QJsonObject{{"context_id","synthetic-inline-death"},{"active",true}};liveSummary=QJsonObject{{"fresh",true},{"current_context",true},{"context_id","synthetic-inline-death"},{"game_time",100.},{"players",modelRows}};
        renderDeathTips();renderAutomaticAdvice();
        uiChecks["death_insight_kept_in_record_without_live_explanatory_copy"]=!deathTips->isVisible()&&!inlineDeathInsight.isEmpty()&&routeLabel->text().isEmpty()&&routeLabel->toolTip().isEmpty();
        deathTips->present(viewport,DeathTipVisual::previewCard(1));
        const auto shown=deathTips->report();uiChecks["death_tip_top_center_click_through"]=shown.value("visible").toBool()&&shown.value("text_fits").toBool()&&shown.value("click_through").toBool()&&!shown.value("accepts_keyboard_focus").toBool()&&deathTips->geometry().center().x()==viewport.center().x();
        if(parser.isSet("snapshot")){const QFileInfo file(parser.value("snapshot"));snapshot(file.absolutePath()+"/"+file.completeBaseName()+"-death-tips.png");}
        gameForeground=false;renderDeathTips();uiChecks["death_tip_leaves_when_game_not_foreground"]=deathTips->report().value("motion").toObject().value("phase")==QStringLiteral("exit")||!deathTips->isVisible();deathTips->hide();
        gameForeground=true;selectionActive=true;renderDeathTips();uiChecks["death_tip_does_not_overlap_selection"]=!deathTips->isVisible();selectionActive=false;
        auto self=game.players[0].toObject();self["is_dead"]=false;game.players[0]=self;renderDeathTips();uiChecks["death_tip_hidden_after_respawn"]=!deathTips->isVisible();
        game=savedGame;liveSummary=savedSummary;gameForeground=savedForeground;hiddenByUser=savedHidden;selectionActive=savedSelection;deathTipsSinceMs=-1;deathTips->hide();renderCardHints();
    }
    void renderCombatCorner(bool allow){
        if(hudEditorOpen||!allow||!game.fresh()||!combatCorner->ready()){
            combatCorner->hide();
            return;
        }
        const auto coach=right->isVisible()?right->geometry():QRect();
        bool expanded=combatCorner->wantsDetails();
        const auto combatLane=safeOverlayPlacement(viewport);
        auto bounds=combatCornerPlacement(combatLane,coach,
            expanded?CornerCombatHud::expandedHeight:CornerCombatHud::compactHeight);
        if(expanded&&bounds.isEmpty()){
            expanded=false;
            bounds=combatCornerPlacement(combatLane,coach,CornerCombatHud::compactHeight);
        }
        if(hudLayout.contains("combat")){
            bounds=hudLayout.resolve("combat",viewport,bounds);
            if(!expanded)bounds.setHeight(std::min(bounds.height(),qRound(bounds.height()*double(CornerCombatHud::compactHeight)/CornerCombatHud::expandedHeight)));
        }
        combatCorner->present(bounds,expanded,right->configuredOpacity());
    }
    void attachHistoricalCardComparison(QJsonObject &hint,const QJsonArray &decisions)const{
        const int augment=hint.value("augment_id").toInt();
        if(augment<=0||hint.value("display_evaluation_status")==QStringLiteral("data_conflict")
            ||hint.value("evaluation_status")==QStringLiteral("data_conflict"))return;
        for(const auto &value:decisions){
            const auto candidate=value.toObject();
            if(candidate.value("augment_id")!=QJsonValue(augment))continue;
            const auto pair=candidate.value("historical_loadout_reference").toObject();
            const int item=pair.value("item_id").toInt();
            if(pair.value("augment_id")!=QJsonValue(augment)||item<=0||!game.owned.contains(item))return;
            const auto exposed=pair.value("exposed").toObject(),control=pair.value("control").toObject();
            const auto attach=[&](const char *metric,const char *samples,const QString &name,int precision){
                const auto a=exposed.value(metric),b=control.value(metric);
                const int n=exposed.value(samples).toInt(),m=control.value(samples).toInt();
                if(n<=0||m<=0||!a.isDouble()||!b.isDouble()||!std::isfinite(a.toDouble())||!std::isfinite(b.toDouble()))return false;
                hint["historical_comparison"]=QStringLiteral("历史%1 %2/%3 · %4/%5局")
                    .arg(name,num(a.toDouble(),precision),num(b.toDouble(),precision)).arg(n).arg(m);
                hint["historical_comparison_description"]=QStringLiteral("同版本同英雄历史 %1 · 同强化有%2 %3（%4局）/ 无该装备 %5（%6局）")
                    .arg(name,catalog.itemName(item),num(a.toDouble(),precision)).arg(n).arg(num(b.toDouble(),precision)).arg(m);
                return true;
            };
            if(!attach("average_damage_per_minute","damage_samples",QStringLiteral("分伤"),0))
                attach("average_kda","kda_samples",QStringLiteral("KDA"),1);
            return;
        }
    }
    void attachCardRerollComparisons(QJsonArray &hints, bool current) const {
        if(!current||hints.size()!=3||!cardDecision.value("ranking_quality").toObject().value("ranking_allowed").toBool())return;
        const auto scope=hints.first().toObject().value("comparison_scope");
        const bool partial=scope==QStringLiteral("known_model_shared_unknowns");
        if(!partial&&scope!=QStringLiteral("complete_current_model"))return;
        QVector<double> scores;
        for(const auto &value:hints){
            const auto row=value.toObject();const auto score=row.value("decision_gain_percent");
            if(row.value("recommendation_basis")!=QStringLiteral("decision_gain_percent")
                ||row.value("comparison_scope")!=scope
                ||row.value("compared_candidate_count")!=3||!score.isDouble()||!std::isfinite(score.toDouble()))return;
            scores.append(score.toDouble());
        }
        const auto rule=observedRerollRule(game);
        const auto ruleBasis=cardDecision.value("rule_basis").toObject();
        for(const auto &value:state.candidates){
            const auto card=value.toObject();const int slot=card.value("candidate_index").toInt(-1);
            if(slot<0||slot>=hints.size()||card.value("rerollable")!=QJsonValue(true))continue;
            auto row=hints.at(slot).toObject();
            if(row.value("augment_id")!=card.value("augment_id"))continue;
            auto comparison=singleSlotRerollTradeoff(scores,slot);
            if(!comparison.value("available").toBool())continue;
            comparison["rule"]=rule;
            comparison["model_scope"]=scope;
            comparison["patch_relation"]=ruleBasis.value("patch_relation");
            comparison["rules_patch"]=ruleBasis.value("source_patch");
            const bool preserved=comparison.value("preserves_current_best").toBool();
            const bool nearBest=!preserved&&comparison.value("floor_delta").toDouble()>=-.05;
            comparison["label"]=preserved?QStringLiteral("留最高"):nearBest?QStringLiteral("留近优"):QStringLiteral("失最高");
            comparison["detail"]=preserved?QStringLiteral("若只替换此槽，其他牌中仍有当前最高模型分")
                :nearBest?QStringLiteral("若只替换此槽，其他牌中仍有接近最高模型分的选择")
                :QStringLiteral("若只替换此槽，会放弃当前最高模型分的牌");
            const auto axes=row.value("scenario_sensitivity").toObject().value("single_slot_retained_by_axis").toObject();
            if(!axes.isEmpty())comparison["retained_by_axis"]=axes;
            QStringList lostWindows;
            for(const int seconds:{3,8,20}){
                const auto delta=axes.value(QStringLiteral("current_model:%1").arg(seconds)).toObject().value("floor_delta_points");
                if(delta.isDouble()&&delta.toDouble()<-.05)lostWindows.append(QString::number(seconds));
            }
            if(preserved&&!lostWindows.isEmpty()){
                comparison["label"]=lostWindows.size()==1?QStringLiteral("失%1秒优").arg(lostWindows.first()):QStringLiteral("失分窗优");
                comparison["detail"]=QStringLiteral("若只替换此槽，综合分最高的牌仍在，但会失去 %1 秒条件下的最高模型分")
                    .arg(lostWindows.join(QLatin1Char('/')));
            }
            if(partial)comparison["detail"]=comparison.value("detail").toString()+QStringLiteral("，仅比较已建模部分");
            row["reroll_comparison"]=comparison;hints[slot]=row;
        }
    }
    QJsonArray buildCardHints(bool current){
        ++cardHintBuilds;
        auto visibleState=state;QJsonArray allowedCards;
        for(const auto &v:state.candidates)if(catalog.ordinaryOfferEligible(v.toObject().value("augment_id").toInt()))allowedCards.append(v);
        visibleState.candidates=allowedCards;
        auto hints=orderedCardHints(visibleState,cardDecision,{},game.selected,current);
        attachCardRerollComparisons(hints,current);
        const bool numericPick=std::any_of(hints.cbegin(),hints.cend(),[](const QJsonValue &v){const auto row=v.toObject();return row.value("numeric_gain_available").toBool()&&row.value("recommended").toBool();});
        const bool needsReferences=!numericPick||std::any_of(hints.cbegin(),hints.cend(),[](const QJsonValue &v){return !v.toObject().value("numeric_gain_available").toBool();});
        QJsonObject references;
        // A new accepted offer may contain the same three IDs. Keep the actual
        // panel round/session/timestamp binding, including batched clear/offer.
        const auto referenceBinding=cardOfferBasis()+"|observed:"+state.observed.toString(Qt::ISODateWithMs);
        if(needsReferences)references=cardReferenceCache.get(catalog,game,referenceBinding);
        else cardReferenceCache.clear();
        for(int i=0;i<hints.size();++i){auto row=hints[i].toObject();if(!row.value("numeric_gain_available").toBool()){
            const auto reference=references.value("mechanism_references").toObject().value(QString::number(row.value("augment_id").toInt())).toObject();
            if(reference.value("available").toBool()){row["source_mechanism_reference"]=reference;row["metric"]=reference.value("title");row["decision_hint"]=reference.value("decision_hint");row["evaluation_hint"]=reference.value("condition");row["recommended"]=false;row["ranking_available"]=false;row.remove("recommendation");row.remove("rank");hints[i]=row;}
        }}
        // A practical reference suggestion is distinct from a qualified numeric
        // comparison. Build it from this offer only, never a previous round.
        if(!numericPick){
            const auto advisory=references.value("advisory").toObject();
            if(advisory.value("available").toBool())for(int i=0;i<hints.size();++i){auto row=hints[i].toObject();row["advisory"]=advisory;
                row["advisory_preferred"]=advisory.value("preferred_augment_id").toInt()>0&&advisory.value("preferred_augment_id")==row.value("augment_id");
                row["advisory_label"]=advisory.value("label").toString(QStringLiteral("条件建议"));
                for(const auto &v:advisory.value("alternatives").toArray()){const auto option=v.toObject();if(option.value("augment_id")==row.value("augment_id")){row["advisory_option"]=option;const auto cue=option.value("condition").toString();if(!cue.isEmpty())row["decision_hint"]=cue;break;}}
                hints[i]=row;
            }
        }
        const auto cardRole=combatRoleIntent(catalog,game,style->currentData().toString());
        if(cardRole.value("support_role").toBool()||cardRole.value("tank_role").toBool())for(int i=0;i<hints.size();++i){auto row=hints[i].toObject();if(row.value("recommended").toBool()){row["recommendation"]=cardRole.value("comparison_label");row["objective_scope"]=QStringLiteral("known_damage_and_self_survival_only");}hints[i]=row;}
        if(current){
            const auto decisions=cardDecision.value("offers").toArray();
            for(int i=0;i<hints.size();++i){auto row=hints[i].toObject();attachHistoricalCardComparison(row,decisions);hints[i]=row;}
            if(localChoiceEvidenceCurrent(catalog,game,augmentChoiceEvidence,true)){
                const auto estimates=augmentChoiceEvidence.value("candidates").toArray();
                for(int i=0;i<hints.size();++i){
                    auto hint=hints.at(i).toObject();
                    for(const auto &value:estimates){
                        const auto row=value.toObject();
                        if(row.value("augment_id")!=hint.value("augment_id"))continue;
                        const auto text=localChoiceComponentText(row);
                        if(!text.isEmpty()){
                            const auto previous=hint.value("evaluation_hint").toString();
                            hint["evaluation_hint"]=previous.isEmpty()?text:previous+QStringLiteral(" · ")+text;
                        }
                        break;
                    }
                    hints[i]=hint;
                }
            }
        }
        if(!modelRulesUsable(catalog,game))for(int i=0;i<hints.size();++i){auto row=hints[i].toObject();row["metric"]=QStringLiteral("规则未就绪");row["numeric_gain_available"]=false;row["recommended"]=false;row["evaluation_status"]=QStringLiteral("input_missing");row["evaluation_reason"]=QStringLiteral("本地规则尚未载入，等待规则恢复");row["evaluation_hint"]=QStringLiteral("规则载入后重新计算");for(const auto &key:{"recommendation","rank","current_gain_percent","output_gain_percent","effective_health_gain_percent","axis_gain_semantics","rule_fit_score","partial","comparison_basis","evaluation_stat_changes","decision_hint","source_mechanism_reference"})row.remove(QString::fromLatin1(key));hints[i]=row;}
        return hints;
    }
    void renderCardHints(){
        if(!right||cardHints.size()!=3)return;
        if(hudEditorOpen){right->setPresentedVisible(false,false);combatCorner->hide();deathTips->hide();for(auto hint:cardHints)hint->hide();selectionActive=false;recordCardState(false,QStringLiteral("hud_editor"));return;}
        gameForeground=gameWindowFound&&gameHasForeground();
        if(!cardLayoutBuilds||viewport!=cardPlacementViewport||hudLayout!=cardPlacementLayout){
            cardPlacementViewport=viewport;
            cardPlacementLayout=hudLayout;
            cardPlacement=selectionCardLayout(viewport);
            for(int i=0;i<cardPlacement.hints.size();++i)cardPlacement.hints[i]=hudLayout.resolve(QStringLiteral("card%1").arg(i+1),viewport,cardPlacement.hints[i]);
            ++cardLayoutBuilds;
        }
        scoreboardHeld=uiBrowser&&browserScene==QStringLiteral("scoreboard");
#ifdef Q_OS_WIN
        // Read only the default hold-to-show scoreboard key, and only while
        // the tracked game owns foreground. The game still receives the key.
        scoreboardHeld=scoreboardHeld||(gameForeground&&(GetAsyncKeyState(VK_TAB)&0x8000));
#endif
        const bool overlayEnabled=preferences.value("coach/overlayEnabled",true).toBool();
        const bool cardHintsEnabled=overlayEnabled&&preferences.value("coach/cardHintsEnabled",true).toBool();
        const bool bound=state.status==QStringLiteral("open")&&offersBoundTo(game);
        const bool choosingCards=selectionHintsVisible(state,bound&&game.fresh()&&game.gameMode==QStringLiteral("KIWI"),gameForeground,demo,hiddenByUser);
        const int confirmedSlots=confirmedOfferSlotCount();
        const bool completeOffer=confirmedSlots==3;
        const int nextIncomplete=choosingCards&&cardHintsEnabled&&!completeOffer?confirmedSlots:-1;
        if(nextIncomplete!=incompleteOfferCount){
            incompleteOfferCount=nextIncomplete;
            renderAutomaticAdvice(false);
        }
        const bool current=completeOffer&&bound&&cardModelCurrent();
        if(!completeOffer||!bound)cardReferenceCache.clear();
        const bool mayShowCards=!scoreboardHeld&&cardHintsEnabled&&completeOffer&&cardPlacement.valid&&choosingCards;
        const auto hints=mayShowCards?buildCardHints(current):QJsonArray{};
        selectionActive=mayShowCards&&hints.size()==3;
        for(int i=0;i<cardHints.size();++i){if(selectionActive)cardHints[i]->present(cardPlacement.hints[i],hints[i].toObject(),right->configuredOpacity());else cardHints[i]->hide();}
        const bool endingVisible=endHudUntil>autoClock.elapsed();const bool finishedHud=QStringList{"EndOfGame","PreEndOfGame","WaitingForStats"}.contains(game.phase)&&!endingVisible;const bool dashboardIdle=dashboard&&!replay&&!game.matchContext.value("active").toBool()&&!game.fresh()&&!overlayPreview&&!endingVisible;
        const bool gameSurfaceVisible=demo||replay||overlayPreview||runningUiChecks||gameForeground;
        const bool safeLane=usingClientViewport?placement.right.contains(right->geometry())&&overlayAvoidsProtectedAreas(placement):hudLayout.contains("coach")?viewport.contains(right->geometry()):placement.right.contains(right->geometry())&&overlayAvoidsProtectedAreas(placement);
        const bool pendingCardStatus=nextIncomplete>=0&&cardPlacement.valid&&!intersectsSelection(right->geometry());
        rightAvoidingCards=choosingCards&&!pendingCardStatus;
        const bool mayPresent=gameSurfaceVisible&&safeLane&&!rightAvoidingCards&&!scoreboardHeld;
        right->setPresentedVisible(mayPresent&&overlayEnabled&&!hiddenByUser&&!dashboardIdle&&!finishedHud,mayPresent);
        const bool inBattle=game.phase==QStringLiteral("InProgress")||game.phase==QStringLiteral("Reconnect");
        renderCombatCorner((demo||replay||gameForeground)&&overlayEnabled&&!hiddenByUser&&!choosingCards&&!scoreboardHeld&&inBattle);
        QString hideReason=QStringLiteral("unknown");
        if(state.awaitingReroll)hideReason=QStringLiteral("reroll_pending");
        else if(state.status==QStringLiteral("closed"))hideReason=QStringLiteral("offer_closed");
        else if(state.status==QStringLiteral("stale"))hideReason=QStringLiteral("offer_stale");
        else if(state.status==QStringLiteral("waiting"))hideReason=QStringLiteral("waiting_offer");
        else if(!bound)hideReason=QStringLiteral("unbound_offer");
        else if(hiddenByUser)hideReason=QStringLiteral("user_hidden");
        else if(!cardHintsEnabled)hideReason=QStringLiteral("overlay_disabled");
        else if(scoreboardHeld)hideReason=QStringLiteral("scoreboard");
        else if(!completeOffer)hideReason=QStringLiteral("incomplete_offer");
        else if(!cardPlacement.valid)hideReason=QStringLiteral("invalid_layout");
        else if(!gameForeground&&!demo&&!replay)hideReason=QStringLiteral("foreground_lost");
        recordCardState(current,hideReason);
        if(scoreboardHeld)deathTips->hide();else renderDeathTips();
    }
    void invalidateAi(){const bool hadAdvice=!aiPlan.isEmpty()||aiPending;ai.invalidate();++aiRevision;aiPlan={};aiPending=false;requestBasis.clear();autoAdvice.clear(autoClock.elapsed());if(hadAdvice&&aiStatus)aiStatus->setText(QStringLiteral("局面已更新 · 自动重新分析"));}
    void applyManual(GameState &s){applyConfirmedMatchInputs(catalog,s,manual,state.round);}
    void editAugments(int initialIndex){
        if(initialIndex<0||initialIndex>=game.players.size())return;
        const auto contextId=game.matchContext.value("context_id");const auto roster=rosterKey(game);const int confirmationRound=state.round;
        QDialog dialog(dashboard);dialog.setWindowTitle(QStringLiteral("记录本局信息"));auto layout=new QVBoxLayout(&dialog);layout->setSpacing(10);
        auto who=new Kda::ComboBox;who->setAccessibleName(QStringLiteral("校正的英雄"));
        for(const auto &v:game.players){const auto p=v.toObject();who->addItem((p.value("self").toBool()?QStringLiteral("你 · "):QString())+catalog.championName(p.value("champion_id").toInt()));}who->setCurrentIndex(initialIndex);layout->addWidget(who);
        auto info=new QLabel(QStringLiteral("填入已选强化的完整中文名或 ID，用逗号分隔。每轮选牌后重新核对。"));info->setWordWrap(true);layout->addWidget(info);
        auto entry=new QLineEdit;entry->setAccessibleName(QStringLiteral("已选强化"));entry->setPlaceholderText(QStringLiteral("已选强化"));layout->addWidget(entry);
        auto complete=new PreferenceSwitch(QStringLiteral("已核对完整列表"));layout->addWidget(complete);
        auto forms=new Kda::ComboBox;forms->setAccessibleName(QStringLiteral("英雄形态情景"));layout->addWidget(forms);
        auto growthLabel=new QLabel(QStringLiteral("心之钢永久生命 · 可选情景"));auto growth=new QLineEdit;growth->setAccessibleName(QStringLiteral("心之钢永久生命情景"));growth->setPlaceholderText(QStringLiteral("留空保持未知"));auto growthNote=new QLabel(QStringLiteral("手动值仅作条件比较，30 秒后失效；不会视为自动观测。"));growthNote->setWordWrap(true);for(auto w:{static_cast<QWidget*>(growthLabel),static_cast<QWidget*>(growth),static_cast<QWidget*>(growthNote)})layout->addWidget(w);
        auto load=[&]{const int index=who->currentIndex();if(index<0||index>=game.players.size())return;const auto p=game.players[index].toObject();QStringList names;for(const auto &v:p.value("augment_ids").toArray())names.append(catalog.augments.value(v.toInt()).value("name_zh_cn").toString(QString::number(v.toInt())));entry->setText(names.join(QStringLiteral("，")));complete->setChecked(p.value("augments_complete").toBool());forms->clear();forms->addItem(QStringLiteral("形态未知 / 默认情景"),QString());if(p.value("self").toBool())for(const auto &v:championScenarioVariants(*catalog.combat,game.championId)){const auto form=v.toObject();if(form.value("key")!="base")forms->addItem(form.value("key").toString(),form.value("key"));}forms->setVisible(forms->count()>1);if(p.value("self").toBool())for(int i=1;i<forms->count();++i)if(game.effectConditions.contains(forms->itemData(i).toString()))forms->setCurrentIndex(i);const bool hasGrowth=p.value("self").toBool()&&game.owned.contains(3084);growthLabel->setVisible(hasGrowth);growth->setVisible(hasGrowth);growthNote->setVisible(hasGrowth);growth->clear();};
        connect(who,qOverload<int>(&QComboBox::currentIndexChanged),&dialog,[&]{load();});load();
        auto warning=new QLabel;warning->setWordWrap(true);layout->addWidget(warning);auto buttons=new QDialogButtonBox;buttons->addButton(new Kda::Button(QStringLiteral("保存")),QDialogButtonBox::AcceptRole);buttons->addButton(new Kda::Button(QStringLiteral("取消")),QDialogButtonBox::RejectRole);layout->addWidget(buttons);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
        connect(buttons,&QDialogButtonBox::accepted,&dialog,[&]{
            if(contextId!=game.matchContext.value("context_id")||roster!=rosterKey(game)||state.round!=confirmationRound||!game.fresh()){warning->setText(QStringLiteral("本局信息或选牌轮次已变化，请关闭后重新打开。"));return;}
            const int index=who->currentIndex();const auto player=game.players[index].toObject();QList<int> ids;
            for(const auto &token:entry->text().split(QRegularExpression(QStringLiteral("[,，;；\\s]+")),Qt::SkipEmptyParts)){bool numeric=false;int id=token.toInt(&numeric);if(!numeric)for(auto it=catalog.augments.cbegin();it!=catalog.augments.cend();++it)if(it.value().value("name_zh_cn").toString()==token){id=it.key();break;}if(!catalog.augments.contains(id)||ids.contains(id)){warning->setText(QStringLiteral("请检查未知或重复的强化。"));return;}ids.append(id);}if(ids.size()>8){warning->setText(QStringLiteral("强化数量超出当前输入范围"));return;}
            QJsonArray conditions;if(!forms->currentData().toString().isEmpty())conditions.append(forms->currentData().toString());QJsonObject value{{"ids",jsonIds(ids)},{"complete",complete->isChecked()},{"confirmed_round",state.round},{"conditions",conditions},{"conditions_at_game_time",game.gameTime}};
            if(growth->isVisible()&&!growth->text().trimmed().isEmpty()){bool ok=false;const double hp=growth->text().toDouble(&ok);if(!ok||!std::isfinite(hp)||hp<0||hp>1e9){warning->setText(QStringLiteral("永久生命需要是有效的非负数。"));return;}value["growth_hp"]=hp;value["growth_at_game_time"]=game.gameTime;}
            manual[index]=value;if(player.value("self").toBool())for(const auto &v:championScenarioVariants(*catalog.combat,game.championId))game.effectConditions.remove(v.toObject().value("key").toString());applyManual(game);invalidateAi();scheduleCardModel();scheduleModel(true);renderCards();writeReport();dialog.accept();
        });dialog.resize(380,300);dialog.exec();
    }
    void resumeAutomaticPresentation(){presentation.resumeAutomatic(presentationNow());renderCoach();writeReport();}
    void setDetailsExpanded(bool value){
        detailsExpanded=value;details->setVisible(value);syncOverlayFocus();
        right->body->layout()->activate();renderCoach();writeReport();
    }
    QString presentationPage()const{return presentationState.value("page").toString(QStringLiteral("waiting"));}
    QJsonObject passiveHudReport()const{
        auto strip=rosterSummary->report();const auto r=rosterSummary->geometry();
        strip["geometry"]=QJsonArray{r.x(),r.y(),r.width(),r.height()};strip["visible"]=rosterSummary->isVisibleTo(right);
        const auto action=automaticHint->geometry();
        const bool dense=liveRoster->isVisibleTo(right),compact=rosterSummary->isVisibleTo(right);
        const auto roster=dense?QRect(liveRoster->mapTo(right->body,QPoint()),liveRoster->size()):r;
        return {{"requires_interaction",false},{"tab_count",0},{"action_visible",automaticHint->isVisibleTo(right)},
            {"purchase_geometry",QJsonArray{action.x(),action.y(),action.width(),action.height()}},
            {"purchase_above_roster",(!dense&&!compact)||action.bottom()<roster.top()},
            {"components_visible",componentsLabel->isVisibleTo(right)},{"components",componentsLabel->report()},{"roster_summary_visible",rosterSummary->isVisibleTo(right)},
            {"dense_roster_visible",liveRoster->isVisibleTo(right)},{"roster_summary",strip},
            {"layout",presentationState.value("layout")},{"roster_detail_fits",right->property("passiveRosterDetailFits").toBool()},{"rotation_period_ms",0}};
    }
    bool eventFilter(QObject *object,QEvent *event)override{
        if(object==rosterPage&&(event->type()==QEvent::Resize||event->type()==QEvent::Show))QTimer::singleShot(0,this,[this]{updateRosterPaging();});
        return QObject::eventFilter(object,event);
    }
    void updateRosterPaging(){
        if(!liveRoster||!rosterPage)return;positionRosterContent();
    }
    void positionRosterContent(){
        const int width=rosterPage->width(),top=rosterInsights->isHidden()?0:rosterInsights->height()+2;
        rosterInsights->setGeometry(0,0,width,rosterInsights->height());liveRoster->setGeometry(0,top,width,liveRoster->sizeHint().height());
    }
    QJsonObject rosterBriefReport()const{
        QTextDocument document;document.setDefaultFont(rosterInsights->font());document.setDocumentMargin(0);document.setHtml(rosterInsights->text());
        const auto text=document.toPlainText();const int lines=text.isEmpty()?0:text.count('\n')+1;
        const auto size=document.size();
        return {{"no_pagination",true},{"brief_lines",lines},{"brief_text_fits",size.width()<=rosterInsights->width()+1&&size.height()<=rosterInsights->height()+1},{"text",text}};
    }
    QJsonObject hudPaginationReport(){
        updateRosterPaging();
        bool noScrollbars=true;for(auto area:right->findChildren<QAbstractScrollArea*>())if(area->isVisibleTo(right))noScrollbars&=area->horizontalScrollBar()->maximum()==0&&area->verticalScrollBar()->maximum()==0;
        for(auto bar:right->findChildren<QScrollBar*>())if(bar->isVisibleTo(right))noScrollbars&=bar->maximum()==bar->minimum();
        const auto purchase=advice->report(),insights=rosterBriefReport(),rosterReport=liveRoster->report();bool contained=true;
        if(advice->isVisibleTo(right))contained&=purchase.value("text_fits").toBool()&&purchase.value("navigation_fits").toBool()&&right->body->rect().contains(QRect(advice->mapTo(right->body,QPoint()),advice->size()));
        if(rosterInsights->isVisibleTo(right))contained&=insights.value("brief_text_fits").toBool()&&rosterPage->rect().contains(rosterInsights->geometry());
        QSet<int> visited;for(const auto &v:rosterReport.value("rows").toArray())visited.insert(v.toObject().value("row_index").toInt());
        const bool allFit=rosterReport.value("content_fits").toBool()&&(!rosterPage->isVisibleTo(right)||rosterPage->rect().contains(liveRoster->geometry()));
        if(rosterPage->isVisibleTo(right))contained&=allFit&&(!rosterInsights->isVisibleTo(right)||!rosterInsights->geometry().intersects(liveRoster->geometry()));
        const auto geometry=[](QWidget *widget){const auto r=widget->geometry();return QJsonArray{r.x(),r.y(),r.width(),r.height()};};
        return {{"no_scrollbars",noScrollbars},{"controls_visible",false},{"roster_summary",passiveHudReport().value("roster_summary")},{"purchase",purchase},{"insights",insights},{"roster",rosterReport},{"visible_page_contained",contained},{"all_roster_players_reachable",visited.size()==liveRoster->rosterPlayerCount()},{"all_roster_pages_fit",allFit},{"roster_page_controls_fit",true},{"roster_all_ten_same_screen",rosterReport.value("all_ten_same_screen")},{"roster_no_pagination",liveRoster->rosterPageCount()<=1},{"roster_pager_visible",false},{"insights_geometry",geometry(rosterInsights)},{"roster_geometry",geometry(liveRoster)},{"roster_page_geometry",geometry(rosterPage)}};
    }
    bool livingCompactHud()const{
        if(!game.fresh()||detailsExpanded)return false;int count=0;QJsonValue dead;
        for(const auto &v:game.players)if(v.toObject().value("self").toBool()){++count;dead=v.toObject().value("is_dead");}
        return count==1&&dead.isBool()&&!dead.toBool();
    }
    void syncOverlayFocus(){
        right->setInteractive(false);
        const bool effectiveThrough=through||((game.fresh()||game.matchContext.value("active").toBool())&&!parser.isSet("inspection-window"));
        if(right->windowFlags().testFlag(Qt::WindowTransparentForInput)!=effectiveThrough)right->setThrough(effectiveThrough);
#ifdef Q_OS_WIN
        // Changing focus flags can recreate the native window. Recovery hotkeys
        // must follow the current handle throughout the automatic carousel.
        if(keyWindow&&keyWindow!=reinterpret_cast<HWND>(right->winId()))registerKeys();
#endif
    }
    qint64 presentationNow()const{return presentationCheckTick>=0?presentationCheckTick:autoClock.elapsed();}
    QString presentationContextId()const{const auto id=game.matchContext.value("context_id").toString();return id.isEmpty()&&(demo||replay)?QStringLiteral("synthetic_replay"):id;}
    void showHistoryQuery(){if(!dashboard)createDashboard();showDashboard();}
    bool automaticHistoryAvailable()const{
        const auto id=game.matchContext.value("context_id");if(!id.isString()||id.toString().isEmpty()||anonymousHistory.value("context_id")!=id)return false;
        const auto patch=anonymousHistory.value("patch").toString();if(patch.isEmpty()||patch!=game.observedPatch)return false;
        const auto stage=game.matchContext.value("stage").toString();if(!game.fresh()&&!(game.matchContext.value("active").toBool()&&(stage==QStringLiteral("champ_select")||stage==QStringLiteral("loading"))))return false;
        for(const auto &v:anonymousHistory.value("players").toArray()){const auto p=v.toObject();if(p.value("same_patch_champion").toObject().value("available").toBool()||p.value("same_patch").toObject().value("available").toBool())return true;}return false;
    }
    void applyPresentation(qint64 tick=-1){
        if(runningUiChecks)return;const qint64 now=tick>=0?tick:presentationNow();
        const auto id=presentationContextId();
        if(id!=manualPresentationContext){manualPresentationUntil=0;manualPresentationPinned=manualPresentationPending=false;manualPresentationContext=id;}
        const auto purchase=coachResult.value("economy").toObject().value("next_purchase").toObject();
        QJsonValue dead;int selves=0;for(const auto &v:game.players)if(v.toObject().value("self").toBool()){++selves;dead=v.toObject().value("is_dead");}if(selves!=1)dead=QJsonValue();
        const auto cue=equipmentCueResult.value("latest").toObject();QJsonObject signal;if(!cue.isEmpty())signal={{"kind","enemy_major_item"},{"key",QString::number(cue.value("observed_at_ms").toInteger())+":"+QString::number(cue.value("champion_id").toInt())+":"+QString::number(cue.value("item_id").toInt())}};
        const auto stage=game.matchContext.value("stage").toString(game.fresh()?QStringLiteral("in_game"):QString());
        const bool phaseContext=game.matchContext.value("active").toBool()&&(stage==QStringLiteral("loading")||(stage==QStringLiteral("champ_select")&&championSelectCurrent()));
        const QJsonObject input{{"context_id",id},{"fresh",game.fresh()||phaseContext},{"live_data_fresh",game.fresh()},{"stage",stage},{"self_dead",dead},{"gold_affordable",purchase.value("affordable")},{"next_item_id",purchase.value("item_id")},{"roster_available",!liveSummary.value("players").toArray().isEmpty()},{"history_available",false},{"history_context_id",anonymousHistory.value("context_id")},{"equipment_cue",signal},{"manual_action",manualPresentationPending},{"manual_pinned",manualPresentationPinned},{"manual_hold_until_ms",manualPresentationUntil},{"manual_page",manualPresentationPage},{"manual_expanded",manualPresentationExpanded},{"overlays_allowed",preferences.value("coach/overlayEnabled",true).toBool()&&!hiddenByUser}};
        presentationState=presentation.update(input,now);
        manualPresentationPending=false;
        applyingPresentation=true;
        detailsExpanded=presentationState.value("expanded").toBool();details->setVisible(detailsExpanded);syncOverlayFocus();applyingPresentation=false;
    }
    bool championSelectStage()const{return game.matchContext.value("active").toBool()&&game.matchContext.value("stage")==QStringLiteral("champ_select");}
    bool championSelectCurrent()const{return championSelectStage()&&(demo||replay||runningUiChecks||game.championSelectFresh());}
    QJsonObject champSelectInput()const{
        QJsonArray bench;for(int id:game.bench)bench.append(id);
        return {{"active",championSelectCurrent()},{"stage",game.matchContext.value("stage")},
            {"self_champion_id",game.championId},{"roster",game.matchContext.value("roster")},
            {"bench_champion_ids",bench},{"bench_known",game.benchKnown},
            {"fresh",championSelectCurrent()}};
    }
    void warmSelectionArtwork(){
        if(!championSelectCurrent())return;
        QList<int> ids{game.championId};for(int id:game.bench)if(ids.size()<13&&!ids.contains(id))ids.append(id);
        std::sort(ids.begin(),ids.end());QStringList key;key.append(game.matchContext.value("context_id").toString());key.append(catalog.patch);
        for(int id:ids)key.append(QString::number(id));const auto basis=key.join(QChar(u'|'));
        if(basis==warmedChampionsKey)return;warmedChampionsKey=basis;
        for(int id:ids)if(catalog.champions.contains(id))assets->champion(id);
    }
    bool clearShopPage(){
        const auto removed=shopStore->cleanup();
        if(!removed.ok){shopCleanupPending=true;shopStatus["state"]=removed.code;return false;}
        shopCleanupPending=false;shopProjectionKey.clear();shopStore->setActiveContext({});return true;
    }
    void refreshShopRecommendations(){
        if(!shopStore||demo||replay||uiBrowser||runningUiChecks||parser.isSet("fixture-feed"))return;
        const bool enabled=preferences.value("coach/shopRecommendationsEnabled",true).toBool();
        shopStatus["enabled"]=enabled;shopStatus["hot_reload_verified"]=false;shopStatus["automatic_purchase"]=false;
        // Reconnect in the lobby (even with the option off) must be able to
        // clean a registered page left by a prior run. Never scan for a root.
        const auto directory=live.localGameDirectory();
        if(!directory.isEmpty()&&shopStore->gameDirectory()!=directory){
            if(!shopStore->gameDirectory().isEmpty()&&!clearShopPage())return;
            const auto configured=shopStore->configureGameDirectory(directory);if(!configured.ok){shopStatus["state"]=configured.code;return;}
        }
        if(shopStore->gameDirectory().isEmpty()){shopStatus["state"]="waiting_client_directory";return;}
        const bool active=game.matchContext.value("active").toBool()&&game.gameMode==QStringLiteral("KIWI");
        const auto stage=game.matchContext.value("stage").toString();
        const bool selecting=championSelectCurrent(),liveNow=stage==QStringLiteral("in_game")&&game.fresh();
        // The loading transition can temporarily omit the self champion. Keep
        // the same session's page, without promoting a hypothetical precompute.
        if(enabled&&active&&stage==QStringLiteral("loading")&&shopStore->activeContext().sessionId==game.matchContext.value("context_id").toString())return;
        if(!enabled||!active||(!selecting&&!liveNow)){
            if(shopCleanupPending||!shopStore->activeContext().sessionId.isEmpty()){
                if(!clearShopPage())return;
            }
            shopStatus["state"]="inactive";
            return;
        }
        const auto champion=catalog.champion(game.championId);
        QString key=champion.value("alias").toString(champion.value("key").toString());
        const ShopRecommendations::Context identity{game.matchContext.value("context_id").toString(),game.championId,key,catalog.patch};
        const auto selectionSelf=selecting?buildChampSelectModel(catalog,champSelectInput()).value("self").toObject():QJsonObject{};
        if(identity.sessionId.isEmpty()||identity.championId<=0||key.isEmpty()||(selecting&&!selectionSelf.value("known").toBool())){
            if((shopCleanupPending||!shopStore->activeContext().sessionId.isEmpty())&&!clearShopPage())return;
            shopStatus["state"]="waiting_champion";return;
        }
        if(shopStore->activeContext()!=identity){if(!clearShopPage())return;shopStore->setActiveContext(identity);}
        if(shopVerifiedItems.isEmpty())for(auto it=catalog.items.cbegin();it!=catalog.items.cend();++it)shopVerifiedItems.insert(it.key());
        QJsonObject purchase;
        const auto quality=coachResult.value("recommendation_quality").toObject();
        if(liveNow&&coachResultContext==identity.sessionId&&quality.value("primary_purchase_visible").toBool()
            &&(!quality.contains("ranking_available")||quality.value("ranking_available").toBool()))purchase=coachResult.value("economy").toObject().value("next_purchase").toObject();
        const auto reference=externalEquipmentReference(catalog,game);
        const auto request=ShopRecommendations::projectRecommendation(identity,shopVerifiedItems,reference,purchase);
        if(!request.confirmed){
            if((shopCleanupPending||!shopProjectionKey.isEmpty())&&!clearShopPage())return;
            shopStatus["state"]="waiting_verified_recommendation";return;
        }
        const auto basis=request.evidenceId+identity.sessionId+QString::number(identity.championId);
        if(basis==shopProjectionKey)return;
        const auto result=shopStore->publish(request,[this,identity]{return preferences.value("coach/shopRecommendationsEnabled",true).toBool()
            &&game.matchContext.value("context_id").toString()==identity.sessionId&&game.championId==identity.championId
            &&(championSelectCurrent()||game.fresh());});
        shopStatus["state"]=result.ok?QStringLiteral("ready"):result.code;
        shopStatus["group_count"]=request.groups.size();shopStatus["source_version"]=identity.sourceVersion;
        if(result.ok){shopProjectionKey=basis;shopCleanupPending=true;}
    }
    bool gameHasForeground(){
        if(demo||replay)return false;
        if(championSelectStage()){
            clientWindowTracker.setExpectedProcess(live.localClientProcessId());
            return championSelectCurrent()&&game.gameMode==QStringLiteral("KIWI")&&clientWindowTracker.refresh().overlayAllowed();
        }
        return gameWindowTracker.refresh().overlayAllowed();
    }
    QRect actualViewport(bool *found){
        if(championSelectStage()&&!demo&&!replay){
            clientWindowTracker.setExpectedProcess(live.localClientProcessId());const auto &surface=clientWindowTracker.refresh();
            *found=championSelectCurrent()&&surface.found&&surface.visible&&!surface.minimized&&!surface.client.isEmpty();
            if(*found){const auto screen=QGuiApplication::screenAt(surface.client.center());return screen?screen->availableGeometry():surface.client;}
            return viewport;
        }
        const auto &surface=gameWindowTracker.refresh();
        *found=surface.found&&surface.visible&&!surface.minimized&&!surface.client.isEmpty();
        if(*found)return surface.client;
        // Never jump a hidden/minimized live HUD onto the remote primary screen.
        if(viewport.isValid())return viewport;
        const auto screen=QGuiApplication::primaryScreen();return screen?screen->availableGeometry():QRect();
    }
    void resetHudLayout(){
        if(hudEditorOpen)return;
        HudLayout defaults;if(!defaults.save(preferences)){Kda::postNotice({QStringLiteral("hud-layout"),QStringLiteral("布局保存失败，请重试"),{},Kda::NoticeKind::Warning,3500});return;}
        hudLayout=defaults;dock(true);renderCoach();writeReport();
        Kda::postNotice({QStringLiteral("hud-position"),QStringLiteral("HUD 已恢复默认布局"),{},Kda::NoticeKind::Success,2800});
    }
    void openHudEditor(){
        if(hudEditorOpen||finishing||viewport.isEmpty())return;
        const QRect editViewport=viewport;
        const auto safe=safeOverlayPlacement(editViewport);
        QMap<QString,QRect> base;
        auto coach=safe.right;
        if(coach.isEmpty())coach=QRect(editViewport.topLeft()+QPoint(20,100),QSize(232,220));
        coach.setWidth(std::min(232,coach.width()));coach.setHeight(std::min(std::max(287,right->height()),coach.height()));coach.moveBottom(safe.right.isEmpty()?editViewport.bottom()-20:safe.right.bottom());
        base["coach"]=coach;
        auto combat=combatCornerPlacement(safe,coach,CornerCombatHud::expandedHeight);
        if(combat.isEmpty())combat=QRect(editViewport.right()-std::min(296,editViewport.width())-19,editViewport.top()+80,std::min(296,editViewport.width()),std::min(CornerCombatHud::expandedHeight,editViewport.height()));
        base["combat"]=combat;
        const auto cards=selectionCardLayout(editViewport);
        for(int i=0;i<3;++i){const auto key=QStringLiteral("card%1").arg(i+1);base[key]=cards.hints.value(i,QRect(editViewport.x()+editViewport.width()*(i+1)/4-100,editViewport.y()+editViewport.height()/2,200,64));}
        // Capture assistant-owned widgets only, before hiding the live surfaces.
        QMap<QString,QImage> previews;
        if(right->isVisible())previews["coach"]=right->grab().toImage();
        if(combatCorner->isVisible())previews["combat"]=combatCorner->grab().toImage();
        for(int i=0;i<cardHints.size();++i)if(cardHints[i]->isVisible())previews[QStringLiteral("card%1").arg(i+1)]=cardHints[i]->grab().toImage();
        hudEditorOpen=true;renderCardHints();writeReport();
        const bool dashboardWasVisible=dashboard&&dashboard->isVisible()&&!dashboard->isMinimized();
        if(dashboardWasVisible)dashboard->hide();
        // Keep the transparent editor independent of the hidden native owner;
        // Windows hides owned windows together with their owner.
        HudLayoutEditor editor(editViewport,hudLayout,base,previews);
        // Use the exact game client rectangle. A fullscreen screen rectangle
        // would move/scale HUD anchors in windowed or mixed-DPI configurations.
        editor.setGeometry(editViewport);editor.show();
        while(editor.exec()==QDialog::Accepted){
            if(editor.editedLayout().save(preferences)){hudLayout=editor.editedLayout();break;}
            QMessageBox::warning(&editor,QStringLiteral("HUD 管理"),QStringLiteral("布局未能保存，请重试。"));
        }
        hudEditorOpen=false;
        if(!finishing){dock(true);renderCoach();renderCardHints();writeReport();if(dashboardWasVisible&&dashboard){dashboard->show();dashboard->raise();dashboard->activateWindow();}}
    }
    void dock(bool reset){
        bool found=false;auto current=parser.isSet("ui-browser")?QGuiApplication::primaryScreen()->availableGeometry():actualViewport(&found);gameWindowFound=found;if(parser.isSet("viewport")){const auto sizes=parser.value("viewport").split('x');if(sizes.size()==2)current=QRect(current.topLeft(),QSize(sizes[0].toInt(),sizes[1].toInt()));}
        const auto surface=cardSurfaceBasis();
        if(lastCardSurface!=surface){lastCardSurface=surface;++cardSurfaceRevision;if(cardWork)cardWork->cancel();cardDecision={};computedCardBasis.clear();cardResultContext.clear();if(!game.offers.isEmpty())scheduleCardModel();}
        if(uiBrowser&&browserViewport.isValid())current.setSize(browserViewport);
        if(uiBrowser&&QGuiApplication::platformName()!=QStringLiteral("offscreen"))current=current.intersected(QGuiApplication::primaryScreen()->availableGeometry());
        const bool clientPhase=championSelectStage()&&!demo&&!replay&&!uiBrowser;
        const auto clientArea=clientPhase?clientWindowTracker.state().client:QRect();
        if(current==viewport&&!reset&&clientPhase==usingClientViewport&&clientArea==dockedClient)return;
        usingClientViewport=clientPhase;dockedClient=clientArea;
        viewport=current;placement=clientPhase?championSelectPlacement(clientArea,viewport):safeOverlayPlacement(viewport);
        if(hudLayout.contains("coach")&&!clientPhase){placement.right=hudLayout.resolve("coach",viewport,placement.right);placement.collapsed=false;}
        if(placement.right.isEmpty()){right->setPresentedVisible(false,false);combatCorner->hide();}
        else{
            if(!clientPhase)preferences.setValue("right/anchor",hudLayout.contains("coach")?QStringLiteral("custom"):QStringLiteral("bottom-left"));
            if(hudLayout.contains("coach")&&!clientPhase)right->setCustomLayout(placement.right);else right->setSafeLane(placement.right,true,true);
            if(placement.collapsed&&!compactAutoFold){rightUserFolded=right->collapsed();compactAutoFold=true;right->setCollapsed(true);}
            else if(!placement.collapsed&&compactAutoFold){right->setCollapsed(rightUserFolded);compactAutoFold=false;}
        }
        renderCardHints();
    }
    void unregisterKeys(){
#ifdef Q_OS_WIN
        if(keyWindow)for(int id:{0x4c56,0x4c57,0x4c58,0x4c59,0x4c5a})UnregisterHotKey(keyWindow,id);keyWindow=nullptr;
#endif
        hotkey=false;
    }
    void registerKeys(){
        if(parser.isSet("ui-browser")){hotkey=false;right->setHotkeyAvailable(false);return;}
#ifdef Q_OS_WIN
        unregisterKeys();keyWindow=reinterpret_cast<HWND>(right->winId());hotkey=RegisterHotKey(keyWindow,0x4c59,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,VK_F9);RegisterHotKey(keyWindow,0x4c58,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,VK_F8);RegisterHotKey(keyWindow,0x4c57,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,VK_F7);RegisterHotKey(keyWindow,0x4c5a,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,VK_F10);if(dashboard)RegisterHotKey(keyWindow,0x4c56,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,VK_F6);
#endif
        right->setHotkeyAvailable(hotkey);
    }
    void setThrough(bool value){if(value&&!hotkey)return;const bool previous=through;unregisterKeys();through=value;syncOverlayFocus();registerKeys();if(value&&!hotkey){through=false;syncOverlayFocus();registerKeys();}if(previous!=through&&!runningUiChecks)Kda::postNotice({QStringLiteral("hud-input"),through?QStringLiteral("HUD 鼠标穿透已开启"):QStringLiteral("HUD 可以操作了"),QStringLiteral("Ctrl+Alt+F9 随时切换"),Kda::NoticeKind::Info,3000});renderCards();writeReport();}
    void nextReplay(){if(samples.isEmpty())return;sample=(sample+1)%samples.size();state=PanelState(true);if(state.apply(samples[sample]))++accepted;rememberOffers();renderCards();}
    bool needsFastPoll()const{
        return replay||uiBrowser||runningUiChecks||overlayPreview||gameWindowFound||game.fresh()
            ||game.matchContext.value("active").toBool()||endHudUntil>autoClock.elapsed();
    }
    void syncPollCadence(){
        const bool fast=needsFastPoll();
        const int interval=fast?50:250;
        if(pollTimer.interval()==interval)return;
        pollTimer.setTimerType(fast?Qt::PreciseTimer:Qt::CoarseTimer);
        pollTimer.setInterval(interval);
    }
    void poll(){
        if(replay||input.isEmpty())return;
        // In a match, read the current size from the open handle. Windows file
        // metadata can lag while the external collector still has a writer open.
        if(!needsFastPoll()){
            const QFileInfo source(input);
            if(!source.isFile()||source.size()==offset)return;
        }
        QFile file(input);++eventFileOpenAttempts;
        if(!file.open(QIODevice::ReadOnly))return;
        const auto size=file.size();
        if(size==offset)return;
        if(size<offset){offset=0;pending.clear();state.clearLiveOffers();invalidateOfferResults();scheduleModel(true);renderCards();}
        if(!file.seek(offset))return;
        const auto bytes=file.read(256*1024);if(bytes.isEmpty())return;
        offset+=bytes.size();pending+=bytes;bool changed=false;
        for(;;){const int end=pending.indexOf('\n');if(end<0)break;const auto line=pending.left(end);pending.remove(0,end+1);if(line.size()>1024*1024)continue;const auto event=QJsonDocument::fromJson(line).object();
            const auto admission=liveOfferAdmission(event,game,state.liveContextId,state.liveMatchRef,catalog.patch);
            if(!admission.value("accepted").toBool()){lastOfferBindingRejection=admission.value("reason").toString();offerBindingRejections[lastOfferBindingRejection]=offerBindingRejections.value(lastOfferBindingRejection).toInt()+1;
                if(unboundOfferRevokesCurrent(event,game,state.observed)){state.clearLiveOffers();invalidateOfferResults();changed=true;}
                continue;}
            const bool offer=event.value("kind")==QStringLiteral("offer");
            const auto previousRevision=state.offerRevision;
            if(!state.apply(event))continue;
            ++accepted;changed=true;
            if(previousRevision!=state.offerRevision){
                // Closing a round revokes its result immediately, even when no
                // replacement offer has arrived. A-B-A offers have distinct revisions.
                if(cardWork)cardWork->cancel();
                ++offerBindingRevision;cardDecision={};computedCardBasis.clear();cardResultContext.clear();
                offerIsReroll=event.value("is_reroll")==QJsonValue(true);
            }
            if(!offer)continue;
            offerReceivedAt=QDateTime::currentDateTimeUtc();
            sourceCollectedAt=QDateTime::fromString(event.value("collected_at").toString(),Qt::ISODateWithMs);
        }
        if(pending.size()>1024*1024)pending.clear();
        if(changed){const auto prior=strategyKey(game);rememberOffers();game.offers=offersBoundTo(game)?offerIds():QList<int>{};game.headlinerContext=offersBoundTo(game)?state.headlinerContext:QJsonObject{};
            const bool modelChanged=prior!=strategyKey(game);if(modelChanged){if(requestBasis!=adviceBasis())invalidateAi();coachResult={};context={};}
            scheduleCardModel();scheduleModel(modelChanged);updateAutomaticAdvice();renderCards();writeReport();}
    }
    void scheduleModel(bool force){
        scheduleLoadingPreparation();
        if(finishing)return;
        if(!game.fresh()||!catalog.combat||!modelRulesUsable(catalog,game)||game.championId<=0||game.gameMode!=QStringLiteral("KIWI")){if(modelWork)modelWork->cancel();++modelTicket;coachResult={};context={};computedStrategy.clear();modelState="waiting";renderCoach();return;}
        const auto key=modelBasis(),soft=coachSoftModelBasis(game);const auto now=autoClock.elapsed();
        refreshPublishedCoach(key);
        if(modelCompletion.pending()){if(force||workerStrategy!=key||workerSoftBasis!=soft)dirty=true;if(workerStrategy!=key){if(modelWork)modelWork->cancel();++modelTicket;}return;}
        if(key!=failedModelBasis&&!failedModelBasis.isEmpty()){modelRetryAt=0;consecutiveModelTimeouts=0;failedModelBasis.clear();}if(now<modelRetryAt)return;
        const bool hardCurrent=!coachResult.isEmpty()&&computedStrategy==key;
        if(uiBrowser&&!force&&hardCurrent)return;
        if(!force&&hardCurrent&&!CoachRefreshPolicy::refreshDue(computedSoftBasis!=soft,publishedSnapshotMs,workerStartedMs,now))return;
        ++modelTicket;workerTicket=modelTicket;workerStrategy=key;modelWorkerContext=game.matchContext.value("context_id").toString();workerSoftBasis=soft;workerStartedMs=now;workerSnapshotGameTime=game.gameTime;workerSnapshotAt=QDateTime::currentDateTimeUtc();workerSourceReceivedAt=game.received;workerInputAgeMs=game.source==GameState::Source::Live?std::max(qint64(0),game.received.msecsTo(workerSnapshotAt)):0;
        modelAssignedBudgetMs=std::min(modelBudgetMs,std::max(qint64(0),qint64(3900)-workerInputAgeMs));
        if(modelAssignedBudgetMs<=0){modelWork.reset();coachResult={};context={};computedStrategy.clear();computedSoftBasis.clear();publishedSnapshotMs=-1;allowedItems.clear();allowedAugments.clear();invalidateAi();modelState=QStringLiteral("waiting");lastModelStopReason=QStringLiteral("insufficient_freshness_budget");modelRetryAt=now+1000;dirty=false;renderCoach();writeReport();return;}
        if(!modelCompletion.tryStart())return;
        const auto snapshot=game;const auto preference=style->currentData().toString();auto settingsOptions=options;settingsOptions.currentPurchaseOnly=true;settingsOptions.deferExecutionReference=!demo&&!replay&&!runningUiChecks&&!uiBrowser;modelWork=std::make_shared<RecommendationWork>(modelAssignedBudgetMs);settingsOptions.work=modelWork;const auto seen=seenOffers;const bool synthetic=demo,diagnostic=diagnosticDsEnabled();modelState=hardCurrent?"refreshing":"calculating";const auto copy=catalog;
        worker.setFuture(QtConcurrent::run(&modelPool,[copy,snapshot,preference,settingsOptions,synthetic,seen,diagnostic]{
            auto coach=buildCoachSnapshot(copy,snapshot,preference,settingsOptions,seen);
            coach["requested_combat_style"]=preference;
            annotateCoachRecommendationQuality(copy,snapshot,coach);
            const bool complete=coach.value("computation_complete").toBool();
            if(complete&&!settingsOptions.work->stopped())
                coach["local_choice_evidence"]=buildLocalChoiceEvidence(copy,snapshot,coach,false,settingsOptions.work);
            const QJsonObject localResult{{"coach",coach},{"context",QJsonObject{}},{"items",QJsonArray{}},{"augments",QJsonArray{}}};
            if(!diagnostic||!complete||coach.value("status")==QStringLiteral("input_required"))return localResult;
            const auto plan=coach.value("plan").toObject();
            auto request=makeAiCandidateContext(copy,snapshot,snapshot.championId,preference,&plan);
            if(settingsOptions.work->stopped())return localResult;
            if(synthetic)request.context["source"]="synthetic_demo_no_real_player_data";
            request.context["observed_game_time"]=snapshot.gameTime;
            request.context["coach_economy"]=coach.value("economy");
            request.context["teamfight_scenario"]=coach.value("simulation");
            request.context["augment_decision"]=coach.value("augment_decision");
            request.context["team_needs"]=coach.value("team_needs");
            request.context["augments_completeness"]=QStringLiteral("Unknown lists must not be interpreted as no augments.");
            return QJsonObject{{"coach",coach},{"context",request.context},{"items",jsonIds(request.items)},{"augments",jsonIds(request.augments)}};
        }));
    }
    void refreshPublishedCoach(const QString &knownBasis={}){
        if(uiBrowser||runningUiChecks||coachResult.isEmpty())return;
        const auto plan=coachResult.value("plan").toObject();const auto now=autoClock.elapsed();
        if(plan.value("mechanic_snapshot_time").toDouble(publishedSnapshotGameTime)>game.gameTime||!CoachRefreshPolicy::retain(publishedSnapshotMs,now+publishedInputAgeMs,computedStrategy==(knownBasis.isEmpty()?modelBasis():knownBasis),game.fresh(),modelRulesUsable(catalog,game),publishedSnapshotGameTime,game.gameTime,plan.value("mechanic_valid_until").toDouble(-1))){
            coachResult={};context={};computedStrategy.clear();computedSoftBasis.clear();publishedSnapshotMs=-1;modelState=worker.isRunning()?"calculating":"waiting";invalidateAi();return;
        }
        auto status=coachResult.value("model_refresh").toObject();status["snapshot_age_ms"]=now-publishedSnapshotMs+publishedInputAgeMs;status["soft_inputs_changed"]=computedSoftBasis!=coachSoftModelBasis(game);status["refresh_pending"]=worker.isRunning()||status.value("soft_inputs_changed").toBool();status["conditional"]=true;coachResult["model_refresh"]=status;
    }
    void updateAutomaticAdvice(){
        if(finishing)return;refreshModelConfiguration();
        if(!diagnosticDsEnabled()){aiStatus->setText(QStringLiteral("本地建议自动更新"));return;}
        const auto now=autoClock.elapsed();
        if(!game.fresh()||!modelRulesUsable(catalog,game)||game.gameMode!=QStringLiteral("KIWI")||game.championId<=0){if(aiPending||!aiPlan.isEmpty()||!autoAdvice.key().isEmpty())invalidateAi();autoBasis.clear();return;}
        if(context.isEmpty()||modelState!="ready")return;
        const auto basis=adviceBasis();if(!autoBasis.isEmpty()&&basis!=autoBasis)invalidateAi();autoBasis=basis;
        if(!automaticSettings.isConfigured()){aiStatus->setText(QStringLiteral("本地建议自动更新 · 对局模型尚未配置"));return;}
        if(aiConfigurationBlocked||context.isEmpty()||modelState!="ready")return;
        const auto key=basis;
        if(key!=autoAdvice.key()){invalidateAi();autoAdvice.observe(key,now);renderCoach();}
        if(autoAdvice.begin(now))requestAi();
    }
    void requestAi(){
        if(!diagnosticDsEnabled()||context.isEmpty())return;aiPending=true;aiSnapshotTime=game.gameTime;requestBasis=adviceBasis();
        if(parser.isSet("test-ds"))dsTestStarted=true;
        auto payload=context;payload["advice_trigger"]="automatic_valid_kiwi_state";
        ai.request(payload,automaticSettings.apiKey,automaticSettings.model,allowedItems,allowedAugments,aiRevision);writeReport();
    }
    QJsonObject purchaseMatchup(int itemId,const QString &displaySource) const {
        const auto match=game.matchContext.value("context_id").toString();
        if(itemId<=0||!game.fresh()||game.source!=GameState::Source::Live||game.gameMode!=QStringLiteral("KIWI")
            ||!modelRulesUsable(catalog,game)||!game.matchContext.value("active").toBool()
            ||game.matchContext.value("stage")!=QStringLiteral("in_game")||match.isEmpty()
            ||match!=combatCornerContext||game.received!=combatCornerReceived)return {};
        const auto corner=combatCorner->report();
        const auto enemies=corner.value("enemies").toArray();
        const int lead=corner.value("self_lead").toInt(-1);
        if(corner.value("available")!=QJsonValue(true)||corner.value("synthetic")!=QJsonValue(false)
            ||enemies.size()!=5||corner.value("known_rows").toInt()!=5||lead<0||lead>=enemies.size())return {};
        const auto threat=enemies.at(lead).toObject();
        const auto champion=threat.value("champion");
        const int kills=threat.value("self_kills").toInt();
        if(champion.toInt()<=0||kills<=0||std::count_if(enemies.cbegin(),enemies.cend(),[&](const auto &row){
            return row.toObject().value("champion")==champion;
        })!=1)return {};
        QJsonObject result{{"status","awaiting_comparison"},{"item_id",itemId},{"champion_id",champion},
            {"champion_name",threat.value("name")},{"kills_against_self",kills},
            {"selection_basis","unique_observed_kills_against_self"},{"display_source",displaySource}};
        const auto refresh=coachResult.value("model_refresh").toObject();
        const double age=refresh.value("snapshot_age_ms").toDouble(-1);
        if(displaySource!=QStringLiteral("computed")||coachResultContext!=match
            ||refresh.value("soft_inputs_changed")!=QJsonValue(false)||age<0||age>CoachRefreshPolicy::maximumPublishAgeMs
            ||!coachResult.value("recommendation_quality").toObject().value("primary_purchase_visible").toBool())return result;
        const auto plan=coachResult.value("plan").toObject();
        const auto sandbox=plan.value("purchase_sandbox").toObject();
        if(sandbox.value("selected_item_id").toInt()!=itemId||!sandbox.value("ranking_available").toBool())return result;
        for(const auto &value:sandbox.value("selected_sandbox").toObject().value("windows").toArray()){
            const auto window=value.toObject();
            if(window.value("seconds").toInt()!=8)continue;
            QJsonObject target;
            for(const auto &entry:window.value("targets").toArray()){
                const auto row=entry.toObject();
                if(row.value("champion_id")!=champion)continue;
                if(!target.isEmpty())return result;
                target=row;
            }
            if(!target.value("available").toBool()||!target.value("ranking_eligible").toBool())return result;
            const auto output=target.value("output_gain");
            const auto defense=target.value("defense").toObject();
            const auto durability=defense.value("relative_gain");
            if(output.isDouble()&&std::isfinite(output.toDouble()))result["output_gain"]=output;
            if(defense.value("available").toBool()&&durability.isDouble()&&std::isfinite(durability.toDouble()))
                result["effective_health_gain"]=durability;
            if(result.contains("output_gain")||result.contains("effective_health_gain")){
                result["status"]="conditional_comparison";
                result["window_seconds"]=8;
            }
            return result;
        }
        return result;
    }
    void renderAutomaticAdvice(bool refreshModel=true){
        if(refreshModel)refreshPublishedCoach();
        CoachAdaptivePurchase::View adaptive;
        QString caption=QStringLiteral("对局"),title=QStringLiteral("等待对局"),detail=QStringLiteral("自动连接"),components=QStringLiteral("组件待更新"),route=growthPlanSummary(coachResult.value("plan").toObject()),displaySource=QStringLiteral("computed"),growthCandidateTooltip;bool buyable=false;int actionItem=0;QJsonArray componentItems;
        const auto stage=game.matchContext.value("stage").toString();
        const bool pregame=game.matchContext.value("active").toBool()&&!game.fresh()&&(stage==QStringLiteral("champ_select")||stage==QStringLiteral("loading"));
        if(!game.fresh()&&game.matchContext.value("active").toBool()&&stage==QStringLiteral("in_game")){caption=QStringLiteral("局内");title=QStringLiteral("等待实时数据");detail=QStringLiteral("自动重连");}
        sourceStatus->hide();engineDetails->hide();liveCounters->hide();summary->hide();style->hide();
        automaticHint->setVisible(!pregame);componentsLabel->setVisible(game.fresh()&&!pregame);
        rosterInsights->setVisible(pregame);routeLabel->parentWidget()->setVisible(!pregame);
        rosterSummary->setRoster(liveSummary.value("players").toArray());
        rosterSummary->setVisible(game.fresh()&&!detailsExpanded);
        if(pregame){
            const auto match=game.matchContext,composition=match.value("composition").toObject();
            caption=match.value("stage")==QStringLiteral("champ_select")?QStringLiteral("阵容 / 选人"):QStringLiteral("阵容 / 载入");title=composition.value("summary").toString(QStringLiteral("阵容待确认"));
            const auto team=composition.value("teams").toObject().value("ALLY").toObject();QStringList needs;for(const auto &v:team.value("needs").toArray())needs.append(v.toString());
            detail=rosterAiPlan.value("summary").toString();if(detail.isEmpty())detail=needs.isEmpty()?team.value("summary").toString():needs.join(QStringLiteral(" · "));
            if(stage==QStringLiteral("loading")){
                const auto basis=loadingPreparationBasis(catalog,game,style->currentData().toString());
                if(!basis.isEmpty()&&basis==loadingComputedBasis&&loadingPreparation.value("available").toBool())detail=QStringLiteral("预演成长候选 %1 项 · 进场校准").arg(loadingPreparation.value("candidate_directions").toArray().size());
                else if(!basis.isEmpty())detail=QStringLiteral("加载预演中 · 进场校准");
            }
        }
        const bool liveInput=game.source==GameState::Source::Live;
        const bool purchaseContext=game.fresh()&&game.gameMode==QStringLiteral("KIWI")
            &&(!liveInput||((game.phase=="InProgress"||game.phase=="Reconnect")
                &&game.matchContext.value("active").toBool()&&!game.matchContext.value("context_id").toString().isEmpty()));
        const bool currentPurchase=purchaseContext&&modelRulesUsable(catalog,game);
        if(currentPurchase){
            const auto economy=coachResult.value("economy").toObject(),purchase=economy.value("next_purchase").toObject(),plan=coachResult.value("plan").toObject();
            actionItem=purchase.value("item_id").toInt();componentItems=purchase.value("buyable_components").toArray();
            caption=QStringLiteral("下一件");title=purchase.value("item_id").toInt()>0?catalog.itemName(purchase.value("item_id").toInt()):coachResult.value("plan").toObject().value("items").toArray().isEmpty()?QStringLiteral("计算中"):QStringLiteral("配装已齐");
            detail=automaticPurchaseStatus(purchase);
            if(detail.isEmpty())detail=purchase.value("item_id").toInt()>0?QStringLiteral("金币待同步"):QStringLiteral("自动更新");
            const auto quality=coachResult.value("recommendation_quality").toObject();
            adaptive=CoachAdaptivePurchase::fromPlan(plan,game.gameTime,[this](int id){return catalog.itemName(id);},quality);
            if(quality.value("candidate_only").toBool()){caption=QStringLiteral("条件候选");detail=quality.value("summary").toString(QStringLiteral("机制信息待齐"));}else if(quality.value("confidence")==QStringLiteral("unavailable")){caption=QStringLiteral("待确认");title=QStringLiteral("信息待齐");detail=quality.value("summary").toString(QStringLiteral("等待当前装备与强化来源"));}
            buyable=purchase.value("affordable").toBool();
            QStringList available;
            for(const auto &v:purchase.value("buyable_components").toArray()){
                const auto component=v.toObject();const auto cost=component.value("remaining_cost");const int id=component.value("item_id").toInt();
                if(id<=0||!cost.isDouble()||!std::isfinite(cost.toDouble())||cost.toDouble()<0)continue;
                const auto name=component.value("name").toString(catalog.itemName(id));available.append(QStringLiteral("%1 %2G").arg(name,num(cost.toDouble())));
            }
            components=QStringLiteral("配方预算  ")+(available.isEmpty()?(economy.value("self_current_gold").isDouble()?QStringLiteral("暂无组件"):QStringLiteral("金币未知")):available.join(QStringLiteral(" / ")));
            route=growthPlanSummary(plan);
            if(quality.value("primary_purchase_visible")!=QJsonValue(true)){
                actionItem=0;buyable=false;componentItems={};components=quality.value("summary").toString();
                caption=QStringLiteral("出装");title=QStringLiteral("出装待定");detail.clear();
                growthCandidateTooltip=quality.value("summary").toString();
            }
            if(actionItem>0&&!quality.value("comparison_label").toString().isEmpty())caption=quality.value("comparison_label").toString();
            const auto role=coachResult.value("role_aware_recommendation").toObject();
            if(!growthCandidatePlan(plan)&&(role.value("prefer_source_route_for_display").toBool()||role.value("prefer_mechanism_reference_for_display").toBool())){
                const auto reference=role.value("next_purchase_reference").toObject();actionItem=reference.value("item_id").toInt();buyable=reference.value("affordable").toBool();componentItems={};displaySource=QStringLiteral("role_guidance");caption=QStringLiteral("辅助参考");
                title=actionItem>0?catalog.itemName(actionItem):QStringLiteral("治疗与保护");detail=QStringLiteral("机制与数据参考");components=QStringLiteral("核心组合 · 顺序按局势调整");
                const auto routes=role.value("routes").toArray();for(const auto &candidateRoute:routes){const auto supportedRoute=candidateRoute.toObject();if(!supportedRoute.value("has_ally_support_reference_semantics").toBool())continue;for(const auto &v:supportedRoute.value("data").toObject().value("item_ids").toArray())if(componentItems.size()<4)componentItems.append(QJsonObject{{"item_id",v},{"name",catalog.itemName(v.toInt())}});break;}
                if(componentItems.isEmpty()){
                    for(const auto &value:coachResult.value("plan").toObject().value("conditional_equipment_options").toArray()){const auto item=value.toObject();if(componentItems.size()>=4)break;componentItems.append(QJsonObject{{"item_id",item.value("item_id")},{"name",item.value("name")}});}
                }
                const auto reasoning=coachResult.value("plan").toObject().value("build_reasoning").toObject();
                for(const auto &value:reasoning.value("items").toArray()){
                    const auto item=value.toObject();const auto relation=item.value("hero_relationship").toObject();const auto why=relation.value("reason").toArray();
                    if(why.isEmpty()||(actionItem>0&&item.value("item_id").toInt()!=actionItem))continue;
                    detail=item.value("name").toString()+QStringLiteral("：")+why.first().toString();
                    const auto conditions=relation.value("conditions").toArray();if(!conditions.isEmpty())components=conditions.first().toString();break;
                }
                growthCandidateTooltip=detail;
            }
            if(growthCandidatePlan(plan)){
                const auto notes=growthCandidateNotes(plan);const int count=plan.value("candidate_loadouts").toArray().size();
                actionItem=0;buyable=false;componentItems={};displaySource=QStringLiteral("growth_conditioned_candidates");caption=QStringLiteral("条件探索");title=QStringLiteral("成长联动候选");
                detail=count>0?QStringLiteral("%1组条件搭配 · 未排序").arg(count):QStringLiteral("待补成长输入 · 未排序");components=notes.first();
                growthCandidateTooltip=notes.join(QStringLiteral("；"))+QStringLiteral("。组合用于比较指标取舍，购买条件与顺序尚需核实。");
            }
            if(adaptive.active){
                actionItem=adaptive.itemId;buyable=adaptive.selected;componentItems={};
                displaySource=QStringLiteral("explicit_chain_condition_comparison");caption=QStringLiteral("条件比较");
                title=adaptive.title;detail=adaptive.status;route=QStringLiteral("按给定动作估算 · 条件参考");
                QJsonValue cost;for(const auto &value:plan.value("purchase_sandbox").toObject().value("candidates").toArray()){const auto row=value.toObject();if(actionItem>0&&row.value("item_id").toInt()==actionItem){cost=row.value("remaining_cost");break;}}
                components=adaptive.selected?QStringLiteral("合成 %1G · 备选见详情").arg(CoachAdaptivePurchase::amount(cost)):adaptive.ranking?QStringLiteral("暂不购买 · 备选见详情"):QStringLiteral("条件待齐 · 详见原因");
                growthCandidateTooltip=adaptive.tooltip;
            }
            if(modelState==QStringLiteral("time_budget_exceeded")){caption=QStringLiteral("出装比较");title=QStringLiteral("本轮超时");detail=QStringLiteral("稍后重试 · 未输出排名");actionItem=0;buyable=false;componentItems={};components=QStringLiteral("保持当前装备");}
        }
        if(purchaseContext&&liveInput&&!uiBrowser&&actionItem<=0&&!buyable&&(!adaptive.active||(!adaptive.selected&&!adaptive.ranking))){
            const auto readiness=coachPurchaseInputReadiness(catalog,game);
            QJsonArray held,selfInventory;for(int id:game.owned)held.append(id);for(const auto &v:game.players){const auto p=v.toObject();if(p.value("self")==QJsonValue(true))selfInventory.append(QJsonObject{{"champion_id",p.value("champion_id")},{"level",p.value("level")},{"items",p.value("items")},{"items_complete",p.value("items_complete")}});}
            const auto basis=QJsonDocument(QJsonObject{{"held",held},{"self_inventory",selfInventory},{"gold",game.goldKnown?QJsonValue(game.gold):QJsonValue::Null},{"champion",game.championId},{"level",game.level},{"context",game.matchContext},{"phase",game.phase},{"mode",game.gameMode},{"queue",game.queueId},{"starting_window",game.gameTime<120},{"strategy_revision",externalReferenceRevision()},{"patch",game.observedPatch}}).toJson(QJsonDocument::Compact);
            if(purchaseFactsBasis!=basis){purchaseFacts=observedPurchaseFacts(catalog,game);purchaseFacts["source_reference"]=sourcePurchaseReference(catalog,game);purchaseFactsBasis=basis;}
            purchaseFacts["inputs_ready"]=readiness.value("ready").toBool();
            caption=QStringLiteral("商店参考");actionItem=0;buyable=false;componentItems={};components.clear();displaySource=QStringLiteral("observed_recipe_facts_no_combat_ranking");
            const bool known=purchaseFacts.value("available").toBool()&&purchaseFacts.value("budget_known").toBool();
            title=known?QStringLiteral("金币 %1 G").arg(int(game.gold)):QStringLiteral("金币待同步");
            const auto upgrades=purchaseFacts.value("upgrades").toArray();const int count=purchaseFacts.value("within_catalog_budget_count").toInt();
            detail=count>0?QStringLiteral("配方价参考 · %1条在预算内").arg(count):readiness.value("ready").toBool()?QStringLiteral("暂无配方候选"):readiness.value("summary").toString();
            // These are catalog recipe quotes, never a selected buy or an
            // optimal build. Augments can alter actual store prices.
            for(const auto &v:upgrades)if(v.toObject().value("within_catalog_budget")==QJsonValue(true)&&componentItems.size()<3)componentItems.append(v);
            if(componentItems.isEmpty()&&!upgrades.isEmpty()&&known){const int gap=std::max(0,upgrades[0].toObject().value("remaining_cost").toInt()-int(game.gold));detail=QStringLiteral("最近配方还差约 %1 G").arg(gap);}
            growthCandidateTooltip=QStringLiteral("按已观测装备、金币与目录配方列出，按合成价排列；不代表战斗收益排名，未核实海克斯改价与实际商店价格。强化记录不全时仍保留这些已知信息。\n自动读取：%1；设置 → 自动读取设置。也可在主页手动记录。").arg(growthCaptureStatusText(growthCapture.report()));
            components=QStringLiteral("目录合成价 · 请核对商店");
            const auto reference=purchaseFacts.value("source_reference").toObject();
            if(reference.value("available").toBool()){
                actionItem=reference.value("target_item_id").toInt();title=catalog.itemName(actionItem);caption=QStringLiteral("本地出装参考");displaySource=QStringLiteral("reference");
                componentItems=reference.value("components").toArray();components=QStringLiteral("按资料路线合成");
                QStringList routeItems;for(const auto &id:reference.value("route_item_ids").toArray())routeItems.append(catalog.itemName(id.toInt()));
                growthCandidateTooltip=QStringLiteral("海克斯大乱斗出装资料（%1）：%2。\n%3。未计入未确认的已选强化；这是资料参考，不是本局战斗收益排名。价格为配方参考，请核对商店。\n来源：%4").arg(reference.value("patch").toString(),routeItems.join(QStringLiteral(" / ")),reference.value("basis")==QStringLiteral("continue_observed_materials")?QStringLiteral("优先延续你已买的材料"):QStringLiteral("结合现有装备选择资料组合中的可合成目标"),reference.value("source_url").toString());
                if(reference.value("source_order_reference").toBool())growthCandidateTooltip+=QStringLiteral("\n材料衔接相同时，沿资料展示顺序推进；不代表本局最优购买顺序。资料未公布的胜率和样本量保持未知。");
                if(reference.value("requires_later_transformation").toBool())growthCandidateTooltip+=QStringLiteral("\n资料中的%1需先购买%2；后续升级状态另行核对。").arg(catalog.itemName(reference.value("source_target_item_id").toInt()),catalog.itemName(actionItem));
            }
        }else{
            purchaseFacts={};purchaseFactsBasis.clear();
        }
        const auto activeHudContext=game.matchContext.value("context_id").toString();
        if(game.fresh()&&game.matchContext.value("active").toBool()&&(game.phase=="InProgress"||game.phase=="Reconnect"))lastActiveHudContext=activeHudContext;
        const bool ended=QStringList{"EndOfGame","PreEndOfGame","WaitingForStats"}.contains(game.phase);
        if(ended&&!lastActiveHudContext.isEmpty()&&endHudContext!=lastActiveHudContext){endHudContext=lastActiveHudContext;endHudUntil=autoClock.elapsed()+1600;}
        const bool ending=ended&&autoClock.elapsed()<endHudUntil;
        const auto pregameRoster=game.matchContext.value("roster").toArray();
        const auto confirmedRoster=std::count_if(pregameRoster.cbegin(),pregameRoster.cend(),[](const auto &row){
            return row.toObject().value("champion_id").toInt()>0;
        });
        const bool rosterPending=pregameRoster.size()!=10||confirmedRoster!=10;
        phasePlate->setStage(ending?QStringLiteral("ending"):pregame?(stage=="loading"?QStringLiteral("loading"):QStringLiteral("selecting")):QString(),
            rosterPending,confirmedRoster>0&&confirmedRoster<10?static_cast<int>(confirmedRoster):-1);
        const bool pendingCards=!ending&&!pregame&&incompleteOfferCount>=0;
        if(pendingCards){
            actionItem=0;buyable=false;adaptive={};componentItems={};
            displaySource=QStringLiteral("incomplete_offer");
        }
        const bool rosterIcons=false;
        automaticHint->setItem(assets,actionItem);
        const auto sourceReference=purchaseFacts.value("source_reference").toObject();
        const auto checklistReference=displaySource=="reference"?sourceReference:QJsonObject{};
        QJsonArray held,selfInventory;for(int id:game.owned)held.append(id);for(const auto &value:game.players){const auto p=value.toObject();if(p.value("self")==QJsonValue(true))selfInventory.append(QJsonObject{{"champion",p.value("champion_id")},{"level",p.value("level")},{"items",p.value("items")},{"complete",p.value("items_complete")}});}
        if(!purchaseContext||pregame||ending||actionItem<=0){checklistState={};checklistBasis.clear();}
        else {
            const auto checklistKey=QJsonDocument(QJsonObject{{"item",actionItem},{"owned",held},{"self_inventory",selfInventory},{"champion",game.championId},{"level",game.level},{"gold",game.goldKnown?QJsonValue(game.gold):QJsonValue::Null},{"context_id",activeHudContext},{"active",game.matchContext.value("active")},{"phase",game.phase},{"mode",game.gameMode},{"patch",game.observedPatch},{"display_source",displaySource},{"reference",checklistReference}}).toJson(QJsonDocument::Compact);
            if(checklistBasis!=checklistKey){
                checklistState=purchaseChecklist(catalog,game,actionItem,checklistReference);
                checklistState["display_source"]=displaySource;checklistBasis=checklistKey;
            }
        }
        purchaseList->setChecklist(checklistState);purchaseList->setVisible(!pendingCards&&checklistState.value("available").toBool());
        const auto change=game.fresh()&&autoClock.elapsed()<purchaseChangeUntil?purchaseChangeReason:QString();
        if(actionItem>0){
            const bool budget=game.goldKnown&&std::isfinite(game.gold)&&game.gold>=0;
            const bool inventory=!liveInput||observedOwnInventory(game).has_value();
            const auto quote=inventory?planInventoryRecipeReference(catalog,game.championId,game.level,game.owned,actionItem):QJsonObject{};
            const int cost=quote.value("remaining_cost").toInt(-1);
            const bool quoted=quote.value("legal").toBool()&&cost>0;
            const bool withinBudget=quoted&&budget&&game.gold>=cost;
            const bool sourceRoute=displaySource==QStringLiteral("reference");
            const bool roleReference=displaySource==QStringLiteral("role_guidance");
            const bool conditional=displaySource==QStringLiteral("explicit_chain_condition_comparison");
            if(liveInput||!conditional){
                if(!inventory)detail=QStringLiteral("装备待同步");
                else if(!budget)detail=QStringLiteral("金币待同步");
                else if(!quoted)detail=QStringLiteral("配方价待核");
                else if(withinBudget)detail=QStringLiteral("配方预算内");
                else detail=QStringLiteral("差≈%1G").arg(num(std::ceil(cost-game.gold)));
                if(sourceRoute||roleReference||liveInput){
                    const auto source=sourceRoute?QStringLiteral("资料参考"):roleReference?QStringLiteral("辅助参考"):QStringLiteral("条件比较");
                    const auto full=source+QStringLiteral(" · ")+detail;
                    detail=automaticHint->purchaseTextFits(title,full)?full:source.left(2)+QStringLiteral(" · ")+detail;
                }
            }
            if(liveInput||sourceRoute||roleReference||conditional)buyable=false;
            auto reason=change;
            if(sourceRoute&&sourceReference.value("basis")==QStringLiteral("continue_observed_materials"))reason=QStringLiteral("沿用组件");
            if(!reason.isEmpty()){
                const auto joined=detail+QStringLiteral(" · ")+reason;
                if(automaticHint->purchaseTextFits(title,joined))detail=joined;
                else growthCandidateTooltip+=(growthCandidateTooltip.isEmpty()?QString():QStringLiteral("\n"))+reason;
            }
        }else if(!pregame){
            if(ended){title=QStringLiteral("对局结束");detail.clear();}
            else if(game.fresh()&&game.gameMode!="KIWI"){title=QStringLiteral("模式待核");detail.clear();}
            else if(sourceReference.value("reason")==QStringLiteral("observed_core_references_exhausted")){
                title=QStringLiteral("资料组合已完成");detail=QStringLiteral("后续路线待核对");
            }
        }
        if(pendingCards){
            caption=QStringLiteral("强化");title=QStringLiteral("海克斯 %1/3").arg(incompleteOfferCount);detail.clear();
        }
        auto matchup=ending?QJsonObject{}:purchaseMatchup(actionItem,displaySource);
        QString matchupDescription;
        if(!matchup.isEmpty()){
            QString label=QStringLiteral("对位待评");
            const auto output=matchup.value("output_gain"),durability=matchup.value("effective_health_gain");
            if(output.isDouble()||durability.isDouble()){
                const bool useOutput=output.isDouble()&&(!durability.isDouble()||std::abs(output.toDouble())>=std::abs(durability.toDouble()));
                const double percent=(useOutput?output:durability).toDouble()*100.;
                label=QStringLiteral("8秒估%1%2%3%").arg(useOutput?QStringLiteral("伤"):QStringLiteral("耐久"),
                    percent>0?QStringLiteral("+"):QString(),QString::number(percent,'f',0));
            }
            const auto name=matchup.value("champion_name").toString();
            matchupDescription=QStringLiteral("%1击杀我%2次 · %3").arg(name).arg(matchup.value("kills_against_self").toInt()).arg(label);
            for(const auto &text:{name+label,label}){
                const auto joined=detail.isEmpty()?text:detail+QStringLiteral(" · ")+text;
                const auto joinedTitle=title+QStringLiteral(" · ")+text;
                if(automaticHint->purchaseTextFits(title,joined))detail=joined;
                else if(automaticHint->purchaseTextFits(joinedTitle,detail))title=joinedTitle;
                else continue;
                matchup["display_label"]=text;
                break;
            }
        }
        automaticHint->setProperty("purchaseMatchup",matchup);
        compactEngine->hide();
        const bool passive=game.fresh()||pregame;
        for(const auto &name:{QStringLiteral("overlayFold"),QStringLiteral("overlayOptions"),QStringLiteral("overlayClose")})if(auto control=right->findChild<QAbstractButton*>(name))control->setVisible(!passive);
        if(auto layout=qobject_cast<QVBoxLayout*>(right->body->layout())){layout->setSpacing(2);layout->setContentsMargins(8,4,8,4);}
        automaticHint->setAction(caption,title,detail,buyable);
        automaticHint->setAccessibleDescription(matchupDescription);
        automaticHint->setProperty("shownItemId",actionItem);
        auto adaptiveReport=adaptive.report;if(adaptive.active){adaptiveReport["displayed_in_action_strip"]=automaticHint->isVisibleTo(right)&&!rosterIcons&&displaySource==QStringLiteral("explicit_chain_condition_comparison");adaptiveReport["displayed_item_id"]=displaySource==QStringLiteral("explicit_chain_condition_comparison")?actionItem:0;adaptiveReport["purchase_ready_displayed"]=displaySource==QStringLiteral("explicit_chain_condition_comparison")&&buyable;}
        automaticHint->setProperty("adaptivePurchasePresentation",adaptiveReport);
        const auto modelRefresh=coachResult.value("model_refresh").toObject();const QString refreshHint=modelRefresh.isEmpty()?QString():QStringLiteral("条件推演 · %1秒前局面%2").arg(modelRefresh.value("snapshot_age_ms").toDouble()/1000.,0,'f',1).arg(modelRefresh.value("refresh_pending").toBool()?QStringLiteral(" · 正在更新"):QString());automaticHint->setToolTip(growthCandidateTooltip.isEmpty()?refreshHint:growthCandidateTooltip+QStringLiteral("\n")+refreshHint);automaticHint->setProperty("model_refresh",modelRefresh);
        if(rosterIcons)componentsLabel->setRoster(liveSummary.value("players").toArray());else componentsLabel->setComponents(componentItems,components);
        if(displaySource==QStringLiteral("observed_recipe_facts_no_combat_ranking")||displaySource==QStringLiteral("reference"))componentsLabel->setToolTip(componentsLabel->text()+QStringLiteral("\n配方参考价；未计强化改价，不是出装排名。"));
        // A single empty-state sentence belongs beside the action, not in two rows.
        const bool repeatedContext=!componentsLabel->hasVisualContent()&&!componentsLabel->text().simplified().isEmpty()&&detail.simplified().contains(componentsLabel->text().simplified());
        componentsLabel->setProperty("repeatedActionContext",repeatedContext);
        if(repeatedContext||!componentsLabel->hasVisualContent())componentsLabel->hide();
        automaticHint->setVisible(!pregame&&!ending);routeLabel->parentWidget()->setVisible(!pregame&&!ending&&!pendingCards);
        const int actionHeight=automaticHint->isHidden()?0:automaticHint->height()+2;
        componentsLabel->hide();rosterInsights->hide();
        const int componentHeight=purchaseList->isHidden()?0:purchaseList->height()+2;
        const int footerHeight=pregame||pendingCards?0:20;
        const int briefHeight=phasePlate->isHidden()?0:phasePlate->height()+2;
        const int baseHeight=OverlayMetrics::titlebarHeight+8+actionHeight+componentHeight+footerHeight+briefHeight;
        const bool selecting=pregame&&championSelectStage();
        champSelectView->setState(champSelectInput());champSelectView->setVisible(selecting);
        const int selectHeight=selecting?std::clamp(placement.right.height()-baseHeight-2,138,286):0;
        if(selecting)champSelectView->setFixedHeight(selectHeight);
        const int wanted=baseHeight+liveRoster->sizeHint().height()+4;
        int rosterBudget=placement.right.height();
        if(!hudLayout.contains("coach")&&!hudLayout.contains("combat")&&game.fresh()&&combatCorner->ready()&&combatCorner->selfDead()&&placement.right.width()>=172){
            const int room=rosterBudget-baseHeight-26-8;
            // Keep the compact coach and the more relevant death comparison
            // together before spending the remaining lane on a full roster.
            if(combatCorner->wantsDetails()&&room>=CornerCombatHud::expandedHeight)
                rosterBudget-=CornerCombatHud::expandedHeight+8;
            else if(room>=CornerCombatHud::compactHeight)
                rosterBudget-=CornerCombatHud::compactHeight+8;
        }
        const bool rosterDetailFits=placement.right.width()>=172&&wanted<=rosterBudget;
        right->setProperty("passiveRosterDetailFits",rosterDetailFits);
        const bool fullRoster=detailsExpanded&&!pregame&&!ending&&!pendingCards&&rosterDetailFits;
        details->setVisible(fullRoster);rosterPage->setVisible(fullRoster);
        rosterSummary->setVisible((game.fresh()||pregame)&&!fullRoster&&!ending&&!selecting&&!pendingCards);
        const int summaryHeight=rosterSummary->isHidden()?0:26;
        const int preferredHeight=ending?OverlayMetrics::titlebarHeight+8+briefHeight:selecting?baseHeight+selectHeight+2:fullRoster?wanted:baseHeight+summaryHeight;
        right->body->layout()->activate();
        // Native input-flag changes can recreate a window at its older size.
        // An unchanged preference does not prove the actual bounds match it.
        if(right->report().value("preferred_content_height").toInt()!=preferredHeight
            ||(!right->collapsed()&&right->height()!=std::min(preferredHeight,placement.right.height())))right->setContentHeight(preferredHeight);
        const auto cue=equipmentCueResult.value("latest").toObject();const bool alert=game.fresh()&&!cue.isEmpty();cueIcon->setVisible(alert);
        if(alert){cueIcon->setPixmap(Kda::roundedPixmap(assets->item(cue.value("item_id").toInt()),QSize(16,16),Kda::radius("tag")-1,cueIcon->devicePixelRatioF()));route=catalog.championName(cue.value("champion_id").toInt())+QStringLiteral(" · ")+cue.value("item_name").toString();routeLabel->setToolTip(catalog.championName(cue.value("champion_id").toInt())+QStringLiteral(" 新增装备 · ")+cue.value("item_name").toString());}
        else routeLabel->setToolTip(route);
        const auto execution=coachResult.value("hero_execution_plan").toObject(),reference=coachResult.value("hero_execution_reference").toObject();
        const auto referenceSequence=reference.value("reference_sequence").toString();const auto currentSequence=execution.value("sequence").toString();
        QString executionTip=execution.value("summary").toString();if(!currentSequence.isEmpty())executionTip+=QStringLiteral("\n条件连招：")+currentSequence;if(!referenceSequence.isEmpty())executionTip+=QStringLiteral("\n技能就绪参考：")+referenceSequence+QStringLiteral("\n固定命中场景；控制、资源与释放前提见详情");
        engineDetails->setToolTip(executionTip);compactEngine->setToolTip(executionTip);if(!executionTip.isEmpty())routeLabel->setToolTip(routeLabel->toolTip()+QStringLiteral("\n")+executionTip);
        routeLabel->setStyleSheet(alert?QStringLiteral("color:#EF8793;"):QString());
        if(right->body->layout())right->body->layout()->activate();
        QJsonObject selfScores;for(const auto &v:liveSummary.value("players").toArray())if(v.toObject().value("self").toBool()){selfScores=v.toObject().value("scores").toObject();break;}
        routeLabel->setContext(selfScores,game.gameTime,game.fresh());routeLabel->setProperty("fullRoute",route);routeLabel->setProperty("iconOnly",!alert&&!game.fresh());routeLabel->setAccessibleName(route);
        routeLabel->setText(QString());routeLabel->setToolTip(QString());routeLabel->setOutlook(observedMatchOutlook(catalog,game));
        rosterInsights->clear();
        if(recommendationTrace)recommendationTrace->record(catalog,game,coachResult,cardModelCurrent()?cardDecision:QJsonObject{},QJsonObject{{"rendered",true},{"visible",right->isVisible()&&automaticHint->isVisibleTo(right)&&!rosterIcons},{"item_id",actionItem},{"source",displaySource},{"role_scope",combatRoleIntent(catalog,game,style->currentData().toString()).value("intent")}},RecommendationTraceBinding{coachResultContext,cardResultContext,game.matchContext.value("context_id").toString()});
    }
    void renderCards(){renderSourceLabels();renderCardHints();publishRuntimeSnapshot();}
    QString loadingPreparationHtml()const{
        const auto match=game.matchContext;QString html;
        if(match.value("stage")==QStringLiteral("loading")&&!loadingPreparationBasis(catalog,game,style->currentData().toString()).isEmpty()){
            html+=QStringLiteral("<small>预演 · 三级空装假设 · 进场重算</small><br>");
            if(loadingComputedBasis==loadingPreparationBasis(catalog,game,style->currentData().toString())&&loadingPreparation.value("available").toBool()){
                QStringList names;for(const auto &v:loadingPreparation.value("candidate_directions").toArray()){if(names.size()>=2)break;names.append(h(v.toObject().value("name").toString()));}html+=names.join(QStringLiteral(" / "));
            }else html+=QStringLiteral("正在比较…");

        }
        return html;
    }
    QString matchRosterHtml()const{
        const auto match=game.matchContext;if(!match.value("active").toBool())return {};
        const auto composition=match.value("composition").toObject();QString html=loadingPreparationHtml();
        for(const auto &side:{QStringLiteral("ALLY"),QStringLiteral("ENEMY"),QStringLiteral("UNKNOWN")}){
            QString body;for(const auto &v:match.value("roster").toArray()){const auto p=v.toObject();if(p.value("team")!=side)continue;const int champion=p.value("champion_id").toInt();
                body+=QStringLiteral("<p><b>%1%2</b>").arg(p.value("self").toBool()?QStringLiteral("你 · "):QString(),champion>0?h(catalog.championName(champion)):QStringLiteral("未公开"));
                QList<QJsonObject> histories;if(automaticHistoryAvailable())for(const auto &value:anonymousHistory.value("players").toArray()){const auto reference=value.toObject();if(champion>0&&reference.value("champion_id").toInt()==champion&&reference.value("team")==side&&(p.value("cell").isNull()||reference.value("cell")==p.value("cell")))histories.append(reference);}
                if(histories.size()==1){const auto reference=histories.first();auto support=reference.value("same_patch_champion").toObject();if(!support.value("available").toBool())support=reference.value("same_patch").toObject();
                    if(support.value("available").toBool()){const auto metrics=support.value("metrics").toObject();body+=QStringLiteral("<br><small>同版 %1 场 · 场均 %2/%3/%4</small>").arg(support.value("sampled_games").toInt()).arg(score(metrics,"kills"),score(metrics,"deaths"),score(metrics,"assists"));}
                }body+=QStringLiteral("</p>");
            }
            if(body.isEmpty()&&side==QStringLiteral("UNKNOWN"))continue;
            html+=QStringLiteral("<h3>%1</h3>").arg(side==QStringLiteral("ALLY")?QStringLiteral("我方"):side==QStringLiteral("ENEMY")?QStringLiteral("对手"):QStringLiteral("队伍待确认"));
            const auto team=composition.value("teams").toObject().value(side).toObject();if(!team.isEmpty()&&team.value("known_champions").toInt()>0)html+=QStringLiteral("<small>%1</small>").arg(h(team.value("summary").toString()));
            html+=body.isEmpty()?QStringLiteral("<p>待公开</p>"):body;
        }
        if(!rosterAiPlan.value("summary").toString().isEmpty())html+=QStringLiteral("<hr><p>DS · %1</p>").arg(h(rosterAiPlan.value("summary").toString()));
        return html;
    }
    void refreshLiveHud(){
        equipmentCueResult=equipmentCues.update(catalog,game);
        liveSummary=liveSummaryEngine.update(catalog,game);
        combatCorner->setCombatState(catalog,game,liveSummary,demo);
        combatCornerReceived=game.received;
        combatCornerContext=game.matchContext.value("context_id").toString();
        if(!game.fresh()&&game.matchContext.value("active").toBool()&&(game.matchContext.value("stage")==QStringLiteral("champ_select")||game.matchContext.value("stage")==QStringLiteral("loading"))){
            QJsonArray players;int index=0;for(const auto &v:game.matchContext.value("roster").toArray()){const auto p=v.toObject();players.append(QJsonObject{{"row_index",index++},{"champion_id",p.value("champion_id")},{"team",p.value("team")},{"self",p.value("self")}});}
            liveSummary["players"]=players;liveSummary["source"]="visible_champion_select";liveSummary["stage"]=game.matchContext.value("stage");
        }
        // Historical references stay separate from the current counters. Only
        // a unique anonymous champion/team match may annotate a roster row.
        if(automaticHistoryAvailable()){
            auto rows=liveSummary.value("players").toArray();
            for(int i=0;i<rows.size();++i){auto row=rows[i].toObject();int localMatches=0;for(const auto &v:rows){const auto other=v.toObject();localMatches+=other.value("champion_id")==row.value("champion_id")&&other.value("team")==row.value("team");}if(localMatches!=1)continue;
                QList<QJsonObject> matches;for(const auto &v:anonymousHistory.value("players").toArray()){const auto p=v.toObject();if(p.value("champion_id")==row.value("champion_id")&&p.value("team")==row.value("team"))matches.append(p);}if(matches.size()!=1)continue;
                auto support=matches.first().value("same_patch_champion").toObject();if(!support.value("available").toBool())support=matches.first().value("same_patch").toObject();if(support.value("available").toBool())row["historical_reference"]=support;rows[i]=row;
            }liveSummary["players"]=rows;
        }
        const auto historyEvidence=game.recommendationEvidence;
        const bool historyCurrent=historyEvidence.value("context_id")==game.matchContext.value("context_id")
            &&!game.matchContext.value("context_id").toString().isEmpty()&&historyEvidence.value("players").toArray().size()==10;
        const int historyKnown=historyCurrent?historyEvidence.value("known_history_players").toInt():0;
        const int historyAnalyzed=historyCurrent?historyEvidence.value("player_analysis_consumed_players").toInt():0;
        const int historyFresh=historyCurrent?historyEvidence.value("fresh_history_players").toInt():0;
        if(historyCurrent){
            auto rows=liveSummary.value("players").toArray();
            for(int i=0;i<rows.size();++i){auto row=rows[i].toObject();QList<QJsonObject> matches;
                for(const auto &value:historyEvidence.value("players").toArray()){const auto ref=value.toObject();if(ref.value("champion_id")==row.value("champion_id")&&ref.value("team")==row.value("team"))matches.append(ref);}
                if(matches.size()!=1)continue;const auto ref=matches.first();const auto summary=ref.value("history").toObject();
                row["history_coverage"]=QJsonObject{{"known",summary.value("games").toInt()>0},{"games",summary.value("games")},{"same_patch_games",ref.value("same_patch").toObject().value("games")},{"analysis_consumed",ref.value("player_analysis_consumed")},{"freshness",ref.value("freshness").toObject().value("status")}};rows[i]=row;
            }liveSummary["players"]=rows;
        }
        liveCounters->setSummary(liveSummary);liveRoster->setSummary(liveSummary);
        const bool pregame=!game.fresh()&&game.matchContext.value("active").toBool()&&(game.matchContext.value("stage")==QStringLiteral("champ_select")||game.matchContext.value("stage")==QStringLiteral("loading"));
        const auto preparation=loadingPreparationHtml();
        const auto historyCaption=QStringLiteral("历史记录 %1/10 · 已分析 %2/10 · 新鲜 %3/10").arg(historyKnown).arg(historyAnalyzed).arg(historyFresh);
        const auto full=(pregame?preparation:coachRosterInsightsHtml(catalog,liveSummary,coachResult,equipmentCueResult))+QStringLiteral("<p>%1</p>").arg(historyCaption);
        rosterInsights->setProperty("lastHtml",full);rosterInsights->setToolTip(QString());rosterInsights->hide();rosterInsights->setFixedHeight(pregame?36:std::max(18,QFontMetrics(rosterInsights->font()).height()));
        int unknown=0;for(const auto &v:liveSummary.value("players").toArray())unknown+=!v.toObject().value("augments_complete").toBool();
        rosterInsights->setText(pregame?preparation:QStringLiteral("≈ 模型 · 历史 %1/10").arg(historyKnown));rosterInsights->setAccessibleDescription(historyCaption+QStringLiteral(" · 强化缺 %1").arg(unknown));updateRosterPaging();
    }
    void refreshPurchaseArtwork(){
        if(!advice)return;
        const qreal dpr=advice->devicePixelRatioF();
        auto rows=coachResult.value("plan").toObject().value("items").toArray();for(int id:game.owned)rows.append(QJsonObject{{"item_id",id}});
        for(const auto &v:rows){
            const int id=v.toObject().value("item_id").toInt();if(id<=0)continue;
            auto art=Kda::roundedPixmap(assets->item(id),QSize(40,40),Kda::radius("tag"),dpr);
            if(art.isNull())art=HudSymbols::pixmap(HudSymbols::Symbol::Coins,40,dpr,Kda::color("muted"));
            advice->document()->addResource(QTextDocument::ImageResource,QUrl(QStringLiteral("hud:item/%1").arg(id)),art.toImage());
        }
        advice->viewport()->update();
    }
    void refreshExecutionReference(){
        if(demo||replay||uiBrowser||runningUiChecks)return;
        if(finishing||coachResult.isEmpty()||!coachResult.contains("hero_execution_reference")||!coachResult.value("computation_complete").toBool()||!game.fresh()||!modelRulesUsable(catalog,game)||game.gameMode!=QStringLiteral("KIWI")){executionReference.invalidate();return;}
        const auto preference=style->currentData().toString();
        executionReference.update(catalog,game,preference);
        executionReference.apply(catalog,game,preference,coachResult);
    }
    void renderCoach(){
        const auto publish=qScopeGuard([this]{publishRuntimeSnapshot();});
        refreshPublishedCoach();refreshExecutionReference();
        refreshLiveHud();applyPresentation();
        if(!uiBrowser)renderDeathTips();
        learnedPrediction=learnedStatePrediction(catalog.root,game);purchaseHistory=historicalPurchaseReference(catalog.root,game);
        auto scopedPrediction=learnedPrediction;scopedPrediction["context_id"]=game.matchContext.value("context_id");scopedPrediction["game_time"]=game.gameTime;
        compactEngine->setSnapshot(liveSummary,scopedPrediction,game.matchContext);engineDetails->setSnapshot(liveSummary,scopedPrediction,game.matchContext);
        renderAutomaticAdvice(false);renderCardHints();renderSourceLabels();
        const bool ready=game.fresh()&&coachResult.value("fresh").toBool();
        right->setHeading((demo?QStringLiteral("演示 · "):replay?QStringLiteral("回放 · "):QString())+(game.fresh()?catalog.championName(game.championId)+QStringLiteral(" Lv.%1").arg(game.level):game.matchContext.value("active").toBool()?QStringLiteral("阵容分析"):QStringLiteral("对局教练")));
        const auto observed=observedCoachScore(game);const QString observedKills=observed.value("available").toBool()?QStringLiteral("%1:%2").arg(observed.value("ally_kills").toInt()).arg(observed.value("enemy_kills").toInt()):QStringLiteral("—");
        if(!ready){
            if(game.fresh())summary->setValues({observedKills,QStringLiteral("—"),game.goldKnown?num(game.gold):QStringLiteral("—"),QStringLiteral("—")});
            else summary->setMessage(game.matchContext.value("active").toBool()?game.matchContext.value("composition").toObject().value("summary").toString():QStringLiteral("未连接"));
            QStringList owned;for(int id:game.owned)owned.append(h(catalog.itemName(id)));
            const QString reason=!game.fresh()?QStringLiteral("等待当前局实时数据"):!modelRulesUsable(catalog,game)?QStringLiteral("本地规则尚未就绪，等待载入"):game.gameMode!=QStringLiteral("KIWI")?QStringLiteral("当前未确认海克斯模式，暂不排序"):modelState==QStringLiteral("time_budget_exceeded")?QStringLiteral("本轮比较超时，未完成排序；稍后用最新局面重试"):modelState==QStringLiteral("calculating")?QStringLiteral("正在比较当前装备与等级；本页可继续查看"): QStringLiteral("暂缺可核实的购买比较，保持已有装备");
            QString body=QStringLiteral("<p><b>%1</b></p><p>已有装备：%2</p>").arg(reason,owned.isEmpty()?QStringLiteral("未观察到装备"):owned.join(QStringLiteral(" / ")));
            if(!observed.value("available").toBool())body+=QStringLiteral("<p><small>比分：%1</small></p>").arg(h(observed.value("reason").toString()));putHtml(advice,body);
            const auto html=matchRosterHtml();putHtml(roster,html.isEmpty()?QStringLiteral("<p>等待局面</p>"):html);return;}
        const auto economy=coachResult.value("economy").toObject(),plan=coachResult.value("plan").toObject();
        const QString kills=observedKills;
        summary->setValues({kills,economy.value("equipment_value_difference").isDouble()?money(economy.value("equipment_value_difference").toInt()):QStringLiteral("—"),economy.value("self_current_gold").isDouble()?num(economy.value("self_current_gold").toDouble()):QStringLiteral("—"),economy.value("average_level_difference").isDouble()?num(economy.value("average_level_difference").toDouble(),1):QStringLiteral("—")});
        refreshPurchaseArtwork();
        const auto quality=coachResult.value("recommendation_quality").toObject();
        const auto adaptive=CoachAdaptivePurchase::fromPlan(plan,game.gameTime,[this](int id){return catalog.itemName(id);},quality);
        const bool growthCandidates=growthCandidatePlan(plan);
        const bool rankingBlocked=growthCandidates||(quality.contains("ranking_available")&&!quality.value("ranking_available").toBool());
        const bool noRecommendation=economy.value("next_purchase").toObject().value("item_id").toInt()<=0;
        const bool ownedOnly=!growthCandidates&&(rankingBlocked||noRecommendation);
        auto shownItems=plan.value("items").toArray();
        if(ownedOnly){shownItems={};for(int id:game.owned)shownItems.append(QJsonObject{{"item_id",id},{"name",catalog.itemName(id)},{"owned",true}});}
        QString text;QStringList accessibleBuild;
        if(adaptive.active){
            QStringList owned;for(int id:game.owned)owned.append(catalog.itemName(id));
            text=adaptive.html+QStringLiteral("<p><small>当前背包 · %1</small></p>").arg(owned.isEmpty()?QStringLiteral("—"):h(owned.join(QStringLiteral(" / "))));
            accessibleBuild.append(adaptive.title+QStringLiteral("，")+adaptive.status);
        }else{
        text=growthCandidates?QStringLiteral("<p><b>成长联动候选</b> · %1组搭配（并列展示，未排序）</p><p><small>组合用于比较不同成长指标的取舍；购买条件与顺序尚需核实。<br>%2</small></p><p>当前背包 / 已核实成长前置：</p>").arg(plan.value("candidate_loadouts").toArray().size()).arg(h(growthCandidateNotes(plan).join(QStringLiteral("；")))):rankingBlocked?QStringLiteral("<p>%1。当前装备：</p>").arg(h(quality.value("summary").toString(QStringLiteral("关键计算条件待核实，先看条件候选")))):noRecommendation?QStringLiteral("<p>%1。当前装备：</p>").arg(h(plan.value("route").toString(QStringLiteral("当前没有可核实的购买方案")))):QString();
        if(growthCandidates&&shownItems.isEmpty())text+=QStringLiteral("<p><small>当前未观察到装备，仍可查看下方成长候选。</small></p>");
        text+=QStringLiteral("<table width='100%' cellspacing='0' cellpadding='0'>");int slot=0;
        for(const auto &v:shownItems){
            const auto item=v.toObject();const auto price=item.value("price");const bool owned=item.value("owned").toBool();
            const auto status=owned?QStringLiteral("✓"):price.isDouble()&&price.toDouble()>=0?num(price.toDouble())+QStringLiteral(" G"):QStringLiteral("—");
            if(slot%3==0)text+=QStringLiteral("<tr>");
            const auto label=growthCandidates?(owned?QStringLiteral("已有装备"):item.value("growth_prerequisite").toBool()?QStringLiteral("成长前置 · 获取待核实"):QStringLiteral("条件参考")):ownedOnly?status:QStringLiteral("%1 · %2").arg(slot+1).arg(status);
            text+=QStringLiteral("<td width='33%' align='center' style='padding:7px 1px'><a href='hud-item:/%1'><img src='hud:item/%1' width='40' height='40' alt='%2'></a><br><span style='font-family:Inter;font-size:14px;font-weight:700;color:%3'>%4</span></td>").arg(item.value("item_id").toInt()).arg(h(item.value("name").toString()),owned?QStringLiteral("#a5c9d6"):QStringLiteral("#cbb7dd"),h(label));
            accessibleBuild.append(item.value("name").toString()+QStringLiteral(" · ")+status);if(++slot%3==0)text+=QStringLiteral("</tr>");
        }
        if(slot%3)text+=QStringLiteral("</tr>");text+=QStringLiteral("</table>");
        if(growthCandidates){
            text+=QStringLiteral("<p><small>以下比值均为同一基线下的部分模型比较，尚非实测成长收益。</small></p>");
            for(const auto &v:plan.value("candidate_loadouts").toArray()){
                const auto candidate=v.toObject();QStringList labels,names,ratios;
                for(const auto &metric:candidate.value("retained_for_metrics").toArray()){const auto label=growthMetricLabel(metric.toString());if(!labels.contains(label))labels.append(label);}
                for(const auto &id:candidate.value("item_ids").toArray()){const int item=id.toInt();if(item>0)names.append(QStringLiteral("<a href='hud-item:/%1'>%2</a>").arg(item).arg(h(catalog.itemName(item))));}
                const auto metrics=candidate.value("metrics").toObject();for(auto it=metrics.constBegin();it!=metrics.constEnd();++it){const auto ratio=it.value().toObject().value("ratio");if(ratio.isDouble()&&std::isfinite(ratio.toDouble())&&ratio.toDouble()>0)ratios.append(growthMetricLabel(it.key())+QStringLiteral(" %1×").arg(num(ratio.toDouble(),2)));}
                const auto label=labels.isEmpty()?QStringLiteral("成长指标比较"):labels.join(QStringLiteral(" / "));
                text+=QStringLiteral("<p><b>条件组合 · %1</b><br>%2%3</p>").arg(h(label),names.isEmpty()?QStringLiteral("装备条件待补充"):names.join(QStringLiteral(" / ")),ratios.isEmpty()?QString():QStringLiteral("<br><small>%1</small>").arg(h(ratios.join(QStringLiteral(" · ")))));
                accessibleBuild.append(QStringLiteral("条件组合 · ")+label+QStringLiteral("：")+names.join(QStringLiteral(" / ")));
            }
        }
        const auto teamPurchase=plan.value("purchase_sandbox").toObject();
        if(!rankingBlocked&&teamPurchase.value("objective_basis")==QStringLiteral("marginal_team_output_and_durability"))text+=QStringLiteral("<p><small>已结合双方装备，比较全队输出与承伤增益；未知机制未计入。</small></p>");
        if(rankingBlocked){const auto groups=quality.value("candidate_actions").toObject();for(const auto &key:{QStringLiteral("available_now"),QStringLiteral("save_for"),QStringLiteral("budget_unknown")}){QMap<int,QJsonObject> rows;for(const auto &v:groups.value(key).toArray()){const auto row=v.toObject();rows.insert(row.value("item_id").toInt(),row);}if(rows.isEmpty())continue;const auto heading=key==QStringLiteral("available_now")?QStringLiteral("金币可覆盖的条件候选"):key==QStringLiteral("save_for")?QStringLiteral("仍需存钱的条件候选"):QStringLiteral("预算待核实的条件候选");text+=QStringLiteral("<p><small>%1（未排序，不含队友增益）<br>").arg(heading);for(auto it=rows.cbegin();it!=rows.cend();++it)text+=QStringLiteral("%1 · %2G<br>").arg(h(catalog.itemName(it.key())),num(it.value().value("remaining_cost").toDouble()));text+=QStringLiteral("</small></p>");}}
        }
        const auto localEvidence=currentPurchaseChoiceEvidence();
        QStringList withinBudget,conditional;
        for(const auto &value:localEvidence.value("candidates").toArray()){
            const auto row=value.toObject();
            const auto estimate=localChoiceComponentText(row);
            if(estimate.isEmpty())continue;
            auto &lines=row.value("native_purchase_verified").toBool()&&row.value("affordable")==QJsonValue(true)?withinBudget:conditional;
            if(lines.size()<3)lines.append(h(catalog.itemName(row.value("item_id").toInt())+QStringLiteral(" · ")+estimate));
        }
        if(!withinBudget.isEmpty())text+=QStringLiteral("<p><small>本地小模型 · 当前金币覆盖目录报价<br>%1</small></p>").arg(withinBudget.join(QStringLiteral("<br>")));
        if(!conditional.isEmpty())text+=QStringLiteral("<p><small>本地小模型 · 条件备选<br>%1</small></p>").arg(conditional.join(QStringLiteral("<br>")));
        if(deepSeekPlanningEnabled()){
            const auto forecastText=dsForecastText(catalog,forecast.currentSelection(game));
            if(!forecastText.isEmpty())text+=QStringLiteral("<p><small>DeepSeek · %1</small></p>").arg(h(forecastText));
        }
        const auto modelRefresh=coachResult.value("model_refresh").toObject();if(!quality.value("summary").toString().isEmpty())text+=QStringLiteral("<p><small>%1</small></p>").arg(h(quality.value("summary").toString()));if(!modelRefresh.isEmpty())text+=QStringLiteral("<p><small>条件推演 · %1秒前局面%2</small></p>").arg(modelRefresh.value("snapshot_age_ms").toDouble()/1000.,0,'f',1).arg(modelRefresh.value("refresh_pending").toBool()?QStringLiteral(" · 正在更新"):QString());
        advice->setAccessibleDescription(accessibleBuild.join(QStringLiteral("，")));putHtml(advice,text);
        QString teams;
        QHash<int,QJsonObject> loadoutPlayers;
        for(const auto &value:coachResult.value("historical_loadout_reference").toObject().value("players").toArray()){
            const auto player=value.toObject();
            loadoutPlayers.insert(player.value("player_index").toInt(-1),player);
        }
        for(bool ally:{true,false}){teams+=ally?QStringLiteral("<h3>我方</h3>"):QStringLiteral("<h3>对手</h3>");int index=0;for(const auto &v:coachResult.value("roster").toArray()){const auto row=v.toObject();const int person=index++;if(row.value("ally").toBool()!=ally)continue;const auto scores=row.value("scores").toObject();QStringList items,augments;for(const auto &id:row.value("items").toArray())items.append(catalog.itemName(id.toInt()));for(const auto &id:row.value("augment_ids").toArray())augments.append(catalog.augments.value(id.toInt()).value("name_zh_cn").toString(QString::number(id.toInt())));
            teams+=QStringLiteral("<p><b>%1%2</b> Lv.%3 · %4/%5/%6%7<br>%8<br><small>海克斯 %9%10</small> <a href='player:%11'>校正</a></p>").arg(row.value("self").toBool()?QStringLiteral("你 · "):QString(),h(row.value("champion_name").toString())).arg(row.value("level").toInt()).arg(score(scores,"kills")).arg(score(scores,"deaths")).arg(score(scores,"assists")).arg(row.value("is_dead").toBool()?QStringLiteral(" · 阵亡"):QString()).arg(h(items.isEmpty()?QStringLiteral("装备未知"):items.join(QStringLiteral(" / "))),h(augments.isEmpty()?QStringLiteral("未知"):augments.join(QStringLiteral(" / "))),!augments.isEmpty()&&!row.value("augments_complete").toBool()?QStringLiteral(" · 不完整"):QString()).arg(person);
            const auto historicalPlayer=loadoutPlayers.value(person);
            const auto pairs=historicalPlayer.value("pairs").toArray();
            if(!pairs.isEmpty()&&historicalPlayer.value("champion_id")==row.value("champion_id")&&historicalPlayer.value("team")==row.value("team")){
                const auto pair=pairs.first().toObject();
                const auto item=pair.value("item_id"),augment=pair.value("augment_id");
                if(row.value("items").toArray().contains(item)&&row.value("augment_ids").toArray().contains(augment)){
                    const auto armText=[](const QJsonObject &arm){
                        return QStringLiteral("%1 局 · KDA %2 · 分伤 %3").arg(arm.value("matches").toInt())
                            .arg(arm.value("average_kda").isDouble()?num(arm.value("average_kda").toDouble(),2):QStringLiteral("—"),
                                 arm.value("average_damage_per_minute").isDouble()?num(arm.value("average_damage_per_minute").toDouble()):QStringLiteral("—"));
                    };
                    const auto itemName=catalog.itemName(item.toInt());
                    const auto augmentName=catalog.augments.value(augment.toInt()).value("name_zh_cn").toString();
                    teams+=QStringLiteral("<p><small>同版本历史 · %1 + %2<br>有该装备 %3<br>同海克斯无该装备 %4</small></p>")
                        .arg(h(itemName),h(augmentName),armText(pair.value("exposed").toObject()),armText(pair.value("control").toObject()));
                }
            }
        }}if(!rosterAiPlan.value("summary").toString().isEmpty())teams+=QStringLiteral("<hr><p>DS · %1</p>").arg(h(rosterAiPlan.value("summary").toString()));putHtml(roster,teams);
    }
    void snapshot(const QString &path){
        QDir().mkpath(QFileInfo(path).absolutePath());const double scale=std::min(1.,1400./viewport.width());
        QImage image(QSize(qRound(viewport.width()*scale),qRound(viewport.height()*scale)),QImage::Format_ARGB32_Premultiplied);image.fill(QColor("#080e13"));
        QPainter painter(&image);painter.scale(scale,scale);painter.translate(-viewport.topLeft());
        painter.setPen(QPen(QColor("#20313a"),1,Qt::DashLine));for(const auto &rect:placement.protectedAreas)painter.drawRect(rect);
        painter.setPen(QColor("#647a82"));painter.drawText(QRect(viewport.x(),viewport.y(),viewport.width(),qRound(viewport.height()*.08)),Qt::AlignCenter,
            QStringLiteral("合成示意 · 非游戏截图"));
        if(selectionActive){
            const auto rows=orderedCardHints(state,{}, {},game.selected,false);
            for(int i=0;i<cardPlacement.cards.size();++i){const auto rect=cardPlacement.cards[i];
                painter.setPen(QPen(QColor("#625839"),1));painter.setBrush(QColor("#15242c"));painter.drawRoundedRect(rect,Kda::radius("control"),Kda::radius("control"));
                painter.setPen(QColor("#c5b98f"));painter.drawText(rect.adjusted(12,12,-12,-12),Qt::AlignHCenter|Qt::AlignTop,rows.at(i).toObject().value("title").toString());
                painter.setPen(QColor("#697e82"));painter.drawText(rect,Qt::AlignCenter,QStringLiteral("参考卡片 %1").arg(i+1));
            }
        }else{painter.setPen(QColor("#647a82"));painter.drawText(placement.protectedAreas[0],Qt::AlignCenter,QStringLiteral("中央战场"));}
        if(right->isVisible())painter.drawPixmap(right->pos(),right->grab());
        if(combatCorner->isVisible())painter.drawPixmap(combatCorner->pos(),combatCorner->grab());
        for(auto hint:cardHints)if(hint->isVisible())painter.drawPixmap(hint->pos(),hint->grab());
        if(deathTips&&deathTips->isVisible())painter.drawPixmap(deathTips->pos(),deathTips->grab());
        painter.end();image.save(path);const QFileInfo file(path);
        right->grab().save(file.absolutePath()+"/"+file.completeBaseName()+"-right.png");
        if(combatCorner->isVisible())combatCorner->grab().save(file.absolutePath()+"/"+file.completeBaseName()+"-combat-corner.png");
        for(int i=0;i<cardHints.size();++i)cardHints[i]->grab().save(file.absolutePath()+"/"+file.completeBaseName()+"-card-"+QString::number(i+1)+".png");
    }
    void matchUiChecks(){
        const auto savedGame=game;const auto savedHistory=anonymousHistory,savedPlan=rosterAiPlan,savedPreparation=loadingPreparation;const auto savedLoadingBasis=loadingComputedBasis;const bool expanded=detailsExpanded;const int dispatches=rosterAiDispatches;anonymousHistory={};rosterAiPlan={};loadingPreparation={};loadingComputedBasis.clear();
        const auto oldCardKey=cardBasis(),oldModelKey=modelBasis(),oldAdviceKey=adviceBasis();game.matchContext=QJsonObject{{"context_id","synthetic-next-match"}};
        uiChecks["new_match_invalidates_all_model_keys"]=cardBasis()!=oldCardKey&&modelBasis()!=oldModelKey&&adviceBasis()!=oldAdviceKey;
        MatchContextTracker tracker;tracker.observePhase(QStringLiteral("ChampSelect"));QJsonArray allies,enemies;
        const QList<int> champions{1,10,22,86,12};for(int i=0;i<5;++i){allies.append(QJsonObject{{"cellId",i},{"championId",champions[i]}});enemies.append(QJsonObject{{"cellId",i+5},{"championId",0}});}
        tracker.observeChampionSelect(QJsonObject{{"localPlayerCellId",0},{"myTeam",allies},{"theirTeam",enemies}},tracker.generation());
        game={};game.championId=1;game.queueId=2400;game.gameMode=QStringLiteral("KIWI");game.observedPatch=catalog.patch;game.bench={516,51,25,223};game.benchKnown=true;game.matchContext=tracker.json();game.matchContext["composition"]=analyzeMatchComposition(game.matchContext,catalog.champions);setDetailsExpanded(false);renderCoach();QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        auto textFits=[this]{const auto hud=automaticHint->report();return visibleChildrenFit(right->body)&&hud.value("text_fits").toBool()&&hud.value("lines_do_not_overlap").toBool()&&right->report().value("heading_fits").toBool();};
        uiChecks["pregame_shows_self_allies_and_bench_without_hidden_enemies"]=phasePlate->report().value("stage")==QStringLiteral("selecting")&&phasePlate->isVisibleTo(right)&&!automaticHint->isVisibleTo(right)&&champSelectView->isVisibleTo(right)&&champSelectView->model().value("visible_ally_count").toInt()==5&&champSelectView->report().value("content_fits").toBool()&&!componentsLabel->isVisibleTo(right)&&!routeLabel->isVisibleTo(right)&&right->height()<=400&&textFits()&&!selectionActive;
        if(parser.isSet("snapshot")){const QFileInfo image(parser.value("snapshot"));snapshot(image.absolutePath()+"/"+image.completeBaseName()+"-pregame.png");}
        setDetailsExpanded(true);QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        uiChecks["pregame_detail_roster_wraps_inside_viewport"]=liveRoster->report().value("content_fits").toBool()&&liveSummary.value("players").toArray().size()==10&&roster->toPlainText().contains(QStringLiteral("未公开"));
        tracker.observePhase(QStringLiteral("GameStart"));QJsonArray teamOne,teamTwo;for(int i=0;i<5;++i){teamOne.append(QJsonObject{{"championId",champions[i]},{"puuid",i==0?QStringLiteral("synthetic-self"):QString()}});teamTwo.append(QJsonObject{{"championId",champions[4-i]}});}
        const auto start=tracker.json().value("started_at_ms");tracker.observeGameflow(QJsonObject{{"gameData",QJsonObject{{"gameId",123456},{"teamOne",teamOne},{"teamTwo",teamTwo}}}},QStringLiteral("synthetic-self"),tracker.generation());game.matchContext=tracker.json();game.matchContext["composition"]=analyzeMatchComposition(game.matchContext,catalog.champions);setDetailsExpanded(false);renderCoach();QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        uiChecks["loading_compact_supplements_ten_visible_champions"]=game.matchContext.value("roster").toArray().size()==10&&phasePlate->report().value("stage")==QStringLiteral("loading")&&phasePlate->isVisibleTo(right)&&!automaticHint->isVisibleTo(right)&&right->height()<=160&&textFits()&&game.matchContext.value("started_at_ms")==start;
        const auto preparationBasis=loadingPreparationBasis(catalog,game,style->currentData().toString());
        uiChecks["loading_preparation_starts_before_live_data"]=!game.fresh()&&!preparationBasis.isEmpty()&&phasePlate->report().value("stage")==QStringLiteral("loading");
        loadingComputedBasis=preparationBasis;loadingPreparation=QJsonObject{{"available",true},{"candidate_directions",QJsonArray{QJsonObject{{"name",QStringLiteral("条件成长A")}},QJsonObject{{"name",QStringLiteral("条件成长B")}}}}};renderAutomaticAdvice();
        uiChecks["loading_preparation_summary_requires_live_calibration"]=!rosterInsights->isVisibleTo(right)&&!automaticHint->isVisibleTo(right)&&phasePlate->report().value("stage")==QStringLiteral("loading")&&textFits();
        if(parser.isSet("snapshot")){const QFileInfo image(parser.value("snapshot"));snapshot(image.absolutePath()+"/"+image.completeBaseName()+"-loading.png");}
        game.matchContext["stage"]=QStringLiteral("in_game");renderAutomaticAdvice();QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);uiChecks["stale_in_game_never_claims_loading"]=phasePlate->isHidden()&&automaticHint->report().value("title")==QStringLiteral("等待实时数据")&&textFits();game.matchContext["stage"]=QStringLiteral("loading");renderAutomaticAdvice();
        setDetailsExpanded(true);QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);uiChecks["loading_detail_roster_wraps_inside_viewport"]=liveRoster->report().value("content_fits").toBool()&&liveSummary.value("players").toArray().size()==10&&!roster->toPlainText().contains(QStringLiteral("未公开"));
        refreshLiveHud();QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        uiChecks["loading_keeps_model_hypotheses_out_of_hud"]=!rosterInsights->isVisibleTo(right)&&rosterInsights->property("lastHtml").toString().contains(QStringLiteral("条件成长A"))&&rosterInsights->property("lastHtml").toString().contains(QStringLiteral("三级空装假设"))&&liveRoster->report().value("source_status")==QStringLiteral("加载")&&liveRoster->report().value("displayed_model_label")==QStringLiteral("阵容");
        updateRosterAdvice();uiChecks["synthetic_pregame_never_dispatches_paid_ai"]=!rosterDsAllowed()&&rosterAiDispatches==dispatches;
        game=savedGame;anonymousHistory=savedHistory;rosterAiPlan=savedPlan;loadingPreparation=savedPreparation;loadingComputedBasis=savedLoadingBasis;setDetailsExpanded(expanded);renderCoach();
    }
    void automaticPresentationUiChecks(){
        // Exercise the production render path with an injected monotonic clock.
        // These are synthetic native-window checks, never live-match evidence.
        const auto savedGame=game;const auto savedCoach=coachResult,savedHistory=anonymousHistory,savedRosterPlan=rosterAiPlan,savedContext=context;const auto savedModelState=modelState;
        const auto savedState=state;const auto savedPresentation=presentation;const auto savedPresentationState=presentationState;
        const auto savedManualPage=manualPresentationPage,savedManualContext=manualPresentationContext;
        const auto savedUntil=manualPresentationUntil;const bool savedExpanded=detailsExpanded,savedManualExpanded=manualPresentationExpanded,savedChecks=runningUiChecks,savedPinned=manualPresentationPinned,savedPending=manualPresentationPending;
        const auto savedComputedStrategy=computedStrategy,savedComputedSoftBasis=computedSoftBasis;
        const auto savedPublishedMs=publishedSnapshotMs,savedPublishedInputAge=publishedInputAgeMs;const double savedPublishedGameTime=publishedSnapshotGameTime;
        const bool polling=pollTimer.isActive();pollTimer.stop();
        runningUiChecks=false;presentation=CoachPresentation{};manualPresentationUntil=0;manualPresentationPinned=manualPresentationPending=false;anonymousHistory={};rosterAiPlan={};state.status="closed";
        auto fixture=makeCoachDemo(catalog);QJsonArray publicRoster;
        for(const auto &v:fixture.players){const auto p=v.toObject();publicRoster.append(QJsonObject{{"champion_id",p.value("champion_id")},{"team",p.value("team")==QStringLiteral("ORDER")?QStringLiteral("ALLY"):QStringLiteral("ENEMY")},{"self",p.value("self")}});}
        const auto contextFor=[&](const QString &stage,const QString &id=QStringLiteral("synthetic-presentation-ui")){return QJsonObject{{"active",true},{"context_id",id},{"stage",stage},{"roster",publicRoster}};};
        const auto shown=[&](qint64 tick,const QString &name,bool capture=false){
            presentationCheckTick=tick;renderCoach();right->body->layout()->activate();QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
            auto actual=placement;actual.right=right->geometry();const auto hud=automaticHint->report();
            presentationUiStates.append(QJsonObject{{"name",name},{"synthetic",true},{"tick_ms",tick},{"presentation",presentationState},{"actual_focus",presentationPage()},{"details_visible",details->isVisibleTo(right)},{"right_panel",right->report()},{"hud",hud},{"components",componentsLabel->report()},{"passive_hud",passiveHudReport()},{"roster_hud",liveRoster->report()},{"economy_summary",summary->report()},{"actual_windows_safe",overlayAvoidsProtectedAreas(actual)},{"purchase_input",coachResult.value("economy").toObject().value("next_purchase")},{"presentation_only_purchase_reference",coachResult.value("presentation_only_synthetic_purchase_reference")}});
            if(capture&&parser.isSet("snapshot")){const QFileInfo image(parser.value("snapshot"));snapshot(image.absolutePath()+"/"+image.completeBaseName()+"-auto-"+name+".png");}
            return visibleChildrenFit(right->body)&&hud.value("text_fits").toBool()&&hud.value("lines_do_not_overlap").toBool()&&overlayAvoidsProtectedAreas(actual);
        };
        game={};game.championId=1;game.gameMode=QStringLiteral("KIWI");game.observedPatch=catalog.patch;game.bench={516,51,25,223};game.benchKnown=true;game.matchContext=contextFor(QStringLiteral("champ_select"));game.matchContext["composition"]=analyzeMatchComposition(game.matchContext,catalog.champions);
        const bool selectFits=shown(0,QStringLiteral("champ-select"),true);
        uiChecks["auto_champ_select_shows_self_composition_and_bench"]=selectFits&&!liveRoster->isVisibleTo(right)&&phasePlate->isVisibleTo(right)&&phasePlate->report().value("stage")==QStringLiteral("selecting")&&champSelectView->isVisibleTo(right)&&champSelectView->report().value("content_fits").toBool()&&liveSummary.value("players").toArray().size()==10;
        game.matchContext["stage"]="loading";uiChecks["auto_loading_retains_phase_and_ten_seats"]=shown(100,QStringLiteral("loading"))&&!liveRoster->isVisibleTo(right)&&phasePlate->report().value("stage")==QStringLiteral("loading")&&rosterSummary->isVisibleTo(right);
        fixture.matchContext=contextFor(QStringLiteral("in_game"));game=fixture;coachResult=savedCoach;
        auto focusEconomy=coachResult.value("economy").toObject();
        focusEconomy["next_purchase"]=QJsonObject{{"item_id",3100},{"affordable",false},{"buyable_components",QJsonArray{}}};
        coachResult["economy"]=focusEconomy;coachResult["presentation_only_synthetic_purchase_reference"]=true;
        computedStrategy=modelBasis();computedSoftBasis=coachSoftModelBasis(game);publishedSnapshotMs=autoClock.elapsed();publishedInputAgeMs=0;publishedSnapshotGameTime=game.gameTime;
        const bool aliveFits=shown(200,QStringLiteral("alive-purchase"),true);const auto primary=automaticHint->report();
        uiChecks["alive_purchase_and_all_ten_same_frame"]=aliveFits&&!detailsExpanded&&automaticHint->isVisibleTo(right)&&rosterSummary->isVisibleTo(right)&&rosterSummary->report().value("roster_count").toInt()==10&&rosterSummary->report().value("all_content_visible").toBool();
        uiChecks["alive_action_stays_above_team"]=passiveHudReport().value("purchase_above_roster").toBool();
        uiChecks["living_hud_is_passive_without_dense_controls"]=livingCompactHud()&&right->findChildren<QTabWidget*>().isEmpty()&&!advice->isVisibleTo(right)&&!roster->isVisibleTo(right)&&right->windowFlags().testFlag(Qt::WindowTransparentForInput)==!parser.isSet("inspection-window");
        uiChecks["alive_no_rotation_after_8s"]=shown(8200,QStringLiteral("alive-roster"),true)&&automaticHint->report().value("title")==primary.value("title")&&automaticHint->report().value("item_id")==primary.value("item_id")&&rosterSummary->isVisibleTo(right);
        uiChecks["alive_no_rotation_after_32s"]=shown(32200,QStringLiteral("history-disabled"),true)&&automaticHint->report().value("title")==primary.value("title")&&!detailsExpanded&&presentationState.value("rotation_period_ms").toInt()==0;
        auto self=game.players[0].toObject();self["is_dead"]=true;game.players[0]=self;
        const auto visibleDeathTeam=[&]{
            if(liveRoster->isVisibleTo(right))return liveRoster->report().value("content_fits").toBool()&&liveSummary.value("players").toArray().size()==10;
            const auto strip=rosterSummary->report();
            return !right->property("passiveRosterDetailFits").toBool()&&rosterSummary->isVisibleTo(right)&&strip.value("roster_count").toInt()==10&&strip.value("all_content_visible").toBool()&&strip.value("text_fits").toBool();
        };
        uiChecks["death_purchase_and_roster_same_frame"]=shown(33200,QStringLiteral("dead-purchase"),true)&&detailsExpanded&&automaticHint->isVisibleTo(right)&&visibleDeathTeam()&&!advice->isVisibleTo(right);
        uiChecks["death_keeps_purchase_above_expanded_team"]=passiveHudReport().value("purchase_above_roster").toBool();
        uiChecks["death_no_rotation_after_8s"]=shown(41200,QStringLiteral("dead-roster"),true)&&detailsExpanded&&automaticHint->report().value("title")==primary.value("title")&&visibleDeathTeam();
        // Reserve the death comparison first in short lanes; a taller viewport
        // must still expose the full roster rather than always using the strip.
        const auto previousPlacement=placement;const auto previousViewport=viewport;
        viewport=QRect(viewport.topLeft(),QSize(1920,1440));placement=safeOverlayPlacement(viewport);right->setSafeLane(placement.right,true,true);
        uiChecks["death_full_roster_expands_when_reserved_corner_and_team_both_fit"]=shown(45200,QStringLiteral("dead-ample-space"))&&liveRoster->isVisibleTo(right)&&liveRoster->report().value("content_fits").toBool();
        placement=previousPlacement;viewport=previousViewport;right->setSafeLane(placement.right,true,true);
        shown(49200,QStringLiteral("dead-purchase-again"));
        self["is_dead"]=false;game.players[0]=self;
        uiChecks["respawn_restores_compact"]=shown(50200,QStringLiteral("respawn"),true)&&!detailsExpanded&&rosterSummary->isVisibleTo(right)&&right->height()<=compactHudHeight;
        uiChecks["no_gameplay_page_controls"]=right->findChildren<QTabWidget*>().isEmpty()&&!right->findChild<QWidget*>(QStringLiteral("detailsToggle"))&&!right->findChild<QWidget*>(QStringLiteral("automaticPresentation"))&&!advice->isVisibleTo(right)&&!roster->isVisibleTo(right);
        uiChecks["roster_summary_state_readable"]=rosterSummary->report().value("all_content_visible").toBool()&&rosterSummary->report().value("text_fits").toBool();
        const auto savedPurchaseContext=coachResultContext;
        game.source=GameState::Source::Live;
        game.received=QDateTime::currentDateTimeUtc();
        game.observedPatch=catalog.patch;
        int threatId=0;
        for(int i=0;i<game.players.size();++i){
            auto row=game.players.at(i).toObject();
            if(row.value("team")==self.value("team"))continue;
            QJsonArray kills{0,0,0,0,0,0,0,0,0,0};
            if(!threatId){threatId=row.value("champion_id").toInt();kills[0]=2;}
            row["kills_by_roster_row"]=kills;
            game.players[i]=row;
        }
        refreshLiveHud();
        coachResultContext=game.matchContext.value("context_id").toString();
        const QJsonObject target{{"champion_id",threatId},{"available",true},{"ranking_eligible",true},{"output_gain",.12}};
        const QJsonObject window{{"seconds",8},{"targets",QJsonArray{target}}};
        const QJsonObject selected{{"selected_item_id",3100},{"ranking_available",true},
            {"selected_sandbox",QJsonObject{{"windows",QJsonArray{window}}}}};
        coachResult={{"model_refresh",QJsonObject{{"soft_inputs_changed",false},{"snapshot_age_ms",0}}},
            {"recommendation_quality",QJsonObject{{"primary_purchase_visible",true}}},
            {"plan",QJsonObject{{"purchase_sandbox",selected}}}};
        const auto matchup=purchaseMatchup(3100,QStringLiteral("computed"));
        uiChecks["purchase_matchup_binds_current_item_to_observed_enemy"]=matchup.value("champion_id").toInt()==threatId
            &&matchup.value("kills_against_self").toInt()==2&&matchup.value("output_gain").toDouble()==.12;
        uiChecks["purchase_matchup_does_not_attach_gain_to_reference_or_other_item"]=
            !purchaseMatchup(3100,QStringLiteral("reference")).contains("output_gain")
            &&!purchaseMatchup(3157,QStringLiteral("computed")).contains("output_gain");
        coachResultContext=savedPurchaseContext;
        game.source=GameState::Source::Offline;game.matchContext["stage"]="in_game";
        uiChecks["stale_clears_passive_hud"]=shown(90000,QStringLiteral("stale-waiting"),true)&&!detailsExpanded&&presentationState.value("page")==QStringLiteral("waiting")&&liveSummary.value("players").toArray().isEmpty()&&!rosterSummary->isVisibleTo(right);
        runningUiChecks=true;game=savedGame;coachResult=savedCoach;anonymousHistory=savedHistory;rosterAiPlan=savedRosterPlan;state=savedState;presentation=savedPresentation;presentationState=savedPresentationState;presentationCheckTick=-1;
        computedStrategy=savedComputedStrategy;computedSoftBasis=savedComputedSoftBasis;publishedSnapshotMs=savedPublishedMs;publishedInputAgeMs=savedPublishedInputAge;publishedSnapshotGameTime=savedPublishedGameTime;context=savedContext;modelState=savedModelState;
        manualPresentationPage=savedManualPage;manualPresentationContext=savedManualContext;manualPresentationUntil=savedUntil;manualPresentationExpanded=savedManualExpanded;manualPresentationPinned=savedPinned;manualPresentationPending=savedPending;setDetailsExpanded(savedExpanded);runningUiChecks=savedChecks;if(polling)pollTimer.start();
    }
    void inspectDashboard(){
        if(parser.isSet("self-check-ui")&&!uiFixtureModelEvidence.value("self_check_complete").toBool()){
            QTimer::singleShot(100,this,[this]{inspectDashboard();});return;
        }
        if(!dashboard)return;
        dashboardInspectionRunning=true;const bool polling=pollTimer.isActive();pollTimer.stop();
        dashboard->selfCheck(parser.value("dashboard-output"));dashboardInspectionRunning=false;writeReport();
        if(dashboardInspectionExitPending)QTimer::singleShot(0,this,[this]{finish();});else if(polling)pollTimer.start();
    }
    void waitForUiFixture(int attempt=0){
        if(coachResult.isEmpty()&&attempt<30){QTimer::singleShot(100,this,[this,attempt]{waitForUiFixture(attempt+1);});return;}
        selfCheck();
    }
    void adaptivePurchaseUiChecks(){
        // These fixtures check JSON-to-view projection, never the combat model.
        const auto savedGame=game;const auto savedCoach=coachResult;const auto savedModelState=modelState;
        const bool savedExpanded=detailsExpanded;
        game=makeCoachDemo(catalog);game.source=GameState::Source::Replay;game.gameTime=300.;game.owned={3084,1031};modelState=QStringLiteral("ready");
        setDetailsExpanded(true);
        const auto row=[](int id,int cost,bool affordable,double damage,double health,double gain){
            return QJsonObject{{"item_id",id},{"remaining_cost",cost},{"affordable_now",affordable},{"ranking_eligible",true},
                {"score_gain",gain},{"metric_deltas",QJsonObject{{"effective_damage_dealt",damage},{"remaining_health",health},{"effective_healing_received",30.},{"permanent_health_growth",90.}}},
                {"event_admission_changes",QJsonArray{}},{"mechanism_admission_changed",false},
                {"transaction",QJsonObject{{"consume_item_ids",QJsonArray{1031}},{"inventory_after",QJsonArray{3084,id}},{"remaining_cost",cost},
                    {"growth_inventory_transition",QJsonObject{{"numeric_transition_verified",true}}}}}};
        };
        const QJsonArray candidates{row(3065,2800,false,180.,360.,.18),row(2502,1800,true,120.,240.,.15),row(3047,1100,true,60.,150.,.08)};
        const QJsonObject source{{"kind","caller_declared_scenario"},{"source_id","synthetic_ui_projection"},{"valid_from_game_time",290.},{"valid_until_game_time",330.}};
        const QJsonObject comparison{{"source",source},{"ranking_available",true},{"selection_available",true},{"complete_candidate_universe",true},{"focus_player_key","synthetic-adaptive-self"},
            {"scenario_sha256","synthetic-ui-not-native-evidence"},{"binding_sha256","synthetic-ui-not-authentication"},
            {"objective_basis",QJsonObject{{"damage_weight",1.},{"survival_weight",1.},{"death_penalty",1.}}},
            {"baseline_metrics",QJsonObject{{"effective_damage_dealt",500.},{"remaining_health",450.},{"permanent_health_growth",0.}}},{"candidates",candidates}};
        const QJsonObject sandbox{{"policy","explicit_chain_next_purchase_v1"},{"score_basis","explicit_chain_condition_comparison"},
            {"status","conditional_chain_purchase_available"},{"selected_item_id",2502},{"ranking_available",true},{"enumeration_complete",true},
            {"critical_unknowns",QJsonArray{}},{"candidates",candidates},{"explicit_chain_condition_comparison",comparison},
            {"health_basis",QJsonObject{{"current_health_used",true},{"native_initial_states",QJsonArray{QJsonObject{{"player_key","synthetic-adaptive-self"},{"initial_health",1000.},{"initially_dead",false}}}}}}};
        const QJsonObject basePlan{{"purchase_sandbox",sandbox},{"explicit_chain_condition_comparison",comparison},{"ranking_available",true},
            {"items",QJsonArray{QJsonObject{{"item_id",3084},{"name",catalog.itemName(3084)},{"owned",true}},QJsonObject{{"item_id",2502},{"name",catalog.itemName(2502)}}}}};
        const QJsonObject quality{{"ranking_available",true},{"primary_purchase_visible",true},{"summary",QStringLiteral("按已知条件估算")}};
        const auto planFor=[&](const QJsonObject &changed){auto plan=basePlan;plan["purchase_sandbox"]=changed;plan["explicit_chain_condition_comparison"]=changed.value("explicit_chain_condition_comparison");return plan;};
        const auto withCost=[&](const QJsonValue &cost,bool includeCost,const QJsonValue &transactionCost,bool includeTransactionCost){
            auto changed=sandbox;auto rows=candidates;auto selectedRow=rows[1].toObject();auto transaction=selectedRow.value("transaction").toObject();
            if(includeCost)selectedRow["remaining_cost"]=cost;else selectedRow.remove("remaining_cost");
            if(includeTransactionCost)transaction["remaining_cost"]=transactionCost;else transaction.remove("remaining_cost");
            selectedRow["transaction"]=transaction;rows[1]=selectedRow;changed["candidates"]=rows;
            auto compared=comparison;compared["candidates"]=rows;changed["explicit_chain_condition_comparison"]=compared;return changed;
        };
        QJsonArray states;
        const auto show=[&](const QString &name,const QJsonObject &plan,const QJsonObject &gate){
            // Deliberately retain an old support-route item to verify override.
            coachResult=QJsonObject{{"fresh",true},{"plan",plan},{"recommendation_quality",gate},
                {"economy",QJsonObject{{"next_purchase",QJsonObject{{"item_id",3100},{"affordable",true},{"buyable_components",QJsonArray{QJsonObject{{"item_id",1052},{"remaining_cost",400}}}}}}}},
                {"role_aware_recommendation",QJsonObject{{"prefer_source_route_for_display",true},{"next_purchase_reference",QJsonObject{{"item_id",3100},{"affordable",true}}}}}};
            renderCoach();advice->setPage(0);QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
            const auto hud=automaticHint->report();
            states.append(QJsonObject{{"name",name},{"scope","synthetic_json_to_view_only_not_native_combat_validation"},
                {"hud",hud},{"presentation",automaticHint->property("adaptivePurchasePresentation").value<QJsonObject>()},{"details_text",advice->toPlainText()},
                {"details",advice->report()},{"components",componentsLabel->report()}});
            return hud;
        };
        const auto blocked=[&](const QJsonObject &hud){return hud.value("item_id").toInt()==0&&!hud.value("purchase_ready").toBool()&&componentsLabel->report().value("components").toArray().isEmpty();};
        auto hud=show(QStringLiteral("ready"),basePlan,quality);
        uiChecks["adaptive_chain_shows_native_selected_not_first_row_or_legacy_route"]=hud.value("item_id").toInt()==2502&&!hud.value("purchase_ready").toBool()&&hud.value("status").toString().startsWith(QStringLiteral("条件比较"))&&advice->toPlainText().contains(catalog.itemName(3065))&&advice->toPlainText().contains(QStringLiteral("金币达到 2800G"));
        uiChecks["adaptive_chain_player_copy_uses_metric_changes_and_cost_without_internal_scores"]=advice->toPlainText().contains(QStringLiteral("有效伤害 +120"))&&advice->toPlainText().contains(QStringLiteral("尚需 1800G"))&&advice->toPlainText().contains(QStringLiteral("按给定动作估算"))&&!advice->toPlainText().contains(QStringLiteral("条件目标分"))&&!advice->toPlainText().contains(QStringLiteral("权重"))&&!advice->toPlainText().contains('%');
        uiChecks["adaptive_chain_initial_health_and_unknown_current_growth_are_distinct"]=advice->toPlainText().contains(QStringLiteral("本次采用的起始生命 1000"))&&advice->toPlainText().contains(QStringLiteral("绑定当前观测"))&&advice->toPlainText().contains(QStringLiteral("本局已观察成长 · —"))&&advice->toPlainText().contains(QStringLiteral("本次估算新增永久生命 +90"));
        uiChecks["adaptive_chain_report_keeps_score_semantics_and_no_probability"]=automaticHint->property("adaptivePurchasePresentation").value<QJsonObject>().value("selected_score_gain")==QJsonValue(.15)&&automaticHint->property("adaptivePurchasePresentation").value<QJsonObject>().value("actual_win_rate").isNull();
        if(parser.isSet("snapshot")){const QFileInfo file(parser.value("snapshot"));right->grab().save(file.absolutePath()+"/"+file.completeBaseName()+"-adaptive-ready-right.png");}
        auto changed=sandbox;auto selectionGate=comparison;selectionGate["selection_available"]=false;changed["explicit_chain_condition_comparison"]=selectionGate;
        hud=show(QStringLiteral("selection-false"),planFor(changed),quality);
        uiChecks["adaptive_chain_native_selection_false_never_buys"]=blocked(hud);
        changed=sandbox;selectionGate=comparison;selectionGate["selection_available"]=QJsonValue(QJsonValue::Null);changed["explicit_chain_condition_comparison"]=selectionGate;
        hud=show(QStringLiteral("selection-null"),planFor(changed),quality);
        uiChecks["adaptive_chain_native_selection_null_never_buys"]=blocked(hud);
        changed=sandbox;selectionGate=comparison;selectionGate.remove("selection_available");changed["explicit_chain_condition_comparison"]=selectionGate;
        hud=show(QStringLiteral("selection-missing"),planFor(changed),quality);
        uiChecks["adaptive_chain_native_selection_missing_never_buys"]=blocked(hud);
        changed=sandbox;changed["status"]="no_affordable_positive_chain_gain";
        hud=show(QStringLiteral("no-affordable-status-with-residual-selected"),planFor(changed),quality);
        uiChecks["adaptive_chain_no_purchase_status_vetoes_residual_selected_positive_row"]=blocked(hud);
        const auto costBlocked=[&](const QString &name,const QJsonObject &candidate){
            const auto shown=show(name,planFor(candidate),quality);return blocked(shown)&&automaticHint->property("adaptivePurchasePresentation").value<QJsonObject>().value("purchase_cost_verified")==QJsonValue(false)&&!advice->toPlainText().contains(QStringLiteral(" · 当前可买"));
        };
        uiChecks["adaptive_chain_missing_cost_never_buys"]=costBlocked(QStringLiteral("cost-missing"),withCost(QJsonValue(),false,1800,true));
        uiChecks["adaptive_chain_null_cost_never_buys"]=costBlocked(QStringLiteral("cost-null"),withCost(QJsonValue(QJsonValue::Null),true,1800,true));
        uiChecks["adaptive_chain_negative_cost_never_buys"]=costBlocked(QStringLiteral("cost-negative"),withCost(-1,true,-1,true));
        uiChecks["adaptive_chain_nan_cost_never_buys"]=costBlocked(QStringLiteral("cost-nan"),withCost(QJsonValue(std::numeric_limits<double>::quiet_NaN()),true,1800,true));
        uiChecks["adaptive_chain_infinite_cost_never_buys"]=costBlocked(QStringLiteral("cost-infinite"),withCost(QJsonValue(std::numeric_limits<double>::infinity()),true,1800,true));
        uiChecks["adaptive_chain_mismatched_transaction_cost_never_buys"]=costBlocked(QStringLiteral("cost-transaction-mismatch"),withCost(1800,true,1801,true));
        uiChecks["adaptive_chain_missing_transaction_cost_never_buys"]=costBlocked(QStringLiteral("cost-transaction-missing"),withCost(1800,true,QJsonValue(),false));
        uiChecks["adaptive_chain_null_transaction_cost_never_buys"]=costBlocked(QStringLiteral("cost-transaction-null"),withCost(1800,true,QJsonValue(QJsonValue::Null),true));
        auto nullGate=quality;nullGate["primary_purchase_visible"]=QJsonValue(QJsonValue::Null);
        hud=show(QStringLiteral("quality-primary-null"),basePlan,nullGate);
        uiChecks["adaptive_chain_null_primary_quality_never_replaces_legacy_clear"]=blocked(hud);
        nullGate=quality;nullGate["ranking_available"]=QJsonValue(QJsonValue::Null);
        hud=show(QStringLiteral("quality-ranking-null"),basePlan,nullGate);
        uiChecks["adaptive_chain_null_ranking_quality_never_buys"]=blocked(hud);
        auto nullPlan=basePlan;nullPlan["ranking_available"]=QJsonValue(QJsonValue::Null);
        hud=show(QStringLiteral("plan-ranking-null"),nullPlan,quality);
        uiChecks["adaptive_chain_null_plan_ranking_never_buys"]=blocked(hud);
        nullPlan=basePlan;nullPlan["primary_purchase_visible"]=QJsonValue(QJsonValue::Null);
        hud=show(QStringLiteral("plan-primary-null"),nullPlan,quality);
        uiChecks["adaptive_chain_null_plan_primary_never_buys"]=blocked(hud);
        auto optionalPlan=basePlan;optionalPlan.remove("ranking_available");
        hud=show(QStringLiteral("optional-veto-fields-absent"),optionalPlan,QJsonObject{});
        uiChecks["adaptive_chain_absent_optional_veto_fields_do_not_break_compatible_packages"]=hud.value("item_id").toInt()==2502&&!hud.value("purchase_ready").toBool();
        changed=sandbox;changed["critical_unknowns"]=QJsonArray{"growth_input_unobserved:heartsteel_stacks"};changed["unavailable_reason"]="critical_inputs_or_complete_chain_comparison_unavailable";
        hud=show(QStringLiteral("unknown"),planFor(changed),quality);
        uiChecks["adaptive_chain_unknown_clears_winner_and_old_components"]=blocked(hud)&&advice->toPlainText().contains(QStringLiteral("心之钢成长 —"))&&!advice->toPlainText().contains(QStringLiteral("当前选择"));
        if(parser.isSet("snapshot")){const QFileInfo file(parser.value("snapshot"));right->grab().save(file.absolutePath()+"/"+file.completeBaseName()+"-adaptive-unknown-right.png");}
        changed=sandbox;changed["enumeration_complete"]=false;auto incomplete=comparison;incomplete["complete_candidate_universe"]=false;changed["explicit_chain_condition_comparison"]=incomplete;
        hud=show(QStringLiteral("incomplete"),planFor(changed),quality);
        uiChecks["adaptive_chain_partial_universe_never_selects"]=blocked(hud)&&!automaticHint->property("adaptivePurchasePresentation").value<QJsonObject>().value("ranking_available").toBool();
        changed=sandbox;changed["status"]="interrupted";changed["ranking_available"]=false;changed["selected_item_id"]=0;
        changed["batches"]=QJsonArray{QJsonObject{{"ranking_available",true},{"selection_available",true},{"selected_item_id",3065},{"conditional_ranking",QJsonArray{3065}},{"candidates",candidates}}};
        hud=show(QStringLiteral("outer-cancelled-with-nested-winner"),planFor(changed),quality);
        uiChecks["adaptive_chain_outer_cancel_never_recovers_nested_old_winner"]=blocked(hud)&&automaticHint->property("adaptivePurchasePresentation").value<QJsonObject>().value("nested_batch_winners_read")==QJsonValue(false)&&!advice->toPlainText().contains(QStringLiteral("当前选择"));
        changed=sandbox;changed["status"]="interrupted";
        hud=show(QStringLiteral("status-cancelled-with-residual-top-flags"),planFor(changed),quality);
        uiChecks["adaptive_chain_interrupted_status_vetoes_residual_top_eligibility"]=blocked(hud);
        changed=sandbox;auto expired=comparison;auto expiredSource=source;expiredSource["valid_until_game_time"]=299.;expired["source"]=expiredSource;changed["explicit_chain_condition_comparison"]=expired;
        hud=show(QStringLiteral("expired"),planFor(changed),quality);
        uiChecks["adaptive_chain_expired_source_clears_winner"]=blocked(hud)&&!automaticHint->property("adaptivePurchasePresentation").value<QJsonObject>().value("source_lease_current").toBool();
        changed=sandbox;auto admissionRows=candidates;auto admission=admissionRows[1].toObject();admission["event_admission_changes"]=QJsonArray{QJsonObject{{"event_id","synthetic-changed-input"}}};admissionRows[1]=admission;changed["candidates"]=admissionRows;
        hud=show(QStringLiteral("event-admission-changed"),planFor(changed),quality);
        uiChecks["adaptive_chain_changed_admission_clears_winner"]=blocked(hud)&&advice->toPlainText().contains(QStringLiteral("当前不能直接比较"));
        changed=sandbox;auto budgetRows=candidates;for(int i=0;i<budgetRows.size();++i){auto candidate=budgetRows[i].toObject();candidate["affordable_now"]=false;budgetRows[i]=candidate;}
        changed["candidates"]=budgetRows;changed["selected_item_id"]=0;changed["status"]="no_affordable_positive_chain_gain";auto noSelection=comparison;noSelection["selection_available"]=false;noSelection["candidates"]=budgetRows;changed["explicit_chain_condition_comparison"]=noSelection;
        hud=show(QStringLiteral("no-affordable-purchase"),planFor(changed),quality);
        uiChecks["adaptive_chain_budget_is_condition_without_fake_low_value"]=blocked(hud)&&hud.value("title")==QStringLiteral("暂不购买")&&advice->toPlainText().contains(QStringLiteral("当前预算不足"));
        auto veto=quality;veto["ranking_available"]=false;veto["primary_purchase_visible"]=false;veto["summary"]=QStringLiteral("关键技能前提待核实");
        hud=show(QStringLiteral("quality-veto"),basePlan,veto);
        uiChecks["adaptive_chain_quality_veto_survives_legacy_support_route"]=blocked(hud)&&advice->toPlainText().contains(QStringLiteral("关键技能前提待核实"));
        changed=sandbox;auto nullRows=candidates;auto nullRow=nullRows[1].toObject();nullRow["score_gain"]=QJsonValue(QJsonValue::Null);auto nullMetrics=nullRow.value("metric_deltas").toObject();nullMetrics["effective_damage_dealt"]=QJsonValue(QJsonValue::Null);nullRow["metric_deltas"]=nullMetrics;nullRows[1]=nullRow;changed["candidates"]=nullRows;
        hud=show(QStringLiteral("null-numeric"),planFor(changed),quality);
        uiChecks["adaptive_chain_null_numeric_stays_unknown_not_zero"]=blocked(hud)&&advice->toPlainText().contains(QStringLiteral("有效伤害 —"));
        auto observedPlan=basePlan;observedPlan["equipment_growth_state"]=QJsonObject{{"entries",QJsonArray{QJsonObject{{"source","item"},{"id",3084},{"kind","permanent_health_while_item_held"},{"actual_observation_attested",true},{"value_supplied",true},{"value",640.}}}}};
        show(QStringLiteral("observed-growth-versus-estimated-growth"),observedPlan,quality);
        uiChecks["adaptive_chain_observed_growth_never_relabels_estimated_growth"]=advice->toPlainText().contains(QStringLiteral("额外生命 640"))&&advice->toPlainText().contains(QStringLiteral("本次估算新增永久生命 +90"));
        const QJsonObject legacyPlan{{"items",QJsonArray{QJsonObject{{"item_id",3100},{"name",catalog.itemName(3100)}}}}};
        hud=show(QStringLiteral("legacy-policy"),legacyPlan,quality);
        uiChecks["adaptive_chain_missing_new_policy_preserves_legacy_view"]=hud.value("item_id").toInt()==3100&&!automaticHint->property("adaptivePurchasePresentation").value<QJsonObject>().value("active").toBool()&&!advice->toPlainText().contains(QStringLiteral("本次估算变化"));
        bool fits=true;for(const auto &uiState:states){const auto item=uiState.toObject();fits&=item.value("hud").toObject().value("text_fits").toBool()&&item.value("hud").toObject().value("lines_do_not_overlap").toBool()&&item.value("details").toObject().value("text_fits").toBool();}
        uiChecks["adaptive_chain_fixture_text_fits_existing_hud_geometry"]=fits;
        automaticHint->setProperty("adaptivePurchaseUiStates",states);
        game=savedGame;coachResult=savedCoach;modelState=savedModelState;setDetailsExpanded(savedExpanded);renderCoach();
    }

    void assistanceSimulation(){
        // Isolated native-widget scenarios use explicitly synthetic live-shaped
        // input. No live client, provider, capture process or paid AI is started.
        runningUiChecks=true;pollTimer.stop();Kda::setReducedMotion(true);
        const QString output=QDir(parser.value("dashboard-output")).absolutePath();QDir().mkpath(output);
        QJsonArray checks,frames,transactions;int failures=0;double maximumRenderMs=0;
        auto check=[&](const QString &name,bool passed){checks.append(QJsonObject{{"name",name},{"passed",passed}});failures+=!passed;};
        const auto array=[](const QList<int> &values){QJsonArray a;for(int id:values)a.append(id);return a;};
        auto base=makeCoachDemo(catalog);base.source=GameState::Source::Live;base.championId=22;base.selected={};base.offers={};base.queueId=2400;
        base.abilities={};const auto ranks=referenceCombatRanks(*catalog.combat,22,11);for(const auto &slot:{QStringLiteral("Q"),QStringLiteral("W"),QStringLiteral("E"),QStringLiteral("R")})base.abilities[slot]=QJsonObject{{"abilityLevel",ranks.value(slot)}};
        auto second=base.players[1].toObject();second["champion_id"]=117;base.players[1]=second;
        const auto contextA=QStringLiteral("synthetic-assistance-A"),contextB=QStringLiteral("synthetic-assistance-B");
        auto setState=[&](const QList<int> &owned,double gold,bool goldKnown,bool complete,const QString &contextId){
            game=base;game.owned=owned;game.gold=gold;game.goldKnown=goldKnown;game.gameTime=500;game.received=QDateTime::currentDateTimeUtc();
            game.matchContext={{"context_id",contextId},{"active",true},{"stage","in_game"}};
            for(int i=0;i<game.players.size();++i){auto p=game.players[i].toObject();p["augment_ids"]=QJsonArray{};p["augments_complete"]=complete;if(p.value("self").toBool()){p["champion_id"]=22;p["items"]=array(owned);p["level"]=11;}game.players[i]=p;}
            state=PanelState(false);state.bindLiveMatch(contextId,offerMatchRefForGameId(QStringLiteral("123456789")));state.status=QStringLiteral("closed");
            coachResult={};cardDecision={};computedCardBasis.clear();modelState=QStringLiteral("waiting");gameForeground=true;hiddenByUser=false;selectionActive=false;
        };
        auto calculate=[&]{CoachSimulationOptions local;local.currentPurchaseOnly=true;local.work=std::make_shared<RecommendationWork>(2800);coachResult=buildCoachSnapshot(catalog,game,QStringLiteral("成长联动"),local);annotateCoachRecommendationQuality(catalog,game,coachResult);coachResultContext=game.matchContext.value("context_id").toString();modelState=QStringLiteral("ready");game.received=QDateTime::currentDateTimeUtc();};
        auto frame=[&](const QString &name){
            QElapsedTimer elapsed;elapsed.start();renderCoach();QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
            const double ms=double(elapsed.nsecsElapsed())/1e6;maximumRenderMs=std::max(maximumRenderMs,ms);
            QJsonArray hints;for(int i=0;i<cardHints.size();++i){hints.append(cardHints[i]->report());if(cardHints[i]->isVisible())cardHints[i]->grab().save(output+QStringLiteral("/%1-card-%2.png").arg(name).arg(i));}
            if(right->isVisible())right->grab().save(output+QStringLiteral("/%1-hud.png").arg(name));
            QJsonObject row{{"name",name},{"render_ms",ms},{"game_time",game.gameTime},{"owned_items",array(game.owned)},{"gold",game.goldKnown?QJsonValue(game.gold):QJsonValue::Null},{"input_readiness",coachPurchaseInputReadiness(catalog,game)},{"model_status",coachResult.value("status")},{"purchase_status",coachResult.value("plan").toObject().value("purchase_sandbox").toObject().value("status")},{"source_reference",purchaseFacts.value("source_reference")},{"hud",automaticHint->report()},{"components",componentsLabel->report()},{"purchase_checklist",checklistState},{"purchase_checklist_view",purchaseList->report()},{"hud_phase",phasePlate->report()},{"selection_active",selectionActive},{"cards",hints},{"right_visible",right->isVisible()},{"death_tip_visible",deathTips->isVisible()}};
            frames.append(row);return row;
        };
        viewport=QRect(0,0,1549,982);placement=safeOverlayPlacement(viewport);right->setSafeLane(placement.right,true,true);right->setPresentedVisible(true);setDetailsExpanded(false);
        setState({1038},500,true,false,contextA);calculate();auto row=frame(QStringLiteral("01-missing-augments"));
        check("unknown_augments_leave_a_source_target_and_component",row.value("hud").toObject().value("item_id").toInt()>0&&!row.value("components").toObject().value("components").toArray().isEmpty());
        const auto quietLayouts=cardLayoutBuilds,quietHints=cardHintBuilds;
        for(int tick=0;tick<32;++tick)renderCardHints();
        check("stable_gameplay_ticks_reuse_geometry_and_skip_unshown_card_work",cardLayoutBuilds==quietLayouts&&cardHintBuilds==quietHints);
        const int referenceTarget=row.value("hud").toObject().value("item_id").toInt();
        const auto referenceStatus=row.value("hud").toObject().value("status").toString();
        check("material_credit_keeps_the_public_route_label_and_price_visible",referenceStatus.startsWith(QStringLiteral("资料"))&&referenceStatus.endsWith(QChar(u'G'))&&!row.value("hud").toObject().value("purchase_ready").toBool());
        const auto referenceCoach=coachResult;
        coachResult["recommendation_quality"]=QJsonObject{{"primary_purchase_visible",true}};
        coachResult["economy"]=QJsonObject{{"next_purchase",QJsonObject{{"item_id",referenceTarget},{"affordable",true}}}};
        const auto changedSource=frame(QStringLiteral("01a-same-target-computed"));
        const auto computedList=changedSource.value("purchase_checklist").toObject();
        check("same_item_source_change_removes_cached_public_followups",computedList.value("available").toBool()&&computedList.value("display_source")==QStringLiteral("computed")&&computedList.value("source_url").toString().isEmpty()&&purchaseFacts.isEmpty());
        coachResult=referenceCoach;
        coachResult["plan"]=QJsonObject{{"purchase_sandbox",QJsonObject{{"policy","explicit_chain_next_purchase_v1"},{"status","input_required"},{"ranking_available",false},{"selected_item_id",0}}}};
        auto unavailableRow=frame(QStringLiteral("01b-unavailable-adaptive-local-reference"));const auto unavailablePresentation=automaticHint->property("adaptivePurchasePresentation").value<QJsonObject>();
        check("active_but_unavailable_adaptive_keeps_local_route_without_stealing_engine_authority",unavailableRow.value("hud").toObject().value("item_id").toInt()>0&&unavailablePresentation.value("active").toBool()&&!unavailablePresentation.value("ranking_available").toBool()&&!unavailablePresentation.value("displayed_in_action_strip").toBool()&&unavailablePresentation.value("displayed_item_id").toInt()==0);
        calculate();
        const int initialTarget=row.value("hud").toObject().value("item_id").toInt();bool finished=false,legalSteps=true,stableTarget=true;
        for(int wave=0;wave<10&&!finished;++wave){const auto reference=sourcePurchaseReference(catalog,game);if(!reference.value("available").toBool()){legalSteps=false;break;}const int target=reference.value("target_item_id").toInt();stableTarget&=target==initialTarget;
            if(!reference.value("within_catalog_budget").toBool()){game.gold+=500;game.gameTime+=80;game.received=QDateTime::currentDateTimeUtc();}
            const auto updated=sourcePurchaseReference(catalog,game);if(!updated.value("within_catalog_budget").toBool())continue;
            const auto visibleSteps=purchaseChecklist(catalog,game,updated.value("target_item_id").toInt(),updated).value("rows").toArray();if(visibleSteps.isEmpty()){legalSteps=false;break;}const int action=visibleSteps[0].toObject().value("item_id").toInt();const auto quote=planInventoryRecipeReference(catalog,22,11,game.owned,action);const int cost=quote.value("remaining_cost").toInt();const auto before=game.owned;const double goldBefore=game.gold;
            if(!quote.value("legal").toBool()||cost<=0||cost>game.gold){legalSteps=false;break;}
            game.owned.clear();for(const auto &id:quote.value("inventory_after").toArray())game.owned.append(id.toInt());game.gold-=cost;finished=game.owned.contains(initialTarget);
            auto p=game.players[0].toObject();p["items"]=array(game.owned);game.players[0]=p;game.received=QDateTime::currentDateTimeUtc();calculate();row=frame(QStringLiteral("02-buy-wave-%1").arg(wave));
            legalSteps&=game.gold==goldBefore-cost&&game.owned.size()<=6;
            transactions.append(QJsonObject{{"item_id",action},{"gold_before",goldBefore},{"gold_after",game.gold},{"inventory_before",array(before)},{"quote",quote}});
        }
        check("component_sequence_reaches_original_target_without_route_flip",legalSteps&&stableTarget&&finished);
        check("completed_item_is_not_suggested_again",finished&&row.value("hud").toObject().value("item_id").toInt()!=initialTarget);
        setState({3032,1043,1043},1100,true,false,contextA);calculate();row=frame(QStringLiteral("03-double-recurve-bow"));check("two_observed_recurve_bows_keep_recipe_reference",row.value("hud").toObject().value("item_id").toInt()>0);
        setState({1038},500,false,false,contextA);calculate();row=frame(QStringLiteral("04-unknown-gold"));check("unknown_gold_never_shows_affordable_components",row.value("components").toObject().value("components").toArray().isEmpty()&&!row.value("hud").toObject().value("purchase_ready").toBool());
        check("unknown_gold_keeps_the_target_and_explicit_status",row.value("hud").toObject().value("item_id").toInt()>0
            &&row.value("hud").toObject().value("status").toString().contains(QStringLiteral("金币待同步")));
        setState({1038},2600,true,true,contextA);calculate();row=frame(QStringLiteral("05-complete-inputs-diagnostic-only"));
        check("completed_but_unranked_model_keeps_a_source_reference",row.value("hud").toObject().value("item_id").toInt()>0&&!row.value("hud").toObject().value("purchase_ready").toBool()&&coachResult.value("economy").toObject().value("next_purchase").toObject().isEmpty());
        CoachSimulationOptions shortWork;shortWork.currentPurchaseOnly=true;shortWork.work=std::make_shared<RecommendationWork>(1);coachResult=buildCoachSnapshot(catalog,game,QStringLiteral("成长联动"),shortWork);modelState=QStringLiteral("time_budget_exceeded");game.received=QDateTime::currentDateTimeUtc();row=frame(QStringLiteral("05b-interrupted-comparison"));check("interrupted_model_keeps_reference_without_a_computed_buy",coachResult.value("status")==QStringLiteral("interrupted")&&coachResult.value("economy").toObject().value("next_purchase").toObject().isEmpty()&&row.value("hud").toObject().value("item_id").toInt()>0&&!row.value("hud").toObject().value("purchase_ready").toBool());
        setState({3032,3091,3153,3118,4005,3165},3000,true,false,contextA);calculate();row=frame(QStringLiteral("06-six-finished-items"));check("full_inventory_does_not_repeat_a_stale_buy",row.value("hud").toObject().value("item_id").toInt()==0&&row.value("components").toObject().value("components").toArray().isEmpty());
        setState({1038},500,true,false,contextA);calculate();frame(QStringLiteral("07-before-expiry"));game.received=QDateTime::currentDateTimeUtc().addSecs(-5);row=frame(QStringLiteral("08-expired"));check("expired_input_clears_displayed_item_and_components",row.value("hud").toObject().value("item_id").toInt()==0&&row.value("components").toObject().value("components").toArray().isEmpty());
        setState({1052},900,true,false,contextB);game.championId=25;auto self=game.players[0].toObject();self["champion_id"]=25;game.players[0]=self;calculate();row=frame(QStringLiteral("09-new-match-morgana"));check("new_match_reference_is_bound_to_new_context",row.value("source_reference").toObject().value("context_id")==contextB&&row.value("hud").toObject().value("item_id").toInt()>0);
        game.observedPatch=QStringLiteral("16.22");row=frame(QStringLiteral("10-current-client-reference"));
        check("client_update_keeps_equipment_and_component_reference",row.value("hud").toObject().value("item_id").toInt()>0&&row.value("purchase_checklist").toObject().value("available").toBool());
        check("client_update_preserves_source_version_without_blocking_rules",!modelPatchMatches(catalog,game)&&modelRulesUsable(catalog,game)&&modelRuleBasis(catalog,game).value("cross_version_estimate").toBool());
        game.observedPatch=QStringLiteral("0.0");row=frame(QStringLiteral("10-unknown-client-reference"));check("unknown_patch_keeps_equipment_without_version_block",row.value("hud").toObject().value("item_id").toInt()>0&&!row.value("hud").toObject().value("title").toString().contains(QStringLiteral("版本")));
        setState({1038},500,true,false,contextA);calculate();self=game.players[0].toObject();self["is_dead"]=true;game.players[0]=self;setDetailsExpanded(true);row=frame(QStringLiteral("11-dead-shopping"));check("death_help_does_not_spawn_the_old_obstructing_window",!row.value("death_tip_visible").toBool()&&!right->geometry().intersects(shopReferenceArea(viewport)));setDetailsExpanded(false);
        const QList<int> offered{1028,1036,1038,1047,1074,1077,1087,1098,1112,1113,1151,1206,1301,1305,1373,1375,1386,1390,2096,2116,3116,3131};
        for(int group=0;group<offered.size();group+=3){setState({1038},500,true,false,contextA);game.offers={};state.status=QStringLiteral("open");state.round=2;state.observed=QDateTime::currentDateTimeUtc();for(int j=0;j<3;++j){const int id=offered[(group+j)%offered.size()];game.offers.append(id);state.candidates.append(QJsonObject{{"augment_id",id},{"augment_name",catalog.augments.value(id).value("name_zh_cn")},{"candidate_index",j}});}
            row=frame(QStringLiteral("12-cold-offers-%1").arg(group));bool useful=selectionActive,unranked=true;for(const auto &v:row.value("cards").toArray()){const auto hint=v.toObject();const auto mode=hint.value("comparison_state");useful&=(mode==QStringLiteral("reference")||mode==QStringLiteral("advisory"))&&hint.value("explanation_visible").toBool()&&!hint.value("displayed_metric").toString().isEmpty();unranked&=!hint.value("numeric_gain_available").toBool()&&!hint.value("recommended_material").toBool();}
            check(QStringLiteral("new_offers_%1_have_immediate_source_conditions_without_a_finished_model").arg(group),useful&&unranked);check(QStringLiteral("selection_%1_yields_the_main_hud").arg(group),!right->isVisible());
        }
        game.offers={1375,1113,2116};state.candidates={};for(int id:game.offers)state.candidates.append(QJsonObject{{"augment_id",id},{"augment_name",catalog.augments.value(id).value("name_zh_cn")},{"candidate_index",state.candidates.size()}});
        QElapsedTimer cardClock;cardClock.start();cardDecision=buildAugmentDecision(catalog,game,{},QStringLiteral("成长联动"),{},true);const auto cardMs=cardClock.elapsed();cardDecision["mechanic_cache_validity"]=coachCardMechanicValidity(catalog,game);computedCardBasis=cardBasis();game.received=QDateTime::currentDateTimeUtc();
        auto decisions=cardDecision.value("offers").toArray();
        for(int i=0;i<decisions.size();++i){
            auto candidate=decisions[i].toObject();
            if(candidate.value("augment_id")!=QJsonValue(1375))continue;
            candidate["historical_loadout_reference"]=QJsonObject{{"item_id",1038},{"augment_id",1375},
                {"exposed",QJsonObject{{"average_damage_per_minute",540},{"damage_samples",8},{"matches",10}}},
                {"control",QJsonObject{{"average_damage_per_minute",470},{"damage_samples",7},{"matches",12}}}};
            decisions[i]=candidate;break;
        }
        cardDecision["offers"]=decisions;row=frame(QStringLiteral("13-warm-card-model"));
        check("unknown_selection_returns_before_expensive_counterfactual_card_search",cardMs<500&&cardDecision.value("numeric_calculation_skipped").toBool());
        bool warmUseful=selectionActive;for(const auto &v:row.value("cards").toArray()){const auto hint=v.toObject();const auto mode=hint.value("comparison_state");warmUseful&=(mode==QStringLiteral("reference")||mode==QStringLiteral("advisory"))&&hint.value("explanation_visible").toBool()&&!hint.value("numeric_gain_available").toBool();}check("finished_card_model_preserves_readable_mechanic_conditions",warmUseful);
        const auto historyHint=row.value("cards").toArray()[0].toObject();
        check("history_is_secondary_to_the_current_rule_and_uses_metric_samples",historyHint.value("explanation_visible").toBool()
            &&historyHint.value("historical_comparison_visible").toBool()&&historyHint.value("displayed_historical_comparison")==QStringLiteral("历史分伤 540/470 · 8/7局"));
        const auto completeCandidates=state.candidates;
        state.candidates.removeAt(1);game.offers.clear();row=frame(QStringLiteral("13b-short-reroll"));
        bool noOldCards=true;for(const auto &v:row.value("cards").toArray())noOldCards&=!v.toObject().value("visible").toBool();
        check("partial_offer_revokes_every_old_slot_and_shows_pending_count",noOldCards&&!selectionActive&&incompleteOfferCount==2
            &&right->isVisible()&&automaticHint->isVisibleTo(right)&&!intersectsSelection(right->geometry())
            &&row.value("hud").toObject().value("title")==QStringLiteral("海克斯 2/3")&&runtimePresentationSnapshot().value("card_hints").toArray().isEmpty());
        state.candidates=completeCandidates;game.offers={1036,1113,2116};
        auto rerolled=state.candidates[0].toObject();rerolled["augment_id"]=1036;state.candidates[0]=rerolled;
        row=frame(QStringLiteral("14-reroll"));check("reroll_replaces_old_card_immediately",selectionActive&&incompleteOfferCount<0&&row.value("cards").toArray()[0].toObject().value("augment_id").toInt()==1036&&row.value("cards").toArray()[0].toObject().value("metric")==QStringLiteral("普攻叠灼烧"));
        check("reroll_does_not_display_the_previous_historical_pair",row.value("cards").toArray()[0].toObject().value("historical_comparison").toString().isEmpty());
        const auto originalHudLayout=hudLayout;
        const QRect customHint(viewport.x()+30,viewport.y()+80,310,84);
        hudLayout.set("card1",customHint,viewport);renderCardHints();
        check("custom_card_position_and_size_reach_the_actual_passive_window",selectionActive&&cardHints[0]->isVisible()&&cardHints[0]->geometry()==customHint);
        const bool previouslyHidden=hiddenByUser;
        hiddenByUser=true;
        const auto hiddenHints=cardHintBuilds,hiddenLayouts=cardLayoutBuilds;
        for(int tick=0;tick<32;++tick)renderCardHints();
        check("a_hidden_offer_skips_card_labels_but_can_reappear_immediately",!selectionActive&&cardLayoutBuilds==hiddenLayouts&&cardHintBuilds==hiddenHints);
        hiddenByUser=previouslyHidden;renderCardHints();
        check("revealing_an_offer_reuses_geometry_and_builds_current_labels",selectionActive&&cardLayoutBuilds==hiddenLayouts&&cardHintBuilds==hiddenHints+1);
        hudEditorOpen=true;renderCardHints();bool overlaysHidden=!right->isVisible()&&!combatCorner->isVisible()&&!deathTips->isVisible();for(auto hint:cardHints)overlaysHidden&=!hint->isVisible();
        check("fullscreen_layout_editor_suppresses_every_live_overlay",overlaysHidden);
        hudEditorOpen=false;hudLayout=originalHudLayout;renderCardHints();
        game.observedPatch=QStringLiteral("0.0");row=frame(QStringLiteral("15-wrong-patch-offers"));bool conditionsVisible=selectionActive;for(const auto &v:row.value("cards").toArray()){const auto h=v.toObject();const auto comparison=h.value("comparison_state");conditionsVisible&=h.value("visible").toBool()&&h.value("explanation_visible").toBool()&&(comparison==QStringLiteral("reference")||comparison==QStringLiteral("advisory"))&&!h.value("numeric_gain_available").toBool();}check("different_patch_preserves_rule_conditions_without_stale_numeric_claims",conditionsVisible);
        setState({1038},500,true,false,contextA);calculate();frame(QStringLiteral("16-before-game-end"));game.phase=QStringLiteral("EndOfGame");row=frame(QStringLiteral("17-ended-context"));check("phase_change_revokes_cached_purchase_reference",row.value("hud").toObject().value("item_id").toInt()==0&&row.value("purchase_checklist").toObject().value("rows").toArray().isEmpty());
        check("ending_plate_replaces_buy_list",row.value("hud_phase").toObject().value("stage")==QStringLiteral("ending")&&!automaticHint->isVisibleTo(right));endHudUntil=autoClock.elapsed()-1;row=frame(QStringLiteral("17b-ended-retired"));check("ended_hud_does_not_reappear",!right->isVisible());
        setState({1038},500,true,false,contextA);calculate();frame(QStringLiteral("18-before-roster-mismatch"));self=game.players[0].toObject();self["level"]=10;game.players[0]=self;row=frame(QStringLiteral("19-self-level-mismatch"));check("self_level_change_revokes_cached_material_credit",row.value("hud").toObject().value("item_id").toInt()==0);
        bool listSync=true;for(const auto &f:frames){const auto v=f.toObject();const auto list=v.value("purchase_checklist").toObject();if(!list.value("available").toBool())continue;const auto rows=list.value("rows").toArray();listSync&=!rows.isEmpty()&&rows[0].toObject().value("inventory_before")==v.value("owned_items")&&v.value("purchase_checklist_view").toObject().value("text_fits").toBool()&&!list.value("actual_shop_prices_verified").toBool();}check("purchase_list_tracks_observed_inventory_every_frame",listSync);
        bool geometry=true;for(const auto &f:frames){const auto v=f.toObject();if(v.value("right_visible").toBool())geometry&=v.value("hud").toObject().value("text_fits").toBool()&&v.value("hud").toObject().value("lines_do_not_overlap").toBool();if(v.value("selection_active").toBool())for(const auto &h:v.value("cards").toArray())geometry&=h.toObject().value("metric_fits").toBool()&&h.toObject().value("text_rects_non_overlapping").toBool();}check("all_visible_simulated_hints_fit_without_overlapping_text",geometry);
        setState({1038},2600,true,false,contextA);calculate();
        const QRect customCoach(viewport.x()+420,viewport.y()+180,360,350);
        hudLayout.set("coach",customCoach,viewport);dock(true);row=frame(QStringLiteral("20-custom-coach-layout"));
        check("custom_main_hud_remains_visible_outside_default_safe_lane",right->isVisible()&&right->pos()==customCoach.topLeft()&&right->width()==customCoach.width()&&right->height()<=customCoach.height());
        game.gold=300;game.received=QDateTime::currentDateTimeUtc();renderCoach();renderCardHints();
        check("live_content_refresh_does_not_overwrite_custom_position_or_width",right->pos()==customCoach.topLeft()&&right->width()==360);
        hudLayout=originalHudLayout;dock(true);renderCoach();
        check("restoring_default_returns_runtime_to_automatic_safe_lane",right->width()==std::min(232,placement.right.width())&&placement.right.contains(right->geometry())&&right->report().value("anchor")==QStringLiteral("bottom-left"));
        const QJsonObject report{{"scope","isolated_native_Qt_scenarios_with_synthetic_live_shaped_inputs_not_actual_match_acceptance"},{"paid_ai_requests",0},{"live_capture_started",false},{"synthetic_income_per_wave",500},{"unknown_selection_card_compute_ms",cardMs},{"maximum_render_ms",maximumRenderMs},{"checks",checks},{"failures",failures},{"passed",failures==0},{"transactions",transactions},{"frames",frames}};
        QSaveFile file(output+QStringLiteral("/simulation.json"));if(file.open(QIODevice::WriteOnly)){file.write(QJsonDocument(report).toJson(QJsonDocument::Indented));file.commit();}
        std::fprintf(stdout,"assistance simulation: %lld checks, %d failures; %lld frames\n",qint64(checks.size()),failures,qint64(frames.size()));finish();QCoreApplication::exit(failures?1:0);
    }

    void selfCheck(){
        // The model is asynchronous and can take longer than the first UI
        // check timer. Wait for observable completion, with a finite limit.
        if(coachResult.isEmpty()&&worker.isRunning()&&autoClock.elapsed()<8000){QTimer::singleShot(100,this,[this]{selfCheck();});return;}
        // Synthetic phase fixtures must not be sent through the real model
        // scheduler by a maintenance timeout during processEvents().
        const bool pollingBeforeCheck=pollTimer.isActive();pollTimer.stop();
        runningUiChecks=true;
        uiChecks["ui_fixture_model_ready"]=!coachResult.isEmpty();
        const auto baselinePlan=coachResult.value("plan").toObject(),baselineSimulation=coachResult.value("simulation").toObject();const auto baselineCapturedAt=autoClock.elapsed();
        const bool baselineRetained=!coachResult.isEmpty()&&CoachRefreshPolicy::retain(publishedSnapshotMs,baselineCapturedAt+publishedInputAgeMs,computedStrategy==modelBasis(),game.fresh(),modelRulesUsable(catalog,game),publishedSnapshotGameTime,game.gameTime,baselinePlan.value("mechanic_valid_until").toDouble(-1));
        uiFixtureModelEvidence=QJsonObject{{"source","completed_local_worker_before_synthetic_ui_mutations"},{"captured_at_elapsed_ms",baselineCapturedAt},{"snapshot_age_ms",baselineCapturedAt-publishedSnapshotMs+publishedInputAgeMs},{"worker_completions",modelCompletions},{"structural_basis_matches",computedStrategy==modelBasis()},{"retention_policy_passed_at_capture",baselineRetained},{"self_check_complete",false},{"simulation",QJsonObject{{"available",baselineSimulation.value("available")},{"match_win_rate",baselineSimulation.value("match_win_rate")}}}};
        uiChecks["ui_fixture_baseline_was_current_completed_worker"]=baselineRetained&&modelCompletions>0&&baselineSimulation.value("available").toBool()&&baselineSimulation.value("match_win_rate").isNull();
        if(parser.isSet("snapshot")){const QFileInfo output(parser.value("snapshot"));snapshot(output.absolutePath()+"/"+output.completeBaseName()+"-baseline.png");}
        const bool beganExpanded=detailsExpanded;
        uiChecks["default_action_card_is_compact"]=parser.isSet("query-summoner")||(!detailsExpanded&&right->height()<=compactHudHeight);
        uiChecks["paid_ai_requires_explicit_diagnostic_mode"]=diagnosticDsEnabled()||(autoAdvice.dispatches()==0&&context.isEmpty()&&allowedItems.isEmpty()&&allowedAugments.isEmpty());
        auto contentFits=[this]{return right->collapsed()||visibleChildrenFit(right->body);};
        uiChecks["compact_content_inside_body"]=contentFits();
        const bool folded=right->collapsed();right->setCollapsed(!folded);uiChecks["collapse_changes_height"]=right->height()==OverlayMetrics::foldedHeight||!right->collapsed();right->setCollapsed(folded);
        right->setWindowOpacity(.75);uiChecks["opacity_changes"]=std::abs(right->configuredOpacity()-.75)<.01&&std::abs(right->windowOpacity()-.75*right->report().value("visibility_motion").toObject().value("progress").toDouble())<.01;right->setWindowOpacity(.88);
        bool executionControl=false;for(auto widget:right->findChildren<QWidget*>())executionControl|=widget->inherits("QSlider");uiChecks["unvalidated_execution_control_absent"]=!executionControl;
        setDetailsExpanded(true);QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        uiChecks["details_expand_only_on_request"]=details->isVisibleTo(right)&&right->height()>180;
        const auto purchaseQuality=coachResult.value("recommendation_quality").toObject();
        uiChecks["purchase_limit_displays_actual_current_reason"]=!purchaseQuality.value("summary").toString().isEmpty()&&advice->toPlainText().contains(purchaseQuality.value("summary").toString())&&!advice->toPlainText().contains(QStringLiteral("队友增益尚未进入评分，暂不提供出装排序"));
        {
            const auto savedGame=game;const auto savedCoach=coachResult;const auto savedModelState=modelState;
            game=makeCoachDemo(catalog);game.source=GameState::Source::Replay;game.owned.clear();game.effectCounters.clear();modelState=QStringLiteral("ready");
            QJsonObject candidatePlan{{"growth_conditioned_candidates",true},{"reference_only",true},{"ranking_available",false},{"items",QJsonArray{}},
                {"growth_objective",QJsonObject{{"summary",QStringLiteral("成长入口已知，层数待核实")},{"equipment_growth_inputs",QJsonArray{QJsonObject{{"item_id",3084},{"required_inputs",QJsonArray{"heartsteel_stacks"}},{"supplied_baseline_inputs",QJsonObject{}}}}},{"unknowns",QJsonArray{"growth_input_unobserved:heartsteel_stacks","future_stacks_hits_kills_and_time_to_growth_not_predicted"}}}},
                {"candidate_loadouts",QJsonArray{QJsonObject{{"item_ids",QJsonArray{3084,3068,2502,3075,3065,3047}},{"retained_for_metrics",QJsonArray{"mixed_effective_hp"}}},QJsonObject{{"item_ids",QJsonArray{3084,3110,3065,3083,3190,3158}},{"retained_for_metrics",QJsonArray{"skill_cycle_capacity"}}}}}};
            coachResult=QJsonObject{{"fresh",true},{"plan",candidatePlan},{"economy",QJsonObject{{"next_purchase",QJsonObject{{"item_id",3100},{"affordable",true},{"buyable_components",QJsonArray{QJsonObject{{"item_id",1052},{"remaining_cost",400}}}}}}}},
                {"recommendation_quality",QJsonObject{{"primary_purchase_visible",true},{"ranking_available",true}}},{"role_aware_recommendation",QJsonObject{{"prefer_source_route_for_display",true},{"next_purchase_reference",QJsonObject{{"item_id",3100},{"affordable",true}}}}}};
            renderCoach();const auto candidateHud=automaticHint->report();
            uiChecks["growth_candidates_override_old_support_route_and_purchase_instruction"]=candidateHud.value("title")==QStringLiteral("成长联动候选")&&candidateHud.value("item_id").toInt()==0&&!candidateHud.value("purchase_ready").toBool()&&componentsLabel->report().value("components").toArray().isEmpty()&&!automaticHint->text().contains(QStringLiteral("治疗与保护"));
            uiChecks["empty_inventory_still_shows_parallel_growth_loadouts_and_missing_stacks"]=advice->toPlainText().contains(QStringLiteral("成长联动候选"))&&advice->toPlainText().contains(catalog.itemName(3084))&&advice->toPlainText().contains(QStringLiteral("混合有效生命"))&&advice->toPlainText().contains(QStringLiteral("技能循环容量"))&&advice->toPlainText().contains(QStringLiteral("心钢层数未自动采集，当前未知"))&&advice->toPlainText().contains(QStringLiteral("并列展示，未排序"))&&!advice->toPlainText().contains(QStringLiteral("配装已齐"));
            candidatePlan["items"]=QJsonArray{QJsonObject{{"item_id",3084},{"name",catalog.itemName(3084)},{"growth_prerequisite",true},{"owned",false}}};coachResult["plan"]=candidatePlan;renderCoach();
            uiChecks["growth_prerequisite_survives_unavailable_ranking"]=advice->toPlainText().contains(QStringLiteral("成长前置 · 获取待核实"));
            game=savedGame;coachResult=savedCoach;modelState=savedModelState;renderCoach();
        }
        adaptivePurchaseUiChecks();
        uiChecks["passive_hud_does_not_require_pages"]=right->findChildren<QTabWidget*>().isEmpty()&&!advice->isVisibleTo(right)&&!roster->isVisibleTo(right);
        uiChecks["fixed_direction_control_removed"]=!style->isVisibleTo(right)&&style->count()==1&&style->currentData()==QStringLiteral("成长联动");
        uiChecks["hud_has_no_player_query_controls"]=right->findChildren<HistoryPane*>().isEmpty();
        uiChecks["automatic_hint_ready"]=!automaticHint->text().isEmpty();
        bool manualAdvice=false;for(auto button:right->findChildren<QPushButton*>())if(button->text().contains(QStringLiteral("解释"))||button->text().contains(QStringLiteral("建议")))manualAdvice=true;
        uiChecks["manual_advice_button_absent"]=!manualAdvice;uiChecks["history_query_absent_from_hud"]=right->findChildren<QLineEdit*>().isEmpty();uiChecks["hotkey_registered"]=hotkey;
        if(hotkey){setThrough(true);uiChecks["click_through_on"]=through;setThrough(false);uiChecks["click_through_recovered"]=!through;}
        uiChecks["default_layout_safe"]=overlayAvoidsProtectedAreas(placement);auto actual=placement;actual.right=right->geometry();uiChecks["actual_layout_safe"]=overlayAvoidsProtectedAreas(actual);
        int mainPanels=0;for(auto widget:QApplication::topLevelWidgets())if(dynamic_cast<OverlayWindow*>(widget))++mainPanels;uiChecks["left_panel_removed"]=mainPanels==1;
        uiChecks["right_content_inside_body"]=contentFits();
        const auto hud=automaticHint->report();uiChecks["automatic_hint_line_height"]=hud.value("text_fits").toBool()&&hud.value("lines_do_not_overlap").toBool();
        uiChecks["hud_uses_readable_primary_type"]=hud.value("title_font_px").toInt()>=16&&hud.value("status_font_px").toInt()>=12;
        uiChecks["hud_footer_is_passive_and_fits"]=routeLabel->parentWidget()->rect().contains(routeLabel->geometry())&&routeLabel->parentWidget()->findChildren<QAbstractButton*>().isEmpty();
        uiChecks["economy_icon_stats_fit"]=summary->report().value("render_fits").toBool();
        uiChecks["compact_action_labels_fit"]=componentsLabel->report().value("text_fits").toBool()&&routeLabel->fontMetrics().horizontalAdvance(routeLabel->text())<=routeLabel->width()&&right->report().value("heading_fits").toBool();
        uiChecks["game_panel_focus_restored"]=parser.isSet("inspection-window")||!right->report().value("accepts_keyboard_focus").toBool();
        setDetailsExpanded(false);QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);uiChecks["details_fold_restores_compact_height"]=!details->isVisibleTo(right)&&right->height()<=compactHudHeight&&contentFits();
        QJsonArray foldedChildren;for(auto child:right->body->findChildren<QWidget*>(QString(),Qt::FindDirectChildrenOnly))if(child->isVisibleTo(right->body)){const auto r=child->geometry();foldedChildren.append(QJsonObject{{"object",child->objectName()},{"class",QString::fromLatin1(child->metaObject()->className())},{"geometry",QJsonArray{r.x(),r.y(),r.width(),r.height()}}});}
        uiFixtureModelEvidence["folded_layout"]=QJsonObject{{"height",right->height()},{"preferred_height",right->report().value("preferred_content_height")},{"collapsed",right->collapsed()},{"details_expanded",detailsExpanded},{"compact_height_limit",compactHudHeight},{"body_width",right->body->width()},{"body_height",right->body->height()},{"content_fits",contentFits()},{"details_visible",details->isVisibleTo(right)},{"children",foldedChildren},{"action",automaticHint->report()},{"components",componentsLabel->report()}};
        renderCardHints();uiChecks["card_reference_geometry_safe"]=selectionCardLayoutSafe(viewport,cardPlacement);uiChecks["card_hints_automatically_visible"]=selectionActive;
        bool controlsClear=true;for(auto hint:cardHints)if(hint->isVisible())controlsClear&=!hint->geometry().intersects(cardPlacement.referenceCardActions)&&!hint->geometry().intersects(cardPlacement.referenceFooterControls);
        uiChecks["card_hints_leave_game_actions_clear"]=selectionActive&&controlsClear;
        bool passThrough=true;for(auto hint:cardHints)passThrough&=hint->report().value("click_through").toBool()&&!hint->report().value("accepts_keyboard_focus").toBool();uiChecks["card_hints_do_not_capture_input"]=passThrough;
        bool readable=true;for(auto hint:cardHints){const auto r=hint->report();readable&=r.value("title_font_px").toInt()>=12&&r.value("metric_font_px").toInt()>=14&&r.value("title_fits").toBool()&&r.value("metric_fits").toBool()&&r.value("line_heights_fit").toBool();}uiChecks["card_text_readable_and_inside_content"]=readable;
        const auto beforeReroll=state;const auto beforeGame=game;const auto beforeDecision=cardDecision;const auto beforeBasis=computedCardBasis;
        int replacement=0;for(auto it=catalog.augments.cbegin();it!=catalog.augments.cend();++it)if(catalog.ordinaryOfferEligible(it.key())&&!game.offers.contains(it.key())&&!game.selected.contains(it.key())){replacement=it.key();break;}
        QElapsedTimer rerollClock;rerollClock.start();const auto rerollStartedAt=autoClock.elapsed();const int rerollCompletionsBefore=cardCompletions;bool rerollApplied=false;
        QTimer::singleShot(50,this,[&]{
            auto rerolled=state.candidates[0].toObject();rerolled["augment_id"]=replacement;rerolled["augment_name"]=catalog.augments.value(replacement).value("name_zh_cn");state.candidates[0]=rerolled;game.offers[0]=replacement;
            renderCardHints();uiChecks["reroll_never_reuses_old_card_value"]=cardHints[0]->report().value("augment_id").toInt()==replacement&&!cardHints[0]->report().value("numeric_gain_available").toBool();
            uiChecks["synthetic_50ms_candidate_event_updates_cache_key"]=replacement>0&&cardBasis()!=beforeBasis&&!cardModelCurrent()&&rerollClock.elapsed()<250;
            rerollApplied=true;scheduleCardModel();
        });
        while((!rerollApplied||!cardModelCurrent())&&rerollClock.elapsed()<600){QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);QThread::msleep(1);}
        // processEvents can finish unrelated queued artwork/snapshot work after
        // the new card was rendered. Measure the real callback publication.
        const auto rerollRenderedLatency=cardRenderedAtMs-rerollStartedAt;
        uiChecks["reroll_scores_update_without_build_search"]=cardCompletions>rerollCompletionsBefore&&cardModelCurrent()&&computedCardBasis==cardWorkerBasis&&cardHints[0]->report().value("augment_id").toInt()==replacement&&rerollRenderedLatency>=0&&rerollRenderedLatency<600;
        uiFixtureModelEvidence["reroll_render_latency_ms"]=rerollRenderedLatency;uiFixtureModelEvidence["reroll_event_loop_elapsed_ms"]=rerollClock.elapsed();uiFixtureModelEvidence["reroll_worker_completion_advanced"]=cardCompletions>rerollCompletionsBefore;
        const auto afterReroll=game;const auto currentCardBasis=cardBasis();++game.level;uiChecks["level_change_invalidates_card_cache"]=cardBasis()!=currentCardBasis&&!cardModelCurrent();game=afterReroll;
        game.owned.append(3135);uiChecks["equipment_change_invalidates_card_cache"]=cardBasis()!=currentCardBasis&&!cardModelCurrent();game=afterReroll;
        QString ownTeam;int enemyIndex=-1,selfIndex=-1;for(int i=0;i<game.players.size();++i)if(game.players[i].toObject().value("self").toBool()){selfIndex=i;ownTeam=game.players[i].toObject().value("team").toString();}
        for(int i=0;i<game.players.size();++i)if(!game.players[i].toObject().value("self").toBool()&&game.players[i].toObject().value("team").toString()!=ownTeam){enemyIndex=i;break;}
        for(const auto &name:{"enemy_item_change_invalidates_card_cache","enemy_level_change_invalidates_card_cache","enemy_inventory_completeness_invalidates_card_cache","enemy_augment_change_invalidates_card_cache","enemy_augment_completeness_invalidates_card_cache","invalid_enemy_item_does_not_alias_valid_inventory","opponent_order_only_keeps_card_cache","opponent_score_and_health_ticks_keep_card_cache","own_team_marker_invalidates_card_cache"})uiChecks[name]=false;
        if(enemyIndex>=0){const auto originalPlayer=game.players[enemyIndex].toObject();auto changed=originalPlayer;auto ids=changed.value("items").toArray();ids.append(3135);changed["items"]=ids;game.players[enemyIndex]=changed;uiChecks["enemy_item_change_invalidates_card_cache"]=cardBasis()!=currentCardBasis&&!cardModelCurrent();game=afterReroll;
            changed=originalPlayer;changed["level"]=changed.value("level").toInt()+1;game.players[enemyIndex]=changed;uiChecks["enemy_level_change_invalidates_card_cache"]=cardBasis()!=currentCardBasis&&!cardModelCurrent();game=afterReroll;
            changed=originalPlayer;changed["items_complete"]=false;game.players[enemyIndex]=changed;const auto incompleteKey=cardBasis();changed["items_complete"]=true;game.players[enemyIndex]=changed;uiChecks["enemy_inventory_completeness_invalidates_card_cache"]=cardBasis()!=incompleteKey;game=afterReroll;
            changed=originalPlayer;auto augments=changed.value("augment_ids").toArray();augments.append(1084);changed["augment_ids"]=augments;game.players[enemyIndex]=changed;uiChecks["enemy_augment_change_invalidates_card_cache"]=cardBasis()!=currentCardBasis&&!cardModelCurrent();game=afterReroll;
            changed=originalPlayer;changed["augments_complete"]=!changed.value("augments_complete").toBool();game.players[enemyIndex]=changed;uiChecks["enemy_augment_completeness_invalidates_card_cache"]=cardBasis()!=currentCardBasis&&!cardModelCurrent();game=afterReroll;
            changed=originalPlayer;ids=changed.value("items").toArray();ids.append(QStringLiteral("invalid-item"));changed["items"]=ids;game.players[enemyIndex]=changed;uiChecks["invalid_enemy_item_does_not_alias_valid_inventory"]=cardBasis()!=currentCardBasis;game=afterReroll;
            changed=originalPlayer;changed["augment_ids"]=QJsonArray{1084,1305};game.players[enemyIndex]=changed;const auto unorderedKey=cardBasis();QJsonArray reversedPlayers;
            for(int i=game.players.size()-1;i>=0;--i){auto p=game.players[i].toObject();for(const auto &key:{QStringLiteral("items"),QStringLiteral("augment_ids")}){const auto source=p.value(key).toArray();QJsonArray reverse;for(int j=source.size()-1;j>=0;--j)reverse.append(source[j]);p[key]=reverse;}reversedPlayers.append(p);}game.players=reversedPlayers;uiChecks["opponent_order_only_keeps_card_cache"]=cardBasis()==unorderedKey;game=afterReroll;
            changed=originalPlayer;changed["scores"]=QJsonObject{{"kills",99}};changed["currentHealth"]=1;changed["is_dead"]=true;game.players[enemyIndex]=changed;game.gold+=50;uiChecks["opponent_score_and_health_ticks_keep_card_cache"]=cardBasis()==currentCardBasis;game=afterReroll;
        }
        if(selfIndex>=0){auto changed=game.players[selfIndex].toObject();changed["team"]=ownTeam==QStringLiteral("ORDER")?QStringLiteral("CHAOS"):QStringLiteral("ORDER");game.players[selfIndex]=changed;uiChecks["own_team_marker_invalidates_card_cache"]=cardBasis()!=currentCardBasis;game=afterReroll;}
        uiChecks["ally_equipment_invalidates_card_cache"]=false;
        uiChecks["ally_level_invalidates_card_cache"]=false;
        uiChecks["ally_incomplete_inventory_invalidates_card_cache"]=false;
        for(int i=0;i<game.players.size();++i){const auto p=game.players[i].toObject();if(p.value("self").toBool()||p.value("team").toString()!=ownTeam)continue;
            auto changed=p;auto ids=changed.value("items").toArray();ids.append(3135);changed["items"]=ids;game.players[i]=changed;
            uiChecks["ally_equipment_invalidates_card_cache"]=cardBasis()!=currentCardBasis&&!cardModelCurrent();game=afterReroll;
            changed=p;changed["level"]=changed.value("level").toInt()+1;game.players[i]=changed;
            uiChecks["ally_level_invalidates_card_cache"]=cardBasis()!=currentCardBasis&&!cardModelCurrent();game=afterReroll;
            changed=p;changed["items_complete"]=false;game.players[i]=changed;
            uiChecks["ally_incomplete_inventory_invalidates_card_cache"]=cardBasis()!=currentCardBasis&&!cardModelCurrent();game=afterReroll;break;
        }
        game.headlinerContext=QJsonObject{{"schema_version",1},{"patch","16.21"},{"source","synthetic_replay"},{"rule_key","KDA"},{"game_time",game.gameTime},{"facts",QJsonObject{{"kda_cards_collected",1}}}};
        uiChecks["headliner_change_invalidates_card_cache"]=cardBasis()!=currentCardBasis&&!cardModelCurrent();game=afterReroll;
        game.championId=412;game.offers={1028,1087,1333};game.selected={};game.effectConditions.clear();game.effectCounters.clear();state.candidates={};
        for(int id:game.offers)state.candidates.append(QJsonObject{{"candidate_index",state.candidates.size()},{"augment_id",id},{"augment_name",catalog.augments.value(id).value("name_zh_cn")}});
        cardDecision=buildAugmentDecision(catalog,game,{},QStringLiteral("成长联动"),{},true);cardDecision["mechanic_cache_validity"]=coachCardMechanicValidity(catalog,game);computedCardBasis=cardBasis();renderCardHints();
        const auto conditionalHint=cardHints[2]->report();
        uiChecks["unobserved_cards_remain_unrated_without_forced_pick"]=(conditionalHint.value("metric")==QStringLiteral("需触发条件")||conditionalHint.value("source_mechanism_reference").toObject().value("available").toBool())&&conditionalHint.value("evaluation_status")==QStringLiteral("trigger_unobserved")&&!conditionalHint.value("evaluation_missing_inputs").toArray().isEmpty()&&!conditionalHint.value("evaluation_reason").toString().isEmpty()&&!conditionalHint.value("recommended").toBool()&&!conditionalHint.contains("recommendation")&&!conditionalHint.value("numeric_gain_available").toBool();
        uiChecks["recommendation_badges_fit"]=true;for(auto hint:cardHints)if(!hint->report().value("badge_fits").toBool())uiChecks["recommendation_badges_fit"]=false;
        if(parser.isSet("snapshot")){const QFileInfo image(parser.value("snapshot"));snapshot(image.absolutePath()+"/"+image.completeBaseName()+"-conditional.png");}
        state=beforeReroll;game=beforeGame;cardDecision=beforeDecision;computedCardBasis=beforeBasis;renderCardHints();
        // Keep the complex demo as a negative control: a displayed numeric
        // change must not become a rank when shared mechanic state is unresolved.
        for(int i=0;i<game.players.size();++i){auto p=game.players[i].toObject();if(!p.value("self").toBool())continue;p["augment_ids"]=jsonIds(game.selected);p["augments_complete"]=true;game.players[i]=p;}
        const QList<int> controlIds{1084,1103,1129,1013,1047};QList<QList<int>> controlGroups;
        for(int i=0;i<controlIds.size();++i)for(int j=i+1;j<controlIds.size();++j)for(int k=j+1;k<controlIds.size();++k)controlGroups.append(QList<int>{controlIds[i],controlIds[j],controlIds[k]});
        auto renderControlGroup=[&](const QList<int> &group){
            game.offers=group;state.status="open";state.candidates={};for(int id:group)state.candidates.append(QJsonObject{{"candidate_index",state.candidates.size()},{"augment_id",id},{"augment_name",catalog.augments.value(id).value("name_zh_cn")},{"rerollable",true}});
            cardDecision=buildAugmentDecision(catalog,game,{},style->currentData().toString(),{},true);cardDecision["mechanic_cache_validity"]=coachCardMechanicValidity(catalog,game);computedCardBasis=cardBasis();renderCardHints();
            QJsonArray shown;for(auto hint:cardHints)shown.append(hint->report());return shown;
        };
        const auto unresolvedHints=renderControlGroup(controlGroups.first());
        bool unresolvedNumeric=false,unresolvedComparison=false,unresolvedUnranked=cardModelCurrent()&&selectionActive&&unresolvedHints.size()==3;
        for(const auto &value:unresolvedHints){const auto row=value.toObject();unresolvedNumeric|=row.value("numeric_gain_available").toBool();unresolvedComparison|=row.value("numeric_gain_available").toBool()&&(!row.value("marginal_comparable").toBool()||!row.value("decision_marginal_comparable").toBool());unresolvedUnranked&=!row.contains("rank")&&!row.contains("recommendation")&&!row.value("recommended").toBool();}
        const bool unresolvedPassed=unresolvedNumeric&&unresolvedComparison&&unresolvedUnranked;
        uiChecks["real_catalog_unresolved_numeric_values_stay_unranked"]=unresolvedPassed;
        uiChecks["unranked_offer_has_no_reroll_comparison"]=std::none_of(unresolvedHints.cbegin(),unresolvedHints.cend(),[](const auto &value){return value.toObject().contains("reroll_comparison");});
        const QJsonObject unresolvedControl{{"synthetic_game_state",true},{"real_catalog_effects",true},{"fabricated_gain",false},{"has_numeric_values",unresolvedNumeric},{"has_unresolved_comparison",unresolvedComparison},{"hints",unresolvedHints},{"passed",unresolvedPassed}};
        // Independent positive control, still using the real compiled hero and
        // augment rules. Repeated Annie rows isolate the numeric/UI contract
        // from other heroes' unobserved marks, ammunition and return-hit state;
        // this is an explicit synthetic scenario, not a claim about a live roster.
        GameState controlled;controlled.source=GameState::Source::Replay;controlled.phase=QStringLiteral("InProgress");controlled.gameMode=QStringLiteral("KIWI");controlled.queueId=2400;controlled.observedPatch=catalog.patch;controlled.championId=1;controlled.level=11;controlled.gameTime=100;
        controlled.matchContext=QJsonObject{{"active",true},{"context_id",QStringLiteral("synthetic-ui-annie-damage-positive-control")},{"stage",QStringLiteral("in_game")}};
        const QJsonObject controlRanks{{"Q",5},{"W",3},{"E",1},{"R",2}},readyCooldowns{{"Q",0},{"W",0},{"E",0},{"R",0}};
        for(auto it=controlRanks.begin();it!=controlRanks.end();++it)controlled.abilities[it.key()]=QJsonObject{{"abilityLevel",it.value()}};
        const auto controlBase=catalog.combat->context(1,11);
        for(int i=0;i<10;++i)controlled.players.append(QJsonObject{{"player_key",QStringLiteral("annie-control-%1").arg(i)},{"self",i==0},{"team",i<5?QStringLiteral("ORDER"):QStringLiteral("CHAOS")},{"champion_id",1},{"level",11},{"ranks",controlRanks},{"items",QJsonArray{}},{"items_complete",true},{"augment_ids",QJsonArray{}},{"augments_complete",true},{"effect_conditions",QJsonArray{}},{"effect_counters",QJsonObject{}},{"action_state",QJsonObject{{"resource_current",controlBase.base[Stat::Mana]},{"cooldown_remaining",readyCooldowns}}}});
        game=controlled;state=beforeReroll;state.replay=true;state.headlinerContext={};state.headlinerObserved={};state.seenCandidates={};state.liveContextId.clear();state.liveMatchRef.clear();
        bool realControlPassed=false;QJsonArray controlTrials;
        for(const auto &group:controlGroups){bool eligible=demo&&game.championId==1;for(int id:group)eligible&=catalog.ordinaryOfferEligible(id)&&catalog.augments.value(id).value("rarity")==QStringLiteral("kGold");if(!eligible)continue;
            const auto shown=renderControlGroup(group);bool allNumeric=cardModelCurrent()&&selectionActive;int knownWinners=0;
            const bool rankingAllowed=cardDecision.value("ranking_quality").toObject().value("ranking_allowed").toBool();
            const auto winnerRole=combatRoleIntent(catalog,game,style->currentData().toString());
            const auto expectedWinnerBadge=(winnerRole.value("support_role").toBool()||winnerRole.value("tank_role").toBool())?winnerRole.value("comparison_label").toString():QStringLiteral("数值领先");
            for(int i=0;i<cardHints.size();++i){const auto row=cardHints[i]->report();allNumeric&=row.value("augment_id").toInt()==group[i]&&row.value("numeric_gain_available").toBool()&&row.value("marginal_comparable").toBool()&&row.value("decision_marginal_comparable").toBool()&&row.value("additional_unknown_count").toInt()==0&&row.value("partial").toBool()&&row.value("metric_fits").toBool()&&row.value("badge_fits").toBool();allNumeric&=row.value("comparison_scope")==QStringLiteral("known_model_shared_unknowns");if(row.value("recommended").toBool()&&row.value("displayed_badge")==expectedWinnerBadge)++knownWinners;}
            realControlPassed=allNumeric&&rankingAllowed&&knownWinners==1;controlTrials.append(QJsonObject{{"augment_ids",jsonIds(group)},{"offers",cardDecision.value("offers")},{"ranking_allowed",rankingAllowed},{"passed",realControlPassed}});
            knownModelUiControl=QJsonObject{{"synthetic_game_state",true},{"real_catalog_effects",true},{"fabricated_gain",false},{"champion_id",game.championId},{"fixture_kind",QStringLiteral("ten_annie_level11_empty_loadouts_explicit_ranks_full_base_mana_ready_cooldowns")},{"unresolved_state_negative_control",unresolvedControl},{"augment_ids",jsonIds(group)},{"hints",shown},{"passed",realControlPassed},{"trials",controlTrials}};
            if(realControlPassed){
                bool rerollFits=true;int retained=0,forfeited=0;
                for(const auto &value:shown){
                    const auto row=value.toObject();const auto reroll=row.value("reroll_comparison").toObject();
                    rerollFits&=reroll.value("available")==QJsonValue(true)&&reroll.value("live_probability")==QJsonValue(false)
                        &&row.value("reroll_comparison_accessible").toBool()&&row.value("line_heights_fit").toBool();
                    if(row.value("displayed_reroll")==QStringLiteral("重随留最高")&&reroll.value("preserves_current_best").toBool())++retained;
                    if(row.value("displayed_reroll")==QStringLiteral("重随失最高")&&row.value("recommended").toBool())++forfeited;
                }
                uiChecks["three_rerollable_slots_show_current_best_tradeoff"]=rerollFits&&retained==2&&forfeited==1;
                const auto rerollableCandidates=state.candidates;
                auto firstSlot=state.candidates[0].toObject();firstSlot.remove("rerollable");state.candidates[0]=firstSlot;
                auto secondSlot=state.candidates[1].toObject();secondSlot["rerollable"]=false;state.candidates[1]=secondSlot;
                renderCardHints();
                uiChecks["unobserved_or_spent_reroll_has_no_hint"]=!cardHints[0]->report().contains("reroll_comparison")
                    &&!cardHints[1]->report().contains("reroll_comparison")&&cardHints[2]->report().contains("reroll_comparison");
                state.candidates=rerollableCandidates;
                const auto allowedDecision=cardDecision;auto quality=cardDecision.value("ranking_quality").toObject();quality["ranking_allowed"]=false;cardDecision["ranking_quality"]=quality;renderCardHints();
                bool numericUnrankedAdvisory=selectionActive;
                for(auto hint:cardHints){const auto row=hint->report();numericUnrankedAdvisory&=row.value("numeric_gain_available").toBool()&&!row.value("recommended").toBool()&&row.value("advisory").toObject().value("available").toBool();}
                uiChecks["all_numeric_but_unranked_cards_keep_reference_advice"]=numericUnrankedAdvisory;
                const auto firstReferenceBuilds=cardReferenceCache.statistics().value("builds").toInteger();renderCardHints();
                uiChecks["same_offer_render_reuses_reference_pair"]=cardReferenceCache.statistics().value("builds").toInteger()==firstReferenceBuilds;
                const auto savedReferenceState=state;state.observed=QDateTime::currentDateTimeUtc();renderCardHints();
                auto builds=cardReferenceCache.statistics().value("builds").toInteger();state.observed=state.observed.addMSecs(1);renderCardHints();
                uiChecks["same_card_new_offer_timestamp_rebuilds_references"]=cardReferenceCache.statistics().value("builds").toInteger()==builds+1;
                builds=cardReferenceCache.statistics().value("builds").toInteger();++state.round;renderCardHints();
                uiChecks["same_card_new_round_rebuilds_references"]=cardReferenceCache.statistics().value("builds").toInteger()==builds+1;
                builds=cardReferenceCache.statistics().value("builds").toInteger();state.session+=QStringLiteral("-reference-next");renderCardHints();
                uiChecks["same_card_new_session_rebuilds_references"]=cardReferenceCache.statistics().value("builds").toInteger()==builds+1;
                state=savedReferenceState;cardDecision=allowedDecision;renderCardHints();break;
            }
        }
        uiChecks["real_catalog_engine_ui_positive_control"]=realControlPassed;
        if(parser.isSet("snapshot")){const QFileInfo image(parser.value("snapshot"));snapshot(image.absolutePath()+"/"+image.completeBaseName()+"-known-model.png");}
        state=beforeReroll;game=beforeGame;cardDecision=beforeDecision;computedCardBasis=beforeBasis;renderCardHints();
        const auto savedHint=cardHints[0]->report();auto longHint=savedHint;longHint["title"]=QString(80,QChar(u'强'));longHint["metric"]=QStringLiteral("适配 +99.9%");longHint["recommendation"]=QStringLiteral("数值同优");longHint["partial"]=true;cardHints[0]->present(cardHints[0]->geometry(),longHint,right->configuredOpacity());const auto elided=cardHints[0]->report();uiChecks["card_duplicate_title_omitted_without_losing_semantics"]=!elided.value("title_visible").toBool()&&!elided.value("title_elided").toBool()&&elided.value("source_title")==longHint.value("title")&&elided.value("source_title_accessible").toBool()&&elided.value("metric_fits").toBool()&&elided.value("line_heights_fit").toBool();uiChecks["known_partial_badge_inside_content"]=elided.value("displayed_badge")==QStringLiteral("数值同优")&&elided.value("badge_fits").toBool();cardHints[0]->present(cardHints[0]->geometry(),savedHint,right->configuredOpacity());
        const auto originalHintGeometry=cardHints[0]->geometry();bool minimumCardFits=true;
        for(const auto &metric:{QStringLiteral("适配 +99.9%"),QStringLiteral("适配 -999.9%"),QStringLiteral("适配 +1e+08%"),QStringLiteral("待评"),QStringLiteral("计算中")}){auto narrow=longHint;narrow["metric"]=metric;cardHints[0]->present(QRect(originalHintGeometry.topLeft(),QSize(150,44)),narrow,right->configuredOpacity());const auto shown=cardHints[0]->report();minimumCardFits&=shown.value("title_fits").toBool()&&shown.value("metric_fits").toBool()&&shown.value("line_heights_fit").toBool()&&shown.value("badge_fits").toBool()&&shown.value("text_rects_non_overlapping").toBool()&&shown.value("displayed_badge").toString()==(metric.startsWith(QStringLiteral("适配 "))?QStringLiteral("数值同优"):QString());}
        uiChecks["minimum_card_150x44_preserves_semantics"]=minimumCardFits;cardHints[0]->present(originalHintGeometry,savedHint,right->configuredOpacity());
        uiChecks["side_panels_avoid_reference_cards"]=sidePanelsAvoidCards();
        setUserHidden(true);bool noneVisible=!right->isVisible();for(auto hint:cardHints)noneVisible&=!hint->isVisible();uiChecks["hide_all_includes_card_hints"]=noneVisible;setUserHidden(false);
        const auto savedState=state;state.status="closed";renderCardHints();noneVisible=true;for(auto hint:cardHints)noneVisible&=!hint->isVisible();uiChecks["closed_selection_hides_card_hints"]=noneVisible;uiChecks["right_panel_restored_after_selection"]=right->isVisible();state=savedState;renderCardHints();
        uiChecks["card_hints_return_after_show"]=selectionActive;matchUiChecks();automaticPresentationUiChecks();deathTipsUiChecks();if(beganExpanded)setDetailsExpanded(true);runningUiChecks=false;manualPresentationUntil=0;manualPresentationPinned=manualPresentationPending=false;presentation=CoachPresentation{};uiFixtureModelEvidence["self_check_complete"]=true;uiFixtureModelEvidence["completed_at_elapsed_ms"]=autoClock.elapsed();renderCoach();writeReport();if(pollingBeforeCheck)pollTimer.start();if(parser.isSet("snapshot"))QTimer::singleShot(400,this,[this]{snapshot(parser.value("snapshot"));writeReport();});
    }
};

int main(int argc,char **argv){
    Kda::configureApplicationRendering();
    QApplication app(argc,argv);
    app.setApplicationVersion(QString::fromLatin1(LIXIN_APP_VERSION));
    app.setFont(Kda::font(Kda::FontRole::Body,13));
    app.setWindowIcon(assistantIcon());
    // Offscreen Qt does not enumerate Windows fonts. Use the same actual
    // Chinese font files so geometry-only checks don't pass with tofu glyphs.
    if(QGuiApplication::platformName()==QStringLiteral("offscreen"))for(const auto &file:{QStringLiteral("msyh.ttc"),QStringLiteral("msyhbd.ttc")})QFontDatabase::addApplicationFont(qEnvironmentVariable("WINDIR",QStringLiteral("C:/Windows"))+QStringLiteral("/Fonts/")+file);
    app.setApplicationName("LiXinCoachPanels");app.setApplicationDisplayName(QStringLiteral("李金金"));app.setStyleSheet(Kda::overlayStyleSheet());QCommandLineParser args;args.addHelpOption();args.addVersionOption();for(const auto &name:{"inspection-window","fixture-feed","demo","test-ds","diagnostic-ds","self-check-ui","self-check-assistance","dashboard","self-check-dashboard","ui-browser","self-check-browser","startup-notice-only","startup-notice-preview","startup-notice-accepted"})args.addOption(QCommandLineOption(name,name));for(const auto &name:{"events","replay","settings","report","snapshot","quit-after-ms","root","viewport","query-summoner","dashboard-output","startup-ready-file","self-check-startup"})args.addOption(QCommandLineOption(name,name,"value"));
    args.addOption(QCommandLineOption("runtime-log-dir", "Write operational diagnostics to a separate log directory", "directory"));
    args.process(app);
    const bool noticeOnly=args.isSet("startup-notice-only"),noticePreview=args.isSet("startup-notice-preview");
    if(!noticeOnly&&!noticePreview&&int(args.isSet("events"))+int(args.isSet("replay"))+int(args.isSet("demo"))!=1){std::fprintf(stderr,"Choose one of --events, --replay, --demo\n");return 2;}
    const bool browser=args.isSet("ui-browser");
    if(args.isSet("self-check-assistance")&&(!args.isSet("demo")||!args.isSet("inspection-window")||args.value("settings").isEmpty()||args.value("dashboard-output").isEmpty()||args.isSet("test-ds")||args.isSet("diagnostic-ds")||args.isSet("ui-browser")||args.isSet("query-summoner")||args.isSet("self-check-ui")||args.isSet("self-check-dashboard"))){std::fprintf(stderr,"Assistance simulation requires isolated --demo --inspection-window --settings and --dashboard-output, without providers or other UI checks.\n");return 2;}
    if((browser&&(!args.isSet("demo")||!args.isSet("inspection-window")||args.isSet("test-ds")||args.isSet("diagnostic-ds")||args.isSet("query-summoner")||args.isSet("dashboard")||args.isSet("self-check-ui")||args.isSet("self-check-dashboard")))||(args.isSet("self-check-browser")&&!browser)){std::fprintf(stderr,"UI browsing requires an isolated --demo --inspection-window instance without live queries or paid diagnostics.\n");return 2;}
    if(args.isSet("viewport")){const auto size=args.value("viewport").split('x');bool w=false,h=false;const int width=size.value(0).toInt(&w),height=size.value(1).toInt(&h);if(size.size()!=2||!w||!h||width<320||height<200||width>16384||height>16384){std::fprintf(stderr,"Invalid viewport size\n");return 2;}}
    QString root=args.value("root");if(root.isEmpty()){QDir dir(QCoreApplication::applicationDirPath());for(int n=0;n<7;++n){if(dir.exists("data/combat.v1.json")){root=dir.absolutePath();break;}if(!dir.cdUp())break;}}
    if(root.isEmpty()||!QFileInfo::exists(root+QStringLiteral("/data/combat.v1.json"))){std::fprintf(stderr,"Workspace unavailable\n");return 2;}
    const auto notice=[&]{
        QSettings appearance(args.value("settings").isEmpty()?root+QStringLiteral("/.local/augment-panel/coach-appearance.ini"):args.value("settings"),QSettings::IniFormat);
        Kda::setSkin(appearance.value("appearance/skin",QStringLiteral("kda")).toString());Kda::setReducedMotion(appearance.value("appearance/reducedMotion",false).toBool());
        return showStartupNotice();
    };
    if(noticePreview)return notice()?0:20;
    if(args.isSet("test-ds")&&!args.isSet("demo")){std::fprintf(stderr,"Automatic DS test requires explicit synthetic --demo\n");return 2;}
    const bool normalLive=args.isSet("events")&&!args.isSet("demo")&&!args.isSet("replay")&&!args.isSet("fixture-feed")&&!args.isSet("inspection-window")&&!args.isSet("self-check-ui")&&!args.isSet("self-check-dashboard")&&!args.isSet("self-check-assistance");
    const bool isolatedCheck=args.isSet("fixture-feed")||args.isSet("self-check-ui")||args.isSet("self-check-dashboard")
        ||args.isSet("self-check-assistance")||args.isSet("self-check-browser")||args.isSet("self-check-startup")
        ||args.isSet("test-ds")||(args.isSet("demo")&&args.isSet("quit-after-ms"));
    const bool exclusive=noticeOnly||!isolatedCheck;
    const auto instanceId=[](const QString &path){
        return QString::fromLatin1(QCryptographicHash::hash(path.toUtf8(),QCryptographicHash::Sha256).toHex().left(24));
    };
    const auto workspaceServerName=(browser?QStringLiteral("LiXinUiBrowser-"):QStringLiteral("LiXinCoach-"))+instanceId(QDir(root).canonicalPath());
    const auto instanceFolder=root+(browser?QStringLiteral("/.local/ui-browser"):QStringLiteral("/.local/augment-panel"));
    const auto userFolder=QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)).filePath(QStringLiteral("LiXin/instance"));
    QDir().mkpath(instanceFolder);
    QLockFile workspaceLock(instanceFolder+QStringLiteral("/coach-instance.lock"));
    QLockFile userLock(QDir(userFolder).filePath(QStringLiteral("coach-instance.lock")));
    std::function<void()> activateWindow;
    QLocalServer workspaceServer,userServer;
    const auto requestActivation=[&](const QString &name){
        QElapsedTimer wait;wait.start();
        do {
            QLocalSocket socket;socket.connectToServer(name);
            if(socket.state()==QLocalSocket::ConnectedState||socket.waitForConnected(120)){
                socket.write("show\n");
                if(!socket.bytesToWrite()||socket.waitForBytesWritten(150))return noticeOnly?21:0;
            }
            QThread::msleep(40);
        }while(wait.elapsed()<1500);
        std::fprintf(stderr,"Li Jin Jin is already running; its main window is not available yet.\n");
        return 1;
    };
    const auto listenForActivation=[&](QLocalServer &server,const QString &name){
        QLocalServer::removeServer(name);
        server.setSocketOptions(QLocalServer::UserAccessOption);
        if(!server.listen(name))return false;
        QObject::connect(&server,&QLocalServer::newConnection,&server,[&server,&activateWindow]{
            while(server.hasPendingConnections()){
                auto *socket=server.nextPendingConnection();
                socket->readAll();socket->disconnectFromServer();socket->deleteLater();
            }
            if(activateWindow)activateWindow();
        });
        return true;
    };
    if(exclusive){
        QString userServerName;
        if(!browser){
            if(!QDir().mkpath(userFolder)){std::fprintf(stderr,"Unable to open the user instance directory.\n");return 1;}
            userServerName=QStringLiteral("LiXinCoachUser-")+instanceId(QDir(userFolder).canonicalPath().toCaseFolded());
            if(!userLock.tryLock(0))return requestActivation(userServerName);
        }
        // The workspace channel also recognizes a still-running older installation.
        if(!workspaceLock.tryLock(0))return requestActivation(workspaceServerName);
        if(!listenForActivation(workspaceServer,workspaceServerName)||(!browser&&!listenForActivation(userServer,userServerName))){
            std::fprintf(stderr,"Unable to open the local window channel.\n");return 1;
        }
    }
    if(noticeOnly){
        StartupNoticeDialog dialog;
        QSettings appearance(args.value("settings").isEmpty()?root+QStringLiteral("/.local/augment-panel/coach-appearance.ini"):args.value("settings"),QSettings::IniFormat);
        Kda::setSkin(appearance.value("appearance/skin",QStringLiteral("kda")).toString());
        activateWindow=[&dialog]{dialog.showNormal();dialog.raise();dialog.activateWindow();};
        return dialog.exec()==QDialog::Accepted?0:20;
    }
    const bool ordinaryStartup=!args.isSet("demo")&&!args.isSet("fixture-feed")&&!args.isSet("inspection-window")&&!browser&&!args.isSet("self-check-ui")&&!args.isSet("self-check-dashboard")&&!args.isSet("self-check-assistance")&&!args.isSet("self-check-browser");
    using LogEvent = RuntimeLog::Event;
    using LogOutcome = RuntimeLog::Outcome;
    auto logDirectory=args.value("runtime-log-dir");
    if(logDirectory.isEmpty()&&normalLive)logDirectory=instanceFolder+QStringLiteral("/logs");
    std::unique_ptr<RuntimeLog> runtimeLog;
    QElapsedTimer launchClock;launchClock.start();
    if(!logDirectory.isEmpty()){
        runtimeLog=std::make_unique<RuntimeLog>(logDirectory);
        auto mode=LogOutcome::Live;
        if(args.isSet("demo"))mode=LogOutcome::Demo;
        else if(args.isSet("replay"))mode=LogOutcome::Replay;
        else if(args.isSet("fixture-feed"))mode=LogOutcome::Fixture;
        runtimeLog->record(LogEvent::PanelStarted,mode);
    }
    const auto stopLogging=[&](int exitCode){
        if(runtimeLog){
            const auto outcome=exitCode==0?LogOutcome::Clean:exitCode==20?LogOutcome::Cancelled:LogOutcome::Failed;
            runtimeLog->record(LogEvent::PanelStopped,outcome,launchClock.elapsed());
            runtimeLog->close();
            if(!runtimeLog->waitForFinished(500))std::fprintf(stderr,"Runtime log drain exceeded 500 ms.\n");
        }
        return exitCode;
    };
    const auto catalogStart=launchClock.elapsed();
    Catalog catalog;QString error;
    const bool catalogLoaded=catalog.load(root,&error);
    if(runtimeLog)runtimeLog->record(LogEvent::CatalogLoad,catalogLoaded?LogOutcome::Ready:LogOutcome::Failed,launchClock.elapsed()-catalogStart);
    if(!catalogLoaded){std::fprintf(stderr,"Catalog unavailable: %s\n",error.toUtf8().constData());return stopLogging(2);}
    if(catalog.runtimeManifest.value(QStringLiteral("scope")).toString()==QStringLiteral("public_synthetic_demo")&&!args.isSet("demo")){
        std::fprintf(stderr,"The public synthetic catalog requires --demo.\n");
        return stopLogging(2);
    }
    const auto startupCheck=args.value("self-check-startup");
    if(!startupCheck.isEmpty()&&(!QStringList{"accept","reject","close","escape"}.contains(startupCheck)||!args.isSet("demo")||!args.isSet("inspection-window")||args.value("settings").isEmpty()||args.value("dashboard-output").isEmpty()||args.isSet("test-ds")||args.isSet("diagnostic-ds")||browser||args.isSet("query-summoner"))){std::fprintf(stderr,"Startup checks require an isolated demo/inspection window and output directory.\n");return stopLogging(2);}
    const auto readyPath=args.value("startup-ready-file");
    if(!readyPath.isEmpty()){
        const QFileInfo ready(readyPath);const auto allowed=QDir(instanceFolder+QStringLiteral("/startup-launch")).canonicalPath();
        if(!ordinaryStartup||allowed.isEmpty()||ready.dir().canonicalPath()!=allowed||!QRegularExpression(QStringLiteral("^[a-f0-9]{32}\\.ready\\.json$")).match(ready.fileName()).hasMatch()||ready.exists()){
            std::fprintf(stderr,"Invalid per-launch startup channel.\n");return stopLogging(2);
        }
    }
    QSettings launchPreferences(args.value("settings").isEmpty()?root+(browser?QStringLiteral("/.local/ui-browser/appearance.ini"):QStringLiteral("/.local/augment-panel/coach-appearance.ini")):args.value("settings"),QSettings::IniFormat);
    Kda::configureOverlayAppearance(launchPreferences,!args.isSet("demo")&&!args.isSet("self-check-ui")&&!args.isSet("self-check-dashboard")&&!args.isSet("self-check-browser")&&!args.isSet("self-check-assistance"));
    std::unique_ptr<CoachDashboard> startupDashboard;QJsonObject startupEvidence;
    const auto saveStartupCheck=[&]{
        QDir output(args.value("dashboard-output"));output.mkpath(QStringLiteral("."));QSaveFile file(output.filePath(QStringLiteral("startup-check.json")));
        return file.open(QIODevice::WriteOnly)&&file.write(QJsonDocument(startupEvidence).toJson(QJsonDocument::Indented))>=0&&file.commit();
    };
    if(ordinaryStartup||!startupCheck.isEmpty()){
        // This is the final dashboard window, before any live engine or helper
        // is constructed. Acceptance replaces only its central reading page.
        startupDashboard=std::make_unique<CoachDashboard>(catalog,launchPreferences,nullptr,true);
        bool accepted=false;QEventLoop reading;
        startupDashboard->beginStartupNotice([&]{accepted=true;reading.quit();},[&]{reading.quit();});
        activateWindow=[&startupDashboard]{startupDashboard->showNormal();startupDashboard->raise();startupDashboard->activateWindow();};
        startupDashboard->show();startupDashboard->raise();startupDashboard->activateWindow();
        if(!startupCheck.isEmpty())QTimer::singleShot(3500,&reading,[&]{startupEvidence["timed_out"]=true;reading.quit();});
        if(!startupCheck.isEmpty())QTimer::singleShot(160,startupDashboard.get(),[&]{
            int visible=0;for(auto widget:QApplication::topLevelWidgets())if(widget->isWindow()&&widget->isVisible())++visible;
            QDir output(args.value("dashboard-output"));output.mkpath(QStringLiteral("."));
            startupEvidence={{"scope","same_native_dashboard_window_isolated_demo_no_provider"},{"action",startupCheck},{"process_id",qint64(QCoreApplication::applicationPid())},{"window_id_before",QString::number(quintptr(startupDashboard->winId()))},{"visible_windows_before",visible},{"runtime_constructed_before",false},{"notice_visible_before",startupDashboard->startupNoticePending()},{"before_screenshot_saved",startupDashboard->grab().save(output.filePath(QStringLiteral("startup-page-before.png")))}};
            if(startupCheck==QStringLiteral("close")){startupEvidence["action_triggered"]=true;startupDashboard->close();}
            else if(startupCheck==QStringLiteral("escape")){
                QKeyEvent key(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);auto page=startupDashboard->findChild<QWidget*>(QStringLiteral("startupNoticePage"));startupEvidence["action_triggered"]=page!=nullptr;QApplication::sendEvent(page? page:startupDashboard.get(),&key);
            }else if(auto button=startupDashboard->findChild<QPushButton*>(startupCheck==QStringLiteral("accept")?QStringLiteral("startupNoticeContinue"):QStringLiteral("startupNoticeExit"))){startupEvidence["action_triggered"]=true;button->click();}
        });
        reading.exec();activateWindow={};
        if(!startupCheck.isEmpty()){
            startupEvidence["accepted"]=accepted;startupEvidence["window_id_after_page"]=QString::number(quintptr(startupDashboard->winId()));
            startupEvidence["same_window_after_page"]=startupEvidence.value("window_id_before")==startupEvidence.value("window_id_after_page");
        }
        if(!accepted){
            if(!startupCheck.isEmpty()){
                startupEvidence["runtime_constructed_after"]=false;startupEvidence["passed"]=startupCheck!=QStringLiteral("accept")&&startupEvidence.value("action_triggered").toBool()&&!startupEvidence.value("timed_out").toBool()&&startupEvidence.value("notice_visible_before").toBool()&&startupEvidence.value("visible_windows_before").toInt()==1&&startupEvidence.value("same_window_after_page").toBool();
                return stopLogging(saveStartupCheck()&&startupEvidence.value("passed").toBool()?0:1);
            }
            return stopLogging(20);
        }
    }
    const auto panelStart=launchClock.elapsed();
    auto initialDashboard=startupDashboard.release();
    DualPanels panels(catalog,args,launchPreferences,initialDashboard,runtimeLog.get());
    if(!readyPath.isEmpty()){
        QSaveFile ready(readyPath);const auto body=QJsonDocument(QJsonObject{{"ready",true},{"process_id",qint64(QCoreApplication::applicationPid())}}).toJson(QJsonDocument::Compact);
        if(!ready.open(QIODevice::WriteOnly)||ready.write(body)!=body.size()||!ready.commit()){std::fprintf(stderr,"Unable to publish startup readiness.\n");panels.finish();panels.drainReports();return stopLogging(2);}
    }
    if(runtimeLog)runtimeLog->record(LogEvent::PanelReady,LogOutcome::Ready,launchClock.elapsed()-panelStart);
    if(exclusive)activateWindow=[&panels]{panels.showDashboard();};
    if(!startupCheck.isEmpty())QTimer::singleShot(350,&panels,[&]{
        startupEvidence["runtime_constructed_after"]=true;startupEvidence["window_id_after_runtime"]=QString::number(quintptr(initialDashboard->winId()));
        startupEvidence["same_window_after_runtime"]=startupEvidence.value("window_id_before")==startupEvidence.value("window_id_after_runtime");
        startupEvidence["notice_visible_after"]=initialDashboard->startupNoticePending();
        startupEvidence["after_screenshot_saved"]=initialDashboard->grab().save(QDir(args.value("dashboard-output")).filePath(QStringLiteral("startup-page-after.png")));
        startupEvidence["passed"]=startupEvidence.value("notice_visible_before").toBool()&&startupEvidence.value("visible_windows_before").toInt()==1&&startupEvidence.value("same_window_after_runtime").toBool()&&!initialDashboard->startupNoticePending()&&initialDashboard->isVisible();
        if(!saveStartupCheck())startupEvidence["passed"]=false;panels.finish();
    });
    if(args.isSet("quit-after-ms")){bool ok=false;const int ms=args.value("quit-after-ms").toInt(&ok);if(!ok||ms<500||ms>60000){panels.drainReports();return stopLogging(2);}QTimer::singleShot(ms,&panels,[&]{panels.finish();});}
    const int exitCode=app.exec();panels.drainReports();
    return stopLogging(!startupCheck.isEmpty()&&!startupEvidence.value("passed").toBool()?1:exitCode);
}
