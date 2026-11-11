#include "hextech_mechanism_state.h"
#include <QJsonDocument>
#include <QRegularExpression>
#include <cmath>

namespace {
QString k(const char *s) { return QString::fromLatin1(s); }
bool num(const QJsonValue &v, double lo=0, double hi=1e9) {
    return v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble()>=lo && v.toDouble()<=hi;
}
bool textId(const QJsonValue &v) { return v.isString() && !v.toString().isEmpty() && v.toString().size()<=512; }
bool fail(QStringList *out, const char *reason) { if(out)out->append(k(reason));return false; }
QString lockKey() { return k("augment:3081:locked_item"); }
QJsonObject effect(const char *op,const QString &key,double value,const QString &source,double at) {
    return {{k("op"),k(op)},{k("key"),key},{k("value"),value},{k("source"),source},
        {k("target"),k("self")},{k("start"),at},{k("expires"),-1}};
}
}

int hextech40LockedItem(const MechanicLedger &ledger) {
    return ledger.values.value(lockKey()).toObject().value(k("item_id")).toInt();
}

bool executeHextech40State(const CombatDatabase *db,const CombatEntry &entry,
    const CombatContext &ctx,const CombatStats &stats,const QJsonObject &obs,
    MechanicLedger &ledger,QStringList *missing) {
    const auto contract=entry.mechanismStateV1;
    static const QRegularExpression digest(k("^[0-9a-f]{64}$"));
    if(contract.value(k("review_id"))!=k("hextech40_conditional_state_v1") ||
       !digest.match(contract.value(k("review_sha256")).toString()).hasMatch() ||
       !digest.match(contract.value(k("source_projection_sha256")).toString()).hasMatch())
        return fail(missing,"hextech40_review_identity_missing");
    if(obs.value(k("source"))!=k("augment") || obs.value(k("id")).toInt()!=entry.id ||
       !ledger.augments.contains(entry.id))return fail(missing,"hextech40_source_not_owned");
    const auto event=obs.value(k("event")).toString();const auto facts=obs.value(k("facts")).toObject();
    if(!contract.value(k("events")).toArray().contains(event))return fail(missing,"hextech40_event_not_supported");
    if(!num(obs.value(k("time"))) || obs.value(k("time")).toDouble()<ledger.lastTime ||
       !textId(obs.value(k("event_id"))))return fail(missing,"hextech40_event_identity_or_time_invalid");
    const double at=obs.value(k("time")).toDouble();const auto eventId=obs.value(k("event_id")).toString();
    const auto occurrence=k("hextech40:")+eventId;
    if(ledger.consumed.contains(occurrence))return fail(missing,"hextech40_duplicate_event");
    auto staged=ledger;const auto source=k("augment:")+QString::number(entry.id)+k(":");
    if(entry.id==3049) {
        const auto key=source+k("primed");auto prime=staged.values.value(key).toObject();
        if(event==k("admitted_spell_cast")) {
            if(facts.value(k("verified_admitted_cast"))!=QJsonValue(true) ||
               !textId(facts.value(k("cast_instance_id"))) ||
               !QStringList{k("Q"),k("W"),k("E"),k("R")}.contains(facts.value(k("cast_slot")).toString()) ||
               facts.value(k("verified_impact_timing_contract"))!=QJsonValue(true) ||
               !num(facts.value(k("primed_window_seconds")),1e-6,120) ||
               !num(facts.value(k("internal_cooldown_seconds")),0,120))
                return fail(missing,"armor_impact_requires_admitted_cast_and_explicit_timing_contract");
            const auto castKey=source+k("cast:")+facts.value(k("cast_instance_id")).toString();
            if(staged.consumed.contains(castKey))
                return fail(missing,"armor_impact_duplicate_cast_instance");
            if(at<prime.value(k("proc_ready_at")).toDouble())return fail(missing,"armor_impact_internal_cooldown");
            prime={{k("active"),true},{k("primed_at"),at},
                {k("expires_at"),at+facts.value(k("primed_window_seconds")).toDouble()},
                {k("last_cast_instance_id"),facts.value(k("cast_instance_id"))},
                {k("internal_cooldown_seconds"),facts.value(k("internal_cooldown_seconds"))},
                {k("proc_ready_at"),prime.value(k("proc_ready_at")).toDouble()}};
            staged.values[key]=prime;
            staged.consumed.insert(castKey);
        } else {
            if(!prime.value(k("active")).toBool() || at>=prime.value(k("expires_at")).toDouble())
                return fail(missing,"armor_impact_not_primed_or_expired");
            const auto hitKind=facts.value(k("attack_kind")).toString();
            if(facts.value(k("verified_accepted_hit"))!=QJsonValue(true) ||
               facts.value(k("verified_current_hit_armor_basis"))!=QJsonValue(true) ||
               (hitKind!=k("basic_attack") && (hitKind!=k("skill_on_hit") ||
                facts.value(k("verified_skill_onhit_eligibility"))!=QJsonValue(true))))
                return fail(missing,"armor_impact_hit_eligibility_or_snapshot_unverified");
            if(hitKind==k("skill_on_hit") && at==prime.value(k("primed_at")).toDouble() &&
               facts.value(k("verified_same_cast_prime_before_hit"))!=QJsonValue(true))
                return fail(missing,"armor_impact_same_cast_order_unverified");
            const auto targets=facts.value(k("verified_impact_target_keys")).toArray();
            if(facts.value(k("verified_impact_target_set"))!=QJsonValue(true) || targets.isEmpty() || targets.size()>5)
                return fail(missing,"armor_impact_requires_actual_aoe_target_set");
            QSet<QString> seen;
            const auto amount=compileCombatFormula(entry.calculations.value(k("ArmorDamage")).toObject(),ctx).evaluate(ctx,stats);
            if(!amount || !std::isfinite(*amount) || *amount<0)return fail(missing,"armor_impact_source_formula_unavailable");
            for(const auto &target:targets) {
                if(!textId(target)||seen.contains(target.toString()))return fail(missing,"armor_impact_duplicate_or_invalid_target");
                seen.insert(target.toString());auto packet=effect("damage",k("physical"),*amount,source,at);
                packet[k("target")]=k("enemy");packet[k("target_id")]=target;
                packet[k("trigger_event_id")]=eventId;packet[k("review_sha256")]=contract.value(k("review_sha256"));
                staged.effects.append(packet);
            }
            prime[k("active")]=false;prime[k("consumed_by")]=eventId;
            prime[k("proc_ready_at")]=at+prime.value(k("internal_cooldown_seconds")).toDouble();staged.values[key]=prime;
        }
    } else if(entry.id==3081) {
        if(!db)return fail(missing,"destiny_requires_item_database");
        auto locked=staged.values.value(lockKey()).toObject();
        if(event==k("destiny_bind")) {
            const auto id=facts.value(k("first_legendary_id"));
            if(!locked.isEmpty())return fail(missing,"destiny_first_legendary_already_locked");
            if(!num(id,1,2147483647)||id.toDouble()!=id.toInt()||!ledger.inventory.contains(id.toInt())||
               db->items.value(id.toInt()).epicness!=5 || db->items.value(id.toInt()).boots ||
               facts.value(k("verified_first_legendary"))!=QJsonValue(true)||
               !textId(facts.value(k("item_instance_id"))))return fail(missing,"destiny_requires_owned_first_legendary_instance");
            locked={{k("item_id"),id},{k("instance_id"),facts.value(k("item_instance_id"))},
                {k("bound_at"),at},{k("review_sha256"),contract.value(k("review_sha256"))},{k("upgrades"),0}};
            staged.values[lockKey()]=locked;
        } else {
            if(locked.isEmpty()||!ledger.inventory.contains(locked.value(k("item_id")).toInt())||
               locked.value(k("review_sha256"))!=contract.value(k("review_sha256")))
                return fail(missing,"destiny_bound_item_missing_or_review_changed");
            const auto victim=facts.value(k("victim_key"));
            if(!textId(victim))return fail(missing,"destiny_victim_identity_required");
            const auto damageKey=source+k("own_damage:")+victim.toString();
            if(event==k("destiny_own_damage")) {
                if(facts.value(k("verified_own_champion_damage"))!=QJsonValue(true)||
                   !num(facts.value(k("actual_damage")),1e-9))return fail(missing,"destiny_requires_positive_own_damage_receipt");
                staged.values[damageKey]=QJsonObject{{k("time"),at},{k("event_id"),eventId}};
            } else {
                if(!textId(facts.value(k("victim_death_id"))))return fail(missing,"destiny_death_identity_required");
                const auto deathKey=source+k("death:")+facts.value(k("victim_death_id")).toString();
                auto death=staged.values.value(deathKey).toObject();
                if(event==k("destiny_victim_death")) {
                    const auto damage=staged.values.value(damageKey).toObject();
                    if(!death.isEmpty())return fail(missing,"destiny_duplicate_victim_death");
                    if(facts.value(k("verified_victim_death"))!=QJsonValue(true)||damage.isEmpty()||
                       at-damage.value(k("time")).toDouble()>contract.value(k("damage_window_seconds")).toDouble() ||
                       at<damage.value(k("time")).toDouble())return fail(missing,"destiny_no_own_damage_within_three_seconds");
                    staged.values[deathKey]=QJsonObject{{k("victim_key"),victim},{k("time"),at},
                        {k("upgraded"),false},{k("damage_event_id"),damage.value(k("event_id"))}};
                    staged.values.remove(damageKey);
                } else {
                    if(death.isEmpty()||death.value(k("victim_key"))!=victim||death.value(k("upgraded")).toBool())
                        return fail(missing,"destiny_upgrade_requires_unconsumed_qualified_death");
                    if(facts.value(k("item_instance_id"))!=locked.value(k("instance_id"))||
                       facts.value(k("verified_upgrade_transaction"))!=QJsonValue(true))
                        return fail(missing,"destiny_upgrade_instance_or_transaction_unverified");
                    const auto deltas=facts.value(k("verified_destiny_stat_deltas")).toArray();
                    if(deltas.isEmpty()||deltas.size()>24)return fail(missing,"destiny_observed_stat_deltas_required");
                    QSet<QString> statsSeen;const auto item=db->items.value(locked.value(k("item_id")).toInt());
                    for(const auto &raw:deltas) {
                        const auto row=raw.toObject();const auto name=row.value(k("stat")).toString();const auto stat=combatStat(name);
                        if(!stat||item.stats[*stat]<=0||statsSeen.contains(name)||!num(row.value(k("value")),1e-9,1e6))
                            return fail(missing,"destiny_delta_not_positive_existing_item_stat");
                        statsSeen.insert(name);auto buff=effect("buff",name,row.value(k("value")).toDouble(),source,at);
                        buff[k("item_id")]=locked.value(k("item_id"));buff[k("item_instance_id")]=locked.value(k("instance_id"));
                        buff[k("upgrade_death_id")]=facts.value(k("victim_death_id"));buff[k("review_sha256")]=contract.value(k("review_sha256"));
                        staged.effects.append(buff);
                    }
                    death[k("upgraded")]=true;staged.values[deathKey]=death;
                    locked[k("upgrades")]=locked.value(k("upgrades")).toInt()+1;staged.values[lockKey()]=locked;
                }
            }
        }
    } else return fail(missing,"hextech40_consumer_not_implemented");
    staged.consumed.insert(occurrence);staged.lastTime=at;ledger=std::move(staged);return true;
}
