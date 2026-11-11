#include "roster_recent_results_view.h"
#include <QJsonArray>
#include <QDateTime>
#include <QRegularExpression>
#include <QStringList>
#include <algorithm>
#include <cmath>

#define K(s) QStringLiteral(s)
namespace {
bool number(const QJsonValue &v,double maximum=100000000.){return v.isDouble()&&std::isfinite(v.toDouble())&&v.toDouble()>=0&&v.toDouble()<=maximum;}
bool integer(const QJsonValue &v,double maximum=100000000.){return number(v,maximum)&&std::floor(v.toDouble())==v.toDouble();}
bool timestamp(const QJsonValue &v){return integer(v,8640000000000000.)&&v.toDouble()>0;}
QString token(const QJsonValue &v){static const QRegularExpression pattern(K("^[A-Za-z0-9_.:-]{1,160}$"));return v.isString()&&pattern.match(v.toString()).hasMatch()?v.toString():QString();}
QString reason(const QJsonValue &v){static const QStringList allowed{K("insufficient_independent_matchups"),K("no_completed_valid_matches"),K("computation_budget_exceeded"),K("invalid_request"),K("identity_missing"),K("identity_unresolved"),K("duplicate_identity"),K("ambiguous_identity"),K("identity_conflict"),K("ambiguous_stable_identity"),K("stable_identity_missing"),K("roster_slot_ambiguous"),K("roster_identity_ambiguous"),K("index_player_missing"),K("index_unavailable"),K("score_index_unavailable"),K("score_snapshot_stale"),K("unsafe_score_snapshot"),K("source_or_index_unavailable"),K("index_build_busy"),K("unsupported_queue"),K("cache_missing"),K("index_missing"),K("invalid_score"),K("invalid_score_context"),K("context_mismatch"),K("future_snapshot"),K("epoch_mismatch"),K("model_mismatch"),K("future_score"),K("score_cutoff_mismatch"),K("invalid_score_metadata"),K("server_mismatch"),K("slot_missing"),K("duplicate_slot")};const auto value=v.toString();return allowed.contains(value)?value:K("invalid_score");}
QString reasonText(const QString &value){
    if(value==K("insufficient_independent_matchups"))return K("不同完整对阵不足");
    if(value==K("no_completed_valid_matches"))return K("暂无有效战果");
    if(value==K("identity_missing")||value==K("identity_unresolved")||value==K("stable_identity_missing"))return K("玩家身份未确认");
    if(value==K("roster_slot_ambiguous")||value==K("roster_identity_ambiguous"))return K("玩家对应有歧义");
    if(value==K("score_snapshot_stale"))return K("缓存已超过可用时限");
    if(value==K("score_cutoff_mismatch"))return K("缓存统计截止不一致");
    if(value==K("slot_missing"))return K("玩家位置未确认");
    if(value==K("server_mismatch"))return K("缓存大区不一致");
    if(value==K("duplicate_identity")||value==K("duplicate_slot"))return K("玩家位置有冲突");
    if(value==K("index_player_missing")||value==K("cache_missing")||value==K("index_missing"))return K("暂无已存分数");
    if(value==K("future_score")||value==K("future_snapshot"))return K("缓存晚于本局截止");
    if(value==K("model_mismatch"))return K("分数版本不同");
    if(value==K("context_mismatch"))return K("等待本局缓存");
    return K("暂无可用缓存");
}
QString side(const QJsonObject &row,const QString &own){const auto team=row.value(K("team")).toString();if(team==K("ALLY")||team==K("ENEMY"))return team;if(!own.isEmpty()&&(team==K("ORDER")||team==K("CHAOS")))return team==own?K("ALLY"):K("ENEMY");return K("UNKNOWN");}
int cell(const QJsonObject &row){return integer(row.value(K("cell")),99)?row.value(K("cell")).toInt():-1;}
QString slot(const QJsonObject &row){static const QRegularExpression pattern(K("^[0-9a-f]{32}$"));const auto value=row.value(K("slot_token")).toString();return pattern.match(value).hasMatch()?value:QString();}
QJsonObject unknown(const QString &why){return {{K("status"),K("unknown")},{K("value"),QJsonValue()},{K("reason"),why}};}
}
QString rosterRecentScoreModel(){return K("recent-results-beta22-half-life14-matchup-cap1.v1");}
QJsonObject sanitizeRosterScoreContext(const QJsonObject &raw,const QJsonValue &cutoff){
    auto out=unknown(K("invalid_score_context"));out[K("network_requests")]=0;out[K("cached_only")]=true;out[K("read_only")]=true;
    if(raw.value(K("status"))!=K("ready")){out[K("reason")]=reason(raw.value(K("reason")));return out;}
    static const QRegularExpression serverPattern(K("^[A-Z0-9_]{2,24}$"));
    if(!timestamp(cutoff)||raw.value(K("schema_version"))!=QJsonValue(1)||raw.value(K("model_version"))!=rosterRecentScoreModel()||token(raw.value(K("epoch"))).isEmpty()||!serverPattern.match(raw.value(K("server")).toString()).hasMatch())return out;
    if(raw.value(K("network_requests"))!=QJsonValue(0)||raw.value(K("cached_only"))!=QJsonValue(true)||raw.value(K("read_only"))!=QJsonValue(true))return out;
    const auto at=raw.value(K("score_as_of_ms")),published=raw.value(K("published_at_ms"));
    if(!timestamp(at)||!timestamp(published)||at.toDouble()>cutoff.toDouble()||published.toDouble()>cutoff.toDouble()||published.toDouble()<at.toDouble()){out[K("reason")]=K("future_snapshot");return out;}
    if(cutoff.toDouble()-at.toDouble()>6*60*60*1000.){out[K("reason")]=K("score_snapshot_stale");return out;}
    if(raw.contains(K("requested_as_of_ms"))&&raw.value(K("requested_as_of_ms"))!=cutoff)return out;
    out={{K("status"),K("ready")},{K("epoch"),token(raw.value(K("epoch")))},{K("server"),raw.value(K("server"))},{K("score_as_of_ms"),at},{K("published_at_ms"),published},{K("requested_as_of_ms"),cutoff},{K("model_version"),rosterRecentScoreModel()},{K("schema_version"),1},{K("network_requests"),0},{K("cached_only"),true},{K("read_only"),true},{K("stale"),raw.value(K("stale")).toBool()}};return out;
}
QJsonObject sanitizeRosterRecentScore(const QJsonObject &raw,const QJsonObject &context){
    if(context.value(K("status"))!=K("ready"))return unknown(context.value(K("reason")).toString(K("invalid_score_context")));
    const auto epoch=raw.value(K("epoch"));if(epoch!=context.value(K("epoch")))return unknown(K("epoch_mismatch"));
    if(raw.value(K("model_version"))!=rosterRecentScoreModel())return unknown(K("model_mismatch"));
    const auto at=raw.value(K("as_of_ms"));if(!timestamp(at)||at!=context.value(K("score_as_of_ms")))return unknown(K("score_cutoff_mismatch"));
    const auto server=raw.value(K("server")).toString();static const QRegularExpression serverPattern(K("^[A-Z0-9_]{2,24}$"));
    if(server!=context.value(K("server")).toString())return unknown(K("server_mismatch"));
    if(!serverPattern.match(server).hasMatch()||raw.value(K("window_days"))!=QJsonValue(30)||raw.value(K("queue_id"))!=QJsonValue(2400)||raw.value(K("mode"))!=K("KIWI"))return unknown(raw.value(K("status"))==K("unknown")?reason(raw.value(K("reason"))):K("invalid_score_metadata"));
    auto out=unknown(reason(raw.value(K("reason"))));for(const auto &key:{K("epoch"),K("model_version"),K("server"),K("as_of_ms"),K("window_days"),K("queue_id"),K("mode")})out[key]=raw.value(key);
    const auto observed=raw.value(K("observations")).toObject();QJsonObject observations;bool counts=true;
    for(const auto &key:{K("matches"),K("wins"),K("losses"),K("independent_matchups")}){const auto value=observed.value(key);if(integer(value,200))observations[key]=value;else counts=false;}
    if(number(observed.value(K("effective_n"))))observations[K("effective_n")]=observed.value(K("effective_n"));else counts=false;
    if(counts){const double matches=observations.value(K("matches")).toDouble(),groups=observations.value(K("independent_matchups")).toDouble();counts=observations.value(K("wins")).toDouble()+observations.value(K("losses")).toDouble()==matches&&groups<=matches&&observations.value(K("effective_n")).toDouble()<=groups+1e-9;if(counts)observations[K("effective_n")]=std::min(observations.value(K("effective_n")).toDouble(),groups);}
    out[K("observations")]=observations;
    const bool support=counts;const double effective=observations.value(K("effective_n")).toDouble();if(support)out[K("sample_support")]=QJsonObject{{K("level"),effective>=30?K("high"):effective>=10?K("medium"):K("low")}};
    const auto remaining=raw.value(K("remaining")).toObject().value(K("independent_matchups_to_score"));if(integer(remaining,1000))out[K("remaining")]=QJsonObject{{K("independent_matchups_to_score"),remaining}};
    if(raw.value(K("status"))==K("scored")&&number(raw.value(K("value")),100)&&counts&&observations.value(K("independent_matchups")).toInt()>=5&&observations.value(K("effective_n")).toDouble()>0&&support){out[K("status")]=K("scored");out[K("value")]=raw.value(K("value"));out.remove(K("reason"));}
    return out;
}
QJsonObject buildRosterRecentResultsView(const QJsonObject &snapshot){
    const auto match=snapshot.value(K("match_context")).toObject(),history=snapshot.value(K("anonymous_player_history")).toObject();const auto cutoff=match.value(K("started_at_ms"));const auto stage=match.value(K("stage")).toString();
    const bool mode=(snapshot.value(K("current_game_mode"))==K("KIWI")||snapshot.value(K("current_queue_id"))==QJsonValue(2400))&&(snapshot.value(K("current_game_mode")).toString().isEmpty()||snapshot.value(K("current_game_mode"))==K("KIWI"))&&(!snapshot.value(K("current_queue_id")).isDouble()||snapshot.value(K("current_queue_id"))==QJsonValue(0)||snapshot.value(K("current_queue_id"))==QJsonValue(2400));
    const bool valid=match.value(K("active")).toBool()&&!match.value(K("context_id")).toString().isEmpty()&&QStringList{K("champ_select"),K("loading"),K("in_game")}.contains(stage)&&mode&&timestamp(cutoff)&&history.value(K("context_id"))==match.value(K("context_id"))&&history.value(K("historical_reference_only"))==QJsonValue(true)&&history.value(K("as_of_ms"))==cutoff;
    auto context=valid?sanitizeRosterScoreContext(history.value(K("score_context")).toObject(),cutoff):unknown(K("context_mismatch"));
    auto roster=match.value(K("roster")).toArray(),references=history.value(K("players")).toArray();if(roster.size()>10||references.size()>10){context=unknown(K("invalid_request"));roster={};references={};}QString own;int selves=0;for(const auto &v:roster){const auto row=v.toObject();if(row.value(K("self")).toBool()){++selves;own=row.value(K("team")).toString();}}if(selves!=1||(own!=K("ORDER")&&own!=K("CHAOS")))own.clear();
    QJsonArray players;QJsonObject teams;int totalKnown=0;bool compatible=context.value(K("status"))==K("ready")&&roster.size()==10&&references.size()==10;QString common;double allySum=0,enemySum=0;int allyCount=0,enemyCount=0,allyKnown=0,enemyKnown=0;
    for(const auto &v:roster){if(players.size()>=10)break;const auto source=v.toObject();const auto team=side(source,own);const int champion=integer(source.value(K("champion_id")),100000)?source.value(K("champion_id")).toInt():0;QJsonObject row{{K("team"),team},{K("champion_id"),champion},{K("cell"),cell(source)>=0?QJsonValue(cell(source)):QJsonValue()},{K("slot_token"),slot(source)}};
        int slotCount=0,referenceSlots=0,candidates=0;QJsonObject selected;for(const auto &b:roster)if(!slot(source).isEmpty()&&slot(b.toObject())==slot(source))++slotCount;
        for(const auto &r:references){const auto candidate=r.toObject();if(slot(source).isEmpty()||slot(candidate)!=slot(source))continue;++referenceSlots;if(side(candidate,own)!=team||candidate.value(K("champion_id"))!=source.value(K("champion_id"))||(cell(source)>=0&&cell(candidate)>=0&&cell(candidate)!=cell(source)))continue;++candidates;selected=candidate;}
        auto score=unknown(K("index_player_missing"));if(context.value(K("status"))!=K("ready"))score=unknown(context.value(K("reason")).toString());else if(team==K("UNKNOWN")||champion<=0)score=unknown(K("identity_missing"));else if(slot(source).isEmpty())score=unknown(K("slot_missing"));else if(slotCount!=1||referenceSlots>1)score=unknown(K("duplicate_slot"));else if(candidates==1){const auto state=selected.value(K("status")).toString();score=QStringList{K("identity_missing"),K("ambiguous_identity"),K("duplicate_identity")}.contains(state)?unknown(state):sanitizeRosterRecentScore(selected.value(K("recent_results_score")).toObject(),context);if(score.value(K("status"))==K("scored")&&score.value(K("server"))!=selected.value(K("resolved_server")))score=unknown(K("server_mismatch"));}
        const bool known=score.value(K("status"))==K("scored");row[K("known")]=known;row[K("score")]=score;row[K("value")]=known?score.value(K("value")):QJsonValue();const auto observations=score.value(K("observations")).toObject();const auto level=score.value(K("sample_support")).toObject().value(K("level")).toString();
        const auto value=known?QString::number(score.value(K("value")).toDouble(),'f',1):K("—");row[K("marker")]=K("战果 ")+value;QString detail=known?K("近期 ")+value+K("/100"):K("近期战果 —");if(integer(observations.value(K("matches"))))detail+=K(" · 本人%1局").arg(observations.value(K("matches")).toInt());if(known)detail+=level==K("low")?K(" · 少样本"):level==K("medium")?K(" · 中等样本"):K(" · 较多样本");if(!known)detail+=K(" · ")+reasonText(score.value(K("reason")).toString());row[K("detail")]=detail;
        QStringList tooltip{K("已保存的近期战果分；不是官方隐藏分、实力评级或本局胜率预测。"),K("载入和局内只读本场截止前缓存，不查询他人历史或重新评分。")};if(known){tooltip.append(K("本人%1胜/%2负；%3组不同完整对阵；有效样本量%4。支持档位只描述样本量，并非准确率。").arg(observations.value(K("wins")).toInt()).arg(observations.value(K("losses")).toInt()).arg(observations.value(K("independent_matchups")).toInt()).arg(observations.value(K("effective_n")).toDouble(),0,'f',1));tooltip.append(K("统计窗口%1天，截至%2；已收录资料不代表完整历史。").arg(score.value(K("window_days")).toInt()).arg(QDateTime::fromMSecsSinceEpoch(qint64(score.value(K("as_of_ms")).toDouble())).toString(K("MM-dd HH:mm"))));}else{tooltip.append(reasonText(score.value(K("reason")).toString()));const auto remaining=score.value(K("remaining")).toObject().value(K("independent_matchups_to_score"));if(score.value(K("reason"))==K("insufficient_independent_matchups")&&integer(remaining,1000)&&remaining.toInt()>0)tooltip.append(K("还需%1组不同完整对阵才达到出分条件。").arg(remaining.toInt()));}tooltip.append(K("同一完整对阵总权重最多1；中立先验4局，时间半衰期14天；不假设各局统计独立。"));row[K("tooltip")]=tooltip.join(QChar(u'\n'));players.append(row);
        if(team==K("ALLY")){++allyCount;if(known){++allyKnown;allySum+=score.value(K("value")).toDouble();}}else if(team==K("ENEMY")){++enemyCount;if(known){++enemyKnown;enemySum+=score.value(K("value")).toDouble();}}else compatible=false;
        if(known){++totalKnown;const auto signature=score.value(K("epoch")).toString()+K("|")+score.value(K("model_version")).toString()+K("|")+score.value(K("server")).toString()+K("|")+QString::number(score.value(K("as_of_ms")).toDouble(),'f',0)+K("|")+QString::number(score.value(K("window_days")).toInt());if(common.isEmpty())common=signature;else if(signature!=common)compatible=false;}else compatible=false;
    }
    compatible=compatible&&allyCount==5&&enemyCount==5&&allyKnown==5&&enemyKnown==5;
    teams[K("ALLY")]=QJsonObject{{K("known"),allyKnown},{K("expected"),5},{K("mean"),compatible?QJsonValue(allySum/5):QJsonValue()}};teams[K("ENEMY")]=QJsonObject{{K("known"),enemyKnown},{K("expected"),5},{K("mean"),compatible?QJsonValue(enemySum/5):QJsonValue()}};
    QString summary=K("近期战果 · 队友%1/5有分 · 对手%2/5有分").arg(allyKnown).arg(enemyKnown);if(compatible)summary+=K(" · 均值 %1 / %2").arg(allySum/5,0,'f',1).arg(enemySum/5,0,'f',1);else if(totalKnown==10)summary+=K(" · 统计口径不同，暂不汇总");
    return {{K("present"),history.value(K("score_context")).isObject()&&match.value(K("active")).toBool()},{K("valid"),valid&&context.value(K("status"))==K("ready")},{K("context"),context},{K("players"),players},{K("teams"),teams},{K("known"),totalKnown},{K("expected"),10},{K("aggregate_allowed"),compatible},{K("summary"),summary},{K("network_requests"),0},{K("prediction"),false},{K("identity_fields_retained"),false}};
}
#undef K
