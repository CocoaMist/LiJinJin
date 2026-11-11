#pragma once
#include "catalog.h"
#include "live_client.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <algorithm>
#include <cmath>

// Presentation of existing observations and explicitly conditional models.
// No new damage estimate, item ranking or player identity is created here.
inline QJsonObject observedCoachScore(const GameState &game){
    const auto fail=[](const QString &reason){return QJsonObject{{"available",false},{"reason",reason}};};
    if(!game.fresh())return fail(QStringLiteral("等待实时比分"));
    QString own;int selves=0;for(const auto &v:game.players){const auto p=v.toObject();if(p.value("self").toBool()){++selves;own=p.value("team").toString();}}
    if(selves!=1||(own!="ORDER"&&own!="CHAOS")||game.players.size()!=10)return fail(QStringLiteral("队伍信息未完整核实"));
    QSet<QString> keys;int allies=0,enemies=0;double ours=0,theirs=0;
    for(const auto &v:game.players){const auto p=v.toObject();const auto side=p.value("team").toString();const auto champion=p.value("champion_id");const auto kills=p.value("scores").toObject().value("kills");
        if((side!="ORDER"&&side!="CHAOS")||!champion.isDouble()||champion.toDouble()!=champion.toInt()||champion.toInt()<=0
           ||!kills.isDouble()||!std::isfinite(kills.toDouble())||kills.toDouble()<0||std::floor(kills.toDouble())!=kills.toDouble()||kills.toDouble()>1000000)
            return fail(QStringLiteral("比分观测不完整"));
        const auto key=side+":"+QString::number(champion.toInt());if(keys.contains(key))return fail(QStringLiteral("阵容身份映射待核实"));keys.insert(key);
        if(side==own){++allies;ours+=kills.toDouble();}else{++enemies;theirs+=kills.toDouble();}
    }
    if(allies!=5||enemies!=5)return fail(QStringLiteral("双方阵容未完整核实"));
    return {{"available",true},{"ally_kills",ours},{"enemy_kills",theirs},{"source","current_fresh_observed_player_kills"},{"depends_on_purchase_model",false}};
}

inline QString coachRosterInsightsHtml(const Catalog &catalog,const QJsonObject &summary,const QJsonObject &coach,const QJsonObject &equipment){
    const auto e=[](const QString &s){return s.toHtmlEscaped();};
    const auto players=summary.value("players").toArray();
    if(!summary.value("fresh").toBool()||players.isEmpty())return QStringLiteral("<p><b>阵容线索</b><br>等待当前局的公开装备与等级；未公开能力不补零。</p>");
    const auto leader=[&](const QString &side,const QString &field){QJsonObject best;double highest=-1;
        for(const auto &v:players){const auto row=v.toObject();const auto value=row.value(field);if(row.value("team")!=side||!value.isDouble()||!std::isfinite(value.toDouble())||value.toDouble()<0)continue;
            if(value.toDouble()>highest){highest=value.toDouble();best=row;}}
        return best;};
    const auto describe=[&](const QJsonObject &row){if(row.isEmpty())return QStringLiteral("模型不足");QString text=catalog.championName(row.value("champion_id").toInt());if(row.value("is_dead").isBool()&&row.value("is_dead").toBool())text+=QStringLiteral("（阵亡）");return text;};
    const auto capability=[&](const QString &side,const QString &axis){QStringList names;for(const auto &v:players){const auto row=v.toObject(),kit=row.value("tactical_role").toObject().value("capabilities").toObject();if(row.value("team")!=side||!kit.value("reviewed").toBool()||!kit.value(axis).toBool())continue;
            QStringList abilitySlots;for(const auto &ability:kit.value("ability_evidence").toArray()){const auto a=ability.toObject();if(a.value("axis")==axis&&!abilitySlots.contains(a.value("slot").toString()))abilitySlots.append(a.value("slot").toString());}
            names.append(catalog.championName(row.value("champion_id").toInt())+(abilitySlots.isEmpty()?QString():" "+abilitySlots.join("/")));if(names.size()==2)break;}
        return names.isEmpty()?QStringLiteral("已核实信息不足"):names.join(QStringLiteral("、"));};
    QStringList lines;
    const auto tactics=summary.value("tactical_context").toObject();if(!tactics.value("summary").toString().isEmpty())lines.append(e(tactics.value("summary").toString()));
    lines.append(QStringLiteral("<b>敌方关注</b> 输出≈%1 · 承伤≈%2").arg(e(describe(leader("ENEMY","output_8s_estimate"))),e(describe(leader("ENEMY","effective_health_estimate")))));
    lines.append(QStringLiteral("开团线索：%1").arg(e(capability("ENEMY","engage"))));
    const auto allyOutput=e(describe(leader("ALLY","output_8s_estimate")));const auto allySupport=e(capability("ALLY","support"));
    QString allyLine=QStringLiteral("<b>队友配合</b> 输出≈%1 · 保护/增益：%2").arg(allyOutput,allySupport);
    const auto needs=coach.value("team_needs").toObject();if(needs.value("available").toBool()&&!needs.value("axes").toArray().isEmpty())allyLine+=QStringLiteral(" · 定位偏弱≈%1（粗估）").arg(e(needs.value("axes").toArray().first().toObject().value("axis").toString()));lines.append(allyLine);
    const auto quality=coach.value("recommendation_quality").toObject();const auto sandbox=coach.value("plan").toObject().value("purchase_sandbox").toObject();
    QString purchase=quality.value("summary").toString();if(purchase.isEmpty())purchase=sandbox.value("status")=="conditional"?QStringLiteral("按当前装备、等级和金币比较；未知触发未计入"):QStringLiteral("暂缺可核实购买方案，可到出装页查看已有装备");
    QString purchaseLine=QStringLiteral("<b>出装依据</b> %1").arg(e(purchase));const auto cue=equipment.value("latest").toObject();if(cue.value("item_id").toInt()>0)purchaseLine+=QStringLiteral(" · 新装备：%1 %2").arg(e(catalog.championName(cue.value("champion_id").toInt())),e(catalog.itemName(cue.value("item_id").toInt())));lines.append(purchaseLine);
    int unknownAugments=0;for(const auto &v:players)unknownAugments+=!v.toObject().value("augments_complete").toBool();
    lines.append(QStringLiteral("<span style='color:#a0aaba'>≈ 条件模型，部分规则；非实测贡献。%1/%2 人海克斯未核实；冷却/命中/站位未知。</span>").arg(unknownAugments).arg(players.size()));
    return QStringLiteral("<div style='font-size:12px;line-height:110%'>")+lines.join(QStringLiteral("<br>"))+QStringLiteral("</div>");
}
