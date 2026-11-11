#include "coach_model.h"
#include "purchase_planner.h"
#include "role_guidance.h"
#include "tactical_context.h"
#include "combat.h"
#include "augment_strategy.h"
#include "headliner.h"
#include "external_strategy_reference.h"
#include "hero_execution.h"
#include "decision_context.h"
#include "coach_recommendation_quality.h"
#include "loadout_outcome_reference.h"
#include <QElapsedTimer>
#include <algorithm>
#include <cmath>
#include <random>
#include <functional>

namespace {
QList<int> integers(const QJsonArray &values){QList<int> out;for(const auto &v:values)if(v.isDouble()&&v.toDouble()==v.toInt()&&v.toInt()>0)out.append(v.toInt());return out;}
QString selfTeam(const GameState &s){for(const auto &v:s.players)if(v.toObject().value(QStringLiteral("self")).toBool())return v.toObject().value(QStringLiteral("team")).toString();return {};}
int equipmentValue(const Catalog &c,const QList<int> &ids,int *unknown=nullptr){int total=0;for(int id:ids){const auto row=c.items.value(id);const auto price=row.value(QStringLiteral("price_total"));if(row.isEmpty()||!price.isDouble()||!std::isfinite(price.toDouble())||price.toDouble()<0){if(unknown)++*unknown;continue;}total+=price.toInt();}return total;}
struct Fighter {CombatContext context;CombatEvaluation result;QList<int> items,augments;bool ally=false;};
QJsonArray interval(double p,int n){
    // Wilson interval only quantifies Monte Carlo sampling error, not model correctness.
    const double z=1.96,den=1+z*z/n,center=(p+z*z/(2*n))/den;
    const double half=z*std::sqrt(p*(1-p)/n+z*z/(4.*n*n))/den;
    return {std::max(0.,center-half),std::min(1.,center+half)};
}
}

QJsonArray championScenarioVariants(const CombatDatabase &db,int cid){
    QJsonArray result{QJsonObject{{QStringLiteral("key"),QStringLiteral("base")},{QStringLiteral("conditions"),QJsonArray{}}}};
    // Only explicit form/weapon states. A hit or wall collision is an event, not a form.
    QSet<QString> found;
    for(const auto &v:db.champions.value(cid).value(QStringLiteral("components")).toArray()){
        const auto condition=v.toObject().value(QStringLiteral("condition")).toString();
        if(condition.startsWith(QStringLiteral("jayce_"))||condition.startsWith(QStringLiteral("aphelios_"))||condition==QStringLiteral("shyvana_dragon_cast"))found.insert(condition);
    }
    auto ordered=found.values();ordered.sort();
    for(const auto &key:ordered)result.append(QJsonObject{{QStringLiteral("key"),key},{QStringLiteral("conditions"),QJsonArray{key}}});
    return result;
}

QJsonObject conditionalAugmentChance(int pool,int desired,int drawSlots,int rerolls){
    if(pool<1||desired<0||desired>pool||drawSlots<1||drawSlots>pool||rerolls<0||rerolls>100)return {{QStringLiteral("available"),false}};
    // Each batch is a fresh independent uniform draw; previously shown cards can reappear between batches.
    double miss=1;
    for(int i=0;i<drawSlots;++i)miss*=std::max(0.,double(pool-desired-i)/(pool-i));
    return {{QStringLiteral("available"),true},{QStringLiteral("probability"),1-std::pow(miss,rerolls+1)},
        {QStringLiteral("pool_size"),pool},{QStringLiteral("desired_count"),desired},{QStringLiteral("slots"),drawSlots},
        {QStringLiteral("independent_redraws"),rerolls},{QStringLiteral("live_probability"),false},
        {QStringLiteral("assumptions"),QStringLiteral("Uniform known eligible same-tier pool; distinct cards within each batch, independent full batches. Exclude owned/ineligible cards before specifying pool. Actual tier odds, card weights, per-slot rerolls and patch eligibility unknown.")}};
}

