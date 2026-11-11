#pragma once
#include "kda_theme.h"
#include "kda_paint_texture.h"
#include "kda_sparkles.h"
#include <QAbstractButton>
#include <QApplication>
#include <QCache>
#include <QDataStream>
#include <QDynamicPropertyChangeEvent>
#include <QElapsedTimer>
#include <QImage>
#include <QIODevice>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QRegion>
#include <QTimer>
#include <QWidget>
#include <algorithm>
#include <cmath>

namespace Kda {
namespace MetallicDetail {
struct Normal {qreal x,y,z;};
inline Normal unit(Normal n){const qreal scale=1/std::sqrt(n.x*n.x+n.y*n.y+n.z*n.z);return {n.x*scale,n.y*scale,n.z*scale};}
inline Normal surfaceNormal(QPointF uv){return unit({(uv.x()-.5)*.24,(uv.y()-.5)*.17,1});}
struct Lighting {qreal x,y;};
inline Lighting lighting(qreal seconds){return {.035+.065*std::sin(seconds*.18),-.035+.025*std::cos(seconds*.18)};}
// A long area light reflected by a shallow virtual surface. The same light,
// view direction and surface coordinates are used for the coat and its flakes.
inline Lighting reflectedRay(Normal n){return {2*n.x*n.z,2*n.y*n.z};}
inline qreal reflectedLight(Lighting ray,Lighting light,qreal roughness){
    const qreal rx=ray.x-light.x,ry=ray.y-light.y;
    const qreal across=(rx+.55*ry)/roughness,along=(-.55*rx+ry)/.65;
    return std::exp(-.5*(across*across+along*along));
}
inline QRectF materialProjection(QWidget *owner,const QRectF &bounds){
    if(!owner)return {0,0,1,1};
    const auto window=owner->window();const QPointF offset=owner->mapTo(window,QPoint())+bounds.topLeft();
    return {offset.x()/std::max(1,window->width()),offset.y()/std::max(1,window->height()),bounds.width()/std::max(1,window->width()),bounds.height()/std::max(1,window->height())};
}
inline QPointF project(QPointF point,QSizeF size,const QRectF &projection){
    return {projection.x()+point.x()/size.width()*projection.width(),projection.y()+point.y()/size.height()*projection.height()};
}
using LightSample=std::array<int,flakeOrientations+2>;
struct LightGrid {
    using Rays=std::array<Lighting,flakeOrientations+1>;
    QSizeF size;
    QRectF projection;
    QSize dimensions;
    std::vector<Rays> rays;
    std::vector<LightSample> values,row;

