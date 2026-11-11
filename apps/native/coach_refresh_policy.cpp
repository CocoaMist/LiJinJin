#include "coach_refresh_policy.h"
#include "combat_observation_scope.h"
#include "combat.h"
#include "combat_sandbox.h"
#include "decision_context.h"
#include "headliner.h"
#include "mechanic_actions.h"
#include "purchase_planner.h"
#include "chain_purchase_planner.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
QString k(const char *s) { return QString::fromLatin1(s); }
QString hash(const QJsonObject &value) { return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(value).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex()); }
QJsonObject rankBasis(const QJsonObject &abilities) {
    QJsonObject out;
    for(const auto &slot:{k("Q"),k("W"),k("E"),k("R")}) {
        const auto ability=abilities.value(slot);const auto row=ability.toObject();
        out[slot]=QJsonObject{{k("present"),abilities.contains(slot)},{k("is_object"),ability.isObject()},
            {k("rank_present"),row.contains(k("abilityLevel"))},{k("rank"),row.value(k("abilityLevel"))}};
    }
    return out;
}
bool validClock(double snapshot,double now,double expiry) {
    return std::isfinite(snapshot)&&std::isfinite(now)&&std::isfinite(expiry)&&now>=snapshot&&(expiry<0||now<expiry);
}
bool finiteJson(const QJsonValue &value,int depth=0) {
    if(depth>16)return false;
    if(value.isDouble())return std::isfinite(value.toDouble());
    if(value.isArray()){for(const auto &child:value.toArray())if(!finiteJson(child,depth+1))return false;}
    else if(value.isObject()){const auto object=value.toObject();for(auto it=object.begin();it!=object.end();++it)if(!finiteJson(it.value(),depth+1))return false;}
    return true;
}
}