QJsonObject simulateCoachFight(const Catalog &catalog,const GameState &s,const CoachSimulationOptions &options){
    QJsonObject out{{QStringLiteral("available"),false},{QStringLiteral("kind"),QStringLiteral("conditional_teamfight_scenario")},
        {QStringLiteral("calibrated_match_win_rate"),false},{QStringLiteral("match_win_rate"),QJsonValue::Null},{QStringLiteral("probability"),QJsonValue::Null},
        {QStringLiteral("trained_model"),false},{QStringLiteral("seed"),qint64(options.seed)}};
    auto fail=[&](const QString &why){out[QStringLiteral("reason")]=why;return out;};
    auto stopped=[&](){return options.work&&options.work->stopped();};
    auto interrupted=[&](){out[QStringLiteral("interrupted")]=true;out[QStringLiteral("stop_reason")]=options.work->stopReason();return fail(QStringLiteral("本次推演已停止，未输出未完成的概率"));};
    if(stopped())return interrupted();
    if(!catalog.combat||!s.fresh())return fail(QStringLiteral("等待新鲜的对局快照"));
    if(s.gameMode!=QStringLiteral("KIWI"))return fail(QStringLiteral("尚未确认海克斯 KIWI 模式"));
    if(!modelRulesUsable(catalog,s))return fail(QStringLiteral("等待本地规则"));
    if(options.samples<128||options.samples>200000||!std::isfinite(options.allyExecution)||!std::isfinite(options.enemyExecution)
       ||options.allyExecution<.5||options.allyExecution>1.5||options.enemyExecution<.5||options.enemyExecution>1.5
       ||!std::isfinite(options.executionSpread)||options.executionSpread<0||options.executionSpread>.5
       ||!std::isfinite(options.horizon)||options.horizon<1||options.horizon>30)return fail(QStringLiteral("模拟参数不在有效范围"));
    const auto team=selfTeam(s);if(team.isEmpty())return fail(QStringLiteral("尚未识别本方队伍"));
    const auto &db=*catalog.combat;QVector<Fighter> fighters;int allies=0,enemies=0,missingAugments=0,unavailableItems=0;
    QSet<QString> unknowns;QJsonArray combatContexts;double armor[2]{},mr[2]{},hp[2]{};int counts[2]{};
    for(const auto &value:s.players){const auto p=value.toObject();const auto playerTeam=p.value(QStringLiteral("team")).toString();
        if(stopped())return interrupted();
        if(playerTeam!=QStringLiteral("ORDER")&&playerTeam!=QStringLiteral("CHAOS"))return fail(QStringLiteral("队伍字段不完整"));
        const int cid=p.value(QStringLiteral("champion_id")).toInt(),level=p.value(QStringLiteral("level")).toInt();
        if(!db.champions.contains(cid)||level<1||level>18)return fail(QStringLiteral("有英雄或等级尚未识别"));
        const bool self=p.value(QStringLiteral("self")).toBool();
        auto playerContext=playerCombatContext(db,p);
        if(playerContext.available&&self){
            auto current=p;
            if(!s.abilities.isEmpty())current[QStringLiteral("abilities")]=s.abilities;
            if(!s.effectCounters.isEmpty()){
                auto counters=current.value(QStringLiteral("effect_counters")).toObject();
                for(auto it=s.effectCounters.cbegin();it!=s.effectCounters.cend();++it)counters[it.key()]=it.value();
                current[QStringLiteral("effect_counters")]=counters;
            }
            if(!s.effectConditions.isEmpty()){
                auto conditions=current.value(QStringLiteral("effect_conditions")).toArray();
                for(const auto &condition:s.effectConditions)if(!conditions.contains(condition))conditions.append(condition);
                current[QStringLiteral("effect_conditions")]=conditions;
            }
            // Apply the same strict target/ledger isolation to the self
            // snapshot instead of bypassing it after resolving other players.
            playerContext=playerCombatContext(db,current);
        }
        combatContexts.append(QJsonObject{{QStringLiteral("champion_id"),cid},{QStringLiteral("team"),playerTeam},
            {QStringLiteral("self"),self},{QStringLiteral("combat_context"),playerContext.report}});
        out[QStringLiteral("combat_contexts")]=combatContexts;
        if(!playerContext.available)return fail(QStringLiteral("有技能等级、成长计数或触发条件矛盾，暂不输出概率"));
        Fighter f;f.ally=playerTeam==team;f.context=playerContext.context;f.context.horizon=options.horizon;
        f.items=integers(p.value(QStringLiteral("items")).toArray());
        if(!p.value(QStringLiteral("items")).isArray()||!p.value(QStringLiteral("items_complete")).toBool(true))return fail(QStringLiteral("有背包数据缺失，等待完整物品列表"));
        if(self)f.augments=effectiveSelectedAugments(catalog,s);
        else f.augments=integers(p.value(QStringLiteral("augment_ids")).toArray());
        QSet<int> uniqueAugments;
        for(int id:f.augments){if(!db.augments.contains(id)||uniqueAugments.contains(id))return fail(QStringLiteral("存在未建模或重复的已选强化"));uniqueAugments.insert(id);}
        if(!p.value(QStringLiteral("augments_complete")).toBool())++missingAugments;
        for(int id:f.items)if(!db.items.contains(id))++unavailableItems;
        f.result=evaluateCombat(db,f.context,f.items,f.augments);
        if(!f.result.valid){out[QStringLiteral("invalid_combatant")]=QJsonObject{{QStringLiteral("champion_id"),cid},{QStringLiteral("item_ids"),p.value(QStringLiteral("items"))}};return fail(QStringLiteral("有组合无法执行，暂不输出概率"));}
        if(self&&!s.liveStats.isEmpty()){
            out[QStringLiteral("self_observed_anchor")]=anchorObservedCombatStats(db,f.context,f.items,f.augments,s.liveStats);
            f.result=evaluateCombat(db,f.context,f.items,f.augments);
            if(!f.result.valid)return fail(QStringLiteral("本人面板校准后无法执行"));
        }
        int side=f.ally?0:1;++counts[side];armor[side]+=f.result.stats[Stat::Armor];mr[side]+=f.result.stats[Stat::Mr];hp[side]+=f.result.stats[Stat::Hp];
        if(f.ally)++allies;else ++enemies;fighters.append(f);
    }
    if(allies!=5||enemies!=5)return fail(QStringLiteral("等待完整双方十人阵容（%1 / %2）").arg(allies).arg(enemies));
    for(auto &f:fighters){if(stopped())return interrupted();const int opposite=f.ally?1:0;f.context.targetArmor=armor[opposite]/counts[opposite];f.context.targetMr=mr[opposite]/counts[opposite];f.context.targetHp=hp[opposite]/counts[opposite];
        f.result=evaluateCombat(db,f.context,f.items,f.augments);if(!f.result.valid)return fail(QStringLiteral("目标情景计算失败"));
        for(const auto &unknown:f.result.unknowns)unknowns.insert(unknown);
    }
    std::mt19937 rng(options.seed);std::uniform_real_distribution<double> uniform(0,1);
    double wins[3]{};
    for(int sample=0;sample<options.samples;++sample){double damage[2]{},health[2]{};
        if((sample&127)==0&&stopped())return interrupted();
        for(const auto &f:fighters){const auto &r=f.result;const int side=f.ally?0:1;
            const double uptime=.55+.4*uniform(rng),hit=.45+.45*uniform(rng);
            const double attack=std::max(0.,r.attackDamage+r.replacementAttackDamage+r.onhitDamage);
            const double spell=std::max(0.,r.skillDamage-r.replacementAttackDamage+r.procDamage-r.onhitDamage);
            damage[side]+=attack*uptime+spell*hit;health[side]+=r.stats[Stat::Hp]+r.stats[Stat::Shield];
        }
        // Team execution is independent of observed KDA. Kills are not counted as bonus damage.
        const double ourFactor=1+options.executionSpread*(2*uniform(rng)-1),theirFactor=1+options.executionSpread*(2*uniform(rng)-1);
        const double ourRate=damage[0]*ourFactor*options.allyExecution/std::max(1.,health[1]);
        const double theirRate=damage[1]*theirFactor*options.enemyExecution/std::max(1.,health[0]);
        const double relative[3]{1,.85,1.15};
        for(int k=0;k<3;++k){const double difference=ourRate*relative[k]-theirRate;wins[k]+=std::abs(difference)<1e-12?.5:difference>0?1:0;}
    }
    if(stopped())return interrupted();
    const double p=wins[0]/options.samples;auto missing=unknowns.values();missing.sort();
    out[QStringLiteral("available")]=true;out[QStringLiteral("probability")]=p;
    out[QStringLiteral("monte_carlo_95_interval")]=interval(p,options.samples);
    out[QStringLiteral("execution_sensitivity")]=QJsonArray{wins[1]/options.samples,wins[2]/options.samples};
    out[QStringLiteral("samples")]=options.samples;out[QStringLiteral("horizon_seconds")]=options.horizon;
    out[QStringLiteral("ally_execution")]=options.allyExecution;out[QStringLiteral("enemy_execution")]=options.enemyExecution;
    out[QStringLiteral("execution_spread")]=options.executionSpread;
    out[QStringLiteral("unknown_augments_players")]=missingAugments;out[QStringLiteral("unknown_items")]=unavailableItems;
    out[QStringLiteral("unknowns")]=QJsonArray::fromStringList(missing);out[QStringLiteral("data_revision")]=db.revision;
    out[QStringLiteral("provisional")]=true;out[QStringLiteral("snapshot_age_ms")]=s.source==GameState::Source::Replay?0:s.received.msecsTo(QDateTime::currentDateTimeUtc());
    out[QStringLiteral("assumptions")]=QJsonArray{QStringLiteral("双方满血、同时接团；不预测站位、复活、控制链、技能命中或玩家真实水平"),
        QStringLiteral("普攻输出窗口采样 55%–95%，技能命中采样 45%–90%；其余执行波动由参数设定，尚未用实战校准"),
        QStringLiteral("按装备与等级计算，本人使用可见面板校准；对手抗性取敌方模型均值，各人加点优先使用明确观测，未观测槽位采用已披露参考分配，未知海克斯不补零为已确认"),
        QStringLiteral("人头、补刀、装备价值用于局面解释；不在已有装备与等级之外重复增加战力"),
        QStringLiteral("95% 区间仅表示抽样误差；操作敏感范围使用本方执行系数 0.85× 至 1.15×"),
        QStringLiteral("规则 %1；未观测的触发与主秀收益保持未知；不是本局最终胜率").arg(catalog.patch)};
    return out;
}

