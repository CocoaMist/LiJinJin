#include "analysis.h"
#include "combat.h"
#include "headliner.h"
#include "purchase_planner.h"
#include "chain_purchase_planner.h"
#include "decision_context.h"
#include "role_guidance.h"
#include "build_reasoning.h"
#include "external_strategy_reference.h"
#include "growth_objective.h"
#include "combat_sandbox.h"
#include <QCache>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QElapsedTimer>
#include <algorithm>
#include <array>
#include <cmath>

namespace {
void attachEnemyKillCounts(GameState &state, const QJsonObject &snapshot) {
    constexpr int rosterSize=10;
    if(state.players.size()!=rosterSize||state.gameMode!=QStringLiteral("KIWI"))return;
    const auto gameClock=snapshot.value("gameData").toObject().value("gameTime");
    if(!gameClock.isDouble()||!std::isfinite(state.gameTime)||state.gameTime<0)return;

    const auto eventBlock=snapshot.value("events").toObject();
    const auto standard=eventBlock.value("Events"), alternate=eventBlock.value("EventList");
    if(!standard.isUndefined()&&!alternate.isUndefined()&&standard!=alternate)return;
    const auto events=(standard.isUndefined()?alternate:standard).toArray();
    if(events.isEmpty()||events.size()>50000)return;

    const auto rawPlayers=snapshot.value("allPlayers").toArray();
    QHash<QString,int> aliases;
    std::array<QString,rosterSize> teams;
    QString ownTeam;
    int orderCount=0;
    for(int i=0;i<rosterSize;++i){
        const auto row=state.players[i].toObject(), raw=rawPlayers[i].toObject();
        teams[i]=row.value("team").toString();
        if(teams[i]!=QStringLiteral("ORDER")&&teams[i]!=QStringLiteral("CHAOS"))return;
        if(teams[i]==QStringLiteral("ORDER"))++orderCount;
        if(row.value("self").toBool())ownTeam=teams[i];
        for(const auto &name:{raw.value("riotId").toString(),raw.value("summonerName").toString(),raw.value("riotIdGameName").toString()}){
            if(name.isEmpty())continue;
            const int previous=aliases.value(name,i);
            aliases.insert(name,previous==i?i:-1);
        }
    }
    if(ownTeam.isEmpty()||orderCount!=5)return;

    std::array<std::array<int,rosterSize>,rosterSize> counts{};
    std::array<int,rosterSize> attributed{};
    double lastTime=0.;
    for(int i=0;i<events.size();++i){
        const auto event=events[i].toObject();
        const auto id=event.value("EventID"), time=event.value("EventTime");
        const auto kind=event.value("EventName").toString();
        if(!id.isDouble()||id.toDouble()!=i||!time.isDouble()||!std::isfinite(time.toDouble())
           ||time.toDouble()<lastTime||time.toDouble()>state.gameTime+1.||kind.isEmpty())return;
        if(i==0&&kind!=QStringLiteral("GameStart"))return;
        lastTime=time.toDouble();
        if(kind!=QStringLiteral("ChampionKill"))continue;
        const int killer=aliases.value(event.value("KillerName").toString(),-1);
        const int victim=aliases.value(event.value("VictimName").toString(),-1);
        if(killer<0||victim<0||teams[killer]==ownTeam||teams[victim]!=ownTeam)continue;
        ++counts[killer][victim];
        ++attributed[killer];
    }
    // Reconcile each attacker with the same snapshot's scoreboard. A missed
    // or ambiguous kill leaves that row unknown, including its zero cells.
    for(int i=0;i<rosterSize;++i){
        if(teams[i]==ownTeam)continue;
        auto row=state.players[i].toObject();
        const auto score=row.value("scores").toObject().value("kills");
        if(!score.isDouble()||score.toDouble()!=attributed[i])continue;
        QJsonArray targets;
        for(int count:counts[i])targets.append(count);
        row["kills_by_roster_row"]=targets;
        state.players[i]=row;
    }
}

void attachGrowthReasoning(const Catalog &catalog,const GameState &state,int champion,QJsonObject &plan) {
    auto objective=plan.value(QStringLiteral("growth_objective")).toObject();
    if(objective.isEmpty())objective=plan.value(QStringLiteral("comparison")).toObject().value(QStringLiteral("growth_objective")).toObject();
    if(objective.isEmpty())objective=plan.value(QStringLiteral("current_comparison")).toObject().value(QStringLiteral("growth_objective")).toObject();
    if(objective.isEmpty()&&catalog.combat&&catalog.combat->champions.contains(champion)){
        auto context=catalog.combat->context(champion,state.fresh()?state.level:18);
        context.counters=state.effectCounters;context.conditions=state.effectConditions;
        context.ranks=referenceCombatRanks(*catalog.combat,champion,context.level);
        if(state.fresh())for(const auto &slot:{QStringLiteral("Q"),QStringLiteral("W"),QStringLiteral("E"),QStringLiteral("R")}){
            const auto rank=state.abilities.value(slot).toObject().value(QStringLiteral("abilityLevel"));
            if(rank.isDouble())context.ranks[slot]=rank.toInt();
        }
        const auto selected=effectiveSelectedAugments(catalog,state);
        if(state.fresh()&&!state.liveStats.isEmpty())anchorObservedCombatStats(*catalog.combat,context,state.owned,selected,state.liveStats,QStringLiteral("成长联动"));
        bindGrowthObjective(*catalog.combat,context,state.owned,selected);
        objective=describeGrowthObjective(*catalog.combat,context,state.owned,selected);
    }
    objective[QStringLiteral("fixed_direction_matching_enabled")]=false;
    objective[QStringLiteral("role_matching_enabled")]=false;
    objective[QStringLiteral("input_source")]=state.source==GameState::Source::Offline?QStringLiteral("explicit_offline_scenario"):QStringLiteral("current_state_with_unknowns_retained");
    objective[QStringLiteral("future_stacks_predicted")]=false;
    if(objective.value(QStringLiteral("summary")).toString().isEmpty())objective[QStringLiteral("summary")]=QStringLiteral("英雄与已选海克斯成长联动");
    plan[QStringLiteral("growth_objective")]=objective;plan[QStringLiteral("growth_reasoning")]=objective;
    plan[QStringLiteral("fixed_direction_matching_enabled")]=false;
    plan[QStringLiteral("resolved_preference")]=QStringLiteral("成长联动");
}
}

