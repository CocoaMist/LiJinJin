#include "lifecycle_sandbox.h"
#include "analysis.h"
#include "augment_strategy.h"
#include "combat.h"
#include "combat_sandbox.h"
#include "decision_context.h"
#include "purchase_planner.h"
#include "tactical_context.h"
#include "teamfight_sandbox.h"
#include <QJsonDocument>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace {
QString q(const char *s) { return QString::fromUtf8(s); }
QJsonValue nil() { return QJsonValue(QJsonValue::Null); }
bool number(const QJsonValue &v, double low, double high) {
    return v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble() >= low && v.toDouble() <= high;
}
bool integer(const QJsonValue &v, int low, int high) { return number(v,low,high) && v.toDouble()==v.toInt(); }
QJsonArray ids(const QList<int> &v) { QJsonArray a; for(int id:v)a.append(id); return a; }
bool readIds(const QJsonValue &v, const CombatDatabase &db, bool augment, QList<int> &out, int max=6) {
    if(!v.isArray() || v.toArray().size()>max)return false;
    QSet<int> seen;
    for(const auto &n:v.toArray()) {
        if(!integer(n,1,100000000))return false;const int id=n.toInt();
        if(augment ? !db.augments.contains(id) || seen.contains(id) : !db.items.contains(id))return false;
        out.append(id);seen.insert(id);
    }
    return true;
}
bool legalInventory(const CombatDatabase &db,const QList<int> &inventory,int champion,int level) {
    if(inventory.size()>6)return false;QSet<int> seen;QMap<QString,int> groups;
    for(int id:inventory) {
        const auto it=db.items.constFind(id);
        if(it==db.items.cend() || it->requiredLevel>level || (champion==69 && it->boots) || (it->eligible && seen.contains(id)))return false;
        seen.insert(id);for(auto g=it->groups.cbegin();g!=it->groups.cend();++g)if(++groups[g.key()]>g.value())return false;
    }
    return true;
}
bool abilitiesValid(const QJsonValue &v,const CombatDatabase &db,int champion,int level,QStringList *unknowns=nullptr) {
    if(!v.isObject())return false;
    QMap<QString,int> maximum;
    for(const auto &spell:db.champions.value(champion).value(q("spells")).toArray()) {
        const auto entry=spell.toObject();if(integer(entry.value(q("max_rank")),1,6))maximum[entry.value(q("slot")).toString()]=entry.value(q("max_rank")).toInt();
    }
    const auto object=v.toObject();
    QMap<QString,int> ranks;
    for(auto it=object.begin();it!=object.end();++it) {
        if(!QStringList{q("Q"),q("W"),q("E"),q("R")}.contains(it.key()) || !it.value().isObject())return false;
        if(!integer(it.value().toObject().value(q("abilityLevel")),0,maximum.value(it.key(),6)))return false;
        ranks[it.key()]=it.value().toObject().value(q("abilityLevel")).toInt();
    }
    return sandboxSkillRanksLegal(db,champion,level,ranks,unknowns);
}
QJsonObject cleanAbilities(const QJsonObject &v) {
    QJsonObject out;for(auto it=v.begin();it!=v.end();++it)out[it.key()]=QJsonObject{{q("abilityLevel"),it.value().toObject().value(q("abilityLevel"))}};return out;
}
// Whitelist public match fields so a submitted recording cannot leak account
// names, identifiers, credentials, or unrelated nested objects into training.
bool playersValid(const QJsonValue &value,const Catalog &c,int champion,QJsonArray &clean) {
    if(!value.isArray() || value.toArray().size()!=10)return false;
    int self=0,order=0,chaos=0;
    for(const auto &v:value.toArray()) {
        if(!v.isObject())return false;const auto row=v.toObject();
        if(!integer(row.value(q("champion_id")),1,100000) || !c.combat->champions.contains(row.value(q("champion_id")).toInt()) || !integer(row.value(q("level")),1,18) || !row.value(q("self")).isBool())return false;
        const auto side=row.value(q("team")).toString();if(side==q("ORDER"))++order;else if(side==q("CHAOS"))++chaos;else return false;
        QList<int> gear,augments;
        if(!readIds(row.value(q("items")),*c.combat,false,gear) || !row.value(q("items_complete")).isBool())return false;
        if(row.contains(q("augment_ids")) && !readIds(row.value(q("augment_ids")),*c.combat,true,augments))return false;
        if(row.contains(q("augments_complete")) && !row.value(q("augments_complete")).isBool())return false;
        if(row.value(q("augments_complete")).toBool() && !row.value(q("augment_ids")).isArray())return false;
        for(int id:augments)if(c.combat->augments[id].requiredLevel>row.value(q("level")).toInt())return false;
        if(!legalInventory(*c.combat,gear,row.value(q("champion_id")).toInt(),row.value(q("level")).toInt()))return false;
        if(row.value(q("self")).toBool()) {
            ++self;
            // A partial observed bag cannot replace the previously known bag
            // and then be promoted to a complete synthetic inventory.
            if(row.value(q("champion_id")).toInt()!=champion || !row.value(q("items_complete")).toBool())return false;
        }
        QJsonObject p{{q("champion_id"),row.value(q("champion_id"))},{q("level"),row.value(q("level"))},{q("team"),side},{q("self"),row.value(q("self"))},
            {q("items"),ids(gear)},{q("items_complete"),row.value(q("items_complete"))},{q("augment_ids"),ids(augments)},
            {q("augments_complete"),row.value(q("augments_complete")).isBool()?row.value(q("augments_complete")):QJsonValue(false)}};
        if(row.contains(q("is_dead"))) {if(!row.value(q("is_dead")).isBool())return false;p[q("is_dead")]=row.value(q("is_dead"));}
        if(row.contains(q("respawn_seconds"))) {if(!number(row.value(q("respawn_seconds")),0,600))return false;p[q("respawn_seconds")]=row.value(q("respawn_seconds"));}
        if(row.contains(q("scores"))) {if(!row.value(q("scores")).isObject())return false;QJsonObject scores;
            for(const auto &key:{q("kills"),q("deaths"),q("assists"),q("creepScore")})if(row.value(q("scores")).toObject().contains(key)) {
                const auto n=row.value(q("scores")).toObject().value(key);if(!integer(n,0,1000000))return false;scores[key]=n;
            }p[q("scores")]=scores;
        }
        // Preserve explicit public skill observations through the whitelist.
        // Dropping them here silently turns known teammate skills into guessed
        // reference ranks and can incorrectly withhold every numeric choice.
        if(row.contains(q("ranks"))||row.contains(q("abilities"))){
            auto candidate=p;
            for(const auto &field:{q("ranks"),q("abilities")})if(row.contains(field))candidate[field]=row.value(field);
            if(!playerCombatContext(*c.combat,candidate).available)return false;
            if(row.contains(q("ranks")))p[q("ranks")]=row.value(q("ranks"));
            if(row.contains(q("abilities"))){QJsonObject abilities;
                const auto raw=row.value(q("abilities")).toObject();
                for(const auto &slot:{q("Q"),q("W"),q("E"),q("R")})if(raw.contains(slot))abilities[slot]=QJsonObject{{q("abilityLevel"),raw.value(slot).toObject().value(q("abilityLevel"))}};
                p[q("abilities")]=abilities;
            }
        }
        clean.append(p);
    }
    return self==1 && order==5 && chaos==5;
}
struct Episode {
    GameState game;
    double health=0,maxHealth=0,combatClock=0,damage=0,taken=0,spent=0,earned=0;
    double deathAt=-1,respawnAt=-1;
    bool dead=false,ended=false;
    int deaths=0,respawns=0,purchases=0,selections=0;
    QString healthBasis=q("explicit_scenario_full_health"),provenance=q("synthetic_scenario"),rosterAnchor;
};
QString rosterSignature(const QJsonArray &players) {
    if(players.size()!=10)return {};
    QStringList people;QString selfTeam;
    for(const auto &v:players) {
        const auto p=v.toObject();const auto team=p.value(q("team")).toString();
        people.append(QString::number(p.value(q("champion_id")).toInt())+q(":")+team);
        if(p.value(q("self")).toBool())selfTeam=team;
    }
    std::sort(people.begin(),people.end());
    return selfTeam+q("|")+people.join(q("|"));
}
void syncSelf(Episode &e) {
    for(int i=0;i<e.game.players.size();++i) {
        auto row=e.game.players[i].toObject();if(!row.value(q("self")).toBool())continue;
        row[q("items")]=ids(e.game.owned);row[q("items_complete")]=true;row[q("level")]=e.game.level;
        row[q("augment_ids")]=ids(e.game.selected);row[q("augments_complete")]=true;row[q("is_dead")]=e.dead;
        if(e.dead && e.respawnAt>=0)row[q("respawn_seconds")]=std::max(0.,e.respawnAt-e.game.gameTime);else row.remove(q("respawn_seconds"));
        e.game.players[i]=row;
    }
    e.game.hp=e.health;
}
QJsonObject summary(const Episode &e) {
    return {{q("champion_id"),e.game.championId},{q("level"),e.game.level},{q("game_time"),e.game.gameTime},{q("gold"),e.game.gold},
        {q("items"),ids(e.game.owned)},{q("augments"),ids(e.game.selected)},{q("offers"),ids(e.game.offers)},
        {q("current_health"),e.health},{q("modeled_max_health"),e.maxHealth},{q("health_basis"),e.healthBasis},{q("is_dead"),e.dead},
        {q("death_at"),e.deathAt>=0?QJsonValue(e.deathAt):nil()},{q("respawn_at"),e.respawnAt>=0?QJsonValue(e.respawnAt):nil()},
        {q("continuous_combat_clock"),e.combatClock},{q("cumulative_modeled_damage"),e.damage},{q("cumulative_incoming_damage"),e.taken},
        {q("cooldowns_known"),false},{q("cooldown_transition_simulated"),false},{q("ended"),e.ended}};
}
CombatContext contextFor(const Catalog &c,const Episode &e,const QJsonObject &event,QStringList &unknowns) {
    auto ctx=c.combat->context(e.game.championId,e.game.level);ctx.discreteSkillCasts=true;
    ctx.ranks=referenceCombatRanks(*c.combat,e.game.championId,e.game.level);
    bool reference=false;
    for(const auto &slot:{q("Q"),q("W"),q("E"),q("R")}) {
        const auto rank=e.game.abilities.value(slot).toObject().value(q("abilityLevel"));
        if(integer(rank,0,6))ctx.ranks[slot]=rank.toInt();else reference=true;
    }
    if(reference)unknowns.append(q("reference_QWE_skill_ranks_are_scenario_not_observation"));
    applyOpponentLoadoutContext(c,e.game,ctx);
    if(event.contains(q("target_armor")))ctx.targetArmor=event.value(q("target_armor")).toDouble();
    if(event.contains(q("target_mr")))ctx.targetMr=event.value(q("target_mr")).toDouble();
    if(event.contains(q("target_health")))ctx.targetHp=event.value(q("target_health")).toDouble();
    unknowns.append(q("initial_skill_readiness_assumed_all_ready_actual_cooldowns_unknown"));
    unknowns.append(q("continuous_potential_clock_no_idle_cast_or_recovery_observations"));
    return ctx;
}
bool refreshHealth(const Catalog &c,Episode &e,QStringList &unknowns,bool initial=false) {
    auto ctx=contextFor(c,e,{},unknowns);ctx.horizon=0;
    const auto model=evaluateCombat(*c.combat,ctx,e.game.owned,e.game.selected,false,q("混合持续输出"));
    unknowns.append(model.unknowns);
    if(!model.valid || !std::isfinite(model.stats[Stat::Hp]) || model.stats[Stat::Hp]<=0)return false;
    e.maxHealth=model.stats[Stat::Hp];if(initial)e.health=e.maxHealth;else e.health=std::min(e.health,e.maxHealth);
    syncSelf(e);return true;
}
QJsonObject fallback(const Episode &e,const QString &reason) {
    return {{q("action"),e.dead?q("wait_for_known_respawn_then_reassess"):q("hold_current_verified_loadout")},{q("reason"),reason},
        {q("unknown_death_timer_never_fabricated"),e.dead && e.respawnAt<0},{q("automatic_game_input"),false}};
}
bool eventNumbersValid(const QJsonObject &event,QString &reason) {
    const QMap<QString,QPair<double,double>> ranges{{q("amount"),{0,1000000}},{q("gold"),{0,1000000}},{q("current_health"),{0,10000000}},
        {q("seconds"),{0.000001,60}},{q("execution_factor"),{0,1}},{q("incoming_dps"),{0,10000000}},{q("respawn_seconds"),{0,600}},
        {q("target_armor"),{-1000,1000000}},{q("target_mr"),{-1000,1000000}},{q("target_health"),{1,10000000}}};
    for(auto i=ranges.cbegin();i!=ranges.cend();++i)if(event.contains(i.key()) && !number(event.value(i.key()),i->first,i->second)) {reason=q("invalid_numeric_field:")+i.key();return false;}
    if(event.contains(q("level")) && !integer(event.value(q("level")),1,18)) {reason=q("unsupported_level");return false;}
    if(event.contains(q("action_parameters"))) {
        if(!event.value(q("action_parameters")).isObject()){reason=q("invalid_action_parameters");return false;}
        const auto p=event.value(q("action_parameters")).toObject();
        for(auto it=p.begin();it!=p.end();++it)if(!QStringList{q("outgoing_multiplier"),q("incoming_multiplier")}.contains(it.key()) || !number(it.value(),0,2)) {reason=q("invalid_action_parameter");return false;}
    }
    return true;
}
}