void refreshCoachSnapshot(const Catalog &catalog,const GameState &s,QJsonObject &out){
    const auto tactics=analyzeTacticalContext(catalog,s);out[QStringLiteral("tactical_context")]=tactics;
    const auto team=selfTeam(s);QJsonArray roster;int teamKills=0,enemyKills=0,teamValue=0,enemyValue=0,teamCs=0,enemyCs=0;double allyLevel=0,enemyLevel=0;int allyCount=0,enemyCount=0,unknownPrices=0,unknownInventories=0;bool scoresComplete=!s.players.isEmpty(),levelsComplete=true;
    for(const auto &v:s.players){auto p=v.toObject();const bool ally=!team.isEmpty()&&p.value(QStringLiteral("team")).toString()==team;
        const int pricesBefore=unknownPrices;const auto ids=integers(p.value(QStringLiteral("items")).toArray());const int value=equipmentValue(catalog,ids,&unknownPrices);const auto scores=p.value(QStringLiteral("scores")).toObject();
        const bool inventory=p.value(QStringLiteral("items")).isArray()&&p.value(QStringLiteral("items_complete")).toBool(true);
        if(!inventory)++unknownInventories;
        levelsComplete&=p.value(QStringLiteral("level")).isDouble()&&p.value(QStringLiteral("level")).toInt()>=1&&p.value(QStringLiteral("level")).toInt()<=18;
        for(const auto &key:{QStringLiteral("kills"),QStringLiteral("deaths"),QStringLiteral("assists"),QStringLiteral("creepScore")})scoresComplete&=scores.value(key).isDouble();
        p[QStringLiteral("champion_name")]=catalog.championName(p.value(QStringLiteral("champion_id")).toInt());p[QStringLiteral("ally")]=ally;p[QStringLiteral("equipment_value")]=inventory&&unknownPrices==pricesBefore?QJsonValue(value):QJsonValue(QJsonValue::Null);p[QStringLiteral("items_complete")]=inventory;
        if(ally){teamValue+=value;teamKills+=scores.value(QStringLiteral("kills")).toInt();teamCs+=scores.value(QStringLiteral("creepScore")).toInt();allyLevel+=p.value(QStringLiteral("level")).toInt();++allyCount;}
        else{enemyValue+=value;enemyKills+=scores.value(QStringLiteral("kills")).toInt();enemyCs+=scores.value(QStringLiteral("creepScore")).toInt();enemyLevel+=p.value(QStringLiteral("level")).toInt();++enemyCount;}
        p[QStringLiteral("tactical_role")]=tacticalPlayer(tactics,int(roster.size()));roster.append(p);
    }
    const bool fullRoster=!team.isEmpty()&&allyCount==5&&enemyCount==5;
    scoresComplete&=fullRoster;
    const bool ready=s.fresh()&&modelRulesUsable(catalog,s)&&s.championId>0&&s.gameMode==QStringLiteral("KIWI");
    const auto plan=out.value(QStringLiteral("plan")).toObject();
    const bool growthCandidates=plan.value(QStringLiteral("growth_conditioned_candidates")).toBool();
    bool usableGrowthLoadout=false;
    for(const auto &value:plan.value(QStringLiteral("candidate_loadouts")).toArray()){
        const auto ids=value.toObject().value(QStringLiteral("item_ids")).toArray();bool usable=!ids.isEmpty()&&ids.size()<=6;
        for(const auto &id:ids)usable&=id.isDouble()&&std::isfinite(id.toDouble())&&id.toDouble()==std::floor(id.toDouble())&&id.toDouble()>=1&&id.toDouble()<=1000000;
        usableGrowthLoadout|=usable;
    }
    const bool growthAnalysisAvailable=growthCandidates&&usableGrowthLoadout
        &&out.value(QStringLiteral("status"))!=QStringLiteral("interrupted")&&plan.value(QStringLiteral("status"))!=QStringLiteral("interrupted")
        &&out.value(QStringLiteral("computation_complete"))!=QJsonValue(false)&&plan.value(QStringLiteral("computation_complete"))!=QJsonValue(false);
    out[QStringLiteral("growth_conditioned_candidates")]=growthCandidates;
    out[QStringLiteral("candidate_loadouts_available")]=growthAnalysisAvailable;
    if(growthCandidates){
        // A bare inventory is normal for an offline candidate comparison.
        // The separate loadouts carry analysis, never a current shopping order.
        out[QStringLiteral("analysis_available")]=growthAnalysisAvailable;
        out[QStringLiteral("ranking_available")]=false;
    }
    QJsonObject nextPurchase;
    if(catalog.combat&&!growthCandidates&&!plan.value(QStringLiteral("reference_only")).toBool())for(const auto &v:plan.value(QStringLiteral("items")).toArray()){
        const auto item=v.toObject();if(item.value(QStringLiteral("owned")).toBool())continue;const int id=item.value(QStringLiteral("item_id")).toInt();
        const int cost=remainingBuildCost(*catalog.combat,{id},s.owned);
        nextPurchase={{QStringLiteral("item_id"),id},{QStringLiteral("name"),catalog.itemName(id)},{QStringLiteral("remaining_cost"),cost>=0?QJsonValue(cost):QJsonValue(QJsonValue::Null)},
            {QStringLiteral("gold_gap"),cost<0||!s.goldKnown?QJsonValue():QJsonValue(std::max(0.,cost-s.gold))},
            {QStringLiteral("affordable"),cost>=0&&s.goldKnown&&s.gold>=cost},{QStringLiteral("gold_known"),s.goldKnown},{QStringLiteral("purchase_window_known"),false}};
        QJsonArray components;QSet<int> visited;
        std::function<void(int,int)> findComponents=[&](int current,int depth){
            if(depth>12||visited.contains(current)||!catalog.combat->items.contains(current))return;
            visited.insert(current);
            const auto quote=planInventoryRecipeReference(catalog,s.championId,s.level,s.owned,current);
            const int remaining=quote.value(QStringLiteral("remaining_cost")).toInt(-1);
            if(quote.value(QStringLiteral("legal")).toBool()&&remaining>0&&remaining<=s.gold){
                const auto after=integers(quote.value(QStringLiteral("inventory_after")).toArray());
                const auto completion=planInventoryRecipeReference(catalog,s.championId,s.level,after,id);
                const int completionCost=completion.value(QStringLiteral("remaining_cost")).toInt(-1);
                if(completion.value(QStringLiteral("legal")).toBool()&&completionCost>=0&&completionCost==cost-remaining)
                    components.append(QJsonObject{{QStringLiteral("item_id"),current},{QStringLiteral("name"),catalog.itemName(current)},{QStringLiteral("remaining_cost"),remaining}});
            }
            for(int child:catalog.combat->items[current].from)findComponents(child,depth+1);
        };
        if(ready&&s.goldKnown&&std::isfinite(s.gold)&&s.gold>=0&&cost>s.gold)
            for(int part:catalog.combat->items.value(id).from)findComponents(part,1);
        nextPurchase[QStringLiteral("buyable_components")]=components;break;
    }
    // Source routes explain candidates. Even a separately verified affordable
    // source item is a reference, not the simulator's next-purchase winner.
    const auto sourceNext=plan.value(QStringLiteral("role_aware_recommendation")).toObject().value(QStringLiteral("next_purchase_reference")).toObject();
    if(!growthCandidates&&plan.value(QStringLiteral("reference_only")).toBool()&&!sourceNext.isEmpty())
        out[QStringLiteral("next_purchase_reference")]=sourceNext;
    else out.remove(QStringLiteral("next_purchase_reference"));
    QJsonObject economy{{QStringLiteral("self_current_gold"),ready&&s.goldKnown?QJsonValue(s.gold):QJsonValue()},
        {QStringLiteral("scores_complete"),scoresComplete},
        {QStringLiteral("ally_kills"),teamKills},{QStringLiteral("enemy_kills"),enemyKills},
        {QStringLiteral("ally_cs"),teamCs},{QStringLiteral("enemy_cs"),enemyCs},
        {QStringLiteral("ally_equipment_value"),teamValue},{QStringLiteral("enemy_equipment_value"),enemyValue},
        {QStringLiteral("equipment_value_difference"),fullRoster&&unknownPrices==0&&unknownInventories==0?QJsonValue(teamValue-enemyValue):QJsonValue()},
        {QStringLiteral("unknown_item_prices"),unknownPrices},{QStringLiteral("roster_complete"),fullRoster},
        {QStringLiteral("unknown_inventories"),unknownInventories},
        {QStringLiteral("average_level_difference"),fullRoster&&levelsComplete?QJsonValue(allyLevel/allyCount-enemyLevel/enemyCount):QJsonValue()},
        {QStringLiteral("enemy_current_gold"),QJsonValue::Null},{QStringLiteral("total_gold_difference"),QJsonValue::Null},
        {QStringLiteral("next_purchase"),nextPurchase},
        {QStringLiteral("scope"),QStringLiteral("装备目录标价合计，不是已花费金币或总经济差；人头不乘固定赏金，未知连杀与赏金不估计。")}};
    const QJsonObject current{{QStringLiteral("source"),s.source==GameState::Source::Replay?QStringLiteral("demo_or_replay"):QStringLiteral("live_read_only")},
        {QStringLiteral("fresh"),ready},{QStringLiteral("version_match"),modelPatchMatches(catalog,s)},{QStringLiteral("headliner"),evaluateHeadliner(catalog,s)},
        {QStringLiteral("champion_id"),s.championId},{QStringLiteral("level"),s.level},
        {QStringLiteral("game_time"),s.gameTime},{QStringLiteral("roster"),roster},{QStringLiteral("economy"),economy}};
    for(auto it=current.begin();it!=current.end();++it)out[it.key()]=it.value();
    out[QStringLiteral("rule_basis")]=modelRuleBasis(catalog,s);
}

