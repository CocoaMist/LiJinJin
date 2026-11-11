#include "dashboard_match_view.h"
#include "roster_recent_results_view.h"
#include "catalog.h"
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStringList>
#include <algorithm>
#include <cmath>

namespace {
bool number(const QJsonValue &v){return v.isDouble()&&std::isfinite(v.toDouble())&&v.toDouble()>=0;}
bool integer(const QJsonValue &v,double maximum=100000000){return number(v)&&v.toDouble()<=maximum&&std::floor(v.toDouble())==v.toDouble();}
int count(const QJsonValue &v){return number(v)&&v.toDouble()<=100000000&&std::floor(v.toDouble())==v.toDouble()?int(v.toDouble()):0;}
int cell(const QJsonObject &v){const auto n=v.value("cell");return number(n)&&n.toDouble()<20&&std::floor(n.toDouble())==n.toDouble()?int(n.toDouble()):-1;}
QString side(const QJsonObject &p,const QString &own){const auto team=p.value("team").toString();if(team=="ALLY"||team=="ENEMY")return team;if(!own.isEmpty()&&(team=="ORDER"||team=="CHAOS"))return team==own?QStringLiteral("ALLY"):QStringLiteral("ENEMY");return QStringLiteral("UNKNOWN");}
QString ownTeam(const QJsonArray &rows){int selves=0;QString team;for(const auto &v:rows){const auto p=v.toObject();if(p.value("self").toBool()){++selves;team=p.value("team").toString();}}return selves==1&&(team=="ORDER"||team=="CHAOS")?team:QString();}
QJsonArray normalize(const QJsonArray &rows,const QString &own,const Catalog &catalog){
    static const QRegularExpression slotPattern(QStringLiteral("^[0-9a-f]{32}$"));QJsonArray out;for(const auto &v:rows){if(out.size()==10)break;const auto p=v.toObject();const int champion=count(p.value("champion_id"));QJsonObject clean{{"champion_id",catalog.champions.contains(champion)?champion:0},{"team",side(p,own)},{"self",p.value("self").toBool()}};if(cell(p)>=0)clean["cell"]=cell(p);const auto slot=p.value("slot_token").toString();if(slotPattern.match(slot).hasMatch())clean["slot_token"]=slot;out.append(clean);}return out;
}
int groupSize(const QJsonArray &rows,const QJsonObject &row){int n=0;for(const auto &v:rows){const auto p=v.toObject();n+=p.value("champion_id")==row.value("champion_id")&&p.value("team")==row.value("team");}return n;}
QJsonObject uniqueMatch(const QJsonArray &base,const QJsonObject &row,const QJsonArray &candidates,const QString &own){
    if(row.value("champion_id").toInt()<=0||row.value("team")==QStringLiteral("UNKNOWN"))return {};
    QJsonArray matches;for(const auto &v:candidates){const auto p=v.toObject();if(p.value("champion_id")!=row.value("champion_id")||side(p,own)!=row.value("team").toString())continue;if(cell(row)>=0&&cell(p)>=0&&cell(row)!=cell(p))continue;matches.append(p);}
    if(matches.size()!=1)return {};const auto p=matches.first().toObject();
    if(groupSize(base,row)>1){if(cell(row)<0||cell(p)!=cell(row))return {};int sameCell=0;for(const auto &v:base){const auto other=v.toObject();sameCell+=other.value("team")==row.value("team")&&cell(other)==cell(row);}if(sameCell!=1)return {};}
    return p;
}
int sampleCount(const QJsonObject &support){return support.contains("sampled_games")?count(support.value("sampled_games")):count(support.value("games"));}
QJsonObject recentForSlot(const QJsonObject &row,const QJsonArray &candidates){const auto token=row.value("slot_token").toString();if(token.isEmpty())return {};QJsonObject result;int matches=0;for(const auto &v:candidates){const auto p=v.toObject();if(p.value("slot_token")==token&&p.value("team")==row.value("team")&&p.value("champion_id")==row.value("champion_id")){result=p;++matches;}}return matches==1?result:QJsonObject{};}
bool supported(const QJsonObject &support){return support.value("available").toBool()&&sampleCount(support)>0;}
QJsonObject historyView(const QJsonObject &p,bool indexReady){
    auto support=p.value("same_patch_champion").toObject();QString scope=QStringLiteral("same_patch_champion");if(!supported(support)){support=p.value("same_patch").toObject();scope=QStringLiteral("same_patch");}
    const auto profile=p.value("multidimensional_profile").toObject();const bool materialized=indexReady&&profile.value("status")==QStringLiteral("known");
    QJsonObject out{{"available",supported(support)},{"scope",scope},{"sampled_games",supported(support)?sampleCount(support):0},{"profile_games",0},{"champion_games",0},{"metrics",QJsonObject{}},{"comparison",QJsonArray{}},{"freshness","unknown"},{"refreshing",false}};
    if(supported(support)){
        QJsonObject metrics;const auto raw=support.value("metrics").toObject(),samples=support.value("field_samples").toObject();
        for(const auto &key:{"kills","deaths","assists","damage_per_minute","gold_per_minute","taken_per_minute"})if(count(samples.value(key))>0&&number(raw.value(key)))metrics[key]=raw.value(key);out["metrics"]=metrics;
    }
    const auto overall=p.value("profile_summary").toObject();if(overall.value("available").toBool())out["profile_games"]=sampleCount(overall);
    const auto references=p.value("champion_references").toObject();if(references.value("available").toBool())out["champion_games"]=count(references.value("same_patch_sampled_games"));
    if(materialized){out["profile_games"]=std::max(out.value("profile_games").toInt(),count(profile.value("overall").toObject().value("games")));out["champion_games"]=std::max(out.value("champion_games").toInt(),count(profile.value("same_patch_champion").toObject().value("games")));}
    const auto freshness=p.value("freshness").toObject();const auto status=freshness.value("status").toString();if(QStringList{"fresh","stale","missing","unknown"}.contains(status))out["freshness"]=status;
    const auto refresh=freshness.value("refresh_status").toString();
    out["refresh_status"]=QStringList{"queued","active","pending","complete","fresh","failed","error","cancelled","cooldown"}.contains(refresh)?refresh:QStringLiteral("unknown");
    out["refreshing"]=QStringList{"queued","active","pending"}.contains(refresh);
    if(materialized){
        const auto reference=profile.value("conditional_performance_reference").toObject();
        if(reference.value("scope")==QStringLiteral("same_champion_same_patch_leave_one_player_out")){
            const auto components=reference.value("components").toObject();QJsonArray comparisons;
            const QList<QStringList> fields{{"damage_to_champions_per_minute","damage",QStringLiteral("历史输出")},{"gold_earned_per_minute","economy",QStringLiteral("历史经济")},{"deaths_per_minute","survival",QStringLiteral("历史死亡")},{"teammate_heal_per_minute","healing",QStringLiteral("历史治疗")},{"shield_per_minute","shielding",QStringLiteral("历史护盾")}};
            for(const auto &field:fields){const auto raw=components.value(field[0]).toObject();const auto direction=raw.value("direction").toString();const int own=count(raw.value("own_participant_rows")),other=count(raw.value("other_participant_rows"));if(raw.value("status")!=QStringLiteral("supported_comparison")||own<5||other<20||count(raw.value("other_distinct_players"))<3||!QStringList{"above_reference","below_reference","similar_reference"}.contains(direction))continue;comparisons.append(QJsonObject{{"key",field[1]},{"label",field[2]},{"direction",direction},{"samples",own},{"reference_samples",other}});}out["comparison"]=comparisons;
        }
    }
    out["mechanics_known"]=false;out["team_awareness_known"]=false;return out;
}
QJsonObject smallJson(const QString &path,qint64 limit){QFile f(path);if(!f.open(QIODevice::ReadOnly)||f.size()<=0||f.size()>limit)return {};QJsonParseError error;const auto doc=QJsonDocument::fromJson(f.readAll(),&error);return error.error==QJsonParseError::NoError&&doc.isObject()?doc.object():QJsonObject{};}
bool within(const QString &root,const QString &path){if(path.isEmpty())return false;const auto relative=QDir(root).relativeFilePath(path);return !QDir::isAbsolutePath(relative)&&relative!=QStringLiteral("..")&&!relative.startsWith(QStringLiteral("../"))&&relative!=QStringLiteral(".");}
}

