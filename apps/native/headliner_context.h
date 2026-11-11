#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QStringList>
#include <algorithm>
#include <cmath>

// The same explicit, identity-free contract is accepted from the local log
// collector and replay fixtures. Account progression and hero K/D/A are absent.
inline QStringList headlinerRuleKeys() {
    return {QStringLiteral("KDA"),QStringLiteral("Heartsteel"),QStringLiteral("TrueDamage"),QStringLiteral("Pentakill"),
        QStringLiteral("Disco"),QStringLiteral("CafeCuties"),QStringLiteral("Bees"),QStringLiteral("CommandLine")};
}
inline QJsonObject sanitizeHeadlinerContext(const QJsonObject &input,bool replay=false) {
    const auto source=input.value(QStringLiteral("source")).toString();
    if(input.value(QStringLiteral("schema_version")).toInt()!=1||input.value(QStringLiteral("patch"))!=QStringLiteral("16.21")
       ||!headlinerRuleKeys().contains(input.value(QStringLiteral("rule_key")).toString())
       ||(source!=QStringLiteral("wegame_local_log")&&!(replay&&source==QStringLiteral("synthetic_replay"))))return {};
    const auto time=input.value(QStringLiteral("game_time"));
    if(!time.isDouble()||!std::isfinite(time.toDouble())||time.toDouble()<0||time.toDouble()>1e7
       ||!input.value(QStringLiteral("facts")).isObject())return {};
    QJsonObject facts;const auto raw=input.value(QStringLiteral("facts")).toObject();
    for(const auto &key:{QStringLiteral("kda_cards_collected"),QStringLiteral("extra_augment_slots_observed"),QStringLiteral("anvils_dropped"),
            QStringLiteral("relics_collected"),QStringLiteral("honey_pots_collected")})if(raw.contains(key)){
        const auto n=raw.value(key);if(!n.isDouble()||!std::isfinite(n.toDouble())||n.toDouble()!=n.toInt()||n.toInt()<0||n.toInt()>100000)return {};facts[key]=n;
    }
    if(raw.contains(QStringLiteral("disco_gold_awarded"))){const auto n=raw.value(QStringLiteral("disco_gold_awarded"));
        if(!n.isDouble()||!std::isfinite(n.toDouble())||n.toDouble()<0||n.toDouble()>1e7)return {};facts[QStringLiteral("disco_gold_awarded")]=n;}
    for(const auto &key:{QStringLiteral("heartsteel_modified_augment_ids"),QStringLiteral("command_line_upgraded_ids"),QStringLiteral("pentakill_reward_ids")})if(raw.contains(key)){
        if(!raw.value(key).isArray()||raw.value(key).toArray().size()>32)return {};QList<int> ids;
        for(const auto &n:raw.value(key).toArray()){const int id=n.toInt();if(!n.isDouble()||!std::isfinite(n.toDouble())||n.toDouble()!=id||id<1||id>1000000||ids.contains(id))return {};
            if(key==QStringLiteral("pentakill_reward_ids")&&!QList<int>{3170,3171,3172,3173,3176}.contains(id))return {};ids.append(id);}
        std::sort(ids.begin(),ids.end());QJsonArray array;for(int id:ids)array.append(id);facts[key]=array;
    }
    const auto slotKey=QStringLiteral("pentakill_upgraded_slots");if(raw.contains(slotKey)){
        if(!raw.value(slotKey).isArray()||raw.value(slotKey).toArray().size()>5)return {};QStringList seen;
        for(const auto &v:raw.value(slotKey).toArray()){const auto slot=v.toString();if(!QStringList{QStringLiteral("Q"),QStringLiteral("W"),QStringLiteral("E"),QStringLiteral("R"),QStringLiteral("Generic")}.contains(slot)||seen.contains(slot))return {};seen.append(slot);}
        seen.sort();facts[slotKey]=QJsonArray::fromStringList(seen);
    }
    return {{QStringLiteral("schema_version"),1},{QStringLiteral("patch"),QStringLiteral("16.21")},{QStringLiteral("rule_key"),input.value(QStringLiteral("rule_key"))},
        {QStringLiteral("source"),source},{QStringLiteral("game_time"),time},{QStringLiteral("facts"),facts}};
}
inline QJsonObject headlinerBasis(const QJsonObject &context) {
    // Repeated observations of identical facts do not schedule another search.
    auto stable=sanitizeHeadlinerContext(context,true);stable.remove(QStringLiteral("game_time"));return stable;
}