    void prepare(QSizeF surfaceSize,const QRectF &surfaceProjection){
        if(size==surfaceSize&&projection==surfaceProjection&&!rays.empty())return;
        size=surfaceSize;projection=surfaceProjection;
        dimensions={std::clamp(qCeil(size.width()/12),8,96),std::clamp(qCeil(size.height()/12),4,64)};
        rays.resize(size_t(dimensions.width())*dimensions.height());
        values.resize(rays.size());row.resize(dimensions.width());
        static const auto facets=[] {
            std::array<Lighting,flakeOrientations> offsets;
            for(int group=0;group<flakeOrientations;++group){
                const qreal angle=group*6.283185307179586/flakeOrientations;
                offsets[group]={.055*std::cos(angle),.055*std::sin(angle)};
            }
            return offsets;
        }();
        for(int y=0;y<dimensions.height();++y)for(int x=0;x<dimensions.width();++x){
            auto &sample=rays[size_t(y)*dimensions.width()+x];
            const auto n=surfaceNormal(project({(x+.5)*size.width()/dimensions.width(),(y+.5)*size.height()/dimensions.height()},size,projection));
            sample[0]=reflectedRay(n);
            for(int group=0;group<flakeOrientations;++group){
                const auto tilt=facets[group];
                sample[group+1]=reflectedRay(unit({n.x+tilt.x,n.y+tilt.y,n.z}));
            }
        }
    }
};

inline QImage layeredFinish(const QRectF &bounds,qreal seconds,qreal ratio,const QRectF &projection,LightGrid *cachedGrid=nullptr){
    if(bounds.isEmpty())return {};
    const auto light=lighting(seconds);const bool red=skin()==QStringLiteral("kda_red");
    auto &textures=flakeTextures();textures.prepare(ratio);
    LightGrid temporaryGrid;
    auto &samples=cachedGrid?*cachedGrid:temporaryGrid;
    samples.prepare(bounds.size(),projection);
    const auto grid=samples.dimensions;
    const auto &lightField=samples.values;
    for(size_t i=0;i<samples.rays.size();++i){
        auto &sample=samples.values[i];
        const auto &rays=samples.rays[i];
        const qreal body=reflectedLight(rays[0],light,.16),lacquer=reflectedLight(rays[0],light,.06);
        sample[0]=qRound(255*(.02+.26*body));
        for(int group=0;group<flakeOrientations;++group){
            const qreal response=reflectedLight(rays[group+1],light,.032);
            sample[group+1]=qRound(245*std::pow(response,1.7)*body);
        }
        sample[flakeOrientations+1]=qRound(lacquer*27+body*3);
    }
    // One compositing pass; nine separately enlarged masked layers are costly
    // on Qt's software renderer. Integer interpolation keeps the coat smooth.
    QImage image(QSize(qCeil(bounds.width()*ratio),qCeil(bounds.height()*ratio)),QImage::Format_ARGB32_Premultiplied);image.setDevicePixelRatio(ratio);
    struct Column {int cell,blend,texel;};std::vector<Column> columns(image.width());
    const int edge=std::max(1,textures.tileEdge());
    const int originX=qRound(projection.x()/projection.width()*bounds.width()*ratio),originY=qRound(projection.y()/projection.height()*bounds.height()*ratio);
    const auto wrap=[edge](int value){return (value%edge+edge)%edge;};
    for(int x=0;x<image.width();++x){const qreal gx=std::clamp((x+.5)*grid.width()/image.width()-.5,qreal(0),qreal(grid.width()-1));const int cell=std::min(int(gx),grid.width()-2);columns[x]={cell,qRound((gx-cell)*256),wrap(x+originX)};}
    const int tintR=red?222:186,tintG=red?184:210,tintB=red?177:229;
    auto &rowLight=samples.row;
    for(int y=0;y<image.height();++y){
        auto row=reinterpret_cast<QRgb*>(image.scanLine(y));const auto texture=textures.loaded()?textures.row(wrap(y+originY)):nullptr;
        const qreal gy=std::clamp((y+.5)*grid.height()/image.height()-.5,qreal(0),qreal(grid.height()-1));const int cellY=std::min(int(gy),grid.height()-2),fy=qRound((gy-cellY)*256);
        // The light field is low frequency. Resolve its vertical interpolation
        // once per grid column, rather than three times for every texture pixel.
        for(int cell=0;cell<grid.width();++cell)for(int channel=0;channel<flakeOrientations+2;++channel)
            rowLight[cell][channel]=(lightField[size_t(cellY)*grid.width()+cell][channel]*(256-fy)+lightField[size_t(cellY+1)*grid.width()+cell][channel]*fy+128)>>8;
        for(int x=0;x<image.width();++x){
            const auto column=columns[x];
            const auto sample=[&](int channel){return (rowLight[column.cell][channel]*(256-column.blend)+rowLight[column.cell+1][channel]*column.blend+128)>>8;};
            int under=0,r=tintR,g=tintG,b=tintB;
            if(texture){const auto texel=texture[column.texel];const int body=texel.body?texel.body*sample(0)/255:0,glint=texel.glint?texel.glint*sample(texel.orientation+1)/255:0;under=body+glint*(255-body)/255;r=(texel.r*3+tintR)/4;g=(texel.g*3+tintG)/4;b=(texel.b*3+tintB)/4;}
            const int coat=sample(flakeOrientations+1),attenuated=under*(255-coat)/255;
            row[x]=qRgba((226*coat+r*attenuated)/255,(236*coat+g*attenuated)/255,(245*coat+b*attenuated)/255,coat+attenuated);
        }
    }
    return image;
}
inline QImage fixedFinish(const QRectF &bounds,qreal ratio,const QRectF &projection,const QPainterPath &localShape){
    // Fields and ownerless plates keep the same lighting on every repaint.
    // Cache final pixels by exact geometry, scale and skin, with a byte budget.
    static QCache<QByteArray,QImage> cache(12*1024*1024);
    QByteArray key;
    QDataStream stream(&key,QIODevice::WriteOnly);
    stream<<bounds.size()<<ratio<<projection<<localShape<<skin();
    if(const auto *image=cache.object(key))return *image;
    auto image=layeredFinish(bounds,.35,ratio,projection);
    applyMaterialCoverage(image,localShape);
    if(!image.isNull()&&image.sizeInBytes()<=cache.maxCost())cache.insert(key,new QImage(image),image.sizeInBytes());
    return image;
}
inline QRegion reflectionRegion(const QRectF &bounds){
    return QRegion(bounds.toAlignedRect());
}
inline bool surfaceActive(QWidget *widget){
    if(!widget||!widget->isVisible()||!widget->isEnabled())return false;
    if(qobject_cast<QAbstractButton*>(widget))return widget->underMouse()||widget->hasFocus();
    return true;
}

// One paced clock per window, invalidating only the material-bearing widget.
// Native text, hit targets and page geometry stay independent of the finish.
class Clock final : public QObject {
    struct Surface {QPointer<QWidget> widget;QRectF bounds,projection;QPainterPath shape;QImage frame;QString skin;qreal frameTime=-1,ratio=0;LightGrid lightGrid;};
public:
    explicit Clock(QWidget *window):QObject(window),owner(window){
        setObjectName(QStringLiteral("kdaMetallicFinishClock"));timer.setInterval(33);timer.setTimerType(Qt::PreciseTimer);
        connect(&timer,&QTimer::timeout,this,[this]{
            if(!canAnimate()){sync();return;}
            seconds+=std::min(elapsed.restart()/1000.,.1);++frames;repaintHighlights();
        });
        if(qApp)qApp->installEventFilter(this);
    }
    void watch(QWidget *widget,const QRectF &bounds){
        for(auto &surface:surfaces)if(surface.widget==widget){
            if(surface.bounds==bounds){++registrationCacheHits;return;}
            surface.bounds=bounds;++registrationChanges;sync();return;
        }
        surfaces.append({widget,bounds});++registrationChanges;sync();
    }
    qreal time()const{return seconds;}
    QImage frame(QWidget *widget,const QRectF &bounds,qreal ratio,const QRectF &projection,const QPainterPath &shape,qreal time){
        for(auto &surface:surfaces)if(surface.widget==widget){
            if(!surface.frame.isNull()&&surface.frameTime==time&&surface.ratio==ratio&&surface.projection==projection&&surface.shape==shape&&surface.skin==Kda::skin()){++cacheHits;return surface.frame;}
            QElapsedTimer build;build.start();surface.frame=layeredFinish(bounds,time,ratio,projection,&surface.lightGrid);
            applyMaterialCoverage(surface.frame,shape.translated(-bounds.topLeft()));
            surface.frameTime=time;surface.ratio=ratio;surface.projection=projection;surface.shape=shape;surface.skin=Kda::skin();
            ++renders;maximumBuildMs=std::max(maximumBuildMs,build.nsecsElapsed()/1.e6);return surface.frame;
        }
        return {};
    }
    QJsonObject report()const{
        int visible=0;for(const auto &surface:surfaces)visible+=surfaceActive(surface.widget);
        return {{"running",timer.isActive()},{"frames",qint64(frames)},{"elapsed_seconds",seconds},{"visible_surfaces",visible},
            {"clock_count",1},{"frame_interval_ms",timer.interval()},{"reflection_model","crushed_crystal_under_clearcoat"},{"star_shapes",false},
            {"material_frame_cache",true},{"frame_cache_hits",qint64(cacheHits)},{"material_renders",qint64(renders)},{"maximum_material_build_ms",maximumBuildMs},{"edge_coverage","cached_fractional_alpha"},{"frame_pacing","precise_30fps"},
            {"surface_registration_changes",qint64(registrationChanges)},{"surface_registration_cache_hits",qint64(registrationCacheHits)},
            {"independent_clearcoat",true},{"shared_window_lighting",true},{"subpixel_flakes_filtered",true},{"persistent_chip_outlines",false},
            {"texture_loaded",flakeTextures().loaded()},{"texture_decode_count",flakeTextures().decodeCount()},{"texture_prepare_count",flakeTextures().prepareCount()},{"textured_flake_count",flakeTextures().cutCount()},
            {"localized_reflection_patches",0},{"surface_only_repaints",true},{"fixed_particle_positions",true},
            {"reduced_motion",reducedMotion()},{"pauses_when_hidden_minimized_or_inactive",true},{"fields_animate",false}};
    }
protected:
    bool eventFilter(QObject *object,QEvent *event)override{
        const auto type=event->type();
        const bool windowEvent=object==owner&&(type==QEvent::Show||type==QEvent::Hide||type==QEvent::WindowStateChange||type==QEvent::WindowActivate||type==QEvent::WindowDeactivate);
        const bool surfaceEvent=(type==QEvent::Hide||type==QEvent::Show||type==QEvent::Enter||type==QEvent::Leave||type==QEvent::FocusIn||type==QEvent::FocusOut)&&qobject_cast<QWidget*>(object)&&static_cast<QWidget*>(object)->window()==owner;
        bool preference=false;
        if(object==qApp&&type==QEvent::DynamicPropertyChange){const auto name=static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName();preference=name=="lixinReducedMotion"||name=="lixinGlassEnabled";}
        if(windowEvent||surfaceEvent||preference){
            sync();
            if(!syncPending){
                syncPending=true;
                QTimer::singleShot(0,this,[this]{syncPending=false;sync();});
            }
        }
        return QObject::eventFilter(object,event);
    }
private:
    bool canAnimate()const{
        if(!owner||!owner->isVisible()||owner->isMinimized()||!owner->isActiveWindow()||!glassEnabled()||highContrastEnabled()||reducedMotion())return false;
        for(const auto &surface:surfaces)if(surfaceActive(surface.widget))return true;return false;
    }
    void repaintHighlights(){for(const auto &surface:surfaces)if(surfaceActive(surface.widget))surface.widget->update(reflectionRegion(surface.bounds));}
    void sync(){
        surfaces.removeIf([](const Surface &surface){return surface.widget.isNull();});
        for(auto &surface:surfaces)if(!surface.widget->isVisible()||(owner&&owner->isMinimized()))surface.frame={};
        if(canAnimate()){if(!timer.isActive()){elapsed.start();timer.start();}}
        else if(timer.isActive()){timer.stop();repaintHighlights();}
    }
    QPointer<QWidget> owner;QList<Surface> surfaces;QTimer timer;QElapsedTimer elapsed;qreal seconds=.35,maximumBuildMs=0;quint64 frames=0,cacheHits=0,renders=0,registrationChanges=0,registrationCacheHits=0;
    bool syncPending=false;
};
inline Clock *clock(QWidget *widget,bool create){
    if(!widget)return nullptr;auto window=widget->window();
    if(auto existing=window->findChild<QObject*>(QStringLiteral("kdaMetallicFinishClock"),Qt::FindDirectChildrenOnly))return static_cast<Clock*>(existing);
    return create?new Clock(window):nullptr;
}
}

inline QJsonObject metallicFinishReport(const QWidget *widget){
    if(widget)if(auto clock=widget->window()->findChild<QObject*>(QStringLiteral("kdaMetallicFinishClock"),Qt::FindDirectChildrenOnly))return static_cast<MetallicDetail::Clock*>(clock)->report();
    return {{"running",false},{"frames",0},{"clock_count",0},{"localized_reflection_patches",0},{"reduced_motion",reducedMotion()}};
}
inline void paintMetallicFinish(QPainter &p,const QPainterPath &shape,qreal amount=1.,QWidget *owner=nullptr){
    if(!glassEnabled()||highContrastEnabled()||shape.isEmpty()||!std::isfinite(amount)||amount<=0)return;
    const auto bounds=shape.boundingRect();qreal ratio=p.device()?p.device()->devicePixelRatioF():1.;if(!std::isfinite(ratio)||ratio<=0)ratio=1.;
    p.save();p.setRenderHint(QPainter::SmoothPixmapTransform,true);p.setOpacity(p.opacity()*std::clamp(amount,qreal(0),qreal(1)));
    qreal seconds=.35;
    MetallicDetail::Clock *materialClock=nullptr;
    if(owner&&!owner->property("kdaGlassField").toBool()&&bounds.width()>=48&&bounds.height()>=20){
        auto clock=MetallicDetail::clock(owner,true);clock->watch(owner,bounds);materialClock=clock;
        // Inactive windows retain their last reflection while the clock pauses.
        if(!reducedMotion())seconds=clock->time();
    }
    const auto projection=MetallicDetail::materialProjection(owner,bounds);
    const auto image=materialClock?materialClock->frame(owner,bounds,ratio,projection,shape,seconds)
        :MetallicDetail::fixedFinish(bounds,ratio,projection,shape.translated(-bounds.topLeft()));
    p.drawImage(bounds.topLeft(),image);
    p.save();p.setClipPath(shape,Qt::IntersectClip);paintEdgeFlow(p,bounds,seconds,.55);p.restore();
    p.restore();
}
}
