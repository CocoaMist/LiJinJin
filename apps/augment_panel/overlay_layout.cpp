#include "overlay_layout.h"
#include <QJsonArray>
#include <algorithm>

QRect shopReferenceArea(const QRect &v){return QRect(v.x()+int(v.width()*.15),v.y()+int(v.height()*.08),int(v.width()*.80),int(v.height()*.77));}
QRect compactDeathTipPlacement(const QRect &v){
    const int width=std::min(310,shopReferenceArea(v).left()-v.left()-20);
    if(width<200||v.height()<540)return {};
    return QRect(v.left()+10,v.top()+std::max(48,int(v.height()*.12))+10,width,90);
}
QRect combatCornerPlacement(const OverlayPlacement &lane,const QRect &coach,int height){
    if(lane.right.width()<172||height<=0)return {};
    const int width=std::min(296,lane.right.width());
    QRect result(lane.right.left(),lane.right.bottom()-height+1,width,height);
    if(!coach.isEmpty()&&result.intersects(coach.adjusted(-8,-8,8,8)))result.moveBottom(coach.top()-9);
    if(!lane.right.contains(result)||!visibleOverlaysAvoidAreas({result},lane.protectedAreas))return {};
    return result;
}
bool visibleOverlaysAvoidAreas(const QList<QRect> &windows,const QList<QRect> &areas){
    for(const auto &window:windows)for(const auto &area:areas)if(window.intersects(area))return false;
    return true;
}

OverlayPlacement safeOverlayPlacement(const QRect &v){
    OverlayPlacement p;
    if(v.width()<320||v.height()<200)return p;
    const int topBarHeight=std::max(48,int(v.height()*.12));
    const int chatTop=v.y()+int(v.height()*.68);
    const auto shop=shopReferenceArea(v);
    p.protectedAreas={QRect(v.x()+int(v.width()*.25),v.y()+int(v.height()*.12),int(v.width()*.50),int(v.height()*.66)),
        QRect(v.x()+int(v.width()*.25),v.y()+int(v.height()*.76),int(v.width()*.50),int(v.height()*.24)),
        QRect(v.x()+int(v.width()*.75),v.y()+int(v.height()*.72),int(v.width()*.25),int(v.height()*.28)),
        QRect(v.x(),v.y(),v.width(),topBarHeight),
        QRect(v.left(),chatTop,int(v.width()*.25),v.bottom()-chatTop+1),shop};
    // Leave the default shop footprint free even while alive, and keep the
    // lower-left chat/minimap envelope and the top strip outside the lane.
    const int left=v.left()+10, top=v.top()+topBarHeight+8;
    const int width=std::min(310,shop.left()-left-8);
    const int height=chatTop-top-8;
    if(height<OverlayMetrics::foldedHeight)return p;
    if(width>=OverlayMetrics::minimumWidth){
        p.right=QRect(left,top,width,height);
    }else{
        // A high-DPI window can have fewer logical pixels while still leaving
        // a readable lane before the shop. Keep the same protected rectangles.
        const int compactWidth=std::min(OverlayMetrics::minimumWidth,shop.left()-v.left()-1);
        if(compactWidth<OverlayMetrics::narrowMinimumWidth)return p;
        const QRect compact(v.left()+1,top,compactWidth,height);
        if(v.contains(compact)&&visibleOverlaysAvoidAreas({compact},p.protectedAreas))p.right=compact;
    }
    if(p.right.isEmpty())return p;
    p.collapsed=height<150;
    return p;
}
bool overlayAvoidsProtectedAreas(const OverlayPlacement &p){
    if(p.right.isEmpty())return false;
    for(const auto &r:p.protectedAreas)if(p.right.intersects(r))return false;
    return true;
}
OverlayPlacement championSelectPlacement(const QRect &client,const QRect &available){
    OverlayPlacement p;
    if(client.width()<640||client.height()<400||available.width()<640||available.height()<400)return p;
    const int gap=8,minWidth=188,preferredWidth=232;
    const int height=std::min(340,available.height()-2*gap);
    const int top=std::clamp(client.top()+client.height()/5,available.top()+gap,available.bottom()-height-gap+1);
    const int rightSpace=available.right()-client.right()-2*gap;
    const int leftSpace=client.left()-available.left()-2*gap;
    if(rightSpace>=minWidth){
        p.right=QRect(client.right()+gap+1,top,std::min(preferredWidth,rightSpace),height);
        p.protectedAreas={client};return p;
    }
    if(leftSpace>=minWidth){
        const int width=std::min(preferredWidth,leftSpace);
        p.right=QRect(client.left()-gap-width,top,width,height);
        p.protectedAreas={client};return p;
    }
    // A short lower-right reading lane. Never place a panel over the bench,
    // central champion/skin choice or the bottom-center confirmation button.
    const QRect visible=client.intersected(available);
    const int width=std::min(preferredWidth,int(client.width()*.22)-2*gap);
    const int fallbackHeight=std::min(228,int(client.height()*.34)-2*gap);
    if(width<minWidth||fallbackHeight<228)return p;
    p.protectedAreas={QRect(client.left(),client.top(),client.width(),int(client.height()*.24)),
        QRect(client.left()+int(client.width()*.22),client.top()+int(client.height()*.24),int(client.width()*.56),int(client.height()*.76)),
        QRect(client.left(),client.top()+int(client.height()*.24),int(client.width()*.22),int(client.height()*.76)),
        QRect(client.left()+int(client.width()*.78),client.top()+int(client.height()*.24),client.width()-int(client.width()*.78),int(client.height()*.42))};
    p.right=QRect(client.right()-width-gap+1,client.bottom()-fallbackHeight-gap+1,width,fallbackHeight);
    if(!visible.contains(p.right)||!overlayAvoidsProtectedAreas(p))p.right={};
    return p;
}
QJsonObject overlayPlacementJson(const OverlayPlacement &p){
    auto rect=[](const QRect &r){return QJsonArray{r.x(),r.y(),r.width(),r.height()};};QJsonArray regions;for(const auto &r:p.protectedAreas)regions.append(rect(r));
    return {{QStringLiteral("right"),rect(p.right)},{QStringLiteral("left_panel_removed"),true},
        {QStringLiteral("protected_regions"),regions},{QStringLiteral("collapsed"),p.collapsed},
        {QStringLiteral("avoids_protected_regions"),overlayAvoidsProtectedAreas(p)},
        {QStringLiteral("anchor"),QStringLiteral("bottom-left")},
        {QStringLiteral("scope"),QStringLiteral("Default shop, central gameplay UI, bottom controls, both lower corners and top strip are reserved. Custom HUD positions are not observed.")}};
}