QJsonObject pendingCoachExecutionExplanation(const GameState &state){
    const QJsonObject pending{{QStringLiteral("available"),false},{QStringLiteral("complete"),false},
        {QStringLiteral("status"),QStringLiteral("pending_async_explanation")},
        {QStringLiteral("execution_status"),QStringLiteral("not_executed")},
        {QStringLiteral("damage_ranking_available"),false},{QStringLiteral("current_damage_ranking_available"),false},
        {QStringLiteral("reference_damage_ranking_available"),false},
        {QStringLiteral("context_id"),state.matchContext.value(QStringLiteral("context_id"))},
        {QStringLiteral("champion_id"),state.championId},{QStringLiteral("model_game_time"),state.gameTime},
        {QStringLiteral("current_state_execution"),false}};
    auto reference=pending;reference[QStringLiteral("synthetic_reference")]=true;
    return {{QStringLiteral("hero_execution_plan"),pending},{QStringLiteral("hero_execution_reference"),reference},
        {QStringLiteral("execution_reference_binding"),QJsonObject{}},
        {QStringLiteral("hero_execution_stage_timings_ms"),QJsonObject{}},
        {QStringLiteral("execution_reference_pending"),true},
        {QStringLiteral("execution_reference_status"),QStringLiteral("pending_async_explanation")},
        {QStringLiteral("hero_execution_budget"),QJsonObject{{QStringLiteral("shared_budget_ms"),350},
            {QStringLiteral("actual_elapsed_ms"),0},{QStringLiteral("deferred"),true},
            {QStringLiteral("purchase_result_independent"),true},{QStringLiteral("preemptive_deadline"),false}}}};
}

