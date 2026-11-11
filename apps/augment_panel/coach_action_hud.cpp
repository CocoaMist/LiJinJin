#include "coach_action_hud.h"
#include "hud_assets.h"
#include "kda_theme.h"
#include "hud_symbols.h"
#include "kda_surface.h"
#include "hud_reading_plane.h"
#include <QFontMetrics>
#include <QJsonArray>
#include <QPainter>
#include <algorithm>
#include <cmath>

namespace {
struct TextLine {QRect rect;QFont font;QString text;};
struct ActionLayout {TextLine caption,title,status;};
TextLine line(const QRect &rect,int pixels,bool bold,const QString &text){
    const auto role=pixels>=16?Kda::FontRole::Title:Kda::FontRole::Body;
    const auto font=Kda::hudFont(role,pixels,bold?QFont::DemiBold:QFont::Medium);
    return {rect,font,QFontMetrics(font).elidedText(text,Qt::ElideRight,rect.width())};
}
QRect actionIconRect(bool purchase,int width){const int side=width<190?40:width<230?48:56;return purchase?QRect(2,(68-side)/2,side,side):QRect(0,4,36,36);}
ActionLayout actionLayout(const QSize &size,const QString &caption,const QString &title,const QString &status,bool icon,bool purchase){
    Q_UNUSED(caption);
    if(purchase){
        const int x=actionIconRect(true,size.width()).right()+12,width=std::max(0,size.width()-x-2);int pixels=20;
        while(pixels>14&&QFontMetrics(Kda::hudFont(Kda::FontRole::Title,pixels,QFont::DemiBold)).horizontalAdvance(title)>width)--pixels;
        return {{},line(QRect(x,3,width,30),pixels,true,title),line(QRect(x,38,width,23),14,false,status)};
    }
    const int x=icon?48:24,width=std::max(0,size.width()-x);
    return {{},line(QRect(x,0,width,23),16,true,title),line(QRect(x,24,width,20),14,false,status)};
}
HudSymbols::Symbol category(const QString &caption){return caption.contains(QStringLiteral("历史"))?HudSymbols::Symbol::History:caption.contains(QStringLiteral("阵容"))?HudSymbols::Symbol::Team:caption==QStringLiteral("下一件")?HudSymbols::Symbol::Coins:HudSymbols::Symbol::Link;}
QJsonArray rectJson(const QRect &r){return {r.x(),r.y(),r.width(),r.height()};}
bool fits(const TextLine &line,const QRect &bounds){const QFontMetrics fm(line.font);return bounds.contains(line.rect)&&line.rect.height()>=fm.height()&&fm.horizontalAdvance(line.text)<=line.rect.width();}
void drawActionMotion(QPainter &p,const QRectF &icon,qreal radius,qreal progress){
    const qreal pulse=std::sin(3.141592653589793*std::clamp(progress,qreal(0),qreal(1)));
    p.save();p.setBrush(Qt::NoBrush);
    p.setPen(QPen(Kda::HudStyle::accent(qRound(115*pulse)),1.3));
    p.drawRoundedRect(icon.adjusted(.75,.75,-.75,-.75),radius,radius);
    p.restore();
}
}
CoachActionHud::CoachActionHud(QWidget *parent):QWidget(parent){
    setObjectName(QStringLiteral("automaticHint"));setMinimumWidth(0);setFixedHeight(44);
    setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);setAttribute(Qt::WA_TransparentForMouseEvents);
    motion=new Kda::Transition(this,220);
}
void CoachActionHud::setItem(HudAssets *value,int id){if(assets==value&&itemId==id&&championId==0)return;const bool changed=itemId!=id||championId!=0;assets=value;itemId=id;championId=0;setFixedHeight(itemId>0?68:44);if(changed)motion->trigger();update();}
void CoachActionHud::setChampion(HudAssets *value,int id){if(assets==value&&championId==id&&itemId==0)return;const bool changed=championId!=id||itemId!=0;assets=value;championId=id;itemId=0;setFixedHeight(44);if(changed)motion->trigger();update();}
void CoachActionHud::setAction(const QString &caption,const QString &title,const QString &status,bool buyable){
    if(caption==captionText&&title==titleText&&status==statusText&&buyable==purchaseReady)return;
    const bool changed=purchaseReady!=buyable;
    captionText=caption;titleText=title;statusText=status;purchaseReady=buyable;setAccessibleName(text());setToolTip(QString());
    if(changed)motion->trigger();update();
}
QString CoachActionHud::text()const{return captionText+QStringLiteral(" · ")+titleText+QStringLiteral("\n")+statusText;}
bool CoachActionHud::purchaseTextFits(const QString &title,const QString &status) const {
    if(itemId<=0)return false;
    const auto layout=actionLayout(size(),{},title,status,true,true);
    return layout.title.text==title&&layout.status.text==status
        &&fits(layout.title,rect())&&fits(layout.status,rect());
}
void CoachActionHud::paintEvent(QPaintEvent *){
    Kda::OverlayCanvas canvas(this);auto &painter=canvas.paint();
    HudReadingPlane::paint(painter,QRectF(rect()).adjusted(0,0,-.5,-.5));
    // The action owns the strongest typography; status and decoration remain
    // subordinate without adding a caption or another framed card.
    // Gold ticks and continuously animated counters must not retrigger it.
    const bool hasIcon=itemId>0||championId>0;
    const qreal iconRadius=Kda::radius("artwork");
    const QColor accent=Kda::HudStyle::color(purchaseReady?"ready":"silver");
    if(hasIcon){
        const auto icon=actionIconRect(itemId>0,width());const auto pixmap=assets?(itemId>0?assets->item(itemId):assets->champion(championId)):QPixmap{};
        // A shallow shadow separates the artwork from the shell's atmosphere.
        painter.setPen(Qt::NoPen);painter.setBrush(HudReadingPlane::ink("bg",170));
        painter.fillRect(QRectF(icon).translated(1.5,1.5),HudReadingPlane::ink("bg",170));
        painter.save();painter.setPen(Qt::NoPen);painter.setBrush(Kda::HudStyle::color("raised"));
        painter.drawRoundedRect(QRectF(icon),iconRadius,iconRadius);
        if(!pixmap.isNull())painter.drawPixmap(icon,Kda::roundedPixmap(pixmap,icon.size(),iconRadius,painter.device()->devicePixelRatioF()));
        else{const qreal placeholderRadius=Kda::radius("small");painter.setPen(Kda::HudStyle::color("muted"));painter.drawRoundedRect(icon.adjusted(9,9,-9,-9),placeholderRadius,placeholderRadius);}
        painter.restore();HudReadingPlane::portraitFrame(painter,QRectF(icon),iconRadius,purchaseReady);
        if(motion->running())drawActionMotion(painter,QRectF(icon),iconRadius,motion->progress());
        painter.setPen(QPen(Kda::HudStyle::color(purchaseReady?"ready":"border"),.9));painter.setBrush(Kda::HudStyle::color("bg"));
        const QRectF badge(icon.right()-10,icon.bottom()-12,16,16);painter.drawEllipse(badge);
        HudSymbols::draw(painter,purchaseReady?HudSymbols::Symbol::Check:category(captionText),badge.adjusted(3,3,-3,-3),accent,purchaseReady?1.8:1.5);
    }else{
        HudSymbols::draw(painter,category(captionText),QRectF(1,4,18,18),accent,1.6);
    }
    const auto p=actionLayout(size(),captionText,titleText,statusText,hasIcon,itemId>0);
    for(const auto &entry:{p.title,p.status}){
        painter.setFont(entry.font);painter.setPen(Kda::HudStyle::color(entry.rect==p.title.rect?"text":entry.rect==p.status.rect?(purchaseReady?"ready":"silver"):"muted"));
        painter.drawText(entry.rect,Qt::AlignLeft|Qt::AlignVCenter,entry.text);
    }
}
QJsonObject CoachActionHud::report()const{
    const auto p=actionLayout(size(),captionText,titleText,statusText,itemId>0||championId>0,itemId>0);
    auto motionReport=motion->report();motionReport["trigger_policy"]="recommendation_item_champion_or_purchase_ready_only";
    motionReport["animates_text_or_numbers"]=false;motionReport["animates_geometry"]=false;motionReport["loops"]=false;
    motionReport["effect"]="item_outline_pulse";motionReport["paint_only"]=true;motionReport["effect_clip_rect"]=rectJson(actionIconRect(itemId>0,width()));
    motionReport["text_region_stable"]=true;motionReport["uses_animated_image"]=false;
    return {{"design","broadcast_information_strip"},{"visual_style","the_baddest_typographic_reading_plane"},{"theme",Kda::HudStyle::identity()},{"motion",motionReport},
            {"material_placement","main_icon_only"},{"finish","restrained_skin_rim"},{"text_surface",HudReadingPlane::surfaceIdentity()},{"primary_information","one_next_action_and_one_status"},
            {"layering","single_protected_reading_surface"},{"palette_binding","shared_kda_skin_and_fixed_semantic_status"},{"text_geometry_animated",false},{"skin_strength",Kda::HudStyle::strength()},{"purchase_ready_badge","semantic_check_circle"},{"inset_frames",false},
            {"rounded_icon_radius",Kda::radius("artwork")},{"font_family",p.title.font.family()},{"caption",captionText},{"title",p.title.text},{"status",p.status.text},
            {"caption_visible",false},{"category_conveyed_by_icon",true},{"accessible_caption",accessibleName().contains(captionText)},{"compact_height",height()},{"prominent_purchase",itemId>0},{"text_weight",int(QFont::DemiBold)},{"rasterization","2x_physical_grayscale_resolve"},
            {"caption_rect",rectJson(p.caption.rect)},{"title_rect",rectJson(p.title.rect)},{"status_rect",rectJson(p.status.rect)},
            {"title_font_px",p.title.font.pixelSize()},{"status_font_px",p.status.font.pixelSize()},
            {"text_fits",fits(p.title,rect())&&fits(p.status,rect())},
            {"lines_do_not_overlap",!p.title.rect.intersects(p.status.rect)},
            {"item_id",itemId},{"champion_id",championId},{"icon_rect",itemId>0||championId>0?QJsonValue(rectJson(actionIconRect(itemId>0,width()))):QJsonValue()},
            {"purchase_ready",purchaseReady},{"visible",isVisible()}};
}
