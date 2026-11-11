#include "death_tips_window.h"
#include "kda_theme.h"
#include "kda_surface.h"
#include "kda_hud_style.h"
#include "kda_sparkles.h"
#include "death_tip_visual.h"
#include "death_tip_mixed.h"
#include "overlay_layout.h"
#include <QApplication>
#include <QDynamicPropertyChangeEvent>
#include <QFontMetrics>
#include <QHideEvent>
#include <QJsonArray>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QStringList>
#include <QTextLayout>
#include <algorithm>
#include <cmath>

namespace {
QJsonArray coords(const QRect &r){return {r.x(),r.y(),r.width(),r.height()};}
struct Lines {QString title;QStringList body;QRect titleRect,bodyRect;QFont titleFont,bodyFont;int lineHeight=18;bool full=true,fits=true;};
Lines lines(const QSize &size,const QJsonObject &content){
    Lines out;const bool expanded=DeathTipVisual::expanded(content),large=expanded&&size.width()>=760;
    const int left=large?30:20,width=expanded?qRound(size.width()*.61)-left-20:std::max(0,size.width()-40);
    out.lineHeight=large?22:expanded?16:18;
    const int top=expanded?(large?102:58):33,maxLines=expanded?(large?3:2):2;
    out.titleRect=QRect(left,expanded?(large?50:28):10,width,large?40:24);out.bodyRect=QRect(left,top,width,maxLines*out.lineHeight);
    const auto titleFont=Kda::hudFont(Kda::FontRole::Label,large?24:expanded?16:14,QFont::DemiBold);
    const auto bodyFont=Kda::hudFont(Kda::FontRole::Body,large?16:expanded?12:14,QFont::Medium);out.titleFont=titleFont;out.bodyFont=bodyFont;
    auto view=content;if(!view.contains("visual")&&view.contains("detail_visual"))view["visual"]=view.value("detail_visual");
    const auto visual=DeathTipVisual::validated(view);const auto kind=visual.value("layout").toString();
    const QString rawTitle=kind=="loadout"?QStringLiteral("出装参考"):kind=="duel"?QStringLiteral("输出 ≈8s"):kind=="rings"?QStringLiteral("承伤参考"):kind=="change"?QStringLiteral("装备变化"):QStringLiteral("复活准备");
    out.title=QFontMetrics(titleFont).elidedText(rawTitle,Qt::ElideRight,width);out.full=false;
    if(!expanded){for(const auto &v:visual.value("bars").toArray()){if(out.body.size()>=2)break;const auto row=v.toObject();out.body.append(QFontMetrics(bodyFont).elidedText(row.value("label").toString()+QStringLiteral(" ≈")+QString::number(row.value("value").toDouble(),'g',4)+visual.value("unit").toString(),Qt::ElideRight,width));}}
    out.bodyRect.setHeight(out.body.size()*out.lineHeight);
    const QFontMetrics titleMetrics(titleFont),bodyMetrics(bodyFont);
    out.fits=out.titleRect.height()>=titleMetrics.height()&&bodyMetrics.height()<=out.lineHeight&&QRect(QPoint(0,0),size).contains(out.bodyRect);
    for(const auto &line:out.body)out.fits&=bodyMetrics.horizontalAdvance(line)<=width;
    return out;
}
}
DeathTipsWindow::DeathTipsWindow():QWidget(nullptr,Qt::Tool|Qt::FramelessWindowHint|Qt::WindowStaysOnTopHint|Qt::WindowTransparentForInput|Qt::WindowDoesNotAcceptFocus){
    sparkles=new Kda::SparkleField(this,Kda::SparkleField::Activity::VisibleOverlay);
    sparkles->setEnabled(false);
    qApp->installEventFilter(this);
    setWindowTitle(QStringLiteral("李金金阵亡提示"));setAttribute(Qt::WA_TranslucentBackground);setAttribute(Qt::WA_ShowWithoutActivating);Kda::attachOverlayBackdrop(this);
    setAttribute(Qt::WA_TransparentForMouseEvents);setFocusPolicy(Qt::NoFocus);
    animation.setStartValue(0.);animation.setEndValue(1.);animation.setEasingCurve(QEasingCurve::OutCubic);
    decorationTimer.setSingleShot(true);decorationTimer.setInterval(1600);
    connect(&decorationTimer,&QTimer::timeout,this,[this]{stopDecorations();update();});
    connect(&animation,&QVariantAnimation::valueChanged,this,[this](const QVariant &value){progress=value.toReal();++frames;update();});
    connect(&animation,&QVariantAnimation::finished,this,[this]{progress=1.;if(phase==QStringLiteral("exit")){hide();phase=QStringLiteral("hidden");}else phase=QStringLiteral("steady");update();});
}
bool DeathTipsWindow::motionEnabled() const{return !content.value("reduced_motion").toBool()&&!Kda::reducedMotion();}
void DeathTipsWindow::stopDecorations(){decorationTimer.stop();sparkles->setEnabled(false);sparkles->setRepaintRegion({});}
bool DeathTipsWindow::eventFilter(QObject *object,QEvent *event){
    if(object==qApp&&event->type()==QEvent::DynamicPropertyChange
        &&static_cast<QDynamicPropertyChangeEvent *>(event)->propertyName()=="lixinReducedMotion"&&Kda::reducedMotion()){
        stopDecorations();animation.stop();progress=1.;
        if(phase==QStringLiteral("exit"))hide();else if(isVisible())phase=QStringLiteral("steady");
        update();
    }
    return QWidget::eventFilter(object,event);
}
void DeathTipsWindow::begin(const QString &next){
    animation.stop();phase=next;++transitions;
    if(!motionEnabled()){progress=1.;if(next==QStringLiteral("exit")){hide();phase=QStringLiteral("hidden");}else phase=QStringLiteral("steady");update();return;}
    progress=0.;animation.setDuration(next==QStringLiteral("exit")?160:260);animation.start();update();
}
void DeathTipsWindow::present(const QRect &viewport,const QJsonObject &data,double opacity){
    if(!viewport.isValid()||viewport.width()<540||viewport.height()<320||data.value("tip_id").toString().isEmpty()||data.value("text").toString().trimmed().isEmpty()){dismiss();return;}
    const bool edge=data.value("compact_edge").toBool();
    const bool expanded=!edge&&DeathTipVisual::expanded(data);
    const int width=std::clamp(qRound(viewport.width()*(expanded?.60:.36)),480,expanded?1120:640);
    const int top=std::max(96,qRound(viewport.height()*(expanded?.16:.12)));
    const auto visual=DeathTipVisual::validated(data);
    const QRect target=edge?compactDeathTipPlacement(viewport):QRect(viewport.center().x()-(width-1)/2,viewport.top()+top,width,expanded?(width>=760?360:224):90);
    if(target.isEmpty()||!viewport.contains(target)){hide();return;}
    const bool first=!isVisible(),changed=content.value("tip_id")!=data.value("tip_id"),exiting=phase==QStringLiteral("exit");
    content=data;
    if(edge){content["detail_visual"]=content.take("visual");content["expanded"]=false;}
    if(!motionEnabled()||!expanded||visual.isEmpty())stopDecorations();
    else if(first||changed){++decorationStarts;decorationTimer.start();sparkles->setEnabled(true);}
    setGeometry(target);setWindowOpacity(std::clamp(opacity,.35,1.));
    const auto accessible=data.value("title").toString(QStringLiteral("阵亡 TIPS"))+QStringLiteral(" · ")+data.value("text").toString();
    const auto visualDescription=DeathTipVisual::describe(visual);setAccessibleName(accessible+(visual.isEmpty()?QString():QStringLiteral(" · ")+visualDescription));setToolTip(accessibleName());if(first)show();
    if(first||changed||exiting)begin(first?QStringLiteral("enter"):QStringLiteral("switch"));
    else if(!motionEnabled()&&animation.state()==QAbstractAnimation::Running){animation.stop();phase=QStringLiteral("steady");progress=1.;}
    update();
}
void DeathTipsWindow::dismiss(){if(!isVisible()||phase==QStringLiteral("exit"))return;stopDecorations();begin(QStringLiteral("exit"));}
void DeathTipsWindow::hideEvent(QHideEvent *event){animation.stop();stopDecorations();phase=QStringLiteral("hidden");progress=1.;QWidget::hideEvent(event);}
void DeathTipsWindow::paintEvent(QPaintEvent *){
    Kda::OverlayCanvas canvas(this);auto &p=canvas.paint();const bool exiting=phase==QStringLiteral("exit");
    const qreal offset=animation.state()==QAbstractAnimation::Running?12*(exiting?progress:1-progress):0;
    const QRectF shell(.7,.7,width()-1.4,height()-14.4);const bool expanded=DeathTipVisual::expanded(content),large=width()>=760;
    p.save();
    // Expanded evidence is present at its final position and contrast from the
    // first frame. Only the art/header finish responds to a newly selected tip.
    if(!expanded){p.translate(0,offset);p.setOpacity(animation.state()==QAbstractAnimation::Running?(exiting?1-progress:.55+.45*progress):1.);}
    Kda::HudStyle::paintSurface(p,shell,false);
    if(expanded){
        static const QPixmap artwork(QStringLiteral(":/lixin/kda/illustrations/soraka-star-guardian-black-evening-halfbody-v3.png"));
        const QColor mask(10,9,15);
        p.save();QPainterPath clip;clip.addRoundedRect(shell.adjusted(1,1,-1,-1),Kda::radius("card_inner"),Kda::radius("card_inner"));p.setClipPath(clip,Qt::IntersectClip);
        if(!artwork.isNull()){
            const QRectF portrait(shell.left()+shell.width()*.46,shell.top(),shell.width()*.54,shell.height());
            const qreal ratio=portrait.width()/portrait.height(),imageRatio=qreal(artwork.width())/artwork.height();
            const QRectF source=ratio>=imageRatio?QRectF(0,0,artwork.width(),artwork.width()/ratio):QRectF(artwork.width()-artwork.height()*ratio,0,artwork.height()*ratio,artwork.height());
            p.fillRect(shell,mask);
            p.save();p.setOpacity(.62+.28*Kda::HudStyle::strength());p.drawPixmap(portrait,artwork,source);p.restore();
            auto tint=Kda::HudStyle::accent(qRound(34*Kda::HudStyle::strength()));p.fillRect(portrait,tint);
        }
        QLinearGradient shade(shell.topLeft(),shell.topRight());auto shadeColor=mask;shadeColor.setAlpha(255);shade.setColorAt(0,shadeColor);shade.setColorAt(.46,shadeColor);shadeColor.setAlpha(238);shade.setColorAt(.60,shadeColor);shadeColor.setAlpha(70);shade.setColorAt(.72,shadeColor);shadeColor.setAlpha(20);shade.setColorAt(1,shadeColor);p.fillRect(shell,shade);
        p.restore();
        p.save();p.translate(0,offset*.35);p.setOpacity(animation.state()==QAbstractAnimation::Running?(exiting?1-progress:.6+.4*progress):1.);
        p.setFont(Kda::font(Kda::FontRole::Label,large?11:9,QFont::DemiBold));p.setPen(Kda::HudStyle::neutral(210));
        p.drawText(QRect(large?30:20,large?24:10,qRound(width()*.45),16),Qt::AlignLeft|Qt::AlignVCenter,content.value("synthetic").toBool()?QStringLiteral("演示"):QStringLiteral("复活"));
        p.restore();
    }

    auto visual=DeathTipVisual::validated(content);visual["caption"]=content.value("synthetic").toBool()?QStringLiteral("演示 · ≈"):QStringLiteral("≈");
    const auto mixed=DeathTipMixed::geometry(DeathTipVisual::chartRect(size()),visual);
    const auto text=lines(size(),content);
    if(expanded&&motionEnabled()&&decorationTimer.isActive()){
        const QRect particleArea(qRound(width()*.63),16,qRound(width()*.35),std::max(0,height()-64));
        const QRegion evidence=DeathTipMixed::stableRegion(mixed)
            +QRegion(text.titleRect.adjusted(-2,-2,2,2))+QRegion(text.bodyRect.adjusted(-2,-2,2,2));
        const auto particles=QRegion(particleArea).subtracted(evidence);
        sparkles->setRepaintRegion(particles+DeathTipMixed::animatedRegion(mixed).subtracted(evidence));
        p.save();p.setClipRegion(particles,Qt::IntersectClip);sparkles->paint(p,particleArea,12,.7);p.restore();
    }
    if(!visual.isEmpty()){
        const auto chart=DeathTipVisual::chartRect(size());p.setPen(QPen(Kda::HudStyle::neutral(45),.7));p.drawLine(QPointF(chart.left(),chart.top()-10),QPointF(chart.right(),chart.top()-10));
        if(mixed.kind.isEmpty())DeathTipVisual::paint(p,chart,visual);
        else DeathTipMixed::paint(p,chart,visual,{iconResolver,motionEnabled()?sparkles->elapsedSeconds():0,1,motionEnabled()});
    }
    p.setPen(Kda::HudStyle::color("silver"));p.setFont(text.titleFont);p.drawText(text.titleRect,Qt::AlignLeft|Qt::AlignVCenter,text.title);
    p.setFont(text.bodyFont);p.setPen(Kda::HudStyle::color("text"));
    for(int i=0;i<text.body.size();++i)p.drawText(QRect(text.bodyRect.x(),text.bodyRect.y()+i*text.lineHeight,text.bodyRect.width(),text.lineHeight),Qt::AlignLeft|Qt::AlignVCenter,text.body[i]);
    if(expanded){
        p.setFont(Kda::font(Kda::FontRole::Body,large?10:9));p.setPen(Kda::HudStyle::color("muted"));p.drawText(QRect(large?30:20,height()-(large?39:32),text.bodyRect.width()-56,14),Qt::AlignLeft|Qt::AlignVCenter,QString());
        const auto label=DeathTipMixed::category(mixed.kind);const int chipWidth=large?84:60;
        p.setFont(Kda::font(Kda::FontRole::Label,large?10:8,QFont::DemiBold));p.setPen(Kda::HudStyle::color("ice"));p.drawText(QRect(text.titleRect.right()-chipWidth,large?24:10,chipWidth,16),Qt::AlignRight|Qt::AlignVCenter,label);
        const int count=std::clamp(content.value("deck_count").toInt(1),1,6),index=std::clamp(content.value("deck_index").toInt(),0,count-1);
        if(count>1){const qreal left=text.bodyRect.right()-(count-1)*9;
            for(int i=0;i<count;++i){p.setPen(Qt::NoPen);p.setBrush(Kda::HudStyle::color(i==index?"ice":"border"));const qreal size=i==index?4:3;p.drawEllipse(QPointF(left+i*9,height()-(large?32:25)),size/2,size/2);}
        }
    }
    p.restore();
}
QJsonObject DeathTipsWindow::report() const{
    const auto text=lines(size(),content);const bool running=animation.state()==QAbstractAnimation::Running;
    auto out=content;out["visible"]=isVisible();out["geometry"]=coords(geometry());out["anchor"]=content.value("compact_edge").toBool()?"left_edge_outside_shop_reference":"top_center_below_scoreboard_reference";
    out["expanded_automatic_overlay"]=false;
    out["click_through"]=windowFlags().testFlag(Qt::WindowTransparentForInput);out["accepts_keyboard_focus"]=!windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus);
    out["text_fits"]=text.fits;out["full_text_visible"]=text.full;out["full_text_accessible"]=accessibleName().contains(content.value("text").toString());
    out["explanatory_copy_visible"]=false;out["style_name_visible"]=false;out["displayed_title"]=text.title;out["displayed_lines"]=QJsonArray::fromStringList(text.body);out["body_rect"]=coords(text.bodyRect);
    const bool expanded=DeathTipVisual::expanded(content);
    out["motion"]=QJsonObject{{"enabled",motionEnabled()},{"running",running},{"phase",phase},{"progress",progress},{"offset_px",running?12*(phase==QStringLiteral("exit")?progress:1-progress):0.},{"transition_count",transitions},{"frames",frames},{"duration_ms",animation.duration()},{"actual_content_translation",!expanded},{"target",expanded?"header_finish":"compact_text"},{"body_and_numeric_evidence_stable",expanded},{"decoration_running",decorationTimer.isActive()},{"decoration_duration_ms",decorationTimer.interval()},{"decoration_starts",decorationStarts},{"decoration_remaining_ms",std::max(0,decorationTimer.remainingTime())},{"infinite_loop",false}};
    const auto visual=DeathTipVisual::validated(content);out["visual"]=visual;out["visual_kind"]=visual.value("kind").toString(QStringLiteral("text"));out["visual_rect"]=visual.isEmpty()?QJsonArray{}:coords(DeathTipVisual::chartRect(size()));
    const auto mixed=DeathTipMixed::geometry(DeathTipVisual::chartRect(size()),visual);out["visual_layout"]=mixed.kind.isEmpty()?visual.value("kind").toString(QStringLiteral("text")):mixed.kind;
    out["animated_icon_count"]=mixed.icons.size();out["icon_motion_enabled"]=motionEnabled()&&decorationTimer.isActive()&&!mixed.icons.isEmpty();out["icon_motion_frames"]=sparkles->report().value("frames");
    out["visual_fits"]=visual.isEmpty()||(rect().contains(DeathTipVisual::chartRect(size()))&&DeathTipMixed::fits(mixed,DeathTipVisual::chartRect(size())));out["particles"]=sparkles->report();
    out["expanded_death_stage"]=DeathTipVisual::expanded(content);out["body_font_px"]=text.bodyFont.pixelSize();
    out["background_artwork"]=DeathTipVisual::expanded(content)?QStringLiteral("soraka-star-guardian-black-evening-halfbody-v3"):QString();
    out["theme"]=Kda::HudStyle::identity();out["skin_strength"]=Kda::HudStyle::strength();out["surface_style"]="single_quiet_skin_edge";
    out["art_mask"]=Kda::skin()==QStringLiteral("kda_red")?QStringLiteral("ember_black_reading_field"):QStringLiteral("silver_prism_reading_field");
    out["score_display_delayed"]=false;out["network_requests"]=0;out["real_game_alignment_verified"]=false;return out;
}
