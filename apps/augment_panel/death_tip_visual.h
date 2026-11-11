#pragma once
#include "kda_theme.h"
#include <QFontMetrics>
#include <QJsonArray>
#include <QPainter>
#include <QPainterPath>
#include <QStringList>
#include <algorithm>
#include <cmath>

namespace DeathTipVisual {
inline QJsonObject previewCard(int variant){
    variant=(variant%4+4)%4;QJsonObject visual;QString title,text;
    if(variant==0){title=QStringLiteral("巫妖之祸 · 条件评分 +16.0%");text=QStringLiteral("三件当前购买候选中，巫妖之祸的已建模收益最高；比第二项高 5.5 个百分点。");visual={{"kind","bars"},{"unit","%"},{"bars",QJsonArray{QJsonObject{{"label",QStringLiteral("巫妖之祸")},{"value",16.0}},QJsonObject{{"label",QStringLiteral("影焰")},{"value",10.5}},QJsonObject{{"label",QStringLiteral("中娅沙漏")},{"value",8.6}}}},{"caption",QStringLiteral("演示数据 · 当前购买候选 / 条件评分变化")}};}
    else if(variant==1){title=QStringLiteral("寒冰 · 条件输出领先 14.8%");text=QStringLiteral("同一 8 秒窗口，寒冰的条件输出为 3260，拉克丝为 2840。当前模型的主要输出压力来自这两位。");visual={{"kind","bars"},{"bars",QJsonArray{QJsonObject{{"label",QStringLiteral("寒冰")},{"value",3260}},QJsonObject{{"label",QStringLiteral("拉克丝")},{"value",2840}},QJsonObject{{"label",QStringLiteral("盖伦")},{"value",1930}}}},{"caption",QStringLiteral("演示数据 · 8秒条件输出 / 非实战伤害")}};}
    else if(variant==2){title=QStringLiteral("盖伦 · 条件承伤约为寒冰的 2.3 倍");text=QStringLiteral("盖伦的有效生命模型为 6200，寒冰为 2700。同一套输出打在两者身上，预计消耗差距明显。");visual={{"kind","bars"},{"bars",QJsonArray{QJsonObject{{"label",QStringLiteral("盖伦")},{"value",6200}},QJsonObject{{"label",QStringLiteral("拉克丝")},{"value",3100}},QJsonObject{{"label",QStringLiteral("寒冰")},{"value",2700}}}},{"caption",QStringLiteral("演示数据 · 有效生命条件模型")}};}
    else{title=QStringLiteral("寒冰新增了凡性的提醒");text=QStringLiteral("这条装备变化来自本轮背包对照。新增装备已经纳入下一轮条件模型，尚未观测其实际触发。");visual={{"kind","facts"},{"rows",QJsonArray{QJsonObject{{"label",QStringLiteral("对手")},{"value",QStringLiteral("寒冰射手")}},QJsonObject{{"label",QStringLiteral("新增装备")},{"value",QStringLiteral("凡性的提醒")}}}},{"caption",QStringLiteral("演示数据 · 最新装备变化")}};}
    if(variant<3){
        const QList<int> ids=variant==0?QList<int>{3100,4645,3157}:variant==1?QList<int>{22,99,86}:QList<int>{86,99,22};
        auto bars=visual.value("bars").toArray();for(int i=0;i<bars.size();++i){auto row=bars[i].toObject();row[variant==0?"item_id":"champion_id"]=ids[i];if(variant==0)row["cost"]=QList<int>{2900,3000,3250}[i];bars[i]=row;}
        visual["bars"]=bars;visual["layout"]=variant==0?QStringLiteral("loadout"):variant==1?QStringLiteral("duel"):QStringLiteral("rings");
    }else{visual["layout"]="change";visual["champion_id"]=22;visual["item_id"]=3033;}
    visual["source"]=QStringLiteral("synthetic_ui_fixture");
    return {{"tip_id",QStringLiteral("synthetic-visual-%1").arg(variant)},{"title",title},{"text",text},{"visual",visual},{"synthetic",true},{"conditional",true},{"source","synthetic_ui_fixture"}};
}
inline QJsonObject validated(const QJsonObject &content){
    const auto visual=content.value("visual").toObject();const auto kind=visual.value("kind").toString();
    if(kind==QStringLiteral("budget")){
        const auto budget=visual.value("budget"),cost=visual.value("cost");
        if(budget.isDouble()&&cost.isDouble()&&std::isfinite(budget.toDouble())&&std::isfinite(cost.toDouble())&&
           budget.toDouble()>0&&cost.toDouble()>0&&cost.toDouble()<=budget.toDouble())return visual;
    }else if(kind==QStringLiteral("bars")){
        const auto bars=visual.value("bars").toArray();if(bars.isEmpty()||bars.size()>3)return {};
        qreal maximum=0;for(const auto &v:bars){const auto row=v.toObject();const auto number=row.value("value");
            if(row.value("label").toString().trimmed().isEmpty()||!number.isDouble()||!std::isfinite(number.toDouble())||number.toDouble()<0)return {};
            maximum=std::max(maximum,number.toDouble());}
        if(maximum>0)return visual;
    }else if(kind==QStringLiteral("facts")){
        const auto rows=visual.value("rows").toArray();if(rows.isEmpty()||rows.size()>3)return {};
        for(const auto &v:rows){const auto row=v.toObject();if(row.value("label").toString().trimmed().isEmpty()||row.value("value").toString().trimmed().isEmpty())return {};}
        return visual;
    }else if(kind==QStringLiteral("sequence")||kind==QStringLiteral("steps")){
        const auto nodes=visual.value("nodes").toArray();if(nodes.isEmpty()||nodes.size()>5)return {};
        for(const auto &node:nodes)if(!node.isString()||node.toString().isEmpty()||node.toString().size()>4)return {};
        return visual;
    }
    return {};
}
inline bool expanded(const QJsonObject &content){return content.value("expanded").toBool()||!validated(content).isEmpty();}
inline QRect chartRect(const QSize &size){
    const bool large=size.width()>=760;const int left=large?30:20,top=large?184:108;
    return {left,top,qRound(size.width()*.61)-left-20,size.height()-top-(large?50:36)};
}
inline QString describe(const QJsonObject &visual){
    QStringList labels{visual.value("caption").toString()};const auto kind=visual.value("kind").toString();
    if(kind==QStringLiteral("budget"))labels<<QStringLiteral("金币 %1，候选花费 %2").arg(visual.value("budget").toDouble(),0,'f',0).arg(visual.value("cost").toDouble(),0,'f',0);
    for(const auto &v:visual.value("bars").toArray()){const auto row=v.toObject();labels<<row.value("label").toString()+QStringLiteral(" %1").arg(row.value("value").toDouble(),0,'f',0);}
    for(const auto &node:visual.value("nodes").toArray())labels<<node.toString();
    for(const auto &v:visual.value("rows").toArray())labels<<v.toObject().value("label").toString()+QStringLiteral(" ")+v.toObject().value("value").toString();
    return labels.join(QStringLiteral(" · "));
}
inline void paint(QPainter &p,const QRect &r,const QJsonObject &visual){
    if(visual.isEmpty())return;p.save();p.setClipRect(r,Qt::IntersectClip);
    const auto draw=[&](const QRectF &rect,const QString &text,QColor color,int pixels=11){
        const auto font=Kda::font(Kda::FontRole::Body,pixels,QFont::Medium);p.setFont(font);p.setPen(color);
        p.drawText(rect,Qt::AlignLeft|Qt::AlignVCenter,QFontMetrics(font).elidedText(text,Qt::ElideRight,qRound(rect.width())));
    };
    const bool large=r.width()>=360;const int captionHeight=large?20:14;
    draw(QRectF(r.x(),r.y(),r.width(),captionHeight),visual.value("caption").toString(),Kda::color("muted"),large?12:10);
    const auto kind=visual.value("kind").toString();
    if(kind==QStringLiteral("budget")){
        const qreal budget=visual.value("budget").toDouble(),cost=visual.value("cost").toDouble();
        draw(QRectF(r.x(),r.y()+captionHeight+10,r.width(),large?30:20),QStringLiteral("金币 %1").arg(budget,0,'f',0),Kda::color("silver"),large?22:14);
        const QRectF track(r.x(),r.y()+r.height()*.55,r.width(),large?10:7);p.setPen(Qt::NoPen);p.setBrush(Kda::color("border"));p.drawRoundedRect(track,2,2);
        const QRectF spent(track.x(),track.y(),track.width()*cost/budget,track.height());QLinearGradient fill(spent.topLeft(),spent.topRight());fill.setColorAt(0,Kda::color("silver"));fill.setColorAt(1,Kda::color("ice"));p.setBrush(fill);p.drawRoundedRect(spent,2,2);
        draw(QRectF(r.x(),r.bottom()-(large?24:19),r.width(),large?24:19),QStringLiteral("花费 %1 · 余 %2").arg(cost,0,'f',0).arg(budget-cost,0,'f',0),Kda::color("text"),large?15:11);
    }else if(kind==QStringLiteral("facts")){
        const auto rows=visual.value("rows").toArray();const qreal lineHeight=(r.height()-captionHeight-6.)/rows.size();
        for(int i=0;i<rows.size();++i){const auto row=rows[i].toObject();const qreal y=r.top()+captionHeight+4+i*lineHeight;
            draw(QRectF(r.x(),y,large?86:64,lineHeight),row.value("label").toString(),Kda::color("muted"),large?12:10);
            draw(QRectF(r.x()+(large?92:70),y,r.width()-(large?92:70),lineHeight),row.value("value").toString(),Kda::color("silver"),large?20:13);
        }
    }else if(kind==QStringLiteral("bars")){
        const auto bars=visual.value("bars").toArray();qreal maximum=0;for(const auto &v:bars)maximum=std::max(maximum,v.toObject().value("value").toDouble());
        const qreal rowHeight=(r.height()-captionHeight-8.)/bars.size(),labelWidth=large?108:65,valueWidth=large?72:48,gap=large?10:8;
        for(int i=0;i<bars.size();++i){const auto bar=bars[i].toObject();const qreal y=r.y()+captionHeight+6+i*rowHeight,lineHeight=large?24:17;
            draw(QRectF(r.x(),y,labelWidth,lineHeight),bar.value("label").toString(),Kda::color("silver"),large?14:11);
            const QRectF track(r.x()+labelWidth+gap,y+lineHeight/2-2,r.width()-labelWidth-valueWidth-2*gap,large?7:4);p.setPen(Qt::NoPen);p.setBrush(Kda::color("raised"));p.drawRoundedRect(track,2,2);
            p.setBrush(Kda::color(i==0?"ice":"silver"));p.drawRoundedRect(QRectF(track.x(),track.y(),track.width()*bar.value("value").toDouble()/maximum,track.height()),2,2);
            const auto unit=visual.value("unit").toString();draw(QRectF(r.right()-valueWidth,y,valueWidth,lineHeight),QString::number(bar.value("value").toDouble(),'f',unit==QStringLiteral("%")?1:0)+unit,Kda::color("text"),large?15:11);
        }
    }else{
        const auto nodes=visual.value("nodes").toArray();const qreal gap=large?14:10,width=std::min(qreal(large?60:42),(r.width()-gap*(nodes.size()-1))/nodes.size());
        const qreal left=r.left()+(r.width()-width*nodes.size()-gap*(nodes.size()-1))/2.;
        for(int i=0;i<nodes.size();++i){const QRectF node(left+i*(width+gap),r.y()+captionHeight+22,width,large?44:28);
            p.setPen(QPen(Kda::color(i==0?"ice":"border"),1));p.setBrush(Kda::color("raised"));p.drawRoundedRect(node,3,3);
            const auto face=Kda::font(Kda::FontRole::Label,large?20:12,QFont::DemiBold);p.setFont(face);p.setPen(Kda::color("text"));p.drawText(node,Qt::AlignCenter,nodes[i].toString());
            if(i+1<nodes.size()){const qreal x=node.right()+3,y=node.center().y();p.setPen(QPen(Kda::color("muted"),1));p.drawLine(QPointF(x,y),QPointF(x+4,y));p.drawLine(QPointF(x+2,y-2),QPointF(x+4,y));p.drawLine(QPointF(x+2,y+2),QPointF(x+4,y));}
        }
    }
    p.restore();
}
}
