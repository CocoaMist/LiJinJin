#pragma once
#include "kda_theme.h"
#include "kda_home_material.h"
#include "kda_gaussian_blur.h"
#include <QAbstractButton>
#include <QElapsedTimer>
#include <QImage>
#include <QMetaObject>
#include <QPainter>
#include <QPainterPath>
#include <QPixmapCache>
#include <QDataStream>
#include <QWidget>
#include <array>

namespace Kda {
namespace GlassButtonDetail {
inline bool eligible(const QWidget *widget){
    for(auto current=widget;current;current=current->parentWidget()){
        const auto name=current->objectName();
        if(current->property("dashboardNavigation").toBool()||current->property("kdaNoGlass").toBool()||name==QStringLiteral("topbar")||name==QStringLiteral("titlebar")||name==QStringLiteral("navigationGroup")||name==QStringLiteral("navigationRail")||name==QStringLiteral("navigation")||name==QStringLiteral("navHistory"))return false;
    }
    return widget!=nullptr;
}
// Only quiet toolbar actions inherit their parent's continuous reading plate.
// Fields and ordinary/primary actions keep their own native control surface.
inline const QWidget *sharedSurface(const QWidget *widget){
    if(!eligible(widget))return nullptr;
    for(auto current=widget?widget->parentWidget():nullptr;current;current=current->parentWidget())
        if(current->property("kdaSharedGlassSurface").toBool())return current;
    return nullptr;
}
class MaterialCache {
public:
    bool ready()const{return prepared&&!textures[0].isNull()&&!textures[1].isNull();}
    bool queue(){if(pending||prepared||!qApp||!glassEnabled())return false;pending=true;return true;}
    void prepare(){
        pending=false;if(prepared)return;prepared=true;++prepares;QElapsedTimer timer;timer.start();
        const QSize size(512,288);
        const auto paintCover=[&](QPainter &painter,const QPixmap &source,qreal opacity){if(source.isNull())return;const qreal ratio=std::max(size.width()/qreal(source.width()),size.height()/qreal(source.height()));const QSizeF cover(source.width()*ratio,source.height()*ratio);const QRectF target((size.width()-cover.width())*.5,(size.height()-cover.height())*.5,cover.width(),cover.height());painter.setOpacity(opacity);painter.drawPixmap(target,source,QRectF(source.rect()));};
        for(int index=0;index<2;++index){
            QImage material(size,QImage::Format_ARGB32_Premultiplied);material.fill(index?QColor("#171017"):QColor("#161923"));
            const auto &back=HomeMaterialDetail::sources().get(HomeMaterialDetail::backdropAssets()[index]);
            const auto &air=HomeMaterialDetail::sources().get(HomeMaterialDetail::atmosphereAssets()[index]);if(back.isNull()&&air.isNull())continue;
            {QPainter painter(&material);configurePainter(painter);paintCover(painter,back,index?.68:.43);paintCover(painter,air,index?.64:.57);}
            const auto softened=gaussianMaterialBlur(material,6.,1.);if(softened.isNull())continue;textures[index]=QPixmap::fromImage(softened);++blurs;
        }
        prepareNs=timer.nsecsElapsed();
    }
    const QPixmap &texture()const{return textures[skin()==QStringLiteral("kda_red")?1:0];}
    void sampled(){++paints;}
    QJsonObject report()const{int count=0;for(const auto &texture:textures)count+=!texture.isNull();return {{"ready",ready()},{"pending",pending},{"prepared_variants",count},{"max_entries",2},{"logical_size",QJsonArray{512,288}},{"prepare_count",qint64(prepares)},{"blur_count",qint64(blurs)},{"paint_count",qint64(paints)},{"prepare_ms",prepareNs/1.e6},{"sigma_logical",6.},{"shared_sources",true},{"blur_in_paint",false},{"window_capture",false}};}
private:
    std::array<QPixmap,2> textures;bool prepared=false,pending=false;quint64 prepares=0,blurs=0,paints=0;qint64 prepareNs=0;
};
inline MaterialCache &cache(){static MaterialCache value;return value;}
inline void schedule(){
    if(!cache().queue())return;
    QMetaObject::invokeMethod(qApp,[]{cache().prepare();for(auto widget:QApplication::allWidgets())if(qobject_cast<QAbstractButton*>(widget)||widget->property("kdaGlassField").toBool()||widget->property("kdaSharedGlassSurface").toBool())widget->update();},Qt::QueuedConnection);
}
inline const QWidget *samplingSurface(const QWidget *widget){for(auto current=widget->parentWidget();current;current=current->parentWidget())if(current->objectName()==QStringLiteral("dashboardWorkspace"))return current;return widget->window();}
}
inline QJsonObject glassButtonReport(){return GlassButtonDetail::cache().report();}
inline bool paintGlassButtonBackground(QPainter &painter,QWidget *widget,const QPainterPath &clip,const QRectF &target){
    if(!glassEnabled()||!GlassButtonDetail::eligible(widget)||target.isEmpty())return false;
    auto &cache=GlassButtonDetail::cache();if(!cache.ready()){GlassButtonDetail::schedule();return false;}
    const auto &texture=cache.texture();const auto anchor=GlassButtonDetail::samplingSurface(widget);if(!anchor||anchor->width()<=0||anchor->height()<=0)return false;
    QRectF cover(texture.rect());const qreal aspect=anchor->width()/qreal(anchor->height());
    if(cover.width()/cover.height()>aspect){const qreal width=cover.height()*aspect;cover.setLeft((texture.width()-width)*.5);cover.setWidth(width);}else{const qreal height=cover.width()/aspect;cover.setTop((texture.height()-height)*.5);cover.setHeight(height);}
    const QPointF origin(widget->mapTo(anchor,QPoint()));const QRectF local=target.translated(origin);
    const QRectF source(cover.x()+local.x()/anchor->width()*cover.width(),cover.y()+local.y()/anchor->height()*cover.height(),local.width()/anchor->width()*cover.width(),local.height()/anchor->height()*cover.height());
    qreal ratio=painter.device()?painter.device()->devicePixelRatioF():1.;if(!std::isfinite(ratio)||ratio<=0)ratio=1.;
    const auto bounds=clip.boundingRect();const auto localClip=clip.translated(-bounds.topLeft());
    QByteArray geometry;QDataStream stream(&geometry,QIODevice::WriteOnly);stream<<localClip<<source<<target.translated(-bounds.topLeft());
    const auto key=QStringLiteral("lixin-glass-covered:%1:%2:%3").arg(texture.cacheKey()).arg(ratio,0,'g',12).arg(qulonglong(qHash(geometry)));
    QPixmap covered;
    if(!QPixmapCache::find(key,&covered)){
        QImage layer(QSize(qCeil(bounds.width()*ratio),qCeil(bounds.height()*ratio)),QImage::Format_ARGB32_Premultiplied);layer.setDevicePixelRatio(ratio);layer.fill(Qt::transparent);
        {QPainter material(&layer);configurePainter(material);material.drawPixmap(target.translated(-bounds.topLeft()),texture,source);}
        applyMaterialCoverage(layer,localClip);covered=QPixmap::fromImage(layer);QPixmapCache::insert(key,covered);
    }
    painter.save();painter.setRenderHint(QPainter::SmoothPixmapTransform,true);painter.drawPixmap(bounds.topLeft(),covered);painter.restore();cache.sampled();return true;
}
inline bool paintGlassButtonBackground(QPainter &painter,QWidget *widget,const QPainterPath &clip){return paintGlassButtonBackground(painter,widget,clip,clip.boundingRect());}
}
