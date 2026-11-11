#include "roster_analysis_bridge.h"
#include "../augment_panel/roster_recent_results_view.h"
#include "recommendation_history.h"
#include <QFileInfo>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTimer>
#include <cmath>
#include <memory>

namespace {
constexpr qsizetype inputLimit=65536,outputLimit=262144,errorLimit=65536;
constexpr qint64 refreshRetryIntervalMs=60000;
void copyCounts(QJsonObject &out,const QJsonObject &source,const QStringList &keys){
    for(const auto &key:keys){const auto value=source.value(key);const double number=value.toDouble(-1);if(value.isDouble()&&std::isfinite(number)&&number>=0&&std::floor(number)==number)out[key]=value;}
}
void copyFlags(QJsonObject &out,const QJsonObject &source,const QStringList &keys){for(const auto &key:keys)if(source.value(key).isBool())out[key]=source.value(key);}
QJsonArray publicReferences(const QJsonArray &source,int maximum){
    QJsonArray out;for(const auto &value:source){if(out.size()>=maximum)break;const auto row=value.toObject();QJsonObject clean;copyCounts(clean,row,{QStringLiteral("id"),QStringLiteral("games")});if(clean.value(QStringLiteral("id")).toDouble()>0&&clean.value(QStringLiteral("id")).toDouble()<=10000000&&clean.contains(QStringLiteral("games")))out.append(clean);}return out;
}
QJsonArray publicIds(const QJsonArray &source){
    QJsonArray out;for(const auto &value:source){if(out.size()>=32)break;const double id=value.toDouble(-1);if(value.isDouble()&&std::isfinite(id)&&id>0&&id<=10000000&&std::floor(id)==id)out.append(value);}return out;
}
QJsonObject publicProfile(const QJsonObject &source,bool champion=false){
    QJsonObject out;out[QStringLiteral("available")]=source.value(QStringLiteral("available")).toBool();
    if(source.value(QStringLiteral("reason"))==QStringLiteral("unsupported_schema"))out[QStringLiteral("reason")]=QStringLiteral("unsupported_schema");
    copyCounts(out,source,{QStringLiteral("eligible_games"),QStringLiteral("sampled_games"),QStringLiteral("known_outcomes"),QStringLiteral("wins"),QStringLiteral("champion_id"),QStringLiteral("all_patch_sampled_games"),QStringLiteral("same_patch_sampled_games")});
    copyFlags(out,source,{QStringLiteral("sample_limited"),QStringLiteral("patch_known"),QStringLiteral("reference_limit_applied")});
    if(source.contains(QStringLiteral("historical_win_rate"))){const auto rate=source.value(QStringLiteral("historical_win_rate"));const double number=rate.toDouble(-1);out[QStringLiteral("historical_win_rate")]=rate.isDouble()&&std::isfinite(number)&&number>=0&&number<=1?rate:QJsonValue();}
    if(champion){const auto patch=source.value(QStringLiteral("patch")).toString();static const QRegularExpression pattern(QStringLiteral("^[0-9]{1,2}\\.[0-9]{1,2}$"));out[QStringLiteral("patch")]=pattern.match(patch).hasMatch()?QJsonValue(patch):QJsonValue();}
    for(const auto &key:{QStringLiteral("champions"),QStringLiteral("fixed_items"),QStringLiteral("items"),QStringLiteral("augments")})if(source.value(key).isArray())out[key]=publicReferences(source.value(key).toArray(),key==QStringLiteral("champions")?4:key==QStringLiteral("fixed_items")?2:6);
    if(source.value(QStringLiteral("build_combinations")).isArray()){
        QJsonArray builds;for(const auto &value:source.value(QStringLiteral("build_combinations")).toArray()){if(builds.size()>=3)break;const auto row=value.toObject();QJsonObject clean;copyCounts(clean,row,{QStringLiteral("games")});if(!clean.contains(QStringLiteral("games")))continue;clean[QStringLiteral("item_ids")]=publicIds(row.value(QStringLiteral("item_ids")).toArray());clean[QStringLiteral("augment_ids")]=publicIds(row.value(QStringLiteral("augment_ids")).toArray());builds.append(clean);}out[QStringLiteral("build_combinations")]=builds;
    }
    out[QStringLiteral("evidence_stage")]=QStringLiteral("terminal_cooccurrence");out[QStringLiteral("choice_state_verified")]=false;out[QStringLiteral("choice_quality_known")]=false;if(!champion)out[QStringLiteral("fixed_items_replaceable")]=false;return out;
}
QJsonObject publicFreshness(const QJsonObject &source){
    QJsonObject out;const auto state=source.value(QStringLiteral("status")).toString();out[QStringLiteral("status")]=QStringList{QStringLiteral("fresh"),QStringLiteral("stale"),QStringLiteral("missing"),QStringLiteral("unknown")}.contains(state)?state:QStringLiteral("unknown");
    const auto refresh=source.value(QStringLiteral("refresh_status")).toString();out[QStringLiteral("refresh_status")]=QStringList{QStringLiteral("not_requested"),QStringLiteral("not_configured"),QStringLiteral("identity_unresolved"),QStringLiteral("fresh"),QStringLiteral("queued"),QStringLiteral("active"),QStringLiteral("succeeded"),QStringLiteral("failed"),QStringLiteral("expired"),QStringLiteral("pending"),QStringLiteral("done"),QStringLiteral("queue_unavailable")}.contains(refresh)?refresh:QStringLiteral("unknown");
    for(const auto &key:{QStringLiteral("history_checked_at_ms"),QStringLiteral("profile_checked_at_ms")}){QJsonObject clean;copyCounts(clean,source,{key});out[key]=clean.contains(key)?clean.value(key):QJsonValue();}
    copyCounts(out,source,{QStringLiteral("max_age_seconds"),QStringLiteral("reference_at_ms")});copyFlags(out,source,{QStringLiteral("refresh_requested"),QStringLiteral("source_halted")});const auto exhausted=source.value(QStringLiteral("accessible_history_exhausted"));out[QStringLiteral("accessible_history_exhausted")]=exhausted.isBool()?exhausted:QJsonValue();return out;
}
QJsonObject publicCoverage(const QJsonObject &source){
    QJsonObject out;copyCounts(out,source,{QStringLiteral("eligible_games"),QStringLiteral("sampled_games"),QStringLiteral("timeline_games"),QStringLiteral("timeline_sampled_games"),QStringLiteral("process_item_games"),QStringLiteral("process_augment_games"),QStringLiteral("growth_observation_games"),QStringLiteral("permanent_growth_observation_games"),QStringLiteral("real_offer_observations"),QStringLiteral("confirmed_choice_observations")});copyFlags(out,source,{QStringLiteral("sample_limited"),QStringLiteral("timeline_sample_limited"),QStringLiteral("observations_sample_limited")});out[QStringLiteral("all_time_history_complete")]=false;out[QStringLiteral("historical_alternatives_complete")]=false;return out;
}
QJsonObject publicLearning(const QJsonObject &source){
    QJsonObject out;const auto state=source.value(QStringLiteral("status")).toString();out[QStringLiteral("status")]=QStringList{QStringLiteral("await_actual_choices"),QStringLiteral("actual_offers_only"),QStringLiteral("observed_choices"),QStringLiteral("unknown")}.contains(state)?state:QStringLiteral("unknown");copyCounts(out,source,{QStringLiteral("real_offer_observations"),QStringLiteral("confirmed_choice_observations"),QStringLiteral("matched_result_observations")});out[QStringLiteral("recommendation_available")]=false;out[QStringLiteral("causal_effect_known")]=false;out[QStringLiteral("terminal_builds_are_choice_samples")]=false;return out;
}
QJsonObject publicMaterializedSummary(const QJsonObject &source){
    QJsonObject out;copyCounts(out,source,{QStringLiteral("games"),QStringLiteral("known_outcomes"),QStringLiteral("latest_ended_at_ms"),QStringLiteral("timeline_games"),QStringLiteral("recorded_augment_events")});
    if(source.contains(QStringLiteral("historical_win_rate"))){const auto rate=source.value(QStringLiteral("historical_win_rate"));const double number=rate.toDouble(-1);out[QStringLiteral("historical_win_rate")]=rate.isDouble()&&std::isfinite(number)&&number>=0&&number<=1?rate:QJsonValue();}
    out[QStringLiteral("mechanical_quality_known")]=false;return out;
}
QJsonObject publicConditionalPerformanceReference(const QJsonObject &source){
    const auto scope=QStringLiteral("same_champion_same_patch_leave_one_player_out");
    if(source.value(QStringLiteral("scope"))!=scope)return {};
    QJsonObject out{{QStringLiteral("scope"),scope},{QStringLiteral("causal_effect_known"),false}},components;
    const auto rawComponents=source.value(QStringLiteral("components")).toObject();
    for(const auto &field:{QStringLiteral("gold_earned_per_minute"),QStringLiteral("damage_to_champions_per_minute"),QStringLiteral("deaths_per_minute"),QStringLiteral("teammate_heal_per_minute"),QStringLiteral("shield_per_minute")}){
        if(!rawComponents.value(field).isObject())continue;
        const auto raw=rawComponents.value(field).toObject();QJsonObject clean;
        copyCounts(clean,raw,{QStringLiteral("own_participant_rows"),QStringLiteral("other_participant_rows"),QStringLiteral("other_distinct_players")});
        const bool supported=raw.value(QStringLiteral("status"))==QStringLiteral("supported_comparison")
            &&clean.value(QStringLiteral("own_participant_rows")).toDouble(-1)>=5
            &&clean.value(QStringLiteral("other_participant_rows")).toDouble(-1)>=20
            &&clean.value(QStringLiteral("other_distinct_players")).toDouble(-1)>=3;
        clean[QStringLiteral("status")]=supported?QStringLiteral("supported_comparison"):QStringLiteral("insufficient_samples");
        const auto direction=raw.value(QStringLiteral("direction")).toString();
        clean[QStringLiteral("direction")]=supported&&QStringList{QStringLiteral("above_reference"),QStringLiteral("below_reference"),QStringLiteral("similar_reference")}.contains(direction)?QJsonValue(direction):QJsonValue();
        components[field]=clean;
    }
    out[QStringLiteral("components")]=components;return out;
}
QJsonObject publicMultidimensionalProfile(const QJsonObject &source){
    QJsonObject out;const auto state=source.value(QStringLiteral("status")).toString();out[QStringLiteral("status")]=QStringList{QStringLiteral("known"),QStringLiteral("no_data"),QStringLiteral("duplicate_identity"),QStringLiteral("stale"),QStringLiteral("unverified")}.contains(state)?state:QStringLiteral("no_data");
    if(state!=QStringLiteral("known"))return out;
    for(const auto &key:{QStringLiteral("overall"),QStringLiteral("champion_experience")})if(source.value(key).isObject())out[key]=publicMaterializedSummary(source.value(key).toObject());
    for(const auto &key:{QStringLiteral("same_patch"),QStringLiteral("same_patch_champion")})if(source.value(key).isObject()){QJsonObject clean;copyCounts(clean,source.value(key).toObject(),{QStringLiteral("games")});out[key]=clean;}
    if(source.value(QStringLiteral("conditional_performance_reference")).isObject()){
        const auto clean=publicConditionalPerformanceReference(source.value(QStringLiteral("conditional_performance_reference")).toObject());
        if(!clean.isEmpty())out[QStringLiteral("conditional_performance_reference")]=clean;
    }
    const auto extension=sanitizeHistoricalProjectionExtension(source);for(auto it=extension.constBegin();it!=extension.constEnd();++it)out[it.key()]=it.value();
    if(source.value(QStringLiteral("ability_inference")).isObject())out[QStringLiteral("ability_inference")]=QJsonObject{{QStringLiteral("mechanics"),QStringLiteral("unknown")},{QStringLiteral("team_awareness"),QStringLiteral("unknown")}};return out;
}
QJsonObject publicMultidimensionalContext(const QJsonObject &source){
    QJsonObject out;const auto state=source.value(QStringLiteral("status")).toString();out[QStringLiteral("status")]=QStringList{QStringLiteral("ready"),QStringLiteral("unavailable"),QStringLiteral("historical_cutoff_requires_specific_prior"),QStringLiteral("stale"),QStringLiteral("unverified")}.contains(state)?state:QStringLiteral("unavailable");
    copyFlags(out,source,{QStringLiteral("freshness_verified"),QStringLiteral("requires_materialization")});
    if(state==QStringLiteral("ready")&&!source.value(QStringLiteral("freshness_verified")).toBool()){
        out[QStringLiteral("status")]=QStringLiteral("unverified");out[QStringLiteral("requires_materialization")]=true;
    }
    const auto freshness=source.value(QStringLiteral("freshness_status")).toString();if(QStringList{QStringLiteral("fresh"),QStringLiteral("stale"),QStringLiteral("unverified"),QStringLiteral("not_checked")}.contains(freshness))out[QStringLiteral("freshness_status")]=freshness;
    copyCounts(out,source,{QStringLiteral("schema_version"),QStringLiteral("as_of_ms"),QStringLiteral("materialized_at_ms")});
    const auto elapsed=source.value(QStringLiteral("lookup_elapsed_ms"));if(elapsed.isDouble()&&std::isfinite(elapsed.toDouble())&&elapsed.toDouble()>=0)out[QStringLiteral("lookup_elapsed_ms")]=elapsed;
    const auto revision=source.value(QStringLiteral("revision")).toString();static const QRegularExpression hash(QStringLiteral("^[a-fA-F0-9]{64}$"));if(hash.match(revision).hasMatch())out[QStringLiteral("revision")]=revision;return out;
}
QJsonObject publicSummary(const QJsonObject &source) {
    QJsonObject out;out[QStringLiteral("available")]=source.value(QStringLiteral("available")).toBool();
    const auto reason=source.value(QStringLiteral("reason")).toString();
    if(QStringList{QStringLiteral("patch_unknown"),QStringLiteral("champion_unknown"),QStringLiteral("patch_or_champion_unknown")}.contains(reason))out[QStringLiteral("reason")]=reason;
    for(const auto &key:{QStringLiteral("games"),QStringLiteral("sampled_games")})if(source.value(key).isDouble()&&source.value(key).toDouble()>=0)out[key]=source.value(key);
    out[QStringLiteral("sample_limited")]=source.value(QStringLiteral("sample_limited")).toBool();
    if(source.value(QStringLiteral("all_fields_missing")).isBool())out[QStringLiteral("all_fields_missing")]=source.value(QStringLiteral("all_fields_missing"));
    QJsonObject metrics,samples;QJsonArray missing;
    for(const auto &key:{QStringLiteral("kills"),QStringLiteral("deaths"),QStringLiteral("assists"),QStringLiteral("gold_earned"),QStringLiteral("damage_to_champions"),QStringLiteral("damage_taken"),QStringLiteral("kda"),QStringLiteral("gold_per_minute"),QStringLiteral("damage_per_minute"),QStringLiteral("taken_per_minute"),QStringLiteral("deaths_per_minute"),QStringLiteral("teammate_heal_per_minute"),QStringLiteral("shield_per_minute"),QStringLiteral("physical_damage_per_minute"),QStringLiteral("magic_damage_per_minute"),QStringLiteral("true_damage_per_minute")}){
        const auto value=source.value(QStringLiteral("metrics")).toObject().value(key),count=source.value(QStringLiteral("field_samples")).toObject().value(key);
        if(value.isDouble()&&std::isfinite(value.toDouble())&&value.toDouble()>=0)metrics[key]=value;else metrics[key]=QJsonValue::Null;
        if(count.isDouble()&&std::isfinite(count.toDouble())&&count.toDouble()>=0)samples[key]=count;
        if(source.value(QStringLiteral("missing_fields")).toArray().contains(key))missing.append(key);
    }
    out[QStringLiteral("metrics")]=metrics;out[QStringLiteral("field_samples")]=samples;out[QStringLiteral("missing_fields")]=missing;return out;
}
QJsonObject publicResult(const QJsonObject &source,const QString &context) {
    QJsonObject out{{QStringLiteral("schema_version"),1},{QStringLiteral("context_id"),context},{QStringLiteral("historical_reference_only"),true},
        {QStringLiteral("mode"),QStringLiteral("KIWI")},{QStringLiteral("queue_id"),2400}};
    const auto status=source.value(QStringLiteral("status")).toString();
    static const QStringList statuses{QStringLiteral("complete"),QStringLiteral("database_unavailable"),QStringLiteral("unsupported_schema"),QStringLiteral("timeout"),QStringLiteral("database_error"),QStringLiteral("invalid_input"),QStringLiteral("output_limit"),QStringLiteral("invalid_numeric_data")};
    out[QStringLiteral("status")]=statuses.contains(status)?status:QStringLiteral("invalid_output");
    copyFlags(out,source,{QStringLiteral("cached_only")});
    const auto patch=source.value(QStringLiteral("patch"));if(patch.isString()&&patch.toString().size()<=5)out[QStringLiteral("patch")]=patch;else out[QStringLiteral("patch")]=QJsonValue::Null;
    const auto at=source.value(QStringLiteral("as_of_ms"));if(at.isDouble()&&std::isfinite(at.toDouble())&&at.toDouble()>0)out[QStringLiteral("as_of_ms")]=at;
    const auto scoreContext=source.value(QStringLiteral("context_id")).toString()==context?sanitizeRosterScoreContext(source.value(QStringLiteral("score_context")).toObject(),at):QJsonObject{{QStringLiteral("status"),QStringLiteral("unknown")},{QStringLiteral("reason"),QStringLiteral("context_mismatch")}};
    out[QStringLiteral("score_context")]=scoreContext;
    if(source.value(QStringLiteral("multidimensional_context")).isObject())out[QStringLiteral("multidimensional_context")]=publicMultidimensionalContext(source.value(QStringLiteral("multidimensional_context")).toObject());
    QJsonArray players;for(const auto &value:source.value(QStringLiteral("players")).toArray()){
        if(players.size()>=10)break;const auto row=value.toObject();QJsonObject p;
        p[QStringLiteral("cell")]=row.value(QStringLiteral("cell")).toInt(-1);p[QStringLiteral("champion_id")]=row.value(QStringLiteral("champion_id")).toInt();p[QStringLiteral("self")]=row.value(QStringLiteral("self")).toBool();
        static const QRegularExpression slotPattern(QStringLiteral("^[0-9a-f]{32}$"));const auto slot=row.value(QStringLiteral("slot_token")).toString();if(slotPattern.match(slot).hasMatch())p[QStringLiteral("slot_token")]=slot;
        const auto team=row.value(QStringLiteral("team")).toString();p[QStringLiteral("team")]=QStringList{QStringLiteral("ORDER"),QStringLiteral("CHAOS"),QStringLiteral("ALLY"),QStringLiteral("ENEMY")}.contains(team)?team:QStringLiteral("UNKNOWN");
        const auto state=row.value(QStringLiteral("status")).toString();p[QStringLiteral("status")]=QStringList{QStringLiteral("known"),QStringLiteral("no_data"),QStringLiteral("fields_missing"),QStringLiteral("identity_missing"),QStringLiteral("ambiguous_identity"),QStringLiteral("duplicate_identity")}.contains(state)?state:QStringLiteral("no_data");
        if(state==QStringLiteral("no_data"))p[QStringLiteral("reason")]=source.value(QStringLiteral("cached_only")).toBool()&&row.value(QStringLiteral("history_snapshot")).toObject().value(QStringLiteral("status"))!=QStringLiteral("cached")?QStringLiteral("no_snapshot_before_match_cutoff"):QStringLiteral("no_eligible_local_history");
        for(const auto &key:{QStringLiteral("history"),QStringLiteral("same_patch"),QStringLiteral("same_champion"),QStringLiteral("same_patch_champion")})if(row.value(key).isObject())p[key]=publicSummary(row.value(key).toObject());
        const auto identity=row.value(QStringLiteral("identity_source")).toString();if(QStringList{QStringLiteral("server_stable"),QStringLiteral("library_unique_stable"),QStringLiteral("server_alias"),QStringLiteral("library_alias_server_unverified"),QStringLiteral("unresolved")}.contains(identity))p[QStringLiteral("identity_source")]=identity;
        const auto server=row.value(QStringLiteral("resolved_server")).toString();static const QRegularExpression serverPattern(QStringLiteral("^[A-Z0-9_]{2,24}$"));p[QStringLiteral("resolved_server")]=serverPattern.match(server).hasMatch()?QJsonValue(server):QJsonValue();
        if(row.value(QStringLiteral("profile_summary")).isObject())p[QStringLiteral("profile_summary")]=publicProfile(row.value(QStringLiteral("profile_summary")).toObject());
        if(row.value(QStringLiteral("champion_references")).isObject())p[QStringLiteral("champion_references")]=publicProfile(row.value(QStringLiteral("champion_references")).toObject(),true);
        if(row.value(QStringLiteral("freshness")).isObject())p[QStringLiteral("freshness")]=publicFreshness(row.value(QStringLiteral("freshness")).toObject());
        if(row.value(QStringLiteral("coverage")).isObject())p[QStringLiteral("coverage")]=publicCoverage(row.value(QStringLiteral("coverage")).toObject());
        p[QStringLiteral("recent_results_score")]=sanitizeRosterRecentScore(row.value(QStringLiteral("recent_results_score")).toObject(),scoreContext);
        if(row.value(QStringLiteral("learning_status")).isObject())p[QStringLiteral("learning_status")]=publicLearning(row.value(QStringLiteral("learning_status")).toObject());
        if(row.value(QStringLiteral("player_analysis")).isObject()){
            const auto rawAnalysis=row.value(QStringLiteral("player_analysis")).toObject();const auto analysisStatus=rawAnalysis.value(QStringLiteral("status")).toString();
            const bool sameCutoff=rawAnalysis.value(QStringLiteral("as_of_ms"))==out.value(QStringLiteral("as_of_ms"))&&out.value(QStringLiteral("as_of_ms")).isDouble();
            if(rawAnalysis.value(QStringLiteral("source"))==QStringLiteral("bounded_source_engine")&&sameCutoff&&QStringList{QStringLiteral("known"),QStringLiteral("no_data"),QStringLiteral("fields_missing"),QStringLiteral("unavailable")}.contains(analysisStatus)){
                auto clean=sanitizeHistoricalProjectionExtension(rawAnalysis);clean[QStringLiteral("status")]=analysisStatus;clean[QStringLiteral("source")]=QStringLiteral("bounded_source_engine");clean[QStringLiteral("as_of_ms")]=out.value(QStringLiteral("as_of_ms"));
                copyCounts(clean,rawAnalysis,{QStringLiteral("engine_version")});copyFlags(clean,rawAnalysis,{QStringLiteral("source_snapshot_verified")});
                const auto reason=rawAnalysis.value(QStringLiteral("reason")).toString();if(QStringList{QStringLiteral("identity_unresolved"),QStringLiteral("identity_missing"),QStringLiteral("no_data"),QStringLiteral("ambiguous_identity"),QStringLiteral("duplicate_identity")}.contains(reason))clean[QStringLiteral("reason")]=reason;
                p[QStringLiteral("player_analysis")]=clean;
            }else p[QStringLiteral("player_analysis")]=QJsonObject{{QStringLiteral("status"),QStringLiteral("unverified")}};
        }
        if(row.value(QStringLiteral("multidimensional_profile")).isObject()){
            const auto materialized=out.value(QStringLiteral("multidimensional_context")).toObject();
            if(materialized.value(QStringLiteral("status"))==QStringLiteral("ready")&&materialized.value(QStringLiteral("freshness_verified")).toBool())
                p[QStringLiteral("multidimensional_profile")]=publicMultidimensionalProfile(row.value(QStringLiteral("multidimensional_profile")).toObject());
            else p[QStringLiteral("multidimensional_profile")]=QJsonObject{{QStringLiteral("status"),materialized.value(QStringLiteral("status"))==QStringLiteral("stale")?QStringLiteral("stale"):QStringLiteral("unverified")}};
        }
        players.append(p);
    }
    out[QStringLiteral("players")]=players;return out;
}
}