QJsonObject buildDashboardMatchView(const Catalog &catalog,const QJsonObject &snapshot){
    const auto context=snapshot.value("match_context").toObject();QString stage=context.value("stage").toString();if(!QStringList{"champ_select","loading","in_game"}.contains(stage))stage.clear();
    const auto id=context.value("context_id").toString();const bool active=context.value("active").toBool()&&!id.isEmpty()&&!stage.isEmpty();const bool fresh=active&&stage==QStringLiteral("in_game")&&snapshot.value("live_data_fresh").toBool();
    const bool phaseValid=active&&(stage!=QStringLiteral("in_game")||fresh);const auto current=phaseValid&&fresh?snapshot.value("current_players").toArray():QJsonArray{};
    auto own=ownTeam(current);if(own.isEmpty())own=ownTeam(context.value("roster").toArray());
    const auto raw=phaseValid?context.value("roster").toArray():QJsonArray{};auto rows=normalize(raw.isEmpty()?current:raw,own,catalog);
    const auto history=snapshot.value("anonymous_player_history").toObject();const auto patch=snapshot.value("observed_patch").toString();static const QRegularExpression version(QStringLiteral("^[0-9]{1,2}\\.[0-9]{1,2}$"));
    const auto cutoff=history.value("as_of_ms"),started=context.value("started_at_ms");const bool timeValid=integer(started,8640000000000000.)&&started.toDouble()>0&&integer(cutoff,8640000000000000.)&&cutoff.toDouble()>0&&cutoff.toDouble()<=started.toDouble();
    const auto mode=snapshot.value("current_game_mode").toString();const auto queue=snapshot.value("current_queue_id");const bool kiwi=(mode==QStringLiteral("KIWI")||queue==QJsonValue(2400))&&(mode.isEmpty()||mode==QStringLiteral("KIWI"))&&(queue.isUndefined()||queue.isNull()||queue==QJsonValue(0)||queue==QJsonValue(2400));
    const bool historyValid=phaseValid&&kiwi&&version.match(patch).hasMatch()&&history.value("context_id").toString()==id&&history.value("patch").toString()==patch&&history.value("mode")==QStringLiteral("KIWI")&&history.value("queue_id")==QJsonValue(2400)&&history.value("historical_reference_only").toBool()&&history.value("status")==QStringLiteral("complete")&&timeValid;
    const auto index=history.value("multidimensional_context").toObject();const bool indexReady=historyValid&&index.value("status")==QStringLiteral("ready")&&index.value("as_of_ms")==cutoff;int matched=0,withSamples=0,withChampion=0,refreshing=0;
    const auto recent=buildRosterRecentResultsView(snapshot);
    QJsonArray players;for(const auto &value:rows){auto row=value.toObject();const auto observation=uniqueMatch(rows,row,current,own);
        if(!observation.isEmpty()){
            const int level=count(observation.value("level"));if(level>0&&level<=18)row["level"]=level;QJsonObject scores;for(const auto &key:{"kills","deaths","assists"})if(integer(observation.value("scores").toObject().value(key)))scores[key]=count(observation.value("scores").toObject().value(key));if(!scores.isEmpty())row["scores"]=scores;
            QJsonArray items;for(const auto &item:observation.value("items").toArray())if(catalog.items.contains(count(item)))items.append(count(item));row["items"]=items;
        }
        const auto reference=historyValid?uniqueMatch(rows,row,history.value("players").toArray(),own):QJsonObject{};auto detail=historyView(reference,indexReady);
        const bool identified=!reference.isEmpty()&&!QStringList{"identity_missing","ambiguous_identity","duplicate_identity"}.contains(reference.value("status").toString());if(!identified)detail=historyView({},false);
        if(identified&&(detail.value("profile_games").toInt()>0||detail.value("sampled_games").toInt()>0))++matched;if(detail.value("available").toBool())++withSamples;if(detail.value("champion_games").toInt()>0||(detail.value("available").toBool()&&detail.value("scope")==QStringLiteral("same_patch_champion")))++withChampion;if(detail.value("refreshing").toBool())++refreshing;row["history"]=detail;row["recent_results"]=recentForSlot(row,recent.value("players").toArray());players.append(row);
    }
    const auto indexStatus=!historyValid?QStringLiteral("unknown"):indexReady?QStringLiteral("ready"):index.value("status")==QStringLiteral("ready")?QStringLiteral("stale"):index.value("status").toString(QStringLiteral("unknown"));
    int queued=0,reading=0,failed=0;for(const auto &value:players){const auto refresh=value.toObject().value("history").toObject().value("refresh_status").toString();queued+=refresh=="queued"||refresh=="pending";reading+=refresh=="active";failed+=refresh=="failed"||refresh=="error";}
    QJsonObject coverage{{"players",players.size()},{"matched",matched},{"same_patch",withSamples},{"same_champion",withChampion},{"refreshing",refreshing},{"queued",queued},{"reading",reading},{"failed",failed},{"history_label",QStringLiteral("历史 %1/%2 已匹配").arg(matched).arg(players.isEmpty()?10:players.size())},{"index_status",indexStatus}};
    if(indexReady&&number(index.value("materialized_at_ms")))coverage["index_updated_at_ms"]=index.value("materialized_at_ms");
    QString summary;const auto analysis=snapshot.value("automatic_match_analysis").toObject().value("analysis").toObject();if(phaseValid&&analysis.value("context_id").toString()==id)summary=analysis.value("summary").toString().left(128);if(summary.isEmpty()&&phaseValid)summary=context.value("composition").toObject().value("summary").toString().left(128);
    return {{"stage",phaseValid?stage:QString()},{"active",active},{"live_fresh",fresh},{"players",players},{"history_context_valid",historyValid},{"coverage",coverage},{"summary",summary},{"recent_results",recent},{"identity_fields_retained",false},{"history_is_live_damage",false},{"uses_network",false}};
}