bool modelRulesUsable(const Catalog &catalog,const GameState &) {
    return !catalog.patch.isEmpty()&&catalog.combat&&catalog.combat->patch==catalog.patch;
}
QJsonObject modelRuleBasis(const Catalog &catalog,const GameState &state){
    const bool known=!state.observedPatch.isEmpty();
    const bool matched=modelRulesUsable(catalog,state)&&known&&state.observedPatch==catalog.patch;
    return {{QStringLiteral("rules_available"),modelRulesUsable(catalog,state)},
        {QStringLiteral("source_patch"),catalog.patch},{QStringLiteral("observed_patch"),state.observedPatch.isEmpty()?QJsonValue::Null:QJsonValue(state.observedPatch)},
        {QStringLiteral("version_match"),matched},{QStringLiteral("version_gate_enabled"),false},
        {QStringLiteral("cross_version_estimate"),!matched},{QStringLiteral("current_patch_verified"),matched},
        {QStringLiteral("patch_relation"),!known?QStringLiteral("unknown"):matched?QStringLiteral("match"):QStringLiteral("mismatch")},
        {QStringLiteral("scope"),QStringLiteral("local_rule_estimate_preserving_observed_inputs_and_source_version")}};
}
bool modelPatchMatches(const Catalog &catalog,const GameState &state) {
    if(catalog.patch.isEmpty()||!catalog.combat||catalog.combat->patch!=catalog.patch)return false;
    if(state.source==GameState::Source::Replay)return state.observedPatch.isEmpty()||state.observedPatch==catalog.patch;
    return state.source==GameState::Source::Offline||(!state.observedPatch.isEmpty()&&state.observedPatch==catalog.patch);
}
QString GameState::fingerprint() const {
    QJsonArray itemIds, offerIds, chosen;
    for (int i : owned) itemIds.append(i);
    for (int i : offers) offerIds.append(i);
    for (int i : selected) chosen.append(i);
    QJsonObject counters;for(auto i=effectCounters.begin();i!=effectCounters.end();++i)counters[i.key()]=i.value();
    QJsonObject stableStats=liveStats;stableStats.remove(QStringLiteral("currentHealth"));stableStats.remove(QStringLiteral("resourceValue"));
    QJsonArray growthFacts;
    for(const auto &value:growthObservations.value(QStringLiteral("accepted")).toArray()){
        const auto row=value.toObject();growthFacts.append(QJsonObject{{QStringLiteral("source"),row.value(QStringLiteral("source"))},
            {QStringLiteral("id"),row.value(QStringLiteral("id"))},{QStringLiteral("counter_key"),row.value(QStringLiteral("counter_key"))},
            {QStringLiteral("value"),row.value(QStringLiteral("value"))},{QStringLiteral("unit"),row.value(QStringLiteral("unit"))},
            {QStringLiteral("disposition"),row.value(QStringLiteral("disposition"))},{QStringLiteral("measurement_kind"),row.value(QStringLiteral("measurement_kind"))}});
    }
    QStringList conditions(effectConditions.begin(),effectConditions.end());conditions.sort();
    QJsonArray activeObservedEffects;
    for(int ix=0;ix<mechanicObservations.size();++ix){const auto event=mechanicObservations[ix].toObject();const double age=gameTime-event.value(QStringLiteral("time")).toDouble();
        if(age>=0&&age<8&&event.value(QStringLiteral("facts")).toObject().value(QStringLiteral("numeric_effect_count")).isDouble())activeObservedEffects.append(ix);}
    QJsonArray modelPlayers;
    for(const auto &value:players){auto player=value.toObject();player.remove("kills_by_roster_row");modelPlayers.append(value.isObject()?QJsonValue(player):value);}
    QJsonObject o{{QStringLiteral("match_context_id"),matchContext.value(QStringLiteral("context_id"))},{QStringLiteral("recommendation_evidence"),recommendationEvidence},{QStringLiteral("champion"), championId}, {QStringLiteral("queue"), queueId},
                  {QStringLiteral("phase"), phase}, {QStringLiteral("game_mode"), gameMode}, {QStringLiteral("items"), itemIds},
                  {QStringLiteral("offers"), offerIds}, {QStringLiteral("selected"), chosen},
                  {QStringLiteral("visible_players"),modelPlayers},
                  {QStringLiteral("source"),static_cast<int>(source)},
                  {QStringLiteral("level"),level}, {QStringLiteral("ap"),ap}, {QStringLiteral("ad"),ad},
                  {QStringLiteral("abilities"),abilities},{QStringLiteral("observed_patch"),observedPatch},{QStringLiteral("headliner_context"),headlinerBasis(headlinerContext)},
                  {QStringLiteral("headliner_observation_current"),!observedHeadliner(*this).isEmpty()},
                  {QStringLiteral("stable_live_stats"),stableStats},{QStringLiteral("effect_counters"),counters},
                  {QStringLiteral("validated_growth_facts"),growthFacts},
                  {QStringLiteral("effect_conditions"),QJsonArray::fromStringList(conditions)},
                  {QStringLiteral("mechanic_observations"),mechanicObservations},
                  {QStringLiteral("mechanic_baseline_time"),mechanicBaselineTime},
                  {QStringLiteral("active_observed_effect_window"),activeObservedEffects}};
    if(!explicitPurchaseChain.isEmpty())o[QStringLiteral("explicit_chain_basis")]=explicitChainPurchaseCacheBasis(*this);
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(o).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}
double resistanceMultiplier(double r) { return r >= 0 ? 100.0 / (100.0 + r) : 2.0 - 100.0 / (100.0 - r); }
DamageResult calculateDamage(const QJsonObject &rule, const DamageInput &i) {
    DamageResult r;
    const auto base = rule.value(QStringLiteral("base")).toArray();
    const auto coeff = rule.value(QStringLiteral("ap_ratio")).toArray();
    if (!std::isfinite(i.ap) || !std::isfinite(i.ad) || !std::isfinite(i.resistance)
        || !std::isfinite(i.percentPen) || !std::isfinite(i.flatPen) || !std::isfinite(i.modifier)
        || i.ap < 0 || i.ad < 0 || i.percentPen < 0 || i.percentPen > 1 || i.flatPen < 0 || i.modifier < 0) return r;
    if (rule.value(QStringLiteral("id")).toString() == QStringLiteral("basic_attack")) {
        r.raw = i.ad;
        r.formula = QStringLiteral("非暴击普攻基础部分 = 总攻击力 %1").arg(i.ad, 0, 'f', 1);
    } else {
        if (rule.value(QStringLiteral("champion_id")).toInt() != i.championId || i.rank < 1 || i.rank > base.size() || coeff.size() != base.size()) return r;
        r.raw = base.at(i.rank - 1).toDouble() + coeff.at(i.rank - 1).toDouble() * i.ap;
        r.formula = QStringLiteral("%1 + %2 × %3 AP = %4").arg(base.at(i.rank-1).toDouble()).arg(coeff.at(i.rank-1).toDouble()).arg(i.ap).arg(r.raw, 0, 'f', 2);
    }
    // Penetration cannot push positive resistance below zero; already negative resistance stays negative.
    r.effectiveResistance = i.resistance > 0 ? std::max(0.0, i.resistance * (1.0 - i.percentPen) - i.flatPen) : i.resistance;
    r.mitigated = r.raw * resistanceMultiplier(r.effectiveResistance) * i.modifier;
    r.scope = rule.value(QStringLiteral("scope")).toString();
    r.supported = true;
    return r;
}
GameState parseLiveData(const QJsonObject &o, const Catalog &catalog) {
    GameState s;
    const auto a = o.value(QStringLiteral("activePlayer")).toObject();
    const auto stats = a.value(QStringLiteral("championStats")).toObject();
    if (a.isEmpty() || stats.isEmpty() || !o.value(QStringLiteral("allPlayers")).isArray()) return s;
    s.source = GameState::Source::Live;
    // Numeric combat fields only; player identities never cross this boundary.
    for(auto it=stats.begin();it!=stats.end();++it)if(it.value().isDouble()&&std::isfinite(it.value().toDouble()))s.liveStats.insert(it.key(),it.value());
    s.phase = QStringLiteral("InProgress");
    s.received = QDateTime::currentDateTimeUtc();
    s.ap = stats.value(QStringLiteral("abilityPower")).toDouble();
    s.ad = stats.value(QStringLiteral("attackDamage")).toDouble();
    s.hp = stats.value(QStringLiteral("currentHealth")).toDouble();
    s.gold = a.value(QStringLiteral("currentGold")).toDouble();
    s.goldKnown=a.value(QStringLiteral("currentGold")).isDouble()&&std::isfinite(s.gold)&&s.gold>=0;
    s.level = a.value(QStringLiteral("level")).toInt(1);
    s.abilities = a.value(QStringLiteral("abilities")).toObject();
    s.gameTime = o.value(QStringLiteral("gameData")).toObject().value(QStringLiteral("gameTime")).toDouble();
    s.gameMode = o.value(QStringLiteral("gameData")).toObject().value(QStringLiteral("gameMode")).toString();
    const auto name = a.value(QStringLiteral("summonerName")).toString();
    const auto riotId = a.value(QStringLiteral("riotId")).toString();
    for (const auto &v : o.value(QStringLiteral("allPlayers")).toArray()) {
        const auto p = v.toObject();
        const int cid = catalog.championId(p.value(QStringLiteral("rawChampionName")).toString(p.value(QStringLiteral("championName")).toString()));
        const bool self = !riotId.isEmpty()?p.value(QStringLiteral("riotId")).toString()==riotId
                          :!name.isEmpty()&&p.value(QStringLiteral("summonerName")).toString()==name;
        QJsonArray items,utilityItems;bool inventoryComplete=p.value(QStringLiteral("items")).isArray();
        for (const auto &item : p.value(QStringLiteral("items")).toArray()) {
            const auto value=item.toObject().value(QStringLiteral("itemID"));
            const int id = value.toInt();
            if(!value.isDouble()||!std::isfinite(value.toDouble())||value.toDouble()!=id||id<0)inventoryComplete=false;
            // Observed slot-six Poro consumables have no combat stats/effects.
            // Preserve them separately; unknown extra items are never dropped.
            const auto slot=item.toObject().value(QStringLiteral("slot"));
            if(slot.isDouble()&&slot.toDouble()==6&&(id==2052||id==220013)){
                utilityItems.append(id);continue;
            }
            if (id > 0) { items.append(id); if (self) s.owned.append(id); }
        }
        if (self) { s.championId = cid; s.level = p.value(QStringLiteral("level")).toInt(s.level); }
        // Drop player names/identifiers at the parser boundary.
        QJsonObject publicScores;
        const auto scores=p.value(QStringLiteral("scores")).toObject();
        for(const auto &key:{QStringLiteral("kills"),QStringLiteral("deaths"),QStringLiteral("assists"),QStringLiteral("creepScore")})
            if(scores.value(key).isDouble() && std::isfinite(scores.value(key).toDouble()) && scores.value(key).toDouble()>=0
                &&scores.value(key).toDouble()==scores.value(key).toInt())
                publicScores.insert(key,scores.value(key));
        s.players.append(QJsonObject{{QStringLiteral("champion_id"), cid}, {QStringLiteral("team"), p.value(QStringLiteral("team"))},
                                     {QStringLiteral("self"), self}, {QStringLiteral("level"), p.value(QStringLiteral("level"))},
                                     {QStringLiteral("items"), items}, {QStringLiteral("utility_items"),utilityItems}, {QStringLiteral("items_complete"),inventoryComplete}, {QStringLiteral("scores"),publicScores},
                                     {QStringLiteral("is_dead"),p.value(QStringLiteral("isDead"))},
                                     {QStringLiteral("respawn_seconds"),p.value(QStringLiteral("respawnTimer")).isDouble()&&std::isfinite(p.value(QStringLiteral("respawnTimer")).toDouble())&&p.value(QStringLiteral("respawnTimer")).toDouble()>=0?p.value(QStringLiteral("respawnTimer")):QJsonValue(QJsonValue::Null)},
                                     {QStringLiteral("augment_ids"),QJsonArray{}},{QStringLiteral("augments_complete"),false}});
    }
    int selfCount=0;for(const auto &v:s.players)if(v.toObject().value(QStringLiteral("self")).toBool())++selfCount;
    if(selfCount!=1){s.championId=0;s.owned.clear();for(int i=0;i<s.players.size();++i){auto p=s.players[i].toObject();p[QStringLiteral("self")]=false;s.players[i]=p;}}
    else attachEnemyKillCounts(s,o);
    return s;
}
QJsonObject recommendations(const Catalog &c, const GameState &s, int cid, const QString &) {
    if(!s.explicitPurchaseChain.isEmpty())return currentPurchaseRecommendations(c,s,cid,QString());
    const QString style=QStringLiteral("成长联动");
    static thread_local QCache<QString,QJsonObject> cache(32);
    const auto key=c.root+(c.combat?c.combat->revision:QString())+heroEquipmentReasoningRevision(c)
        +currentExternalStrategyEvidence(c).value(QStringLiteral("revision")).toString()+s.fingerprint()+purchaseBudgetBasis(c,s)+QString::number(s.fresh())+QString::number(cid)+style;
    if(auto result=cache.object(key)){const double expiry=result->value(QStringLiteral("mechanic_valid_until")).toDouble(-1),snapshot=result->value(QStringLiteral("mechanic_snapshot_time")).toDouble();if(s.gameTime>=snapshot&&(expiry<0||s.gameTime<expiry))return *result;}
    auto result=optimizeCombatBuild(c,s,cid,style);
    if(!result.value(QStringLiteral("reference_only")).toBool() && cid==s.championId && s.fresh() && s.gameMode==QStringLiteral("KIWI") && modelRulesUsable(c,s))
        result=reconnectPurchasePlan(c,s,result,buildPurchaseSandbox(c,s,style));
    if(!result.contains(QStringLiteral("build_reasoning"))){QList<int> ids;for(const auto &v:result.value(QStringLiteral("items")).toArray())ids.append(v.toObject().value(QStringLiteral("item_id")).toInt());
        result[QStringLiteral("build_reasoning")]=heroEquipmentBuildReasoning(c,s,cid,ids);}
    attachGrowthReasoning(c,s,cid,result);
    cache.insert(key,new QJsonObject(result));return result;
}
QJsonObject currentPurchaseRecommendations(const Catalog &c,const GameState &s,int cid,const QString &,
                                           const RecommendationWorkPtr &work) {
    const QString style=QStringLiteral("成长联动");
    QElapsedTimer total;total.start();QJsonObject timings;
    QJsonArray held;
    for(int id:s.owned){
        const int price=c.combat&&c.combat->items.contains(id)?c.combat->items.value(id).price:-1;
        held.append(QJsonObject{{QStringLiteral("item_id"),id},{QStringLiteral("name"),c.itemName(id)},
            {QStringLiteral("price"),price>=0?QJsonValue(price):QJsonValue(QJsonValue::Null)},
            {QStringLiteral("owned"),true},{QStringLiteral("availability"),QStringLiteral("当前已持有")}});
    }
    QJsonObject plan{{QStringLiteral("current_purchase_only"),true},{QStringLiteral("items"),held},
        {QStringLiteral("alternatives"),QJsonArray{}},{QStringLiteral("route"),QStringLiteral("当前背包与下一件装备")},
        {QStringLiteral("method"),QStringLiteral("actual inventory baseline and exhaustive legal next-purchase sandbox; no six-slot search")},
        {QStringLiteral("resolved_preference"),resolvedCombatPreference(c,s,style)},
        {QStringLiteral("version_match"),modelPatchMatches(c,s)},
        {QStringLiteral("mechanic_snapshot_time"),s.gameTime},{QStringLiteral("mechanic_valid_until"),-1},
        {QStringLiteral("mechanic_cache_policy"),QStringLiteral("no_cross_snapshot_cache")},
        {QStringLiteral("all_combinations_enumerated"),false},{QStringLiteral("globally_optimal_full_build"),false},
        {QStringLiteral("ranking_available"),false},{QStringLiteral("enumeration_complete"),false},
        {QStringLiteral("search"),QJsonObject{{QStringLiteral("six_slot_search_executed"),false},
            {QStringLiteral("offer_build_search_executed"),false},{QStringLiteral("beam_width"),0},
            {QStringLiteral("evaluated_full_build_combinations"),0},{QStringLiteral("exhaustive"),false}}},
        {QStringLiteral("advice"),QJsonArray{QStringLiteral("仅比较真实背包下的合法下一件；成长、触发和队友收益仍受已建模条件限制。")}}};
    auto stopped=[&](){return work&&work->stopped();};
    auto finish=[&](bool interrupted=false){
        if(interrupted||stopped()){
            auto sandbox=plan.value(QStringLiteral("purchase_sandbox")).toObject();
            sandbox[QStringLiteral("status")]=QStringLiteral("interrupted");
            auto reason=work?work->stopReason():QString();
            if(reason.isEmpty())reason=sandbox.value(QStringLiteral("stop_reason")).toString(QStringLiteral("incomplete_enumeration"));
            sandbox[QStringLiteral("stop_reason")]=reason;
            sandbox[QStringLiteral("selected_item_id")]=0;sandbox[QStringLiteral("candidates")]=QJsonArray{};
            sandbox[QStringLiteral("conditional_predictions")]=QJsonArray{};
            sandbox[QStringLiteral("enumeration_complete")]=false;sandbox[QStringLiteral("ranking_available")]=false;
            sandbox.remove(QStringLiteral("selected_inventory"));sandbox.remove(QStringLiteral("selected_sandbox"));
            sandbox.remove(QStringLiteral("selected_score_basis"));sandbox.remove(QStringLiteral("recommendation_kind"));
            plan[QStringLiteral("purchase_sandbox")]=sandbox;plan[QStringLiteral("items")]=held;
            plan[QStringLiteral("alternatives")]=QJsonArray{};plan[QStringLiteral("ranking_available")]=false;
            plan[QStringLiteral("enumeration_complete")]=false;plan[QStringLiteral("computation_complete")]=false;
            plan[QStringLiteral("status")]=QStringLiteral("interrupted");
            plan[QStringLiteral("stop_reason")]=sandbox.value(QStringLiteral("stop_reason"));
            plan[QStringLiteral("route")]=QStringLiteral("本次计算未完成，暂无当前购买排名");
            plan.remove(QStringLiteral("growth_projection"));plan.remove(QStringLiteral("remaining_build_gold"));
            plan.remove(QStringLiteral("comparison"));
            if(!s.explicitPurchaseChain.isEmpty())plan=withdrawExplicitChainPurchaseSelection(plan);
        }else plan[QStringLiteral("computation_complete")]=true;
        if(!interrupted&&!stopped()&&s.explicitPurchaseChain.isEmpty())attachGrowthReasoning(c,s,cid,plan);
        plan[QStringLiteral("fixed_direction_matching_enabled")]=false;
        timings[QStringLiteral("total")]=double(total.nsecsElapsed())/1e6;
        plan[QStringLiteral("stage_timings_ms")]=timings;
        return plan;
    };
    if(stopped())return finish(true);
    if(cid!=s.championId||cid<=0||!s.fresh()||s.gameMode!=QStringLiteral("KIWI")||!modelRulesUsable(c,s)){
        plan[QStringLiteral("status")]=QStringLiteral("unavailable");
        plan[QStringLiteral("route")]=QStringLiteral("等待当前英雄与同版对局数据");
        return finish();
    }
    if(s.explicitPurchaseChain.isEmpty()&&combatRoleIntent(c,s,style).value(QStringLiteral("intent"))==QStringLiteral("ally_protection")){
        auto reference=protectionEquipmentReference(c,s,cid,style);
        reference[QStringLiteral("current_purchase_only")]=true;
        reference[QStringLiteral("status")]=QStringLiteral("conditional_reference");
        reference[QStringLiteral("computation_complete")]=true;
        reference[QStringLiteral("stage_timings_ms")]=QJsonObject{{QStringLiteral("total"),double(total.nsecsElapsed())/1e6}};
        attachGrowthReasoning(c,s,cid,reference);
        return reference;
    }
    QElapsedTimer stage;stage.start();
    const auto sandbox=buildPurchaseSandbox(c,s,style,work);
    timings[QStringLiteral("purchase_sandbox")]=double(stage.nsecsElapsed())/1e6;
    plan[QStringLiteral("purchase_sandbox")]=sandbox;
    plan[QStringLiteral("current_comparison")]=sandbox.value(QStringLiteral("current_comparison"));
    plan[QStringLiteral("target_context")]=sandbox.value(QStringLiteral("target_context"));
    if(sandbox.value(QStringLiteral("mechanic_snapshot_time")).isDouble())
        plan[QStringLiteral("mechanic_snapshot_time")]=sandbox.value(QStringLiteral("mechanic_snapshot_time"));
    if(sandbox.value(QStringLiteral("mechanic_valid_until")).isDouble())
        plan[QStringLiteral("mechanic_valid_until")]=sandbox.value(QStringLiteral("mechanic_valid_until"));
    if(sandbox.value(QStringLiteral("executed_mechanics")).isObject())
        plan[QStringLiteral("executed_mechanics")]=sandbox.value(QStringLiteral("executed_mechanics"));
    if(sandbox.value(QStringLiteral("current_comparison")).isObject())
        plan[QStringLiteral("comparison")]=sandbox.value(QStringLiteral("current_comparison"));
    if(stopped()||sandbox.value(QStringLiteral("status"))==QStringLiteral("interrupted"))return finish(true);
    // A partial detailed pass must never authorize a ranking through reconnect.
    const bool complete=sandbox.value(QStringLiteral("enumeration_complete")).toBool();
    if(complete){stage.restart();plan=reconnectPurchasePlan(c,s,plan,sandbox);
        timings[QStringLiteral("reconnect")]=double(stage.nsecsElapsed())/1e6;}
    else plan[QStringLiteral("route")]=sandbox.value(QStringLiteral("unavailable_reason"))==QStringLiteral("self_inventory_unobserved_or_inconsistent")
        ?QStringLiteral("背包信息不完整，等待刷新")
        :sandbox.value(QStringLiteral("unavailable_reason"))==QStringLiteral("skill_ranks_invalid")
        ?QStringLiteral("技能等级信息异常，等待刷新"):QStringLiteral("当前没有可核实的购买方案");
    plan[QStringLiteral("enumeration_complete")]=complete;
    plan[QStringLiteral("ranking_available")]=complete&&sandbox.value(QStringLiteral("ranking_available")).toBool();
    plan[QStringLiteral("status")]=sandbox.value(QStringLiteral("status"));
    if(plan.value(QStringLiteral("ranking_available")).toBool())plan[QStringLiteral("route")]=QStringLiteral("当前背包与下一件装备");
    plan[QStringLiteral("method")]=QStringLiteral("actual inventory baseline and legal next-purchase sandbox; no six-slot or offer-build search");
    plan[QStringLiteral("future_route_optimized_after_sandbox")]=false;
    if(!stopped()){QList<int> ids;for(const auto &v:plan.value(QStringLiteral("items")).toArray())ids.append(v.toObject().value(QStringLiteral("item_id")).toInt());
        plan[QStringLiteral("build_reasoning")]=heroEquipmentBuildReasoning(c,s,cid,ids);}
    return finish();
}
namespace {
bool aiContractFailure(QString *error,const QString &message) { if(error)*error=message;return false; }
bool aiChoiceArray(const QJsonObject &plan) {
    const auto value=plan.value(QStringLiteral("choices"));
    if(!value.isArray()||value.toArray().isEmpty()||value.toArray().size()>3)return false;
    for(const auto &choice:value.toArray())if(!choice.isObject())return false;
    return true;
}
bool aiEntityIds(const QJsonValue &value,QSet<int> &ids) {
    if(!value.isArray()||value.toArray().size()>6)return false;
    for(const auto &id:value.toArray()) {
        if(!id.isDouble()||!std::isfinite(id.toDouble())||id.toDouble()!=id.toInt()||id.toInt()<=0||ids.contains(id.toInt()))return false;
        ids.insert(id.toInt());
    }
    return true;
}
bool aiRouteIndex(const QJsonValue &value,const QJsonArray &loadouts,int &index) {
    if(!value.isDouble()||!std::isfinite(value.toDouble())||value.toDouble()!=value.toInt()||value.toInt()<0||value.toInt()>=loadouts.size())return false;
    index=value.toInt();return true;
}
bool aiRoute(const QJsonValue &value,QSet<int> &items,int &required) {
    if(!value.isObject())return false;
    const auto route=value.toObject();
    if(!aiEntityIds(route.value(QStringLiteral("item_ids")),items)||items.isEmpty())return false;
    required=0;
    if(route.contains(QStringLiteral("requires_augment_id"))) {
        const auto id=route.value(QStringLiteral("requires_augment_id"));
        if(!id.isDouble()||!std::isfinite(id.toDouble())||id.toDouble()!=id.toInt()||id.toInt()<0)return false;
        required=id.toInt();
    }
    return true;
}
}
bool validateAiPlan(const QJsonObject &o, const QList<int> &items, const QList<int> &augments, QString *error) {
    if(!aiChoiceArray(o))return aiContractFailure(error,QStringLiteral("方案数量必须为 1–3，且每条均为对象"));
    for(const auto &value:o.value(QStringLiteral("choices")).toArray()) {
        const auto choice=value.toObject();
        if(choice.value(QStringLiteral("title")).toString().trimmed().isEmpty()||choice.value(QStringLiteral("reason")).toString().trimmed().isEmpty())
            return aiContractFailure(error,QStringLiteral("方案缺少标题或理由"));
        QSet<int> itemIds,cardIds;
        if(!aiEntityIds(choice.value(QStringLiteral("item_ids")),itemIds)||!aiEntityIds(choice.value(QStringLiteral("augment_ids")),cardIds))
            return aiContractFailure(error,QStringLiteral("候选 ID 格式、数量或重复项不正确"));
        if(itemIds.isEmpty()&&cardIds.isEmpty())return aiContractFailure(error,QStringLiteral("方案未包含装备或强化"));
        for(int id:itemIds)if(!items.contains(id))return aiContractFailure(error,QStringLiteral("返回了候选外装备"));
        for(int id:cardIds)if(!augments.contains(id))return aiContractFailure(error,QStringLiteral("返回了当前候选外强化"));
    }
    return true;
}
bool resolveAiLoadoutIndices(QJsonObject &plan,const QJsonArray &loadouts,QString *error) {
    if(!aiChoiceArray(plan))return aiContractFailure(error,QStringLiteral("方案数量必须为 1–3，且每条均为对象"));
    QJsonArray resolved;
    for(const auto &value:plan.value(QStringLiteral("choices")).toArray()) {
        const auto choice=value.toObject();QJsonObject clean;
        for(const auto &field:{QStringLiteral("title"),QStringLiteral("reason"),QStringLiteral("tradeoffs")}) {
            if(choice.contains(field)&&!choice.value(field).isString())return aiContractFailure(error,QStringLiteral("方案说明格式不正确"));
            if(choice.contains(field))clean[field]=choice.value(field).toString().trimmed();
        }
        if(!loadouts.isEmpty()) {
            int index=0,required=0;QSet<int> routeIds;
            if(!aiRouteIndex(choice.value(QStringLiteral("loadout_index")),loadouts,index))return aiContractFailure(error,QStringLiteral("必须明确选择当前配装编号"));
            if(!aiRoute(loadouts[index],routeIds,required))return aiContractFailure(error,QStringLiteral("编号对应配装无效"));
            if(choice.contains(QStringLiteral("item_ids"))) {
                QSet<int> provided;
                if(!aiEntityIds(choice.value(QStringLiteral("item_ids")),provided)||provided!=routeIds)
                    return aiContractFailure(error,QStringLiteral("配装编号与完整装备清单冲突"));
            }
            QJsonArray cards=required>0?QJsonArray{required}:QJsonArray{};
            if(choice.contains(QStringLiteral("augment_ids"))) {
                QSet<int> provided;
                if(!aiEntityIds(choice.value(QStringLiteral("augment_ids")),provided)||(required>0&&!provided.contains(required)))
                    return aiContractFailure(error,QStringLiteral("配装所需强化缺失或格式不正确"));
                // validateAiLoadouts separately checks any echoed selected cards.
                // A route index is never permission to add another offered card.
                cards=choice.value(QStringLiteral("augment_ids")).toArray();
            }
            clean[QStringLiteral("loadout_index")]=index;
            clean[QStringLiteral("item_ids")]=loadouts[index].toObject().value(QStringLiteral("item_ids"));
            clean[QStringLiteral("augment_ids")]=cards;
        }else {
            if(choice.contains(QStringLiteral("loadout_index")))return aiContractFailure(error,QStringLiteral("当前没有可编号的本地配装"));
            QSet<int> items,cards;
            if(!aiEntityIds(choice.value(QStringLiteral("item_ids")),items)||!aiEntityIds(choice.value(QStringLiteral("augment_ids")),cards)||(items.isEmpty()&&cards.isEmpty()))
                return aiContractFailure(error,QStringLiteral("方案必须明确包含非空候选装备或强化"));
            clean[QStringLiteral("item_ids")]=choice.value(QStringLiteral("item_ids"));
            clean[QStringLiteral("augment_ids")]=choice.value(QStringLiteral("augment_ids"));
        }
        resolved.append(clean);
    }
    // Model scores, numeric authority and arbitrary extra fields never enter
    // downstream recommendation objects through this normalisation boundary.
    plan=QJsonObject{{QStringLiteral("choices"),resolved}};return true;
}
bool validateAiLoadouts(const QJsonObject &plan,const QJsonArray &loadouts,const QList<int> &selected,QString *error) {
    if(loadouts.isEmpty()||!aiChoiceArray(plan))return aiContractFailure(error,QStringLiteral("缺少当前本地配装或有效方案"));
    for(const auto &value:plan.value(QStringLiteral("choices")).toArray()) {
        const auto choice=value.toObject();int index=0,required=0;QSet<int> routeItems,items,cards;
        if(!aiRouteIndex(choice.value(QStringLiteral("loadout_index")),loadouts,index)||!aiRoute(loadouts[index],routeItems,required))
            return aiContractFailure(error,QStringLiteral("配装编号不在当前有效方案中"));
        if(!aiEntityIds(choice.value(QStringLiteral("item_ids")),items)||items!=routeItems||!aiEntityIds(choice.value(QStringLiteral("augment_ids")),cards))
            return aiContractFailure(error,QStringLiteral("回复未完整匹配所选配装"));
        if(required>0&&!cards.contains(required))return aiContractFailure(error,QStringLiteral("配装所需强化未包含"));
        for(int id:cards)if(id!=required&&!selected.contains(id))return aiContractFailure(error,QStringLiteral("配装混入另一张未选强化"));
    }
    return true;
}
QJsonArray visibleNumericComponents(const Catalog &c, const GameState &s, int cid) {
    QJsonArray values;
    if (!s.fresh() || s.championId != cid || !modelRulesUsable(c,s)) return values;
    values.append(QJsonObject{{QStringLiteral("label"),QStringLiteral("普攻基础部分（非暴击）")},
                              {QStringLiteral("raw"),s.ad},{QStringLiteral("scope"),QStringLiteral("AD component only; excludes champion attack modifiers and on-hit effects")}});
    for (const auto &v:c.mechanics) {
        const auto rule=v.toObject();
        if (rule.value(QStringLiteral("champion_id")).toInt()!=cid) continue;
        const int rank=s.abilities.value(rule.value(QStringLiteral("spell")).toString()).toObject().value(QStringLiteral("abilityLevel")).toInt();
        DamageInput input;input.championId=cid;input.rank=rank;input.ap=s.ap;input.ad=s.ad;input.resistance=0;
        const auto result=calculateDamage(rule,input);
        if(result.supported) values.append(QJsonObject{{QStringLiteral("label"),rule.value(QStringLiteral("label"))},
            {QStringLiteral("raw"),result.raw},{QStringLiteral("rank"),rank},{QStringLiteral("scope"),result.scope}});
    }
    return values;
}