RosterAnalysisBridge::RosterAnalysisBridge(QObject *parent):QObject(parent){
    refreshTimer.setParent(this);refreshTimer.setObjectName(QStringLiteral("playerHistoryRefresh"));refreshTimer.setInterval(15000);refreshTimer.setTimerType(Qt::CoarseTimer);
    connect(&refreshTimer,&QTimer::timeout,this,&RosterAnalysisBridge::refreshIfNeeded);
}
QJsonObject RosterAnalysisBridge::sanitizeAnonymousResult(const QJsonObject &source,const QString &context){return publicResult(source,context);}
bool RosterAnalysisBridge::sameHistoryRoster(const QJsonObject &a,const QJsonObject &b){
    for(const auto &name:{"context_id","as_of_ms","patch","server","mode","queue_id"})
        if(a.value(name).isNull()||a.value(name).isUndefined()||a.value(name)!=b.value(name))return false;
    if(a.value("context_id").toString().isEmpty()||a.value("server").toString().isEmpty())return false;
    auto identities=[](const QJsonObject &roster){QMap<QString,QJsonObject> out;const auto players=roster.value("players").toArray();if(players.isEmpty()||players.size()>10)return out;
        for(const auto &v:players){const auto p=v.toObject();const auto id=p.value("puuid").toString();if(id.isEmpty()||out.contains(id)||p.value("identity_merge_status")==QStringLiteral("conflict"))return QMap<QString,QJsonObject>{};QJsonObject row;for(const auto &name:{"champion_id","team","self","cell","slot_token","server"})row[name]=p.value(name);out[id]=row;}return out;};
    const auto first=identities(a);return !first.isEmpty()&&first==identities(b);
}
QJsonObject RosterAnalysisBridge::retainPregameHistory(const QJsonObject &current,const QJsonObject &previous){
    if(previous.value("status")!=QStringLiteral("complete")||current.value("status")!=QStringLiteral("complete"))return current;
    for(const auto &name:{"context_id","as_of_ms","patch"})if(previous.value(name).isUndefined()||previous.value(name)!=current.value(name))return current;
    auto out=current;auto rows=current.value("players").toArray();int retained=0;
    for(int i=0;i<rows.size()&&i<10;++i){const auto row=rows[i].toObject();if(row.value("status")!=QStringLiteral("no_data"))continue;
        QJsonObject match;int matches=0;for(const auto &v:previous.value("players").toArray()){const auto old=v.toObject();bool same=true;for(const auto &name:{"cell","team","champion_id","self","slot_token"})same&=old.value(name)==row.value(name);if(same&&(old.value("status")==QStringLiteral("known")||old.value("status")==QStringLiteral("fields_missing"))){match=old;++matches;}}
        if(matches!=1)continue;auto freshness=match.value("freshness").toObject();freshness["status"]="stale";match["freshness"]=freshness;match["pregame_context_retained"]=true;rows[i]=match;++retained;
    }
    out["players"]=rows;out["pregame_context_retained_players"]=retained;return out;
}
bool RosterAnalysisBridge::usesCachedOnly(const QJsonObject &roster){return roster.value(QStringLiteral("phase"))!=QStringLiteral("ChampSelect");}
QStringList RosterAnalysisBridge::adapterArguments(const QString &root,const QJsonObject &roster){
    QStringList arguments{QStringLiteral("-I"),QStringLiteral("-B"),root+QStringLiteral("/scripts/live_roster_analysis.py"),QStringLiteral("--db"),root+QStringLiteral("/.local/player-intelligence/players.sqlite3"),QStringLiteral("--no-refresh")};
    if(usesCachedOnly(roster))arguments.append(QStringLiteral("--cached-only"));return arguments;
}
void RosterAnalysisBridge::setGamePhase(const QString &phase){if(privateRoster.isEmpty()||privateRoster.value(QStringLiteral("phase"))==phase)return;auto next=privateRoster;next[QStringLiteral("phase")]=phase;request(next);}
void RosterAnalysisBridge::configure(const QString &root,const QString &python){invalidate();workspace=root;pythonPath=python;}
void RosterAnalysisBridge::invalidate(){++generation;refreshTimer.stop();privateRoster={};retainedRoster={};retainedResult={};databaseBasis.clear();nextRefreshAtMs=0;nextRefreshReason.clear();if(pending){pending->disconnect(this);pending->kill();pending->deleteLater();pending=nullptr;++discarded;}contextId.clear();status=QStringLiteral("idle");}
QJsonObject RosterAnalysisBridge::report() const{return {{QStringLiteral("status"),status},{QStringLiteral("pending"),!pending.isNull()},{QStringLiteral("requests"),requests},{QStringLiteral("completed"),completed},{QStringLiteral("failures"),failures},{QStringLiteral("discarded"),discarded},{QStringLiteral("network_requests"),0},{QStringLiteral("refresh_enqueue_enabled"),false},{QStringLiteral("cached_only"),usesCachedOnly(privateRoster)},{QStringLiteral("database_read_only"),true},{QStringLiteral("automatic_refresh_enabled"),refreshTimer.isActive()},{QStringLiteral("refresh_interval_ms"),15000},{QStringLiteral("automatic_refreshes"),automaticRefreshes},{QStringLiteral("next_due_at_ms"),nextRefreshAtMs>0?QJsonValue(nextRefreshAtMs):QJsonValue()},{QStringLiteral("next_due_reason"),nextRefreshReason.isEmpty()?QJsonValue():QJsonValue(nextRefreshReason)},{QStringLiteral("refresh_retry_interval_ms"),refreshRetryIntervalMs}};}
QByteArray RosterAnalysisBridge::databaseRevision()const{
    QByteArray revision;
    if(usesCachedOnly(privateRoster)){
        for(const auto &relative:{QStringLiteral("/.local/player-intelligence/score-index/latest.json"),QStringLiteral("/.local/player-intelligence/roster-snapshot-cache/snapshots.sqlite3")}){const QFileInfo file(workspace+relative);revision+=file.exists()?QByteArray::number(file.size())+":"+QByteArray::number(file.lastModified().toMSecsSinceEpoch()):QByteArrayLiteral("missing");revision+=';';}return revision;
    }
    // Both SQLite checkpoints and uncheckpointed WAL commits can change the view.
    for(const auto &relative:{QStringLiteral("/.local/player-intelligence/players.sqlite3"),QStringLiteral("/.local/player-intelligence/players.sqlite3-wal"),QStringLiteral("/.local/player-intelligence/live-bridge/revision.json"),QStringLiteral("/.local/player-intelligence/roster-refresh/revision.json"),QStringLiteral("/.local/player-intelligence/analysis/latest.json")}){
        const QFileInfo file(workspace+relative);revision+=file.exists()?QByteArray::number(file.size())+":"+QByteArray::number(file.lastModified().toMSecsSinceEpoch()):QByteArrayLiteral("missing");revision+=';';
    }
    return revision;
}
void RosterAnalysisBridge::refreshIfNeeded(){
    if(privateRoster.isEmpty()||contextId.isEmpty()||pending)return;
    const auto next=databaseRevision();
    const bool due=nextRefreshAtMs>0&&QDateTime::currentMSecsSinceEpoch()>=nextRefreshAtMs;
    if(next==databaseBasis&&!due)return;
    ++generation;++automaticRefreshes;startRequest();
}
void RosterAnalysisBridge::scheduleNextRefresh(const QJsonObject &anonymousResult){
    nextRefreshAtMs=0;nextRefreshReason.clear();const auto now=QDateTime::currentMSecsSinceEpoch();
    if(usesCachedOnly(privateRoster)){nextRefreshAtMs=now+15000;nextRefreshReason=QStringLiteral("cached_snapshot_read");return;}
    const auto schedule=[&](qint64 when,const QString &reason){if(nextRefreshAtMs==0||when<nextRefreshAtMs){nextRefreshAtMs=when;nextRefreshReason=reason;}};
    const auto state=anonymousResult.value(QStringLiteral("status")).toString();
    if(state!=QStringLiteral("complete")){
        if(QStringList{QStringLiteral("timeout"),QStringLiteral("database_error"),QStringLiteral("database_unavailable"),QStringLiteral("adapter_unavailable"),QStringLiteral("process_error")}.contains(state))schedule(now+refreshRetryIntervalMs,QStringLiteral("adapter_retry"));
        return;
    }
    const auto timestamp=[](const QJsonValue &value)->qint64{const double number=value.toDouble(-1);return value.isDouble()&&std::isfinite(number)&&number>0&&number<4102444800000.&&std::floor(number)==number?qint64(number):0;};
    for(const auto &value:anonymousResult.value(QStringLiteral("players")).toArray()){
        const auto freshness=value.toObject().value(QStringLiteral("freshness")).toObject();if(freshness.isEmpty())continue;
        const auto refresh=freshness.value(QStringLiteral("refresh_status")).toString();
        if(refresh==QStringLiteral("identity_unresolved"))continue;
        if(freshness.value(QStringLiteral("source_halted")).toBool()||QStringList{QStringLiteral("queued"),QStringLiteral("active"),QStringLiteral("failed"),QStringLiteral("expired"),QStringLiteral("pending"),QStringLiteral("queue_unavailable")}.contains(refresh))schedule(now+refreshRetryIntervalMs,QStringLiteral("refresh_retry"));
        if(freshness.value(QStringLiteral("status"))!=QStringLiteral("fresh")){schedule(now+refreshRetryIntervalMs,QStringLiteral("refresh_retry"));continue;}
        const auto reference=timestamp(freshness.value(QStringLiteral("reference_at_ms")));
        auto checked=timestamp(freshness.value(QStringLiteral("history_checked_at_ms")));
        if(!checked)checked=timestamp(freshness.value(QStringLiteral("profile_checked_at_ms")));
        if(!checked)checked=reference;
        const auto rawAge=freshness.value(QStringLiteral("max_age_seconds"));const double age=rawAge.toDouble(-1);
        if(!reference||!checked||checked>reference||!rawAge.isDouble()||!std::isfinite(age)||age<=0||age>86400||std::floor(age)!=age){schedule(now+refreshRetryIntervalMs,QStringLiteral("refresh_retry"));continue;}
        const auto expiry=checked+qint64(age)*1000+1;
        schedule(expiry>now?expiry:now+refreshRetryIntervalMs,expiry>now?QStringLiteral("freshness_expiry"):QStringLiteral("refresh_retry"));
    }
}
void RosterAnalysisBridge::fail(QProcess *process,quint64 ticket,const QString &code){
    if(ticket!=generation||pending!=process)return;
    pending=nullptr;process->disconnect(this);process->kill();process->deleteLater();status=code;++failures;scheduleNextRefresh(QJsonObject{{QStringLiteral("status"),code}});
    emit result(QJsonObject{{QStringLiteral("schema_version"),1},{QStringLiteral("context_id"),contextId},{QStringLiteral("status"),code},{QStringLiteral("historical_reference_only"),true},{QStringLiteral("players"),QJsonArray{}}});
}
void RosterAnalysisBridge::request(const QJsonObject &roster){
    const auto previous=sameHistoryRoster(retainedRoster,roster)?retainedResult:QJsonObject{};
    invalidate();const auto id=roster.value(QStringLiteral("context_id")).toString();const auto input=QJsonDocument(roster).toJson(QJsonDocument::Compact);
    if(id.isEmpty()||id.size()>160||!roster.value(QStringLiteral("players")).isArray()||roster.value(QStringLiteral("players")).toArray().size()>10||input.size()>inputLimit){status=QStringLiteral("invalid_input");++failures;return;}
    contextId=id;privateRoster=roster;retainedRoster=roster;retainedResult=previous;refreshTimer.start();startRequest();
}
void RosterAnalysisBridge::startRequest(){
    const auto id=contextId;const auto input=QJsonDocument(privateRoster).toJson(QJsonDocument::Compact);
    nextRefreshAtMs=0;nextRefreshReason.clear();
    // Reuse the original cutoff on every refresh; never admit the current match.
    databaseBasis=databaseRevision();++requests;
    if(!QFileInfo::exists(pythonPath)||!QFileInfo::exists(workspace+QStringLiteral("/scripts/live_roster_analysis.py"))){status=QStringLiteral("adapter_unavailable");++failures;scheduleNextRefresh(QJsonObject{{QStringLiteral("status"),status}});emit result(QJsonObject{{QStringLiteral("context_id"),id},{QStringLiteral("status"),status},{QStringLiteral("players"),QJsonArray{}}});return;}
    auto process=new QProcess(this);pending=process;const auto ticket=generation;status=QStringLiteral("running");
    auto output=std::make_shared<QByteArray>();auto errors=std::make_shared<qsizetype>(0);
    auto deadline=new QTimer(process);deadline->setSingleShot(true);
    connect(deadline,&QTimer::timeout,this,[this,process,ticket]{fail(process,ticket,QStringLiteral("timeout"));});
    connect(process,&QProcess::readyReadStandardOutput,this,[this,process,ticket,output]{const auto bytes=process->readAllStandardOutput();if(output->size()+bytes.size()>outputLimit){fail(process,ticket,QStringLiteral("output_limit"));return;}*output+=bytes;});
    connect(process,&QProcess::readyReadStandardError,this,[this,process,ticket,errors]{*errors+=process->readAllStandardError().size();if(*errors>errorLimit)fail(process,ticket,QStringLiteral("output_limit"));});
    connect(process,&QProcess::errorOccurred,this,[this,process,ticket](QProcess::ProcessError){fail(process,ticket,QStringLiteral("process_error"));});
    connect(process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[this,process,ticket,id,output,deadline](int code,QProcess::ExitStatus exit){
        if(ticket!=generation||pending!=process){process->deleteLater();return;}deadline->stop();*output+=process->readAllStandardOutput();
        if(output->size()>outputLimit||exit!=QProcess::NormalExit||code!=0){fail(process,ticket,QStringLiteral("process_error"));return;}
        const auto doc=QJsonDocument::fromJson(*output);const auto raw=doc.object();
        if(!doc.isObject()||raw.value(QStringLiteral("schema_version")).toInt()!=1||raw.value(QStringLiteral("context_id")).toString()!=id){fail(process,ticket,QStringLiteral("invalid_output"));return;}
        pending=nullptr;process->deleteLater();auto clean=sanitizeAnonymousResult(raw,id);if(usesCachedOnly(privateRoster)||clean.value(QStringLiteral("cached_only")).toBool())clean=retainPregameHistory(clean,retainedResult);else if(clean.value(QStringLiteral("status"))==QStringLiteral("complete"))retainedResult=clean;status=clean.value(QStringLiteral("status")).toString();scheduleNextRefresh(clean);if(status==QStringLiteral("complete"))++completed;else ++failures;emit result(clean);
    });
    connect(process,&QProcess::started,this,[process,input]{process->write(input);process->closeWriteChannel();});
    auto environment=QProcessEnvironment::systemEnvironment();environment.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"),QStringLiteral("1"));process->setProcessEnvironment(environment);
    process->setProgram(pythonPath);process->setArguments(adapterArguments(workspace,privateRoster));
    deadline->start(5000);process->start();
}