QJsonObject runLifecycleSandbox(const Catalog &catalog,const QJsonObject &request) {
    QJsonObject out{{q("schema_version"),1},{q("available"),false},{q("status"),q("invalid_request")},{q("scope"),q("conditional_numeric_lifecycle_no_movement")},
        {q("observed_match"),false},{q("actual_win_rate"),nil()},{q("no_future_information"),true},{q("automatic_game_input"),false},
        {q("steps"),QJsonArray{}},{q("training_trajectory"),QJsonArray{}}};
    auto fail=[&](const QString &reason){out[q("reason")]=reason;return out;};
    if(!catalog.combat)return fail(q("combat_database_unavailable"));
    if(!request.value(q("initial_state")).isObject() || !request.value(q("events")).isArray())return fail(q("initial_state_and_events_required"));
    if(request.value(q("events")).toArray().size()>256)return fail(q("event_limit_256"));
    if(request.contains(q("seed")) && !integer(request.value(q("seed")),0,2147483647))return fail(q("invalid_seed"));
    const auto initial=request.value(q("initial_state")).toObject();const auto &db=*catalog.combat;
    if(!integer(initial.value(q("champion_id")),1,100000) || !db.champions.contains(initial.value(q("champion_id")).toInt()))return fail(q("unknown_champion"));
    if(!integer(initial.value(q("level")),1,18) || !number(initial.value(q("game_time")),0,86400) || !number(initial.value(q("gold")),0,1000000))return fail(q("invalid_initial_level_time_or_gold"));
    Episode episode;auto &s=episode.game;s.source=GameState::Source::Replay;s.gameMode=q("KIWI");s.observedPatch=catalog.patch;s.championId=initial.value(q("champion_id")).toInt();
    s.level=initial.value(q("level")).toInt();s.gameTime=initial.value(q("game_time")).toDouble();s.gold=initial.value(q("gold")).toDouble();s.goldKnown=true;s.phase=q("conditional_sandbox");
    s.matchContext={{q("context_id"),q("anonymous-lifecycle-scenario")}};
    if(!readIds(initial.value(q("items")),db,false,s.owned) || !readIds(initial.value(q("augments")),db,true,s.selected) || !legalInventory(db,s.owned,s.championId,s.level))return fail(q("invalid_initial_inventory_or_augments"));
    for(int id:s.selected)if(db.augments[id].requiredLevel>s.level)return fail(q("initial_augment_level_unavailable"));
    QStringList rankUnknowns;
    if(initial.contains(q("abilities"))) {if(!abilitiesValid(initial.value(q("abilities")),db,s.championId,s.level,&rankUnknowns))return fail(q("invalid_initial_abilities"));s.abilities=cleanAbilities(initial.value(q("abilities")).toObject());}
    QJsonObject initialSelf;
    if(initial.contains(q("players"))) {
        if(!playersValid(initial.value(q("players")),catalog,s.championId,s.players))return fail(q("invalid_initial_ten_player_roster"));
        for(const auto &v:s.players)if(v.toObject().value(q("self")).toBool()) {
            initialSelf=v.toObject();
            auto gear=v.toObject().value(q("items")).toArray(),owned=ids(s.owned);QList<int> a,b;for(const auto &n:gear)a.append(n.toInt());for(const auto &n:owned)b.append(n.toInt());std::sort(a.begin(),a.end());std::sort(b.begin(),b.end());
            if(a!=b || v.toObject().value(q("level")).toInt()!=s.level)return fail(q("initial_self_roster_inconsistent"));
            if(v.toObject().value(q("augments_complete")).toBool()) {
                QList<int> rowAugments;readIds(v.toObject().value(q("augment_ids")),db,true,rowAugments);
                auto chosen=s.selected;std::sort(rowAugments.begin(),rowAugments.end());std::sort(chosen.begin(),chosen.end());
                if(rowAugments!=chosen)return fail(q("initial_self_augments_inconsistent"));
            }
        }
    } else s.players.append(QJsonObject{{q("champion_id"),s.championId},{q("level"),s.level},{q("self"),true},{q("team"),q("ORDER")},{q("items"),ids(s.owned)},{q("items_complete"),true}});
    episode.rosterAnchor=rosterSignature(s.players);
    double previous=s.gameTime;
    // Reject an invalid schedule before executing any prefix. A caller cannot
    // accidentally train on a partly applied, time-travelling episode.
    for(const auto &v:request.value(q("events")).toArray()) {
        if(!v.isObject() || !number(v.toObject().value(q("time")),0,86400) || v.toObject().value(q("time")).toDouble()<previous)return fail(q("non_monotonic_or_invalid_event_time"));
        previous=v.toObject().value(q("time")).toDouble();
    }
    QStringList globalUnknowns{q("scenario_not_observed_match_or_real_player_win_rate"),q("movement_target_access_control_hit_and_enemy_augments_not_fully_simulated"),
        q("action_multipliers_are_explicit_scenario_budgets_not_learned_behavior"),q("no_unobserved_growth_stacks_or_future_gold_awarded"),
        q("hold_idle_death_cooldown_recovery_not_simulated_not_action_value_training_truth"),q("mana_costs_target_death_and_healing_not_applied_to_lifecycle_damage_budget"),
        q("hero_rank_caps_validated_skillpoint_level_unlock_allocation_not_fully_validated")};
    globalUnknowns.append(rankUnknowns);
    QStringList initialUnknowns;if(!refreshHealth(catalog,episode,initialUnknowns,true))return fail(q("initial_combat_model_unavailable"));
    for(const auto &u:initialUnknowns)if(!globalUnknowns.contains(u))globalUnknowns.append(u);
    if(initial.contains(q("current_health"))) {
        if(!number(initial.value(q("current_health")),0,episode.maxHealth))return fail(q("invalid_initial_current_health"));
        episode.health=initial.value(q("current_health")).toDouble();episode.healthBasis=q("provided_scenario_health");
        if(episode.health<=0){episode.dead=true;episode.deathAt=s.gameTime;}
    }
    if(initialSelf.value(q("is_dead")).isBool()) {
        const bool dead=initialSelf.value(q("is_dead")).toBool();
        if(initial.contains(q("current_health")) && dead!=(episode.health<=0))return fail(q("initial_life_state_inconsistent"));
        if(dead) {
            episode.dead=true;episode.health=0;episode.deathAt=s.gameTime;episode.healthBasis=q("provided_scenario_dead_at_initial_clock");
            if(number(initialSelf.value(q("respawn_seconds")),0,600))episode.respawnAt=s.gameTime+initialSelf.value(q("respawn_seconds")).toDouble();
        }
    }
    syncSelf(episode);out[q("initial_state")]=summary(episode);out[q("patch")]=catalog.patch;out[q("combat_revision")]=db.revision;
    const auto style=request.value(q("style")).toString(q("按英雄定位"));
    QJsonArray steps,trajectory;int acceptedCount=0,rejectedCount=0;TacticalTimeline timeline;
    episode.game.tacticalHistory=timeline.observe(episode.game);
    int index=0;
    for(const auto &v:request.value(q("events")).toArray()) {
        const auto event=v.toObject();const auto type=event.value(q("type")).toString();const double time=event.value(q("time")).toDouble();
        const auto safeType=QStringList{q("enter"),q("augment_offer"),q("select_augment"),q("income"),q("purchase"),q("combat"),q("death"),q("respawn"),q("observe"),q("end")}.contains(type)?type:q("unsupported");
        auto before=summary(episode);before[q("players")]=episode.game.players;before[q("abilities")]=episode.game.abilities;
        before[q("combat_revision")]=db.revision;
        Episode next=episode;bool accepted=false;QString reason;QStringList unknowns;
        QJsonObject decision;const auto provenance=type==q("observe")?q("provided_observation"):q("synthetic_scenario");
        if(episode.ended)reason=q("episode_already_ended");
        else if(time<episode.game.gameTime)reason=q("event_precedes_completed_interval");
        else if(!eventNumbersValid(event,reason)){}
        else {
            next.game.gameTime=time;
            if(type==q("enter")) {accepted=true;reason=q("entered_conditional_episode");decision[q("tactical_context")]=analyzeTacticalContext(catalog,next.game);}
            else if(type==q("augment_offer")) {
                QList<int> offers;
                if(!readIds(event.value(q("offers")),db,true,offers,6) || offers.isEmpty())reason=q("invalid_augment_offers");
                else {bool eligible=true;for(int id:offers)eligible&=catalog.ordinaryOfferEligible(id)&&!next.game.selected.contains(id)&&db.augments[id].requiredLevel<=next.game.level;
                    if(!eligible)reason=q("ineligible_or_already_selected_offer");
                    else {next.game.offers=offers;decision=buildAugmentDecision(catalog,next.game,{},style,{},true);accepted=true;reason=q("offers_recorded");}
                }
            }
            else if(type==q("select_augment")) {
                int selected=0;bool explicitChoice=event.contains(q("augment_id"));decision=buildAugmentDecision(catalog,next.game,{},style,{},true);
                if(explicitChoice) {if(integer(event.value(q("augment_id")),1,100000000))selected=event.value(q("augment_id")).toInt();else reason=q("invalid_augment_id");}
                else {
                    bool allComparable=decision.value(q("available")).toBool()&&!next.game.offers.isEmpty();double best=-std::numeric_limits<double>::infinity();
                    for(const auto &row:decision.value(q("offers")).toArray()) {const auto a=row.toObject();const int id=a.value(q("augment_id")).toInt();
                        const bool known=a.value(q("available")).toBool()&&a.value(q("marginal_comparable")).toBool()&&number(a.value(q("current_gain_percent")),-1e12,1e12)&&db.augments.contains(id)&&db.augments[id].coverage==q("modeled_numeric_effects");
                        allComparable&=known;if(known && (a.value(q("current_gain_percent")).toDouble()>best || (a.value(q("current_gain_percent")).toDouble()==best && id<selected))) {best=a.value(q("current_gain_percent")).toDouble();selected=id;}
                    }
                    if(!allComparable){selected=0;reason=q("automatic_augment_withheld_unknown_or_incomparable_effects");}
                }
                if(selected>0 && next.game.offers.contains(selected) && !next.game.selected.contains(selected) && next.game.selected.size()<6 && catalog.ordinaryOfferEligible(selected)) {
                    next.game.selected.append(selected);next.game.offers.clear();
                    if(refreshHealth(catalog,next,unknowns)){accepted=true;++next.selections;reason=explicitChoice?q("explicit_augment_choice_applied"):q("known_model_augment_choice_applied");decision[q("selected_augment_id")]=selected;
                        if(db.augments[selected].coverage!=q("modeled_numeric_effects"))unknowns.append(q("explicit_choice_contains_unmodeled_augment_effects"));}
                    else reason=q("selected_augment_model_unavailable");
                } else if(reason.isEmpty())reason=q("augment_not_in_current_offers_or_limit_reached");
            }
            else if(type==q("income")) {
                if(!event.contains(q("amount")))reason=q("income_amount_required");
                else if(next.game.gold+event.value(q("amount")).toDouble()>1000000)reason=q("gold_limit_exceeded");
                else {const double amount=event.value(q("amount")).toDouble();next.game.gold+=amount;next.earned+=amount;accepted=true;reason=q("explicit_scenario_income_applied");}
            }
            else if(type==q("purchase")) {
                if(!event.value(q("purchase_window")).isBool() || !event.value(q("purchase_window")).toBool())reason=q("explicit_purchase_window_required");
                else {
                    int id=0;
                    if(event.contains(q("item_id"))) {if(integer(event.value(q("item_id")),1,100000000))id=event.value(q("item_id")).toInt();}
                    else {decision=buildPurchaseSandbox(catalog,next.game,style);id=decision.value(q("selected_item_id")).toInt();}
                    const auto transaction=planInventoryPurchase(db,next.game.championId,next.game.level,next.game.owned,id);
                    decision[q("purchase_transaction")]=transaction;
                    if(!transaction.value(q("legal")).toBool())reason=transaction.value(q("reason")).toString();
                    else {QList<int> inventory;for(const auto &value:transaction.value(q("inventory_after")).toArray())inventory.append(value.toInt());
                        const int cost=transaction.value(q("remaining_cost")).toInt();
                        if(cost<=0 || next.game.gold+1e-9<cost)reason=q("insufficient_gold_or_invalid_recipe_cost");
                        else {next.game.owned=inventory;next.game.gold-=cost;next.spent+=cost;
                            if(refreshHealth(catalog,next,unknowns)){accepted=true;++next.purchases;reason=q("recipe_consumed_and_budget_debited");decision[q("purchased_item_id")]=id;decision[q("spent_gold")]=cost;decision[q("no_historical_growth_awarded")]=true;}
                            else reason=q("purchased_inventory_model_unavailable");
                        }
                    }
                }
            }
            else if(type==q("combat")) {
                const auto action=event.value(q("action")).toString();
                if(!QStringList{q("engage"),q("focus_damage"),q("peel"),q("hold")}.contains(action))reason=q("unsupported_combat_action");
                else if(!event.contains(q("seconds")) || !event.contains(q("execution_factor")) || !event.contains(q("incoming_dps")))reason=q("explicit_seconds_execution_factor_and_incoming_dps_required");
                else if(time+event.value(q("seconds")).toDouble()>86400)reason=q("combat_end_time_exceeds_limit");
                else {
                    const double seconds=event.value(q("seconds")).toDouble();const auto p=event.value(q("action_parameters")).toObject();
                    const double outputMultiplier=action==q("hold")?0:p.value(q("outgoing_multiplier")).toDouble(1),incomingMultiplier=p.value(q("incoming_multiplier")).toDouble(1);
                    const double incoming=event.value(q("incoming_dps")).toDouble()*incomingMultiplier;
                    const double active=next.dead?0:incoming>0?std::min(seconds,next.health/incoming):seconds;
                    auto ctx=contextFor(catalog,next,event,unknowns);ctx.horizon=next.combatClock;
                    const auto start=evaluateCombat(db,ctx,next.game.owned,next.game.selected,false,resolvedCombatPreference(catalog,next.game,style));
                    ctx.horizon=next.combatClock+active;
                    const auto finish=evaluateCombat(db,ctx,next.game.owned,next.game.selected,false,resolvedCombatPreference(catalog,next.game,style));
                    if(!start.valid || !finish.valid)reason=q("combat_interval_model_unavailable");
                    else {
                        const auto total=[](const CombatEvaluation &r){return r.attackDamage+r.skillDamage+r.procDamage;};
                        const double potential=std::max(0.,total(finish)-total(start));const double damage=potential*event.value(q("execution_factor")).toDouble()*outputMultiplier;
                        const double taken=std::min(next.health,active*incoming);next.damage+=damage;next.taken+=taken;next.health=std::max(0.,next.health-taken);next.combatClock+=active;
                        if(!next.dead && next.health<=1e-9){next.health=0;next.dead=true;next.deathAt=time+active;next.respawnAt=-1;++next.deaths;
                            if(event.contains(q("respawn_seconds")))next.respawnAt=next.deathAt+event.value(q("respawn_seconds")).toDouble();}
                        next.game.gameTime=time+seconds;next.healthBasis=q("conditional_incoming_damage_budget");
                        unknowns.append(finish.unknowns);accepted=true;reason=active==0?q("dead_interval_zero_output"):q("continuous_conditional_combat_interval");
                        decision={{q("action"),action},{q("seconds"),seconds},{q("alive_seconds"),active},{q("modeled_potential_damage"),potential},{q("realized_scenario_damage"),damage},{q("incoming_damage"),taken},
                            {q("combat_clock_start"),episode.combatClock},{q("combat_clock_end"),next.combatClock},{q("timing_policy"),q("same_origin_cumulative_potential_difference_no_new_openers_on_step_or_build_change")},
                            {q("incoming_dps_basis"),q("explicit_scenario_already_mitigated_damage_per_second_not_game_observation")},
                            {q("scenario_parameters"),QJsonObject{{q("execution_factor"),event.value(q("execution_factor"))},{q("incoming_dps"),event.value(q("incoming_dps"))},{q("outgoing_multiplier"),outputMultiplier},{q("incoming_multiplier"),incomingMultiplier}}},
                            {q("skill_hit_or_control_prediction"),false},{q("cooldown_transition_simulated"),false},{q("healing_budget_applied"),false}};
                    }
                }
            }
            else if(type==q("death")) {
                if(next.dead && next.respawnAt>=0)reason=q("death_already_recorded_with_known_timer");
                else {if(!next.dead){next.dead=true;next.health=0;next.deathAt=time;++next.deaths;}
                    if(event.contains(q("respawn_seconds")))next.respawnAt=next.deathAt+event.value(q("respawn_seconds")).toDouble();
                    accepted=true;reason=next.respawnAt>=0?q("death_and_explicit_respawn_timer_recorded"):q("death_recorded_respawn_time_unknown");
                    if(next.respawnAt<0)unknowns.append(q("respawn_timer_unknown_no_automatic_respawn"));}
            }
            else if(type==q("respawn")) {
                if(!next.dead)reason=q("player_not_dead");else if(next.respawnAt<0)reason=q("respawn_timer_unknown");else if(time+1e-9<next.respawnAt)reason=q("respawn_before_known_ready_time");
                else {next.dead=false;next.health=next.maxHealth;next.healthBasis=q("explicit_respawn_full_modeled_health_scenario");next.respawnAt=-1;++next.respawns;accepted=true;reason=q("respawned_no_item_level_stack_or_cast_reset");}
            }
            else if(type==q("observe")) {
                bool valid=true,rosterChanged=false;
                if(event.contains(q("level"))) {valid&=event.value(q("level")).toInt()>=next.game.level;next.game.level=event.value(q("level")).toInt();}
                if(event.contains(q("abilities"))) {
                    valid&=abilitiesValid(event.value(q("abilities")),db,next.game.championId,next.game.level,&unknowns);
                    if(valid) {
                        const auto ranks=cleanAbilities(event.value(q("abilities")).toObject());
                        for(auto rank=ranks.begin();rank!=ranks.end();++rank) {
                            const int old=next.game.abilities.value(rank.key()).toObject().value(q("abilityLevel")).toInt(-1);
                            const int value=rank.value().toObject().value(q("abilityLevel")).toInt();
                            if(old>value)valid=false;next.game.abilities[rank.key()]=rank.value();
                        }
                        valid&=abilitiesValid(next.game.abilities,db,next.game.championId,next.game.level,&unknowns);
                    }
                }
                if(event.contains(q("players"))) {QJsonArray players;valid&=playersValid(event.value(q("players")),catalog,next.game.championId,players);
                    if(valid) {
                        const auto signature=rosterSignature(players);
                        if(!next.rosterAnchor.isEmpty() && next.rosterAnchor!=signature){valid=false;rosterChanged=true;}
                        else if(next.rosterAnchor.isEmpty())next.rosterAnchor=signature;
                    }
                    if(valid){next.game.players=players;for(const auto &pv:players)if(pv.toObject().value(q("self")).toBool()) {
                        valid&=pv.toObject().value(q("level")).toInt()==next.game.level;
                        QList<int> gear;readIds(pv.toObject().value(q("items")),db,false,gear);next.game.owned=gear;
                        if(pv.toObject().value(q("augments_complete")).toBool()){QList<int> aug;readIds(pv.toObject().value(q("augment_ids")),db,true,aug);next.game.selected=aug;}
                        if(pv.toObject().value(q("is_dead")).isBool()) {
                            const bool observedDead=pv.toObject().value(q("is_dead")).toBool();
                            if(!observedDead && next.dead)valid=false;
                            if(observedDead && !next.dead){next.dead=true;next.health=0;next.deathAt=time;next.respawnAt=-1;++next.deaths;}
                            const auto remaining=pv.toObject().value(q("respawn_seconds"));
                            if(observedDead && number(remaining,0,600))next.respawnAt=time+remaining.toDouble();
                        }
                    }}
                }
                if(event.contains(q("gold")))next.game.gold=event.value(q("gold")).toDouble();
                for(int id:next.game.selected)valid&=db.augments[id].requiredLevel<=next.game.level;
                if(valid)valid=legalInventory(db,next.game.owned,next.game.championId,next.game.level)&&refreshHealth(catalog,next,unknowns);
                if(valid && event.contains(q("current_health"))) {
                    const double hp=event.value(q("current_health")).toDouble();valid=hp<=next.maxHealth && !(next.dead&&hp>0);
                    if(valid){next.health=hp;next.healthBasis=q("provided_observation_health");if(hp<=0&&!next.dead){next.dead=true;next.deathAt=time;next.respawnAt=-1;++next.deaths;}}
                }
                if(valid){accepted=true;reason=q("anonymous_provided_observation_applied_as_of_time");next.provenance=provenance;}
                else reason=rosterChanged?q("match_roster_or_self_team_changed"):q("invalid_observation_or_implicit_resurrection");
            }
            else if(type==q("end")) {next.ended=true;accepted=true;reason=q("episode_ended");}
            else reason=q("unsupported_event_type");
        }
        if(accepted){syncSelf(next);next.game.tacticalHistory=timeline.observe(next.game);episode=next;++acceptedCount;}else ++rejectedCount;
        unknowns.removeDuplicates();for(const auto &u:unknowns)if(!globalUnknowns.contains(u))globalUnknowns.append(u);
        const auto after=summary(episode);
        QJsonObject step{{q("index"),index},{q("time"),time},{q("type"),safeType},{q("accepted"),accepted},{q("reason"),reason},{q("state"),after},
            {q("decision"),decision},{q("fallback"),fallback(episode,reason)},{q("unknowns"),QJsonArray::fromStringList(unknowns)}};
        steps.append(step);
        // Features are captured before this event and contain no future event,
        // future offer, eventual winner, account identifier, or hidden label.
        QJsonObject trainingAction{{q("type"),safeType}};
        // Rebuild per event type: an accepted enter event may still contain
        // arbitrary unused fields, which must never enter an anonymous trace.
        if(accepted && type==q("combat"))for(const auto &key:{q("action"),q("seconds"),q("execution_factor"),q("incoming_dps"),q("action_parameters")})if(event.contains(key))trainingAction[key]=event.value(key);
        if(accepted && type==q("augment_offer"))trainingAction[q("offers")]=ids(episode.game.offers);
        if(accepted && type==q("income"))trainingAction[q("amount")]=event.value(q("amount"));
        if(accepted && decision.contains(q("selected_augment_id")))trainingAction[q("augment_id")]=decision.value(q("selected_augment_id"));
        if(accepted && decision.contains(q("purchased_item_id")))trainingAction[q("item_id")]=decision.value(q("purchased_item_id"));
        trajectory.append(QJsonObject{{q("index"),index},{q("as_of_time"),before.value(q("game_time"))},{q("event_time"),time},{q("provenance"),provenance},
            {q("features_before"),before},{q("action"),trainingAction},
            {q("result_state"),after},{q("accepted"),accepted},{q("reason"),reason},{q("no_future_information"),true},
            {q("eligible_for_action_value_training"),false},{q("eligible_for_model_consistency_checks"),true},{q("observation_verified_against_game"),false},{q("real_world_reward"),nil()}});
        ++index;
    }
    out[q("available")]=true;out[q("status")]=rejectedCount?q("completed_with_withheld_steps"):q("completed");out[q("steps")]=steps;out[q("training_trajectory")]=trajectory;
    out[q("final_state")]=summary(episode);out[q("final_tactical_context")]=analyzeTacticalContext(catalog,episode.game);
    out[q("metrics")]=QJsonObject{{q("accepted_events"),acceptedCount},{q("rejected_events"),rejectedCount},{q("purchases"),episode.purchases},{q("augment_selections"),episode.selections},
        {q("deaths"),episode.deaths},{q("respawns"),episode.respawns},{q("spent_gold"),episode.spent},{q("scenario_income"),episode.earned},
        {q("modeled_damage"),episode.damage},{q("incoming_damage"),episode.taken},{q("active_combat_seconds"),episode.combatClock},{q("actual_win_rate"),nil()}};
    out[q("policy")]=q("verified_legal_transitions_known_model_choices_unknown_actions_withheld");out[q("policy_config_applied")]=false;
    out[q("deterministic")]=true;out[q("seed_used_for_randomness")]=false;
    out[q("unknowns")]=QJsonArray::fromStringList(globalUnknowns);return out;
}
