#pragma once
#include "catalog.h"
#include "hud_assets.h"
#include "hud_symbols.h"
#include "kda_surface.h"
#include "kda_theme.h"
#include "hud_reading_plane.h"
#include <QFontMetrics>
#include <QJsonArray>
#include <QJsonObject>
#include <QWidget>
#include <cmath>

// A projection of the current engine snapshot. This view never estimates a
// probability, changes a purchase decision, or queries a second AI context.
class CoachEngineStrip final : public QWidget {
public:
    CoachEngineStrip(const Catalog &c,HudAssets *a,bool small,QWidget *parent=nullptr)
        :QWidget(parent),catalog(c),assets(a),compact(small){
        setFixedHeight(compact?24:44);setMinimumWidth(0);
        setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);setFocusPolicy(Qt::NoFocus);
    }
    void setSnapshot(const QJsonObject &summary,const QJsonObject &prediction,const QJsonObject &match){
        snapshot={};const auto id=match.value("context_id").toString();
        const bool current=match.value("active").toBool()&&!id.isEmpty()&&summary.value("context_id")==id&&summary.value("current_context").toBool()&&summary.value("fresh").toBool();
        const bool live=current&&summary.value("source")==QStringLiteral("live_read_only");
        const auto value=prediction.value("probability");
        const bool valid=live&&prediction.value("available").toBool()
            &&prediction.value("context_id")==id&&prediction.value("game_time")==summary.value("game_time")
            &&prediction.value("kind")==QStringLiteral("validated_observed_state_association")
            &&prediction.value("patch").isString()&&!prediction.value("patch").toString().isEmpty()
            &&prediction.value("patch")==summary.value("observed_patch")&&summary.value("observed_patch")==summary.value("catalog_patch")
            &&prediction.value("causal_choice_value").isBool()&&!prediction.value("causal_choice_value").toBool()
            &&prediction.value("uses_augment_or_item_outcome_leakage").isBool()&&!prediction.value("uses_augment_or_item_outcome_leakage").toBool()
            &&value.isDouble()&&std::isfinite(value.toDouble())&&value.toDouble()>=0&&value.toDouble()<=1;
        snapshot["context_id"]=id;snapshot["current_context"]=current;snapshot["probability_available"]=valid;
        snapshot["probability"]=valid?value:QJsonValue(QJsonValue::Null);
        snapshot["probability_display"]=valid?QString::number(qRound(value.toDouble()*100))+QStringLiteral("%"):QStringLiteral("—");
        snapshot["probability_status"]=valid?QStringLiteral("已校准"):QStringLiteral("未校准");
        snapshot["probability_reason"]=valid?QString():prediction.value("reason").toString(QStringLiteral("validated_current_match_prediction_required"));
        snapshot["scope"]=QStringLiteral("current_loadout_roles_and_observed_state_association");
        const auto tactics=current?summary.value("tactical_context").toObject():QJsonObject{};
        snapshot["tactical_summary"]=tactics.value("summary");
        QJsonArray teams;
        for(const auto &side:{QStringLiteral("ALLY"),QStringLiteral("ENEMY")}){
            QJsonObject team{{"team",side}};
            for(const auto &v:tactics.value("teams").toArray()){const auto t=v.toObject();if(t.value("ally").toBool()==(side==QStringLiteral("ALLY")))team["observed_alive"]=t.value("observed_alive");}
            for(const auto &axis:{QStringLiteral("damage_share"),QStringLiteral("durability_share")}){
                double maximum=-1;QJsonObject selected;
                if(tactics.value("available").toBool())for(const auto &v:summary.value("players").toArray()){
                    const auto p=v.toObject(),role=p.value("tactical_role").toObject();const auto share=role.value(axis);
                    if(p.value("team")!=side||!role.value("availability_identity_unambiguous").toBool()
                        ||!share.isDouble()||!std::isfinite(share.toDouble())||share.toDouble()<0||share.toDouble()>1)continue;
                    if(share.toDouble()>maximum){maximum=share.toDouble();selected={{"champion_id",p.value("champion_id")},{"row_index",p.value("row_index")},{"share",share},{"is_dead",p.value("is_dead")},{"partial",p.value("model_partial")}};}
                }
                team[axis]=selected;
            }
            const bool pregame=match.value("active").toBool()&&(match.value("stage")==QStringLiteral("champ_select")||match.value("stage")==QStringLiteral("loading"));
            const auto composition=pregame?match.value("composition").toObject().value("teams").toObject().value(side).toObject():QJsonObject{};
            const auto needs=composition.value("needs").toArray();int visible=0;for(const auto &v:summary.value("players").toArray())visible+=v.toObject().value("team")==side&&v.toObject().value("champion_id").toInt()>0;
            if(!match.value("active").toBool()){
                team["fallback"]=QStringLiteral("未开局");team["fallback_reason"]=QStringLiteral("当前没有进行中的对局。");
            }else if(pregame){
                const bool known=visible>0||composition.value("visible_champions").toInt()>0;
                team["fallback"]=!needs.isEmpty()?needs.first().toString():known?QStringLiteral("已选定"):QStringLiteral("未公开");
                team["fallback_reason"]=known?QStringLiteral("选人阶段仅显示已公开阵容；实装职责需当前等级和装备。"):QStringLiteral("客户端尚未公开这一方的英雄，不推测隐藏阵容。");
            }else if(!summary.value("fresh").toBool()){
                team["fallback"]=QStringLiteral("已断开");team["fallback_reason"]=QStringLiteral("未收到新鲜对局数据，已清除旧局职责。");
            }else if(!current){
                team["fallback"]=QStringLiteral("局势失效");team["fallback_reason"]=QStringLiteral("当前摘要未通过本局身份与时效校验，不能继续展示旧职责。");
            }else if(visible==0){
                team["fallback"]=QStringLiteral("未读取");team["fallback_reason"]=QStringLiteral("当前摘要没有这一方的英雄记录。");
            }else{
                team["fallback"]=QStringLiteral("未建模");team["fallback_reason"]=QStringLiteral("英雄已读取，但规则、输入或身份校验不足，暂不指定输出/承伤核心。");
            }
            teams.append(team);
        }
        snapshot["teams"]=teams;
        QStringList lines{QStringLiteral("胜率估计：")+snapshot.value("probability_display").toString()};
        if(valid)lines.append(QStringLiteral("同版本当前局势关联模型；不是选牌收益或结果保证。"));
        else lines.append(QStringLiteral("当前缺少通过验证的同版本胜率模型；不以历史胜率或适配分代替。"));
        for(const auto &v:teams){const auto team=v.toObject();const auto side=team.value("team")=="ALLY"?QStringLiteral("我方"):QStringLiteral("对手");
            if(team.value("damage_share").toObject().isEmpty()&&team.value("durability_share").toObject().isEmpty())lines.append(side+QStringLiteral("：")+team.value("fallback_reason").toString());
            for(const auto &axis:{QStringLiteral("damage_share"),QStringLiteral("durability_share")}){const auto key=team.value(axis).toObject();if(key.isEmpty())continue;
                lines.append(side+(axis=="damage_share"?QStringLiteral("输出核心≈ "):QStringLiteral("承伤核心≈ "))+catalog.championName(key.value("champion_id").toInt())+QStringLiteral(" %1%").arg(qRound(key.value("share").toDouble()*100))+(key.value("is_dead").toBool()?QStringLiteral(" · 阵亡"):QString())+(key.value("partial").toBool()?QStringLiteral(" · 部分规则"):QString()));}
        }
        if(!snapshot.value("tactical_summary").toString().isEmpty())lines.append(snapshot.value("tactical_summary").toString());
        lines.append(QStringLiteral("职责基于当前装备的参考输出/有效生命；站位、命中和技能冷却未知。"));
        setAccessibleName(lines.join(QStringLiteral("；")));setToolTip(lines.join('\n'));update();
    }
    QJsonObject report()const{
        auto out=snapshot;out["visible"]=isVisible();out["compact"]=compact;out["runtime_network"]=false;out["historical_win_rate_used"]=false;
        bool fits=width()>=170&&height()==(compact?24:44);QJsonArray roleGeometry;
        const auto teams=snapshot.value("teams").toArray();
        for(int team=0;team<teams.size()&&team<2;++team){const auto row=teams[team].toObject();
            for(int stat=0;stat<(compact?1:2);++stat){if(row.value(stat?"durability_share":"damage_share").toObject().isEmpty())continue;
                const auto icon=roleRect(team,stat),art=portraitRect(team,stat);fits&=rect().contains(icon)&&rect().contains(art)&&!icon.intersects(art);
                roleGeometry.append(QJsonObject{{"team",team?"ENEMY":"ALLY"},{"role",stat?"durability_share":"damage_share"},{"role_icon_rect",QJsonArray{icon.x(),icon.y(),icon.width(),icon.height()}},{"portrait_rect",QJsonArray{art.x(),art.y(),art.width(),art.height()}}});
            }
        }
        out["geometry"]=QJsonArray{x(),y(),width(),height()};out["content_fits"]=fits;
        out["design"]="platinum_ice_role_portraits";out["theme"]=Kda::HudStyle::identity();out["skin_strength"]=Kda::HudStyle::strength();out["material_placement"]="portrait_frames_only";out["text_surface"]=HudReadingPlane::surfaceIdentity();out["inset_frames"]=false;
        out["compact_team_labels"]=compact;out["minimum_role_icon_px"]=12;out["key_portrait_px"]=compact?22:width()<190?16:20;
        out["displayed_probability_label"]=snapshot.value("probability_available").toBool()?QStringLiteral("胜率≈"):QStringLiteral("未校准");
        out["probability_font_px"]=compact?16:20;out["probability_semantics"]="validated_current_match_or_dash";out["role_slots"]=roleGeometry;
        return out;
    }
