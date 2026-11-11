#include "tactical_context.h"
#include "combat.h"
#include "combat_sandbox.h"
#include "decision_context.h"
#include <QCache>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>

namespace {
QString q(const char *s) { return QString::fromUtf8(s); }
bool number(const QJsonValue &v) { return v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble() >= 0; }
QString person(const QJsonObject &p) { return p.value(q("team")).toString()+q(":")+QString::number(p.value(q("champion_id")).toInt()); }
QJsonObject capabilities(const Catalog &c, int id) {
    // A deliberately bounded reviewed table. Every cited slot must exist in
    // this catalog. Unlisted champions have unknown control capability, not 0.
    struct Profile { int id; const char *engage; const char *support; const char *peel; };
    static const Profile profiles[]{
        {54,"R","","R"},{89,"E,R","","Q,R"},{12,"W,Q","","Q,W"},
        {32,"Q,R","","R"},{111,"Q,R","","Q,R"},{516,"R,E","","R,E"},
        {154,"E,R","","E,R"},{113,"Q,R","","Q,R"},{57,"W,R","","Q,R"},
        {59,"E,Q,R","","E,Q"},{22,"R","","R"},{497,"W,R","E,Q","W,R"},
        {53,"Q","","E,R"},{412,"Q","W","E,R"},{526,"W,R","","Q,W"},
        {98,"E","R","E,R"},{201,"R","W,E","Q,R"},{40,"","E,R","Q,R"},
        {117,"","E,R","W,R"},{16,"","W,R","E"},{267,"Q,R","W,E","Q,R"},
        {37,"R","W","R"},{350,"","E,R","R"},{43,"","E","W"},
        {99,"","W","Q"},{61,"R","E","R"},{25,"","E","Q,R"}
    };
    QJsonObject out{{q("reviewed"),false},{q("engage"),QJsonValue::Null},{q("support"),QJsonValue::Null},{q("peel"),QJsonValue::Null},
        {q("ready_now_known"),false},{q("hit_or_control_duration_simulated"),false}};
    const Profile *profile=nullptr; for(const auto &p:profiles)if(p.id==id){profile=&p;break;}
    if(!profile)return out;
    const auto hero=c.champion(id);const auto abilities=hero.value(q("abilities")).toArray();QJsonArray refs;
    for(const auto &axis:{q("engage"),q("support"),q("peel")}) {
        const auto abilitySlots=q(axis==q("engage")?profile->engage:axis==q("support")?profile->support:profile->peel).split(',',Qt::SkipEmptyParts);
        bool complete=true;
        for(const auto &slot:abilitySlots){bool found=false;for(const auto &v:abilities){const auto a=v.toObject();if(a.value(q("spellKey"))==slot && !a.value(q("description")).toString().isEmpty()){
            refs.append(QJsonObject{{q("axis"),axis},{q("slot"),slot},{q("name"),a.value(q("name"))},{q("description"),plainText(a.value(q("description")).toString())}});found=true;break;}}
            complete &= found;
        }
        if(complete)out[axis]=!abilitySlots.isEmpty();
    }
    out[q("reviewed")]=true;out[q("ability_evidence")]=refs;out[q("source")]=hero.value(q("source"));
    out[q("scope")]=q("reviewed conditional kit capability; not observed casting, readiness, reachability or successful engage");
    return out;
}

QJsonObject loadoutRoles(const Catalog &c,const GameState &s) {
    QJsonObject out{{q("available"),false},{q("players"),QJsonArray{}},{q("roles_are_observed_behavior"),false},
        {q("scope"),q("full-health reference-target numeric shares; conditional support/engage/peel ability capabilities")}};
    if(!c.combat || !s.fresh() || !modelRulesUsable(c,s) || s.gameMode!=q("KIWI") || s.players.size()!=10)return out;
    QString own;int selves=0;QMap<QString,int> counts;
    for(const auto &v:s.players){const auto p=v.toObject();++counts[p.value(q("team")).toString()];if(p.value(q("self")).toBool()){++selves;own=p.value(q("team")).toString();}}
    if(selves!=1 || counts.value(q("ORDER"))!=5 || counts.value(q("CHAOS"))!=5 || (own!=q("ORDER") && own!=q("CHAOS")))return out;
    // Keep rows in canonical order in the cache; row_index is projected later.
    const QJsonObject basis{{q("loadouts"),liveLoadoutBasis(s)},{q("db"),QString::number(quintptr(c.combat.get()),16)},
        {q("root"),c.root},{q("revision"),c.combat->revision},{q("patch"),c.patch}};
    const auto bytes=QJsonDocument(basis).toJson(QJsonDocument::Compact);
    static thread_local QCache<QByteArray,QJsonObject> cache(64);
    if(const auto *p=cache.object(bytes))return *p;
    const auto &db=*c.combat;QJsonArray rows;QMap<QString,double> damage,health;QMap<QString,int> modeled;
    for(const auto &v:s.players){const auto p=v.toObject();const int id=p.value(q("champion_id")).toInt(),level=p.value(q("level")).toInt();
        QJsonObject row{{q("player_key"),person(p)},{q("champion_id"),id},{q("team"),p.value(q("team"))},{q("self"),p.value(q("self"))},
            {q("capabilities"),capabilities(c,id)},{q("model_available"),false},{q("damage_share"),QJsonValue::Null},{q("durability_share"),QJsonValue::Null}};
        bool valid=db.champions.contains(id)&&p.value(q("level")).isDouble()&&level==p.value(q("level")).toDouble()&&level>=1&&level<=18
            &&p.value(q("items_complete")).toBool()&&p.value(q("items")).isArray()&&p.value(q("items")).toArray().size()<=6;
        QList<int> items,augments;
        for(const auto &x:p.value(q("items")).toArray()){valid &= number(x)&&x.toDouble()==x.toInt()&&db.items.contains(x.toInt());items.append(x.toInt());}
        if(p.contains(q("augment_ids"))&&!p.value(q("augment_ids")).isArray())valid=false;
        for(const auto &x:p.value(q("augment_ids")).toArray()){valid &= number(x)&&x.toDouble()==x.toInt()&&db.augments.contains(x.toInt())&&!augments.contains(x.toInt());augments.append(x.toInt());}
        if(augments.size()>12)valid=false;
        if(valid){auto ctx=db.context(id,level);ctx.ranks=referenceCombatRanks(db,id,level);ctx.discreteSkillCasts=true;
            const auto value=evaluateCombat(db,ctx,items,augments);const double d=value.attackDamage+value.skillDamage+value.procDamage;
            if(value.valid&&std::isfinite(d)&&d>=0&&std::isfinite(value.survival)&&value.survival>0){
                row[q("model_available")]=true;row[q("output_8s")]=d;row[q("reference_effective_hp")]=value.survival;
                damage[p.value(q("team")).toString()]+=d;health[p.value(q("team")).toString()]+=value.survival;++modeled[p.value(q("team")).toString()];
            }
        }
        row[q("augments_complete")]=p.value(q("augments_complete")).toBool();rows.append(row);
    }
    for(int i=0;i<rows.size();++i){auto row=rows[i].toObject();const auto side=row.value(q("team")).toString();QStringList roles;
        if(modeled.value(side)==5&&damage.value(side)>0&&health.value(side)>0){
            const double ds=row.value(q("output_8s")).toDouble()/damage[side],hs=row.value(q("reference_effective_hp")).toDouble()/health[side];
            row[q("damage_share")]=ds;row[q("durability_share")]=hs;
            if(ds>=.24)roles.append(q("伤害位"));if(hs>=.24)roles.append(q("承伤位"));
            if(roles.isEmpty())roles.append(ds>=hs?q("副输出"):q("副承伤"));
        }
        const auto kit=row.value(q("capabilities")).toObject();
        if(kit.value(q("support")).toBool())roles.append(q("辅助位"));if(kit.value(q("engage")).toBool())roles.append(q("开团位"));if(kit.value(q("peel")).toBool())roles.append(q("保护位"));
        if(roles.isEmpty())roles.append(q("职责待确认"));row[q("roles")]=QJsonArray::fromStringList(roles);rows[i]=row;
    }
    out[q("available")]=true;out[q("players")]=rows;out[q("fully_modeled_allies")]=modeled.value(own)==5;
    out[q("numeric_role_threshold")]=.24;out[q("unknowns")]=QJsonArray{q("reference_ranks_and_full_health"),q("hidden_augments_stacks_and_mode_mechanics_partial"),q("unreviewed_control_kits_unknown"),q("ability_cooldowns_hit_rate_and_position_unobserved")};
    cache.insert(bytes,new QJsonObject(out));return out;
}
}

