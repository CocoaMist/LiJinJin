#pragma once
#include "analysis.h"
#include <QRegularExpression>
#include <QRect>
#include <algorithm>
#include <cmath>

// Own-HUD names observed twice. The helper is an owned producer, not a public
// arbitrary sidecar contract. Complete means the native resolved round count
// matches this match's observed unique names, never merely that a tooltip shut.
class AutomaticSelectedInputs {
public:
    static bool validBinding(const QJsonObject &binding){
        const auto k=[](const char *s){return QString::fromLatin1(s);};
        if(binding.value(k("kind"))!=k("self_hud_calibration"))return false;
        const int w=binding.value(k("client_width")).toInt(),h=binding.value(k("client_height")).toInt();if(w<640||h<400||w>16384||h>16384)return false;
        QList<QRect> regions;for(const auto &name:{k("self_inventory_rect"),k("self_augment_rect")}){
            if(!binding.contains(name))continue;const auto a=binding.value(name).toArray();if(a.size()!=4)return false;
            for(const auto &v:a)if(!v.isDouble()||!std::isfinite(v.toDouble())||v.toDouble()!=std::floor(v.toDouble()))return false;
            const QRect r(a[0].toInt(),a[1].toInt(),a[2].toInt(),a[3].toInt());if(r.width()<8||r.height()<8||!QRect(0,0,w,h).contains(r))return false;regions.append(r);
        }
        return !regions.isEmpty()&&(regions.size()!=2||!regions[0].intersects(regions[1]));
    }
    QJsonObject apply(const Catalog &catalog,GameState &state,const QJsonObject &input,
                      const QJsonObject &binding,int expected,int round,bool open,qint64 now=0){
        const auto key=[](const char *s){return QString::fromLatin1(s);};
        if(!now)now=QDateTime::currentMSecsSinceEpoch();
        const auto context=state.matchContext.value(key("context_id")).toString();
        const bool active=state.source==GameState::Source::Live&&state.fresh()&&state.gameMode==key("KIWI")
            &&modelRulesUsable(catalog,state)&&state.matchContext.value(key("active")).toBool()&&!context.isEmpty();
        const bool clockRewound=lastGameTime_>=0&&state.gameTime+2<lastGameTime_;
        const bool reset=!active||context!=context_||state.championId!=champion_||binding!=binding_||state.observedPatch!=patch_||clockRewound;
        if(reset){known_.clear();observations_.clear();frameOwners_.clear();context_=context;champion_=state.championId;binding_=binding;patch_=state.observedPatch;}
        lastGameTime_=active?state.gameTime:-1;
        if(!active)return {{key("status"),key("waiting_for_game")}};
        auto normalized=[](QString value){return value.normalized(QString::NormalizationForm_KC).remove(QRegularExpression(QStringLiteral("\\s+"))).toCaseFolded();};
        int self=-1,count=0;for(int i=0;i<state.players.size();++i)if(state.players[i].toObject().value(key("self")).toBool()){self=i;++count;}
        if(count!=1||state.players[self].toObject().value(key("champion_id")).toInt()!=state.championId||!validBinding(binding)||!binding.value(key("self_augment_rect")).isArray())return {{key("status"),key("self_hud_calibration_required")}};
        const auto captured=QDateTime::fromString(input.value(key("captured_at_utc")).toString(),Qt::ISODateWithMs).toMSecsSinceEpoch();
        const auto time=input.value(key("game_time"));
        const bool current=expected>0&&expected<=8&&round>=expected&&round<=100
            &&input.value(key("source"))==key("visible_game_tooltip_ocr")&&input.value(key("context_id"))==context
            &&input.value(key("champion_id")).toInt()==state.championId&&input.value(key("capture_binding")).toObject()==binding
            &&input.value(key("selection_round"))==QJsonValue(round)&&captured>0&&captured<=now&&now-captured<=3000
            &&time.isDouble()&&std::isfinite(time.toDouble())&&time.toDouble()<=state.gameTime&&state.gameTime-time.toDouble()<=2;
        if(current&&!open)for(const auto &value:input.value(key("selected_observations")).toArray()){
            const auto row=value.toObject();const auto idValue=row.value(key("augment_id"));const int id=idValue.toInt();
            if(!idValue.isDouble()||idValue.toDouble()!=id||!catalog.augments.contains(id))continue;
            const auto title=normalized(catalog.augments.value(id).value(key("name_zh_cn")).toString());if(title.isEmpty()||row.value(key("title"))!=title)continue;
            int names=0;for(auto it=catalog.augments.cbegin();it!=catalog.augments.cend();++it)if(normalized(it.value().value(key("name_zh_cn")).toString())==title)++names;if(names!=1)continue;
            const auto frames=row.value(key("repeats")).toArray();if(frames.size()!=2)continue;
            qint64 previous=0;QSet<QString> ids;bool valid=true;
            for(const auto &raw:frames){const auto frame=raw.toObject();const auto stamp=QDateTime::fromString(frame.value(key("captured_at_utc")).toString(),Qt::ISODateWithMs).toMSecsSinceEpoch();
                const auto frameId=frame.value(key("frame_id")).toString();const auto hash=frame.value(key("image_sha256")).toString();
                valid&=frame.value(key("augment_id")).toInt()==id&&frame.value(key("title"))==title&&!frameId.isEmpty()&&frameId.size()<=128&&!ids.contains(frameId)
                    &&(!frameOwners_.contains(frameId)||frameOwners_.value(frameId)==id)
                    &&QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(hash).hasMatch()&&stamp>0&&stamp<=captured&&now-stamp<=3000
                    &&(!previous||stamp-previous>=100);previous=stamp;ids.insert(frameId);
            }
            if(valid&&previous==captured&&known_.size()<8&&!known_.contains(id)){
                known_.insert(id);for(const auto &frameId:ids)frameOwners_[frameId]=id;
                // This is when the owned card was observed, not proof of the
                // round in which it was originally acquired.
                observations_[id]=QJsonObject{{key("augment_id"),id},{key("title"),title},
                    {key("source"),key("visible_self_hud_tooltip")},{key("context_id"),context},
                    {key("observed_after_round"),round},{key("acquired_round"),QJsonValue(QJsonValue::Null)},
                    {key("captured_at_utc"),input.value(key("captured_at_utc"))},{key("game_time"),time},
                    {key("capture_binding"),binding},{key("frames"),frames}};
            }
        }
        QList<int> observed=known_.values();std::sort(observed.begin(),observed.end());QJsonArray ids;for(int id:observed)ids.append(id);
        const bool complete=expected>0&&expected<=8&&!open&&known_.size()==expected;
        auto player=state.players[self].toObject();
        // A direct structured list takes precedence. Partial visual readings
        // are useful, but never overwrite a complete higher-priority provider.
        const bool structuredComplete=player.value(key("augments_complete")).toBool()&&player.value(key("augment_source"))!=key("visible_self_hud_tooltip");
        if(!structuredComplete){
            player[key("augment_ids")]=ids;player[key("augments_complete")]=complete;player[key("augment_source")]=key("visible_self_hud_tooltip");state.players[self]=player;state.selected=observed;
        }
        QJsonArray evidence;for(int id:observed)evidence.append(observations_.value(id));
        return {{key("status"),structuredComplete?key("structured_complete"):complete?key("complete"):key("partial")},{key("observed_count"),known_.size()},
            {key("expected_count"),expected>=0?QJsonValue(expected):QJsonValue()},{key("selected_complete"),structuredComplete||complete},
            {key("source"),structuredComplete?key("structured_provider"):key("visible_self_hud_tooltip")},
            {key("context_id"),context},{key("observations"),evidence},{key("visual_list_complete"),complete},
            {key("completeness_basis"),structuredComplete?key("structured_provider"):key("owned_tooltip_names_and_native_resolved_round_count")},
            {key("clock_reset"),clockRewound},{key("measurement_probability_available"),false}};
    }
private:
    QString context_,patch_;int champion_=0;double lastGameTime_=-1;QJsonObject binding_;QSet<int> known_;
    QMap<int,QJsonObject> observations_;QMap<QString,int> frameOwners_;
};
