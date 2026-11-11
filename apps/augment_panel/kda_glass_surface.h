#pragma once
#include "kda_glass_button.h"
#include "kda_surface.h"
#include "kda_metallic_finish.h"

namespace Kda {
enum class GlassSurfaceRole { Toolbar, Reading, Field, Popup };

inline void markSharedGlassSurface(QWidget *widget){
    if(!widget)return;
    widget->setProperty("kdaSharedGlassSurface",true);
    GlassButtonDetail::schedule();
}

// Native fields draw their text/selection above this plate. Their owner's QSS
// must leave the native background transparent while the glass mode is on.
inline QString glassFieldBackground(){return glassEnabled()?QStringLiteral("transparent"):color("surface").name();}

inline void paintPolishedEdge(QPainter &painter,const QPainterPath &shape,const QRectF &plate,qreal rounding,qreal strength=1.){
    auto lit=color("silver"),edge=color("border"),shade=color("bg"),tail=color("silver");
    lit.setAlpha(qRound(58*strength));edge.setAlpha(qRound(116*strength));shade.setAlpha(qRound(180*strength));tail.setAlpha(qRound(23*strength));
    QLinearGradient rim(plate.topLeft(),plate.bottomRight());rim.setColorAt(0,lit);rim.setColorAt(.32,edge);rim.setColorAt(.74,shade);rim.setColorAt(1,tail);
    painter.save();painter.setPen(QPen(rim,.8));painter.setBrush(Qt::NoBrush);painter.drawPath(shape);
    auto light=color("silver");light.setAlpha(qRound(25*strength));
    QLinearGradient bevel(plate.topLeft(),plate.topRight());bevel.setColorAt(0,Qt::transparent);bevel.setColorAt(.18,light);bevel.setColorAt(.72,Qt::transparent);bevel.setColorAt(1,Qt::transparent);
    painter.setPen(QPen(bevel,.7));painter.drawLine(QPointF(plate.left()+rounding,plate.top()+.8),QPointF(plate.right()-rounding,plate.top()+.8));painter.restore();
}

// All roles sample the same two cached materials at their actual workspace
// position. A grouped toolbar paints this once, including the inter-button gaps.
inline bool paintGlassSurface(QPainter &painter,QWidget *widget,const QRectF &bounds,GlassSurfaceRole role=GlassSurfaceRole::Toolbar){
    if(!glassEnabled()||!GlassButtonDetail::eligible(widget)||bounds.isEmpty())return false;
    const QRectF plate=bounds.adjusted(.5,.5,-.5,-.5);
    const qreal rounding=radius(role==GlassSurfaceRole::Field?"control":"section");
    QPainterPath shape;shape.addRoundedRect(plate,rounding,rounding);
    if(!paintGlassButtonBackground(painter,widget,shape,plate))return false;
    const bool red=skin()==QStringLiteral("kda_red");
    int opacity=132;
    if(role==GlassSurfaceRole::Reading)opacity=164;
    else if(role==GlassSurfaceRole::Field)opacity=174;
    else if(role==GlassSurfaceRole::Popup)opacity=208;
    QColor top(red?"#291923":"#20232c"),bottom(red?"#160f16":"#13161e");
    top.setAlpha(opacity);bottom.setAlpha(std::min(240,opacity+18));
    QLinearGradient tint(plate.topLeft(),plate.bottomRight());tint.setColorAt(0,top);tint.setColorAt(1,bottom);
    painter.save();painter.fillPath(shape,tint);
    paintMetallicFinish(painter,shape,role==GlassSurfaceRole::Field?.6:1.,widget);
    // Field borders/focus remain the native QSS control's responsibility.
    if(role!=GlassSurfaceRole::Field){
        paintPolishedEdge(painter,shape,plate,rounding,role==GlassSurfaceRole::Popup?1.15:.85);
    }
    painter.restore();return true;
}

// Use for otherwise plain QWidget containers. Existing custom painters can call
// markSharedGlassSurface in their constructor and paintGlassSurface in paintEvent.
class GlassSurface : public QWidget {
public:
    explicit GlassSurface(QWidget *parent=nullptr,GlassSurfaceRole value=GlassSurfaceRole::Reading):QWidget(parent),role(value){markSharedGlassSurface(this);}
protected:
    void paintEvent(QPaintEvent *)override{
        QPainter painter(this);configurePainter(painter);
        if(!paintGlassSurface(painter,this,QRectF(rect()),role)){
            painter.save();QPainterPath clip;clip.addRoundedRect(QRectF(rect()),radius("section"),radius("section"));painter.setClipPath(clip);paintFacetedMarble(painter,rect(),.5);painter.restore();
        }
    }
private:
    GlassSurfaceRole role;
};
}