QJsonObject DashboardMatchViewCache::project(const QJsonObject &snapshot){
    QJsonObject next;
    for(const auto key:{"match_context","live_data_fresh","current_players","anonymous_player_history",
                        "observed_patch","current_game_mode","current_queue_id","automatic_match_analysis"})
        next[QString::fromLatin1(key)]=snapshot.value(QString::fromLatin1(key));
    if(builds>0&&next==input){++hits;return view;}
    input=next;view=buildDashboardMatchView(catalog,input);++builds;return view;
}
QJsonObject DashboardMatchViewCache::report() const{
    return {{"builds",builds},{"cache_hits",hits},{"freshness_in_cache_key",true},{"catalog_immutable",true}};
}

QJsonObject readDashboardCorpusSummary(const QString &projectRoot){
    struct Cache{QElapsedTimer elapsed;QJsonObject summary;};static QHash<QString,Cache> caches;const auto root=QDir(projectRoot).canonicalPath();if(root.isEmpty())return {{"available",false}};auto &cached=caches[root];if(cached.elapsed.isValid()&&cached.elapsed.elapsed()<2000)return cached.summary;cached.elapsed.start();cached.summary={{"available",false},{"uses_network",false}};
    const auto analysis=QDir(root+QStringLiteral("/.local/player-intelligence/analysis")).canonicalPath();if(analysis.isEmpty())return cached.summary;
    const auto pointerPath=QFileInfo(analysis+QStringLiteral("/latest.json")).canonicalFilePath();if(!within(analysis,pointerPath))return cached.summary;
    const auto pointer=smallJson(pointerPath,16384);const auto directory=QFileInfo(pointer.value("directory").toString()).canonicalFilePath();if(!within(analysis,directory)||!QFileInfo(directory).isDir())return cached.summary;
    const auto manifestPath=QFileInfo(directory+QStringLiteral("/manifest.json")).canonicalFilePath();if(!within(directory,manifestPath)||!within(analysis,manifestPath))return cached.summary;
    const auto manifest=smallJson(manifestPath,65536);const auto completed=manifest.value("completed_at_ms");if(manifest.value("schema_version").toInt()!=1||manifest.value("status")!=QStringLiteral("complete")||!number(completed)||completed.toDouble()<=0||completed!=pointer.value("completed_at_ms"))return cached.summary;
    const auto counts=manifest.value("counts").toObject();QJsonObject out{{"available",true},{"uses_network",false},{"historical_only",true},{"timeline_scope","all_modes"}};
    for(const auto &key:{"all_matches","kiwi_matches","analyzed_kiwi_matches","player_profiles","timeline_matches"}){const auto value=counts.value(key);if(!number(value)||value.toDouble()>100000000||std::floor(value.toDouble())!=value.toDouble())return cached.summary;out[key]=count(value);}
    if(out.value("analyzed_kiwi_matches").toInt()>out.value("kiwi_matches").toInt()||out.value("kiwi_matches").toInt()>out.value("all_matches").toInt()||out.value("timeline_matches").toInt()>out.value("all_matches").toInt())return cached.summary;
    const auto pointerAfter=smallJson(pointerPath,16384);if(pointerAfter.value("directory")!=pointer.value("directory")||pointerAfter.value("completed_at_ms")!=pointer.value("completed_at_ms"))return cached.summary;
    out["updated_at_ms"]=completed;cached.summary=out;if(caches.size()>8)caches.clear();return out;
}
