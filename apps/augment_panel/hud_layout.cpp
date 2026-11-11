#include "hud_layout.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>
#include <algorithm>
#include <cmath>

QStringList HudLayout::keys(){return {"coach","combat","card1","card2","card3"};}
QString HudLayout::title(const QString &key){
    if(key=="coach")return QStringLiteral("出装与阵容");
    if(key=="combat")return QStringLiteral("本局战况");
    return QStringLiteral("选牌提示 %1").arg(key.right(1));
}
QSize HudLayout::minimumSize(const QString &key){return key=="coach"?QSize(200,287):key=="combat"?QSize(190,250):QSize(140,48);}
HudLayout HudLayout::load(QSettings &settings){
    HudLayout result;
    const auto data=QJsonDocument::fromJson(settings.value("hud/layoutV1").toByteArray()).object();
    if(data.value("version").toInt()!=1)return result;
    const auto entries=data.value("elements").toObject();
    for(const auto &key:keys()){
        const auto row=entries.value(key).toArray();if(row.size()!=4)continue;
        bool valid=true;for(const auto &v:row)valid&=v.isDouble()&&std::isfinite(v.toDouble());
        const QRectF r(row[0].toDouble(),row[1].toDouble(),row[2].toDouble(),row[3].toDouble());
        if(valid&&r.x()>=0&&r.y()>=0&&r.width()>0&&r.height()>0&&r.right()<=1.000001&&r.bottom()<=1.000001)result.overrides[key]=r;
    }
    return result;
}
bool HudLayout::save(QSettings &settings)const{
    settings.setValue("hud/layoutV1",QJsonDocument(json()).toJson(QJsonDocument::Compact));settings.sync();
    return settings.status()==QSettings::NoError;
}
QJsonObject HudLayout::json()const{
    QJsonObject entries;for(auto it=overrides.cbegin();it!=overrides.cend();++it){const auto r=it.value();entries[it.key()]=QJsonArray{r.x(),r.y(),r.width(),r.height()};}
    return {{"version",1},{"elements",entries}};
}
void HudLayout::set(const QString &key,const QRect &bounds,const QRect &v){
    if(!keys().contains(key)||v.isEmpty())return;
    const auto minimum=minimumSize(key).boundedTo(v.size());
    const QSize size(std::clamp(bounds.width(),minimum.width(),v.width()),std::clamp(bounds.height(),minimum.height(),v.height()));
    const QPoint pos(std::clamp(bounds.x(),v.x(),v.right()-size.width()+1),std::clamp(bounds.y(),v.y(),v.bottom()-size.height()+1));
    overrides[key]=QRectF(double(pos.x()-v.x())/v.width(),double(pos.y()-v.y())/v.height(),double(size.width())/v.width(),double(size.height())/v.height());
}
QRect HudLayout::resolve(const QString &key,const QRect &v,const QRect &fallback)const{
    if(!overrides.contains(key)||v.isEmpty())return fallback;
    const auto r=overrides.value(key);const auto minimum=minimumSize(key).boundedTo(v.size());
    const QSize size(std::clamp(qRound(r.width()*v.width()),minimum.width(),v.width()),std::clamp(qRound(r.height()*v.height()),minimum.height(),v.height()));
    return {QPoint(std::clamp(v.x()+qRound(r.x()*v.width()),v.left(),v.right()-size.width()+1),std::clamp(v.y()+qRound(r.y()*v.height()),v.top(),v.bottom()-size.height()+1)),size};
}