QJsonObject buildCoachExecutionExplanation(const Catalog &catalog,const GameState &state,const QString &style,
                                           int budgetMs,const RecommendationWorkPtr &work){
    const int sharedBudgetMs=std::clamp(budgetMs,0,350);QElapsedTimer clock;clock.start();QJsonObject out;
    auto cancelled=[&]{return work&&work->stopReason()==QStringLiteral("cancelled");};
    auto bind=[&](QJsonObject &value){value[QStringLiteral("context_id")]=state.matchContext.value(QStringLiteral("context_id"));
        value[QStringLiteral("model_game_time")]=state.gameTime;value[QStringLiteral("current_state_execution")]=false;};
    if(cancelled()){out=pendingCoachExecutionExplanation(state);out[QStringLiteral("execution_reference_status")]=QStringLiteral("cancelled");return out;}
    QElapsedTimer stage;stage.start();auto execution=buildHeroExecutionPlan(catalog,state,style,sharedBudgetMs);bind(execution);
    out[QStringLiteral("hero_execution_plan")]=execution;QJsonObject timings{{QStringLiteral("hero_execution_plan"),double(stage.nsecsElapsed())/1e6}};
    const int referenceBudgetMs=std::max(0,sharedBudgetMs-int(clock.elapsed()));
    if(cancelled()){out=pendingCoachExecutionExplanation(state);out[QStringLiteral("execution_reference_status")]=QStringLiteral("cancelled");return out;}
    stage.restart();auto reference=buildHeroExecutionReference(catalog,state,style,referenceBudgetMs);bind(reference);
    out[QStringLiteral("hero_execution_reference")]=reference;timings[QStringLiteral("hero_execution_reference")]=double(stage.nsecsElapsed())/1e6;
    out[QStringLiteral("execution_reference_pending")]=false;
    out[QStringLiteral("execution_reference_status")]=cancelled()?QStringLiteral("cancelled"):QStringLiteral("completed_optional_explanation");
    out[QStringLiteral("hero_execution_stage_timings_ms")]=timings;
    out[QStringLiteral("hero_execution_budget")]=QJsonObject{{QStringLiteral("shared_budget_ms"),sharedBudgetMs},
        {QStringLiteral("reference_remaining_budget_ms"),referenceBudgetMs},{QStringLiteral("actual_elapsed_ms"),double(clock.nsecsElapsed())/1e6},
        {QStringLiteral("purchase_result_independent"),true},{QStringLiteral("preemptive_deadline"),false}};
    return out;
}