void TacticalTimeline::clear(){context.clear();roster.clear();lastTime=-1;continuousSince=-1;previous.clear();events={};last={};}
QJsonObject TacticalTimeline::observe(const GameState &s){
    const auto id=s.matchContext.value(q("context_id")).toString();
    if(!s.fresh()||s.gameMode!=q("KIWI")||id.isEmpty()||!std::isfinite(s.gameTime)||s.gameTime<0){clear();return {{q("available"),false},{q("reason"),q("fresh_kiwi_match_clock_required")}};}
    QStringList keys;QMap<QString,QJsonObject> now;
    for(const auto &v:s.players){const auto p=v.toObject();const auto k=person(p);keys.append(k);now[k]=p;}
    keys.sort();const auto rosterKey=keys.join('|');
    bool validRoster=true;QMap<QString,int> teamCounts;
    for(const auto &v:s.players){const auto p=v.toObject();const auto side=p.value(q("team")).toString();++teamCounts[side];validRoster &= p.value(q("champion_id")).isDouble()&&p.value(q("champion_id")).toInt()>0&&(side==q("ORDER")||side==q("CHAOS"));}
    if(!validRoster||teamCounts[q("ORDER")]!=5||teamCounts[q("CHAOS")]!=5||s.players.size()!=10||now.size()!=10){clear();return {{q("available"),false},{q("reason"),q("ten_unique_anonymous_players_required")}};}
    QString reset;
    if(id!=context)reset=q("match_changed");else if(rosterKey!=roster)reset=q("roster_changed");
    // Old network observations must never move an active match backwards.
    if(reset.isEmpty()&&lastTime>=0&&s.gameTime<lastTime){auto out=last;out[q("observation_accepted")]=false;out[q("reason")]=q("out_of_order_clock_ignored");return out;}
    if(reset.isEmpty()&&lastTime>=0&&s.gameTime==lastTime){auto out=last;out[q("observation_accepted")]=false;out[q("reason")]=q("duplicate_clock_ignored");return out;}
    if(reset.isEmpty()&&lastTime>=0&&s.gameTime-lastTime>5)reset=q("observation_gap_over_5_seconds");
    if(!reset.isEmpty()){clear();context=id;roster=rosterKey;continuousSince=s.gameTime;}
    if(lastTime>=0)for(auto it=now.cbegin();it!=now.cend();++it){const auto p=it.value(),before=previous.value(it.key());
        const auto scores=p.value(q("scores")).toObject(),old=before.value(q("scores")).toObject();
        QJsonObject deltas;for(const auto &stat:{q("kills"),q("deaths"),q("assists")})if(number(scores.value(stat))&&number(old.value(stat))){const double delta=scores.value(stat).toDouble()-old.value(stat).toDouble();if(delta>0)deltas[stat]=delta;}
        const bool lifeChanged=p.value(q("is_dead")).isBool()&&before.value(q("is_dead")).isBool()&&p.value(q("is_dead"))!=before.value(q("is_dead"));
        if(!deltas.isEmpty()||lifeChanged)events.append(QJsonObject{{q("time"),s.gameTime},{q("previous_observation_time"),lastTime},{q("player_key"),it.key()},
            {q("score_delta"),deltas},{q("life_changed"),lifeChanged},{q("is_dead"),p.value(q("is_dead"))}});
    }
    QJsonArray retained;for(const auto &v:events)if(s.gameTime-v.toObject().value(q("time")).toDouble()<60)retained.append(v);events=retained;
    const bool truncated=events.size()>512;while(events.size()>512)events.removeFirst();
    last={{q("available"),true},{q("context_id"),id},{q("observation_accepted"),true},{q("game_time"),s.gameTime},
        {q("continuous_since"),continuousSince},{q("continuous_seconds"),s.gameTime-continuousSince},{q("window_seconds"),60},
        {q("full_window_observed"),!truncated&&s.gameTime-continuousSince>=60},{q("recent_events"),events},{q("events_truncated"),truncated},{q("reset_reason"),reset},
        {q("event_time_is_exact"),false},{q("scope"),q("changes occurred between consecutive observed snapshots; not a cast or damage log")}};
    previous=now;lastTime=s.gameTime;return last;
}