protected:
    void paintEvent(QPaintEvent *)override{
        Kda::OverlayCanvas canvas(this);auto &p=canvas.paint();
        HudReadingPlane::paint(p,QRectF(rect()).adjusted(0,0,-.5,-.5));
        const int probabilityWidth=probabilityAreaWidth(),cell=teamCellWidth();
        const auto labelFont=Kda::hudFont(Kda::FontRole::Body,12),valueFont=Kda::hudFont(Kda::FontRole::Numeric,compact?16:20);
        QLinearGradient rule(QPointF(probabilityWidth-3,5),QPointF(probabilityWidth-3,height()-5));
        rule.setColorAt(0,HudReadingPlane::ink("silver",45));rule.setColorAt(.45,HudReadingPlane::ink("border",120));rule.setColorAt(1,HudReadingPlane::ink("border",20));
        p.setPen(QPen(rule,.8));p.drawLine(probabilityWidth-3,5,probabilityWidth-3,height()-5);
        p.setFont(labelFont);p.setPen(Kda::color("muted"));
        if(snapshot.value("probability_available").toBool()){
            if(!compact||width()>=190)p.drawText(QRect(0,0,compact?34:52,compact?24:18),Qt::AlignVCenter|Qt::AlignLeft,QStringLiteral("胜率≈"));
            p.setFont(valueFont);p.setPen(Kda::color("silver"));
            p.drawText(compact?(width()<190?QRect(0,0,50,24):QRect(35,0,40,24)):QRect(0,18,52,26),Qt::AlignVCenter|Qt::AlignLeft,snapshot.value("probability_display").toString());
        }else p.drawText(QRect(0,0,probabilityWidth-5,height()),Qt::AlignVCenter|Qt::AlignLeft,QStringLiteral("未校准"));
        const auto teams=snapshot.value("teams").toArray();
        for(int i=0;i<teams.size()&&i<2;++i){const auto team=teams[i].toObject();const int x=probabilityWidth+i*cell;const auto ink=Kda::color("silver");
            p.setFont(labelFont);p.setPen(ink);
            if(compact)p.drawText(QRect(x,0,13,24),Qt::AlignLeft|Qt::AlignVCenter,i?QStringLiteral("敌"):QStringLiteral("我"));
            else p.drawText(QRect(x+4,0,cell-4,20),Qt::AlignLeft|Qt::AlignVCenter,(i?QStringLiteral("对手 "):QStringLiteral("我方 "))+(team.value("observed_alive").isDouble()?QString::number(team.value("observed_alive").toInt()):QStringLiteral("≈")));
            const int slotCount=compact?1:2;bool any=false;
            for(int stat=0;stat<slotCount;++stat){const auto key=team.value(stat?"durability_share":"damage_share").toObject();if(key.isEmpty())continue;any=true;
                HudSymbols::draw(p,stat?HudSymbols::Symbol::Shield:HudSymbols::Symbol::Sword,roleRect(i,stat),ink,1.6);
                const QRect box=portraitRect(i,stat);const auto art=assets?assets->champion(key.value("champion_id").toInt()):QPixmap{};
                p.setPen(Qt::NoPen);p.setBrush(Kda::color("raised"));p.drawRoundedRect(QRectF(box),3,3);
                if(!art.isNull())p.drawPixmap(box,Kda::roundedPixmap(art,box.size(),3,p.device()->devicePixelRatioF()));
                else HudSymbols::draw(p,HudSymbols::Symbol::Team,box.adjusted(4,4,-4,-4),Kda::color("muted"),1.4);
                HudReadingPlane::portraitFrame(p,QRectF(box),3);
                if(key.value("is_dead").toBool()){HudReadingPlane::deadPortrait(p,box,3,160);HudSymbols::draw(p,HudSymbols::Symbol::Close,box.adjusted(4,4,-4,-4),Kda::color("danger"),1.8);}
            }
            if(!any){p.setFont(labelFont);p.setPen(Kda::color("muted"));const auto text=team.value("fallback").toString(QStringLiteral("未建模"));const QRect bounds(x+(compact?15:4),compact?0:20,std::max(0,cell-(compact?16:5)),24);p.drawText(bounds,Qt::AlignLeft|Qt::AlignVCenter,QFontMetrics(labelFont).elidedText(text,Qt::ElideRight,bounds.width()));}
        }
    }
private:
    int probabilityAreaWidth()const{return compact&&width()>=190?78:54;}
    int teamCellWidth()const{return std::max(0,(width()-probabilityAreaWidth())/2);}
    QRect roleRect(int team,int stat)const{
        const int x=probabilityAreaWidth()+team*teamCellWidth();
        return compact?QRect(x+14,6,12,12):QRect(x+(width()<190?0:1)+stat*(teamCellWidth()/2),26,12,12);
    }
    QRect portraitRect(int team,int stat)const{
        const auto icon=roleRect(team,stat);return compact?QRect(icon.x()+15,1,22,22):width()<190?QRect(icon.x()+13,24,16,16):QRect(icon.x()+13,22,20,20);
    }
    const Catalog &catalog;HudAssets *assets;bool compact;QJsonObject snapshot;
};
