#pragma once
#include "kda_theme.h"
#include "kda_gaussian_blur.h"
#include <QApplication>
#include <QDynamicPropertyChangeEvent>
#include <QJsonArray>
#include <QJsonObject>
#include <QLinearGradient>
#include <QMetaObject>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QResizeEvent>
#include <QShowEvent>
#include <QWidget>
#include <algorithm>
#include <array>
#include <cmath>

namespace Kda {
namespace HomeMaterialDetail {
inline bool fire(){return skin()==QStringLiteral("kda_red");}
inline const std::array<QString,2> &backdropAssets(){static const std::array<QString,2> paths{QStringLiteral(":/lixin/kda/textures/home-platinum-stage-v1.jpg"),QStringLiteral(":/lixin/kda/textures/home-fire-mirror-v2.png")};return paths;}
inline const std::array<QString,2> &atmosphereAssets(){static const std::array<QString,2> paths{QStringLiteral(":/lixin/kda/textures/home-silver-ribbon-v2.png"),QStringLiteral(":/lixin/kda/textures/home-fire-ribbon-v2.png")};return paths;}
inline QString backdropAsset(){return backdropAssets()[fire()?1:0];}
inline QString atmosphereAsset(){return atmosphereAssets()[fire()?1:0];}
inline qint64 pixmapBytes(const QPixmap &image){return image.isNull()?0:qint64(image.width())*image.height()*std::max(1,image.depth()/8);}

// GUI-thread-only source ownership. These four immutable resources are decoded
// at most once, shared by both material layers and every diagnostic probe.
class MaterialSources {
    struct Entry {QString path;QPixmap image;bool attempted=false;};
public:
    MaterialSources(){for(int i=0;i<2;++i){entries[i].path=backdropAssets()[i];entries[i+2].path=atmosphereAssets()[i];}}
    const QPixmap &get(const QString &path){
        for(auto &entry:entries)if(entry.path==path){if(entry.attempted){++hits;return entry.image;}entry.attempted=true;++decodes;entry.image=QPixmap(path);return entry.image;}
        return empty;
    }
    bool loaded(const QString &path)const{for(const auto &entry:entries)if(entry.path==path)return !entry.image.isNull();return false;}
    QJsonObject report()const{int loadedCount=0,attemptedCount=0;qint64 bytes=0;for(const auto &entry:entries){loadedCount+=!entry.image.isNull();attemptedCount+=entry.attempted;bytes+=pixmapBytes(entry.image);}return {{"entry_count",4},{"max_entries",4},{"loaded_count",loadedCount},{"attempted_count",attemptedCount},{"decode_count",qint64(decodes)},{"load_count",qint64(decodes)},{"hit_count",qint64(hits)},{"estimated_bytes",bytes},{"shared",true}};}
private:
    std::array<Entry,4> entries;QPixmap empty;quint64 decodes=0,hits=0;
};
inline MaterialSources &sources(){static MaterialSources value;return value;}

// Each layer owns exactly two covers at one physical size/DPR. Changing skins
// selects a retained cover; geometry changes retire both old covers together.
class CoverCache {
public:
    explicit CoverCache(const std::array<QString,2> &variants,qreal logicalSigma=7):paths(variants),configuredSigma(std::isfinite(logicalSigma)&&logicalSigma>0?logicalSigma:0){}
    bool matches(const QSizeF &logical,qreal dpr)const{
        dpr=validRatio(dpr);return validSize(logical)&&physicalSize==targetSize(logical,dpr)&&qFuzzyCompare(ratio,dpr)&&glassMode==glassEnabled()&&prepared[0]&&prepared[1];
    }
    bool ready(const QSizeF &logical,qreal dpr)const{return matches(logical,dpr)&&!covers[0].isNull()&&!covers[1].isNull();}
    bool prepare(const QSizeF &logical,qreal dpr){
        ++prepareRequests;dpr=validRatio(dpr);if(!validSize(logical)||matches(logical,dpr))return false;
        if(!physicalSize.isEmpty())++invalidations;
        covers={};prepared={};physicalSize=targetSize(logical,dpr);ratio=dpr;glassMode=glassEnabled();++preparations;
        for(int i=0;i<2;++i){const auto &source=sources().get(paths[i]);prepared[i]=true;if(source.isNull())continue;
            auto expanded=source.scaled(physicalSize,Qt::KeepAspectRatioByExpanding,Qt::SmoothTransformation);
            auto clipped=expanded.copy(std::max(0,(expanded.width()-physicalSize.width())/2),std::max(0,(expanded.height()-physicalSize.height())/2),physicalSize.width(),physicalSize.height());
            if(glassMode&&configuredSigma>0&&!clipped.isNull()){covers[i]=QPixmap::fromImage(gaussianMaterialBlur(clipped.toImage(),configuredSigma,dpr));++filters;}
            else covers[i]=clipped;
            covers[i].setDevicePixelRatio(dpr);++scales;
        }
        return true;
    }
    const QPixmap &forSize(const QString &path,const QSizeF &logical,qreal dpr,bool reuseGeometry=false){
        int index=-1;for(int i=0;i<2;++i)if(paths[i]==path)index=i;if(index<0||!validSize(logical))return empty;
        ++requests;
        if(matches(logical,dpr)||(reuseGeometry&&glassMode==glassEnabled()&&!covers[index].isNull()))++hits;
        else{++misses;prepare(logical,dpr);}
        return covers[index];
    }
    QJsonObject report()const{int count=0;qint64 bytes=0;for(const auto &image:covers){count+=!image.isNull();bytes+=pixmapBytes(image);}return {{"cover_count",count},{"max_entries",2},{"physical_size",QJsonArray{physicalSize.width(),physicalSize.height()}},{"device_pixel_ratio",ratio},{"scale_count",qint64(scales)},{"hit_count",qint64(hits)},{"miss_count",qint64(misses)},{"request_count",qint64(requests)},{"prepare_count",qint64(preparations)},{"prepare_request_count",qint64(prepareRequests)},{"invalidation_count",qint64(invalidations)},{"estimated_bytes",bytes},{"current_geometry_only",true},{"both_variants_prepared",prepared[0]&&prepared[1]},
        {"filter_count",qint64(filters)},{"cache_mode",physicalSize.isEmpty()?"unprepared":glassMode?"gaussian":"original"},{"blur_applied",glassMode&&configuredSigma>0&&count==2},{"logical_sigma",glassMode?configuredSigma:0.},{"configured_sigma",configuredSigma},{"blur_method","three_box_gaussian"},{"current_mode_only",true},{"filter_in_prepare_only",true}};}
private:
    static qreal validRatio(qreal dpr){return std::isfinite(dpr)&&dpr>0?dpr:1;}
    static bool validSize(QSizeF size){return std::isfinite(size.width())&&std::isfinite(size.height())&&size.width()>0&&size.height()>0;}
    static QSize targetSize(QSizeF logical,qreal dpr){return {std::max(1,int(std::ceil(logical.width()*dpr))),std::max(1,int(std::ceil(logical.height()*dpr)))};}
    std::array<QString,2> paths;std::array<QPixmap,2> covers;std::array<bool,2> prepared{};QPixmap empty;
    QSize physicalSize;qreal ratio=0,configuredSigma=0;bool glassMode=false;quint64 requests=0,hits=0,misses=0,scales=0,preparations=0,prepareRequests=0,invalidations=0,filters=0;
};
inline CoverCache &backdropCache(){static CoverCache value(backdropAssets());return value;}
inline QPainterPath roundedClip(const QRectF &area){QPainterPath path;path.addRoundedRect(area,4,4);return path;}
}

// This base never receives a pointer phase. The independent atmosphere below
// moves over it while native foreground widgets retain their own geometry.
inline void paintHomeBackdrop(QPainter &painter,const QRectF &area){
    if(area.isEmpty())return;
    const qreal dpr=painter.device()?painter.device()->devicePixelRatioF():1;
    const auto &image=HomeMaterialDetail::backdropCache().forSize(HomeMaterialDetail::backdropAsset(),area.size(),dpr);
    painter.save();configurePainter(painter);painter.setClipPath(HomeMaterialDetail::roundedClip(area),Qt::IntersectClip);
    painter.fillRect(area,color("bg"));
    if(!image.isNull()){painter.setOpacity(HomeMaterialDetail::fire()?.65:.36);painter.drawPixmap(area.topLeft(),image);}
    painter.restore();
}

class HomeAtmosphere final : public QWidget {
public:
    explicit HomeAtmosphere(QWidget *parent=nullptr):QWidget(parent),cache(HomeMaterialDetail::atmosphereAssets(),3.5){
        setObjectName(QStringLiteral("homeAtmosphere"));setAttribute(Qt::WA_TransparentForMouseEvents);setAttribute(Qt::WA_NoSystemBackground);
        setAutoFillBackground(false);setFocusPolicy(Qt::NoFocus);setProperty("kdaDepthMapped",true);
        if(qApp)qApp->installEventFilter(this);
    }
    ~HomeAtmosphere()override{if(qApp)qApp->removeEventFilter(this);}
    QJsonObject report()const{
        const auto delta=actualOffset();const auto &pool=HomeMaterialDetail::sources();
        return {{"backdrop_asset",HomeMaterialDetail::backdropAsset()},{"midground_asset",HomeMaterialDetail::atmosphereAsset()},
            {"backdrop_loaded",pool.loaded(HomeMaterialDetail::backdropAsset())},{"asset_loaded",pool.loaded(HomeMaterialDetail::atmosphereAsset())},{"actual_offset",QJsonArray{delta.x(),delta.y()}},
            {"same_as_pointer",true},{"click_through",testAttribute(Qt::WA_TransparentForMouseEvents)},{"static_backdrop",true},
            {"safe_extension",8},{"offset_limit",QJsonArray{6,3}},{"scaled_cache",true},{"idle_timer",false},
            {"source_cache",pool.report()},{"backdrop_cache",HomeMaterialDetail::backdropCache().report()},{"midground_cache",cache.report()},
            {"paired_variants_ready",HomeMaterialDetail::backdropCache().ready(size(),devicePixelRatioF())&&cache.ready(expandedSize(),devicePixelRatioF())},
            {"prewarm_pending",preparePending},{"prewarm_runs",qint64(prewarmRuns)},{"report_decodes_images",false},
            {"glass_enabled",glassEnabled()},{"report_filters_images",false},{"per_pointer_filter",false},
            {"reduced_motion",reducedMotion()},{"identities_exported",false}};
    }
protected:
    bool event(QEvent *event)override{
        if(event->type()==QEvent::DynamicPropertyChange&&static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName()=="lixinDepthOffset")update();
        if(event->type()==QEvent::DevicePixelRatioChange||event->type()==QEvent::ScreenChangeInternal){schedulePrepare();update();}
        return QWidget::event(event);
    }
    bool eventFilter(QObject *object,QEvent *event)override{
        if(event->type()==QEvent::DynamicPropertyChange){
            const auto key=static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName();
            if(object==qApp){
                if(key=="lixinSkin"||key=="lixinDepthEnabled"||key=="lixinReducedMotion")update();
                else if(key=="lixinGlassEnabled"){schedulePrepare();update();}
            }else if(object==window()&&key=="dashboardInteractiveMoveSize"&&!interactiveMoveSize()){
                schedulePrepare();
                if(auto *workspace=parentWidget())workspace->update();
            }
        }
        return QWidget::eventFilter(object,event);
    }
    void resizeEvent(QResizeEvent *event)override{QWidget::resizeEvent(event);schedulePrepare();}
    void showEvent(QShowEvent *event)override{QWidget::showEvent(event);schedulePrepare();}
    void paintEvent(QPaintEvent *)override{
        QPainter painter(this);configurePainter(painter);
        const QRectF bounds(rect());if(bounds.isEmpty())return;
        painter.setClipPath(HomeMaterialDetail::roundedClip(bounds.adjusted(.5,.5,-.5,-.5)));
        const auto delta=actualOffset();const QRectF expanded=bounds.adjusted(-8,-8,8,8);
        const bool reuseGeometry=interactiveMoveSize();
        const auto &image=cache.forSize(HomeMaterialDetail::atmosphereAsset(),expanded.size(),devicePixelRatioF(),reuseGeometry);
        if(!image.isNull()){
            painter.setOpacity(HomeMaterialDetail::fire()?.74:.47);
            if(reuseGeometry)painter.drawPixmap(expanded.translated(delta),image,QRectF(image.rect()));
            else painter.drawPixmap(expanded.topLeft()+QPointF(delta),image);
            painter.setOpacity(1);
        }
        // A quiet reading field is part of this material, not an input panel.
        QLinearGradient quiet(bounds.left(),0,bounds.left()+bounds.width()*.74,0);
        const QColor dark=HomeMaterialDetail::fire()?QColor(9,9,13):QColor(14,16,22);
        auto tone=dark;tone.setAlpha(HomeMaterialDetail::fire()?105:76);quiet.setColorAt(0,tone);
        tone.setAlpha(HomeMaterialDetail::fire()?69:42);quiet.setColorAt(.55,tone);tone.setAlpha(0);quiet.setColorAt(1,tone);
        painter.fillRect(bounds,quiet);
    }
private:
    bool interactiveMoveSize()const{return window()->property("dashboardInteractiveMoveSize").toBool();}
    QSizeF expandedSize()const{return {width()+16.,height()+16.};}
    void schedulePrepare(){
        if(preparePending||interactiveMoveSize())return;
        preparePending=true;
        if(!QMetaObject::invokeMethod(this,[this]{
            preparePending=false;
            if(size().isEmpty()||interactiveMoveSize())return;
            ++prewarmRuns;
            HomeMaterialDetail::backdropCache().prepare(size(),devicePixelRatioF());
            cache.prepare(expandedSize(),devicePixelRatioF());
            update();
        },Qt::QueuedConnection))preparePending=false;
    }
    QPoint actualOffset()const{
        if(!depthEnabled()||reducedMotion())return {};
        const auto phase=property("lixinDepthOffset").toPointF();
        const qreal x=std::isfinite(phase.x())?std::clamp(phase.x(),qreal(-1),qreal(1)):0;
        const qreal y=std::isfinite(phase.y())?std::clamp(phase.y(),qreal(-1),qreal(1)):0;
        return {qRound(x*6),qRound(y*3)};
    }
    HomeMaterialDetail::CoverCache cache;bool preparePending=false;quint64 prewarmRuns=0;
};
}