QJsonObject buildCoachSnapshot(const Catalog &catalog,const GameState &s,const QString &,const CoachSimulationOptions &options,const QSet<int> &seen){
    const QString style=QStringLiteral("成长联动");
    QElapsedTimer total;total.start();QElapsedTimer stage;stage.start();QJsonObject timings;
    const bool ready=s.fresh()&&modelRulesUsable(catalog,s)&&s.championId>0&&s.gameMode==QStringLiteral("KIWI");
    QJsonObject result{{QStringLiteral("plan"),QJsonObject{}},
        {QStringLiteral("model_game_time"),s.gameTime},
        {QStringLiteral("computation_scope"),options.currentPurchaseOnly?QStringLiteral("actual_inventory_and_legal_next_purchase_only"):QStringLiteral("offline_full_build_and_offer_comparison")},
        {QStringLiteral("current_purchase_only"),options.currentPurchaseOnly},
        {QStringLiteral("all_variants_complete"),false},{QStringLiteral("all_combinations_enumerated"),false}};
    result[QStringLiteral("external_strategy_reference")]=externalStrategyReference(catalog,s);
    result[QStringLiteral("rule_basis")]=modelRuleBasis(catalog,s);
    auto stopped=[&](){return options.work&&options.work->stopped();};
    auto interrupted=[&](){
        auto plan=result.value(QStringLiteral("plan")).toObject();QJsonArray held;
        auto sandbox=plan.value(QStringLiteral("purchase_sandbox")).toObject();
        auto reason=options.work?options.work->stopReason():QString();
        if(reason.isEmpty())reason=plan.value(QStringLiteral("stop_reason")).toString();
        if(reason.isEmpty())reason=sandbox.value(QStringLiteral("stop_reason")).toString(QStringLiteral("incomplete_enumeration"));
        const bool observedBudget=s.goldKnown&&std::isfinite(s.gold)&&s.gold>=0;
        const bool usableBudget=observedBudget&&ready&&s.fresh();
        const bool purchaseCompleted=plan.value(QStringLiteral("computation_complete")).toBool()
            &&sandbox.value(QStringLiteral("enumeration_complete")).toBool()
            &&sandbox.value(QStringLiteral("status"))!=QStringLiteral("interrupted");
        // A later diagnostic stage may exhaust the shared budget after the
        // purchase comparison finished. Keep its numbers only as an explicitly
        // non-actionable reference; cancellation or expiry never preserves it.
        QJsonObject completedReference{{QStringLiteral("available"),false},{QStringLiteral("reference_only"),true},
            {QStringLiteral("ranking_available"),false},{QStringLiteral("actionable"),false}};
        if(purchaseCompleted&&reason==QStringLiteral("time_budget_exceeded")&&ready&&s.fresh()){
            completedReference[QStringLiteral("available")]=true;
            completedReference[QStringLiteral("status")]=QStringLiteral("completed_purchase_comparison_pipeline_interrupted");
            completedReference[QStringLiteral("model_game_time")]=s.gameTime;
            completedReference[QStringLiteral("context_id")]=s.matchContext.value(QStringLiteral("context_id"));
            completedReference[QStringLiteral("current_comparison")]=plan.value(QStringLiteral("current_comparison"));
            completedReference[QStringLiteral("candidate_comparison")]=plan.value(QStringLiteral("comparison"));
            completedReference[QStringLiteral("candidate_rows")]=sandbox.value(QStringLiteral("candidates"));
            completedReference[QStringLiteral("selected_item_id")]=sandbox.value(QStringLiteral("selected_item_id"));
            completedReference[QStringLiteral("stop_reason")]=reason;
        }
        result[QStringLiteral("completed_purchase_reference")]=completedReference;
        result[QStringLiteral("purchase_stage_complete_before_interruption")]=purchaseCompleted;
        // A cancelled counterfactual purchase never consumes actual components.
        for(int id:s.owned){const int price=catalog.combat&&catalog.combat->items.contains(id)?catalog.combat->items.value(id).price:-1;
            held.append(QJsonObject{{QStringLiteral("item_id"),id},{QStringLiteral("name"),catalog.itemName(id)},
                {QStringLiteral("owned"),true},{QStringLiteral("price"),price>=0?QJsonValue(price):QJsonValue(QJsonValue::Null)}});}
        plan[QStringLiteral("items")]=held;plan[QStringLiteral("alternatives")]=QJsonArray{};
        plan[QStringLiteral("ranking_available")]=false;plan[QStringLiteral("enumeration_complete")]=false;
        plan[QStringLiteral("computation_complete")]=false;plan[QStringLiteral("status")]=QStringLiteral("interrupted");
        plan.remove(QStringLiteral("comparison"));plan.remove(QStringLiteral("growth_projection"));
        plan.remove(QStringLiteral("remaining_build_gold"));plan.remove(QStringLiteral("offer_comparisons"));
        sandbox[QStringLiteral("status")]=QStringLiteral("interrupted");sandbox[QStringLiteral("selected_item_id")]=0;
        sandbox[QStringLiteral("stop_reason")]=reason;
        // Budget availability is an input fact, independent of how many
        // counterfactual candidates the worker managed to finish.
        sandbox[QStringLiteral("budget_known")]=usableBudget;
        sandbox[QStringLiteral("candidates")]=QJsonArray{};sandbox[QStringLiteral("conditional_predictions")]=QJsonArray{};
        sandbox[QStringLiteral("enumeration_complete")]=false;sandbox[QStringLiteral("ranking_available")]=false;
        auto coverage=sandbox.value(QStringLiteral("comparison_coverage")).toObject();
        coverage[QStringLiteral("all_legal_candidates_evaluated")]=false;
        coverage[QStringLiteral("all_comparable_candidates_evaluated")]=false;
        coverage[QStringLiteral("ranking_available")]=false;
        sandbox[QStringLiteral("comparison_coverage")]=coverage;
        sandbox.remove(QStringLiteral("selected_inventory"));sandbox.remove(QStringLiteral("selected_sandbox"));
        sandbox.remove(QStringLiteral("selected_score_basis"));sandbox.remove(QStringLiteral("recommendation_kind"));
        plan[QStringLiteral("stop_reason")]=reason;
        plan[QStringLiteral("purchase_sandbox")]=sandbox;result[QStringLiteral("plan")]=plan;
        result[QStringLiteral("simulation")]=QJsonObject{{QStringLiteral("available"),false},{QStringLiteral("probability"),QJsonValue::Null},{QStringLiteral("interrupted"),true}};
        result[QStringLiteral("augment_decision")]=QJsonObject{};result[QStringLiteral("computation_complete")]=false;
        result[QStringLiteral("status")]=QStringLiteral("interrupted");
        result[QStringLiteral("stop_reason")]=reason;
        // Reading the already supplied snapshot is cheap and does not restart
        // search or simulation. Even a pre-cancelled job must retain its input
        // economy and roster rather than report observations as missing.
        refreshCoachSnapshot(catalog,s,result);
        auto economy=result.value(QStringLiteral("economy")).toObject();economy[QStringLiteral("next_purchase")]=QJsonObject{};
        economy[QStringLiteral("self_current_gold")]=usableBudget?QJsonValue(s.gold):QJsonValue(QJsonValue::Null);
        result[QStringLiteral("economy")]=economy;
        result[QStringLiteral("observed_budget")]=QJsonObject{{QStringLiteral("known"),observedBudget},
            {QStringLiteral("current_gold"),observedBudget?QJsonValue(s.gold):QJsonValue(QJsonValue::Null)},
            {QStringLiteral("source"),QStringLiteral("input_snapshot")},{QStringLiteral("snapshot_fresh"),s.fresh()},
            {QStringLiteral("usable_for_current_purchase"),usableBudget},
            {QStringLiteral("inferred_from_computation"),false}};
        timings[QStringLiteral("total")]=double(total.nsecsElapsed())/1e6;result[QStringLiteral("stage_timings_ms")]=timings;
        return result;
    };
    if(stopped())return interrupted();
    result[QStringLiteral("historical_loadout_reference")] = LoadoutOutcomeReference::project(catalog, s);
    // The live action gate already requires a verified selected list. Report
    // that prerequisite immediately, rather than spending the entire budget
    // enumerating purchases that cannot be authorized. Card and roster workers
    // remain independent. Explicit chain/scenario work is not short-circuited.
    if(ready&&options.currentPurchaseOnly&&s.source==GameState::Source::Live&&s.explicitPurchaseChain.isEmpty()){
        const auto input=coachPurchaseInputReadiness(catalog,s);
        result[QStringLiteral("input_readiness")]=input;
        if(!input.value(QStringLiteral("ready")).toBool()){
            QJsonArray held;for(int id:s.owned)held.append(QJsonObject{{QStringLiteral("item_id"),id},{QStringLiteral("name"),catalog.itemName(id)},{QStringLiteral("owned"),true}});
            result[QStringLiteral("plan")]=QJsonObject{{QStringLiteral("status"),QStringLiteral("input_required")},
                {QStringLiteral("items"),held},{QStringLiteral("alternatives"),QJsonArray{}},
                {QStringLiteral("current_purchase_only"),true},{QStringLiteral("computation_complete"),true},
                {QStringLiteral("ranking_available"),false},{QStringLiteral("enumeration_complete"),false},
                {QStringLiteral("route"),input.value(QStringLiteral("summary"))},
                {QStringLiteral("purchase_sandbox"),QJsonObject{{QStringLiteral("status"),QStringLiteral("input_required")},
                    {QStringLiteral("selected_item_id"),0},{QStringLiteral("ranking_available"),false},
                    {QStringLiteral("enumeration_complete"),false},{QStringLiteral("candidates"),QJsonArray{}}}}};
            result[QStringLiteral("status")]=QStringLiteral("input_required");
            result[QStringLiteral("computation_complete")]=true;
            result[QStringLiteral("simulation")]=QJsonObject{{QStringLiteral("available"),false},{QStringLiteral("probability"),QJsonValue::Null}};
            refreshCoachSnapshot(catalog,s,result);
            result[QStringLiteral("observed_purchase_facts")]=observedPurchaseFacts(catalog,s);
            result[QStringLiteral("source_purchase_reference")]=sourcePurchaseReference(catalog,s);
            timings[QStringLiteral("input_validation")]=double(total.nsecsElapsed())/1e6;
            timings[QStringLiteral("total")]=double(total.nsecsElapsed())/1e6;
            result[QStringLiteral("stage_timings_ms")]=timings;
            return result;
        }
    }
    auto plan=ready?(options.currentPurchaseOnly?currentPurchaseRecommendations(catalog,s,s.championId,style,options.work)
        :recommendations(catalog,s,s.championId,style)):QJsonObject{};
    plan[QStringLiteral("external_strategy_reference")]=result.value(QStringLiteral("external_strategy_reference"));
    result[QStringLiteral("plan")]=plan;timings[QStringLiteral("purchase_plan")]=double(stage.nsecsElapsed())/1e6;
    if(stopped()||plan.value(QStringLiteral("status"))==QStringLiteral("interrupted"))return interrupted();
    stage.restart();result[QStringLiteral("simulation")]=simulateCoachFight(catalog,s,options);
    timings[QStringLiteral("teamfight_simulation")]=double(stage.nsecsElapsed())/1e6;
    if(stopped())return interrupted();
    stage.restart();
    result[QStringLiteral("augment_decision")]=options.currentPurchaseOnly
        ?QJsonObject{{QStringLiteral("available"),false},{QStringLiteral("status"),QStringLiteral("handled_by_dedicated_card_worker")},
            {QStringLiteral("full_build_comparison_executed"),false}}
        :buildAugmentDecision(catalog,s,plan,style,seen,false,options.work);
    timings[QStringLiteral("augment_decision")]=double(stage.nsecsElapsed())/1e6;
    if(stopped())return interrupted();
    stage.restart();result[QStringLiteral("team_needs")]=coachTeamNeeds(catalog,s);
    timings[QStringLiteral("team_needs")]=double(stage.nsecsElapsed())/1e6;
    if(stopped())return interrupted();
    stage.restart();refreshCoachSnapshot(catalog,s,result);
    timings[QStringLiteral("visible_snapshot")]=double(stage.nsecsElapsed())/1e6;
    if(stopped())return interrupted();
    // An optional explanation owns a separate small budget. An incomplete
    // sequence search must not erase an already completed purchase comparison.
    // Native calls cooperate between bounded scenarios; this is not a
    // preemptive wall-clock guarantee and actual elapsed time is reported.
    const int executionBudgetMs=ready?int(options.work?std::min<qint64>(350,options.work->remainingMs()):350):0;
    const auto explanation=options.deferExecutionReference?pendingCoachExecutionExplanation(s)
        :buildCoachExecutionExplanation(catalog,s,style,executionBudgetMs,options.work);
    for(auto it=explanation.begin();it!=explanation.end();++it)result[it.key()]=it.value();
    const auto executionTimings=explanation.value(QStringLiteral("hero_execution_stage_timings_ms")).toObject();
    timings[QStringLiteral("hero_execution_plan")]=executionTimings.value(QStringLiteral("hero_execution_plan")).toDouble();
    timings[QStringLiteral("hero_execution_reference")]=executionTimings.value(QStringLiteral("hero_execution_reference")).toDouble();
    if(options.work&&options.work->stopReason()==QStringLiteral("cancelled"))return interrupted();
    timings[QStringLiteral("total")]=double(total.nsecsElapsed())/1e6;
    result[QStringLiteral("stage_timings_ms")]=timings;result[QStringLiteral("computation_complete")]=true;
    if(options.currentPurchaseOnly&&result.value(QStringLiteral("economy")).toObject().value(QStringLiteral("next_purchase")).toObject().isEmpty())result[QStringLiteral("source_purchase_reference")]=sourcePurchaseReference(catalog,s);
    return result;
}