QJsonObject analyzeTacticalContext(const Catalog &c,const GameState &s){
    auto out=loadoutRoles(c,s);out[q("timeline")]=s.tacticalHistory;if(!out.value(q("available")).toBool())return out;
    const auto models=out.value(q("players")).toArray();QJsonArray rows;QMap<QString,int> alive,known;QMap<QString,QJsonArray> missing;
    QString own;for(const auto &v:s.players)if(v.toObject().value(q("self")).toBool())own=v.toObject().value(q("team")).toString();
    QMap<QString,int> occurrences;for(const auto &v:s.players)++occurrences[person(v.toObject())];
    for(int i=0;i<s.players.size();++i){const auto raw=s.players[i].toObject();QJsonObject row;
        for(const auto &v:models){const auto m=v.toObject();if(m.value(q("player_key"))==person(raw)){row=m;break;}}
        row[q("row_index")]=i;row[q("is_dead")]=raw.value(q("is_dead"));const auto side=raw.value(q("team")).toString();
        if(raw.value(q("is_dead")).isBool()){++known[side];if(!raw.value(q("is_dead")).toBool())++alive[side];else missing[side].append(row.value(q("champion_id")));}
        const auto respawn=raw.value(q("respawn_seconds"));row[q("respawn_in")]=raw.value(q("is_dead")).toBool()&&number(respawn)?respawn:QJsonValue(QJsonValue::Null);
        row[q("availability_identity_unambiguous")]=occurrences[person(raw)]==1;rows.append(row);
        if(occurrences[person(raw)]!=1){row[q("damage_share")]=QJsonValue::Null;row[q("durability_share")]=QJsonValue::Null;row[q("roles")]=QJsonArray{q("重复英雄，职责待确认")};rows[rows.size()-1]=row;}
    }
    QJsonArray teams;
    for(const auto &side:{q("ORDER"),q("CHAOS")}){QJsonArray phases;
        for(double horizon:{0.,3.,8.,20.}){int count=0,unknown=0;double outputShare=0,healthShare=0;bool shares=true;int engagers=0,supports=0;
            for(const auto &v:rows){const auto p=v.toObject();if(p.value(q("team"))!=side)continue;
                const bool life=p.value(q("is_dead")).isBool(),dead=p.value(q("is_dead")).toBool();const auto respawn=p.value(q("respawn_in"));
                // Alive is observed only now. Future rows are conditional on no
                // further deaths; a respawn does not imply reaching the fight.
                const bool present=life&&(!dead||(horizon>0&&number(respawn)&&respawn.toDouble()<=horizon));
                if(!life||(horizon>0&&dead&&!number(respawn)))++unknown;if(!present)continue;++count;
                shares &= p.value(q("damage_share")).isDouble()&&p.value(q("durability_share")).isDouble();outputShare+=p.value(q("damage_share")).toDouble();healthShare+=p.value(q("durability_share")).toDouble();
                const auto kit=p.value(q("capabilities")).toObject();engagers+=kit.value(q("engage")).toBool();supports+=kit.value(q("support")).toBool();
            }
            phases.append(QJsonObject{{q("seconds_from_now"),horizon},{q("alive_if_no_further_deaths"),count},{q("unknown_life_or_respawn"),unknown},
                {q("available_output_share"),shares&&!unknown?QJsonValue(outputShare):QJsonValue(QJsonValue::Null)}, {q("available_durability_share"),shares&&!unknown?QJsonValue(healthShare):QJsonValue(QJsonValue::Null)},
                {q("reviewed_engagers_alive"),engagers},{q("reviewed_supports_alive"),supports},{q("presence_at_fight_known"),false}});
        }
        teams.append(QJsonObject{{q("team"),side},{q("ally"),side==own},{q("observed_alive"),known[side]==5?QJsonValue(alive[side]):QJsonValue(QJsonValue::Null)},
            {q("dead_champions"),missing[side]},{q("availability_windows"),phases}});
    }
    out[q("players")]=rows;out[q("teams")]=teams;
    QString other=own==q("ORDER")?q("CHAOS"):q("ORDER");QStringList absent;
    for(const auto &v:rows){const auto p=v.toObject();if(p.value(q("team"))!=other||!p.value(q("is_dead")).toBool())continue;
        if(p.value(q("damage_share")).toDouble()>=.24&&!absent.contains(q("对方伤害位暂缺")))absent.append(q("对方伤害位暂缺"));
        if(p.value(q("durability_share")).toDouble()>=.24&&!absent.contains(q("对方承伤位暂缺")))absent.append(q("对方承伤位暂缺"));
    }
    if(known[own]==5&&known[other]==5)out[q("summary")]=q("当前存活 %1 : %2").arg(alive[own]).arg(alive[other])+(absent.isEmpty()?QString():q(" · ")+absent.join(q(" / ")));
    out[q("purchase_horizon")]=q("next_full_team_fight; transient deaths do not erase long-term responsibilities");
    out[q("cooldowns_known")]=false;out[q("movement_simulated")]=false;return out;
}
QJsonObject tacticalPlayer(const QJsonObject &context,int index){for(const auto &v:context.value(q("players")).toArray())if(v.toObject().value(q("row_index")).toInt(-1)==index)return v.toObject();return {};}
double tacticalSurvivalWeight(const QJsonObject &p,double base){
    if(!p.value(q("damage_share")).isDouble()||!p.value(q("durability_share")).isDouble())return base;
    // Explicit bounded policy, not a learned win probability. Frozen at the
    // current loadout for every candidate, so an item cannot change its weights.
    return std::clamp(base+std::clamp(.6*(p.value(q("durability_share")).toDouble()-p.value(q("damage_share")).toDouble()),-.12,.12),.12,.88);
}
