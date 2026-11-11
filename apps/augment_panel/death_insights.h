#pragma once
#include "death_tips_content.h"

inline QString deathInsightHeadline(const QJsonObject &tip){
    const auto id=tip.value("tip_id").toString();const auto visual=tip.value("visual").toObject();
    const auto bars=visual.value("bars").toArray();
    if((id==QStringLiteral("enemy_output")||id==QStringLiteral("enemy_durability"))&&bars.size()>=2){
        const auto first=bars[0].toObject(),second=bars[1].toObject();
        const auto a=first.value("value"),b=second.value("value");
        if(a.isDouble()&&b.isDouble()&&std::isfinite(a.toDouble())&&std::isfinite(b.toDouble())&&a.toDouble()>0&&b.toDouble()>0){
            const double gap=(a.toDouble()/b.toDouble()-1)*100;
            const auto axis=id==QStringLiteral("enemy_output")?QStringLiteral("8秒条件输出"):QStringLiteral("条件承伤");
            return gap<5?QStringLiteral("%1 · 前两名差距不足5%").arg(axis)
                :QStringLiteral("%1 · %2 +%3%").arg(first.value("label").toString(),axis).arg(gap,0,'f',1);
        }
    }
    return tip.value("title").toString();
}

// The death overlay only shows a new comparison or an observed inventory
// change. Capability reminders and generic preparation checklists stay out.
inline QJsonArray deathInsightsForState(const Catalog &catalog,const GameState &game,const QJsonObject &coach,
                                       const QJsonObject &summary,const QJsonObject &equipment){
    const auto evidence=deathTipsForState(catalog,game,coach,summary,equipment);QJsonArray insights;
    for(const auto &v:evidence){auto card=v.toObject();const auto id=card.value("tip_id").toString();
        if(id==QStringLiteral("enemy_equipment")){
            const auto latest=equipment.value("latest").toObject();const auto champion=catalog.championName(latest.value("champion_id").toInt()),item=catalog.itemName(latest.value("item_id").toInt());
            card["title"]=champion+QStringLiteral("新增了")+item;
            card["text"]=QStringLiteral("本轮背包对照新增了这件装备；这是已观测到的变化，尚未观测其实际触发。");
            card["visual"]=QJsonObject{{"kind","facts"},{"caption",QStringLiteral("最新装备变化 · 背包对照")},{"rows",QJsonArray{QJsonObject{{"label",QStringLiteral("对手")},{"value",champion}},QJsonObject{{"label",QStringLiteral("新增装备")},{"value",item}}}}};
            auto visual=card["visual"].toObject();visual["layout"]="change";visual["champion_id"]=latest.value("champion_id");visual["item_id"]=latest.value("item_id");card["visual"]=visual;
            insights.prepend(card);
        }else if(id==QStringLiteral("enemy_output")||id==QStringLiteral("enemy_durability")){
            auto visual=card.value("visual").toObject();const auto bars=visual.value("bars").toArray();if(bars.size()<2)continue;
            const auto first=bars[0].toObject(),second=bars[1].toObject();const double a=first.value("value").toDouble(),b=second.value("value").toDouble();
            if(!std::isfinite(a)||!std::isfinite(b)||a<=0||b<=0)continue;
            const bool output=id==QStringLiteral("enemy_output");const auto label=output?QStringLiteral("8秒条件输出"):QStringLiteral("条件承伤");
            visual["layout"]=output?QStringLiteral("duel"):QStringLiteral("rings");card["visual"]=visual;
            const double gap=(a/b-1)*100;
            card["title"]=gap<5?QStringLiteral("%1与%2的%3接近").arg(first.value("label").toString(),second.value("label").toString(),label)
                :QStringLiteral("%1 · %2领先 %3%").arg(first.value("label").toString(),label).arg(gap,0,'f',1);
            card["text"]=QStringLiteral("同一条件下，%1为 %2，%3为 %4。%5")
                .arg(first.value("label").toString()).arg(a,0,'f',0).arg(second.value("label").toString()).arg(b,0,'f',0)
                .arg(gap<5?QStringLiteral("差距不足 5%，不据此区分优先级。"):(output?QStringLiteral("该比较用于识别当前输出压力。") :QStringLiteral("该比较用于区分当前承伤结构。")));
            insights.append(card);
        }else if(id==QStringLiteral("purchase_candidate")){
            const auto actions=coach.value("recommendation_quality").toObject().value("candidate_actions").toObject();QList<QJsonObject> candidates;
            for(const auto &value:actions.value("available_now").toArray()){
                const auto row=value.toObject();const auto gain=row.value("current_model_score_gain");
                if(!gain.isDouble()||!std::isfinite(gain.toDouble())||gain.toDouble()<=0||row.value("current_model_windows").toInt()!=3
                    ||row.value("ranking_eligible")!=QJsonValue(true)||row.value("diagnostic_only")==QJsonValue(true)
                    ||!deathPurchaseFitsCatalogueBudget(catalog,game,row))continue;
                candidates.append(row);
            }
            if(candidates.isEmpty())continue;
            std::sort(candidates.begin(),candidates.end(),[](const auto &a,const auto &b){return a.value("current_model_score_gain").toDouble()>b.value("current_model_score_gain").toDouble();});
            const auto best=candidates.first();const double gain=best.value("current_model_score_gain").toDouble()*100;
            card["title"]=QStringLiteral("%1 · 条件评分 +%2%").arg(catalog.itemName(best.value("item_id").toInt())).arg(gain,0,'f',1);
            card["text"]=QStringLiteral("当前金币 %1，配方估价 %2，实际价格请核对商店。条件评分比较 3 / 8 / 20 秒的输出与自身生存。")
                .arg(game.gold,0,'f',0).arg(best.value("remaining_cost").toDouble(),0,'f',0);
            QJsonArray bars;for(int i=0;i<std::min(3,int(candidates.size()));++i)bars.append(QJsonObject{{"label",catalog.itemName(candidates[i].value("item_id").toInt())},{"item_id",candidates[i].value("item_id")},{"cost",candidates[i].value("remaining_cost")},{"value",candidates[i].value("current_model_score_gain").toDouble()*100}});
            card["visual"]=QJsonObject{{"kind","bars"},{"layout","loadout"},{"unit","%"},{"bars",bars},{"caption",QStringLiteral("配方预算内 · 条件评分变化")},{"source","current_quality_gated_candidate_model_scores"}};
            insights.append(card);
        }
    }
    return insights;
}