QString coachStructuralBasis(const GameState &state) {
    auto projected=state;projected.players=liveLoadoutBasis(state);projected.ap=0;projected.ad=0;projected.liveStats={};projected.abilities=rankBasis(state.abilities);
    std::sort(projected.owned.begin(),projected.owned.end());std::sort(projected.selected.begin(),projected.selected.end());
    // Keep offer ordering, duplicate/invalid inventory markers, forms, growth
    // counters, mechanic observations and their expiry windows in fingerprint.
    QList<QJsonObject> extra;
    for(const auto &raw:state.players) {
        const auto p=raw.toObject();QJsonObject row{{k("champion_id"),p.value(k("champion_id"))},{k("team"),p.value(k("team"))},{k("self"),p.value(k("self"))}};
        for(const auto &name:{k("ranks"),k("ranks_complete"),k("abilities_complete"),k("form"),k("form_known"),k("effect_counters"),k("effect_conditions")})
            row[name]=QJsonObject{{k("present"),p.contains(name)},{k("value"),p.value(name)}};
        row[k("abilities")]=QJsonObject{{k("present"),p.contains(k("abilities"))},{k("is_object"),p.value(k("abilities")).isObject()},{k("ranks"),rankBasis(p.value(k("abilities")).toObject())}};extra.append(row);
    }
    std::sort(extra.begin(),extra.end(),[](const QJsonObject &a,const QJsonObject &b) {return QJsonDocument(a).toJson(QJsonDocument::Compact)<QJsonDocument(b).toJson(QJsonDocument::Compact);});
    QJsonArray extras;for(const auto &row:extra)extras.append(row);
    return hash({{k("structural_fingerprint"),projected.fingerprint()},{k("observed_player_ranks_forms_growth"),extras},
        {k("required_observation_eligibility"),combatHealthEligibilityBasis(state)},
        {k("explicit_chain_basis"),explicitChainPurchaseCacheBasis(state)}});
}
QString coachHardModelBasis(const Catalog &catalog,const GameState &state,const QString &style,double allyExecution) {
    return hash({{k("root"),catalog.root},{k("patch"),catalog.patch},{k("combat_patch"),catalog.combat?catalog.combat->patch:QString()},{k("revision"),catalog.combat?catalog.combat->revision:QString()},
        {k("structure"),coachStructuralBasis(state)},{k("budget"),purchaseBudgetBasis(catalog,state)},{k("style"),style},{k("ally_execution"),allyExecution}});
}
QString coachSoftModelBasis(const GameState &state) {
    // The full panel still participates in the numerical model and its cache.
    // Only UI invalidation/publication classifies it as a bounded soft update.
    auto copy=state;copy.players=liveLoadoutBasis(state);
    return hash({{k("panel_fingerprint"),copy.fingerprint()},{k("live_stats"),state.liveStats},{k("health"),state.hp}});
}
QString coachCardModelBasis(const Catalog &catalog,const GameState &state,const QString &style) {
    auto projected=state;projected.players=liveLoadoutBasis(state);
    std::sort(projected.owned.begin(),projected.owned.end());std::sort(projected.selected.begin(),projected.selected.end());
    return hash({{k("root"),catalog.root},{k("patch"),catalog.patch},
        {k("revision"),catalog.combat?catalog.combat->revision:QString()},
        {k("structure"),coachStructuralBasis(state)},{k("panel"),projected.fingerprint()},{k("style"),style}});
}
QJsonObject coachCardMechanicValidity(const Catalog &catalog,const GameState &state) {
    QJsonObject out{{k("available"),false},{k("snapshot_game_time"),state.gameTime},{k("valid_until"),-1},
        {k("live_snapshot"),state.source==GameState::Source::Live},
        {k("source_received_at_ms"),state.received.isValid()?state.received.toMSecsSinceEpoch():qint64(-1)},
        {k("scope"),k("committed_owned_ledger_and_observed_event_window_boundaries")}};
    if(!catalog.combat||!std::isfinite(state.gameTime)||state.gameTime<0||state.level<1||state.level>18
        ||!catalog.combat->champions.contains(state.championId))return out;
    const auto &db=*catalog.combat;auto ctx=db.context(state.championId,state.level);
    ctx.ranks=referenceCombatRanks(db,state.championId,state.level);
    for(const auto &slot:{k("Q"),k("W"),k("E"),k("R")}){
        const auto rank=state.abilities.value(slot).toObject().value(k("abilityLevel"));
        if(rank.isDouble()&&std::isfinite(rank.toDouble())&&rank.toDouble()==std::floor(rank.toDouble())&&rank.toDouble()>=0&&rank.toDouble()<=6)ctx.ranks[slot]=rank.toInt();
    }
    int counters=0;for(auto it=state.effectCounters.cbegin();it!=state.effectCounters.cend();++it)
        if(++counters<=256&&it.key().size()<=128&&std::isfinite(it.value()))ctx.counters[it.key()]=it.value();
    for(const auto &condition:state.effectConditions)if(condition.size()<=128&&ctx.conditions.size()<256)ctx.conditions.insert(condition);
    ctx.counters[k("observed_game_time")]=state.gameTime;
    if(std::isfinite(state.mechanicBaselineTime)&&state.mechanicBaselineTime>=0&&state.mechanicBaselineTime<=state.gameTime)ctx.counters[k("mechanic_baseline_time")]=state.mechanicBaselineTime;
    const auto augments=effectiveSelectedAugments(catalog,state);
    QVector<QJsonObject> events;QSet<QByteArray> seen;double next=-1;
    auto boundary=[&](double time){if(std::isfinite(time)&&time>state.gameTime&&(next<0||time<next))next=time;};
    if(!state.explicitPurchaseChain.isEmpty()){
        const auto source=state.explicitPurchaseChain.value(k("source")).toObject();const auto until=source.value(k("valid_until_game_time"));
        const auto horizon=state.explicitPurchaseChain.value(k("scenario")).toObject().value(k("horizon"));
        const auto basis=explicitChainPurchaseBasis(catalog,state);
        if(!basis.value(k("available")).toBool()||state.explicitPurchaseChain.value(k("binding_sha256"))!=basis.value(k("binding_sha256"))
            ||source.value(k("kind"))!=k("caller_declared_scenario")||source.value(k("source_id")).toString().trimmed().isEmpty()
            ||source.value(k("context_id"))!=basis.value(k("context_id"))||source.value(k("patch"))!=basis.value(k("patch"))
            ||source.value(k("combat_revision"))!=basis.value(k("combat_revision"))||source.value(k("source_received_at_ms"))!=basis.value(k("source_received_at_ms"))
            ||!source.value(k("valid_from_game_time")).isDouble()||source.value(k("valid_from_game_time")).toDouble()>state.gameTime
            ||!horizon.isDouble()||!std::isfinite(horizon.toDouble())||horizon.toDouble()<=0
            ||!until.isDouble()||!std::isfinite(until.toDouble())||until.toDouble()<state.gameTime+horizon.toDouble())return out;
        boundary(until.toDouble());
    }
    const int start=std::max(0,int(state.mechanicObservations.size())-512);
    for(int index=start;index<state.mechanicObservations.size();++index){
        const auto raw=state.mechanicObservations[index];const auto event=raw.toObject();
        const auto id=event.value(k("id")),time=event.value(k("time"));
        if(!raw.isObject()||!id.isDouble()||!std::isfinite(id.toDouble())||id.toDouble()<1||id.toDouble()>std::numeric_limits<int>::max()
            ||std::floor(id.toDouble())!=id.toDouble()||!time.isDouble()||!std::isfinite(time.toDouble())||time.toDouble()<0
            ||!event.value(k("event")).isString()||event.value(k("event")).toString().isEmpty()
            ||(event.contains(k("facts"))&&(!event.value(k("facts")).isObject()||!finiteJson(event.value(k("facts"))))))continue;
        const auto source=event.value(k("source")).toString();
        const bool owned=source==k("item")&&state.owned.contains(id.toInt())&&db.items.contains(id.toInt());
        const bool selected=source==k("augment")&&augments.contains(id.toInt())&&db.augments.contains(id.toInt());
        if(!owned&&!selected)continue;
        const auto bytes=QJsonDocument(event).toJson(QJsonDocument::Compact);if(seen.contains(bytes))continue;seen.insert(bytes);
        // Future evidence is not replayed. Re-evaluate at its timestamp rather
        // than allowing the present decision to survive its admission boundary.
        boundary(time.toDouble());
        const auto count=event.value(k("facts")).toObject().value(k("numeric_effect_count"));
        if(count.isDouble()&&std::isfinite(count.toDouble())&&count.toDouble()>=0)
            for(double seconds:{3.,8.,20.})boundary(time.toDouble()+seconds);
        if(time.toDouble()<=state.gameTime)events.append(event);
    }
    std::stable_sort(events.begin(),events.end(),[](const QJsonObject &a,const QJsonObject &b){return a.value(k("time")).toDouble()<b.value(k("time")).toDouble();});
    QJsonArray observations;for(const auto &event:events)observations.append(event);
    const auto ledger=replayCombatMechanics(db,ctx,state.owned,augments,observations);
    for(const auto &raw:ledger.effects){const auto effect=raw.toObject();boundary(effect.value(k("start")).toDouble(-1));boundary(effect.value(k("expires")).toDouble(-1));}
    out[k("available")]=true;out[k("valid_until")]=next;out[k("committed_effects")]=ledger.effects.size();
    return out;
}
bool coachCardClockCurrent(const QJsonObject &validity,double currentGameTime,qint64 currentUtcMs) {
    const auto snapshot=validity.value(k("snapshot_game_time")),expiry=validity.value(k("valid_until"));
    if(validity.value(k("live_snapshot")).toBool()){
        const auto source=validity.value(k("source_received_at_ms"));
        if(currentUtcMs<0)currentUtcMs=QDateTime::currentMSecsSinceEpoch();
        if(!source.isDouble()||!std::isfinite(source.toDouble())||std::floor(source.toDouble())!=source.toDouble()
            ||source.toDouble()<0||source.toDouble()>double(currentUtcMs)||double(currentUtcMs)-source.toDouble()>=4000)return false;
    }
    return validity.value(k("available")).toBool()&&snapshot.isDouble()&&expiry.isDouble()
        &&snapshot.toDouble()>=0&&(expiry.toDouble()==-1||expiry.toDouble()>=snapshot.toDouble())
        &&validClock(snapshot.toDouble(),currentGameTime,expiry.toDouble());
}
QJsonObject CoachRefreshPolicy::publication(const QString &workerHard,const QString &currentHard,const QString &workerSoft,const QString &currentSoft,
    qint64 snapshotMs,qint64 nowMs,bool fresh,bool patchMatches,double snapshotGameTime,double currentGameTime,double mechanicValidUntil) {
    const qint64 age=nowMs-snapshotMs;QString reason;
    if(!fresh)reason=k("live_snapshot_not_fresh");
    else if(!patchMatches)reason=k("patch_mismatch");
    else if(workerHard.isEmpty()||workerHard!=currentHard)reason=k("structural_basis_changed");
    else if(snapshotMs<0||age<0||age>maximumPublishAgeMs)reason=k("model_snapshot_too_old");
    else if(!validClock(snapshotGameTime,currentGameTime,mechanicValidUntil))reason=k("mechanic_expired_or_clock_rewound");
    return {{k("accepted"),reason.isEmpty()},{k("reason"),reason.isEmpty()?k("accepted"):reason},{k("snapshot_age_ms"),age},
        {k("conditional"),true},{k("soft_inputs_changed"),workerSoft!=currentSoft},{k("snapshot_game_time"),snapshotGameTime}};
}
bool CoachRefreshPolicy::refreshDue(bool softChanged,qint64 publishedSnapshotMs,qint64 lastDispatchMs,qint64 nowMs) {
    return nowMs-lastDispatchMs>=minimumSoftDispatchMs&&(softChanged||publishedSnapshotMs<0||nowMs-publishedSnapshotMs>=periodicRefreshMs);
}
bool CoachRefreshPolicy::retain(qint64 publishedSnapshotMs,qint64 nowMs,bool hardMatches,bool fresh,bool patchMatches,
    double snapshotGameTime,double currentGameTime,double mechanicValidUntil) {
    return publishedSnapshotMs>=0&&nowMs>=publishedSnapshotMs&&nowMs-publishedSnapshotMs<=maximumRetainedAgeMs&&hardMatches&&fresh&&patchMatches&&validClock(snapshotGameTime,currentGameTime,mechanicValidUntil);
}