GameState makeCoachDemo(const Catalog &catalog){
    GameState s;s.source=GameState::Source::Replay;s.observedPatch=catalog.patch;s.gameMode=QStringLiteral("KIWI");s.phase=QStringLiteral("InProgress");
    s.championId=1;s.level=11;s.gameTime=680;s.gold=1260;s.goldKnown=true;s.selected={1180};s.offers={1058,2055,1071};s.owned={6655,3020};
    s.abilities={{QStringLiteral("Q"),QJsonObject{{QStringLiteral("abilityLevel"),5}}},{QStringLiteral("W"),QJsonObject{{QStringLiteral("abilityLevel"),3}}},{QStringLiteral("E"),QJsonObject{{QStringLiteral("abilityLevel"),1}}},{QStringLiteral("R"),QJsonObject{{QStringLiteral("abilityLevel"),2}}}};
    const int ids[]{1,22,54,64,99,103,202,89,122,81};
    for(int n=0;n<10;++n){QJsonArray items=n==0?QJsonArray{6655,3020}:n%3==0?QJsonArray{3078,3047}:QJsonArray{3089,3020};
        s.players.append(QJsonObject{{QStringLiteral("champion_id"),ids[n]},{QStringLiteral("team"),n<5?QStringLiteral("ORDER"):QStringLiteral("CHAOS")},
            {QStringLiteral("self"),n==0},{QStringLiteral("level"),n==0?s.level:11+(n%3)-1},{QStringLiteral("items"),items},{QStringLiteral("items_complete"),true},
            {QStringLiteral("scores"),QJsonObject{{QStringLiteral("kills"),2+n%4},{QStringLiteral("deaths"),1+n%3},{QStringLiteral("assists"),5+n%5},{QStringLiteral("creepScore"),25+n*3}}},
            {QStringLiteral("augment_ids"),n==0?QJsonArray{1180}:QJsonArray{}},{QStringLiteral("augments_complete"),false},{QStringLiteral("is_dead"),false}});
    }
    Q_UNUSED(catalog);return s;
}
