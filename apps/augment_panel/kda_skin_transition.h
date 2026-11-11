#pragma once
#include "kda_theme.h"
#include "kda_background_motion.h"
#include <QApplication>
#include <QBrush>
#include <QDynamicPropertyChangeEvent>
#include <QEasingCurve>
#include <QElapsedTimer>
#include <QHideEvent>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QLinearGradient>
#include <QLineF>
#include <QMetaObject>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPointer>
#include <QPolygonF>
#include <QRadialGradient>
#include <QRandomGenerator>
#include <QRegion>
#include <QTransform>
#include <QVariantAnimation>
#include <QVector>
#include <QWidget>
#include <QtMath>
#include <algorithm>
#include <array>
#include <cmath>

namespace Kda {
// A finite, display-only child layer. Native controls take the new skin
// immediately. Only the captured old pixels receive an alpha reveal.
class SkinTransition final : public QWidget {
    struct Particle {
        QPointF p0,p1,p2,p3;
        qreal size=1,delay=0,life=1,rotation=0,spin=0,opacity=1,heat=0;
        int tone=0,shape=0;
    };
    struct BackgroundCapture {BackgroundMotionProfile profile;QJsonObject source;};
    struct FrameCosts {qint64 captureNs=0,backgroundNs=0,analysisNs=0,prepareNs=0,paintNs=0,maxPaintNs=0,lastPaintNs=0;int paints=0;};
    static QWidget *requireOwner(QWidget *owner){if(!owner)qFatal("Kda::SkinTransition requires an existing parent widget");return owner;}
public:
    explicit SkinTransition(QWidget *owner):QWidget(requireOwner(owner)),host(owner),animation(this){
        setObjectName(QStringLiteral("kdaSkinTransition"));setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_TransparentForMouseEvents);setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_NoSystemBackground);setAutoFillBackground(false);hide();
        animation.setDuration(DurationMs);animation.setStartValue(0.);animation.setEndValue(1.);animation.setEasingCurve(QEasingCurve::Linear);
        connect(&animation,&QVariantAnimation::valueChanged,this,[this]{
            if(!active)return;
            if(!frameFits()||reducedMotion()){clearSnapshot();return;}
            if(elapsedMs()>=SnapshotFadeMs)releaseOldFrame();
            update();
        });
        connect(&animation,&QVariantAnimation::finished,this,[this]{clearSnapshot();});
        host->installEventFilter(this);if(qApp)qApp->installEventFilter(this);
    }
    ~SkinTransition()override{if(host)host->removeEventFilter(this);if(qApp)qApp->removeEventFilter(this);}

    bool captureBeforeChange(){
        if(capturing)return false;
        if(!canCapture()){clearSnapshot();return false;}
        // Analysis renders the workspace paint only, never its children. The
        // separate visual snapshot includes an in-flight reveal on rapid flips.
        costs=FrameCosts{};QElapsedTimer captureTimer;captureTimer.start();
        capturing=true;const auto background=captureBackground();const auto displayed=host->grab();capturing=false;costs.captureNs=captureTimer.nsecsElapsed();
        clearSnapshot();if(displayed.isNull()||!canCapture())return false;
        analysisFrom=background.profile;captureFrom=background.source;analysisTo=BackgroundMotionProfile();captureTo=QJsonObject();
        snapshot=displayed;capturedSize=host->size();capturedDpr=host->devicePixelRatioF();
        clipRect=workspaceRect();setGeometry(host->rect());pending=true;return true;
    }
    void revealAfterChange(){
        if(!pending||snapshot.isNull()||!frameFits()||reducedMotion()){clearSnapshot();return;}
        QElapsedTimer prepareTimer;prepareTimer.start();const auto background=captureBackground();analysisTo=background.profile;captureTo=background.source;
        pending=false;prepareFlow();makeParticles();
        costs.prepareNs=prepareTimer.nsecsElapsed();
        show();raise();clock.start();active=true;animation.start();
        if(animation.state()!=QAbstractAnimation::Running)clearSnapshot();
    }
    QJsonObject report()const{
        const bool running=active&&animation.state()==QAbstractAnimation::Running;
        const auto bounds=clipRect.isEmpty()?workspaceRect():clipRect;
        const auto bar=host?host->findChild<QWidget*>(QStringLiteral("topbar")):nullptr;
        const QRect barRect=bar?QRect(bar->mapTo(host,QPoint()),bar->size()):QRect();
        const qreal phase=clamp01(elapsedMs()/qreal(DurationMs));
        const QJsonObject flow{{"from_focus",pointJson(analysisFrom.focus)},{"to_focus",pointJson(analysisTo.focus)},
            {"from_direction",pointJson(analysisFrom.direction)},{"to_direction",pointJson(analysisTo.direction)},
            {"direction",pointJson(flowDirection)},{"palette_blend",smooth(phase)},
            {"front_progress",smooth(phase)},{"reveal_progress",clamp01(elapsedMs()/qreal(SnapshotFadeMs))},
            {"particle_path",fireMode?"rising_cubic_bezier_with_curl":"cubic_bezier_decelerated"},{"spawn_source","background_salience"},{"layout","clustered_material_lanes"}};
        return {{"active",running},{"running",running},{"has_snapshot",!snapshot.isNull()},{"has_reveal_buffer",!revealBuffer.isNull()},
            {"input_passthrough",testAttribute(Qt::WA_TransparentForMouseEvents)&&focusPolicy()==Qt::NoFocus},
            {"duration",DurationMs},{"duration_ms",DurationMs},{"snapshot_fade_ms",SnapshotFadeMs},{"elapsed_ms",elapsedMs()},
            {"particle_count",particles.size()},{"particle_peak_count",peakCount},{"particle_alive_count",aliveCount()},
            {"particle_visible_count",aliveCount(true)},{"particle_style",fireMode?"fire_flow":"background_flow"},{"material_style",fireMode?"coherent_flame_streams":"crystal_refraction_streams"},
            {"fire_mode",fireMode},{"particle_palette_start",paletteJson(particlePalette(0))},
            {"particle_palette",paletteJson(particlePalette(phase))},{"particle_palette_target",paletteJson(particlePalette(1))},
            {"fire_tongue_count",fireTongueCount()},{"fire_upward_paths",fireUpwardPaths()},
            {"fire_reading_area_alpha_limit",0.},{"reading_area_particles",false},{"reading_quiet_rect",QJsonArray{quietReadingRect().x(),quietReadingRect().y(),quietReadingRect().width(),quietReadingRect().height()}},{"material_stream_count",materialStreams.size()},{"material_stream_peak_count",streamPeakCount},
            {"particle_distribution","clustered_along_material_streams"},{"clustered",true},{"stream_count",materialStreams.size()},{"hero_path_count",materialStreams.size()},{"particle_draw_batches",lastParticleBatches},{"particle_drawn_count",lastDrawnParticles},{"material_paths_drawn",lastMaterialPaths},
            {"particle_clip_rect",QJsonArray{bounds.x(),bounds.y(),bounds.width(),bounds.height()}},
            {"titlebar_excluded",!bounds.isEmpty()&&workspaceRect().contains(bounds)&&!bounds.intersects(barRect)},
            {"analysis_source","background_only"},{"background_only",true},{"analysis_from",analysisFrom.report()},
            {"analysis_to",analysisTo.report()},{"background_capture_from",captureFrom},{"background_capture_to",captureTo},
            {"adaptive_flow",true},{"spatial_reveal",true},{"old_frame_masking","direct_pixmap_material_bands_no_resample"},{"old_frame_band_count",RevealBands},{"flow",flow},
            {"timing_ms",QJsonObject{{"capture_before_total",costs.captureNs/1.e6},{"background_render_total",costs.backgroundNs/1.e6},{"analysis_total",costs.analysisNs/1.e6},{"reveal_prepare_total",costs.prepareNs/1.e6},{"paint_total",costs.paintNs/1.e6},{"paint_last",costs.lastPaintNs/1.e6},{"paint_max",costs.maxPaintNs/1.e6},{"paint_frames",costs.paints}}},
            {"stages",QJsonArray{"background_analysis","spatial_reveal","flowing_particles","release"}},
            {"stage",pending?"captured":running?(elapsedMs()<SnapshotFadeMs?"spatial_reveal":"flowing_particles"):"idle"},
            {"idle_timer",false},{"looping",false},{"reduced_motion",reducedMotion()}};
    }
protected:
    void paintEvent(QPaintEvent *)override{
        if(!active)return;
        if(!frameFits()||reducedMotion()||animation.state()!=QAbstractAnimation::Running){queueRelease();return;}
        QElapsedTimer paintTimer;paintTimer.start();
        QPainter painter(this);configurePainter(painter);
        QPainterPath clip;clip.addRoundedRect(QRectF(clipRect),radius("workspace"),radius("workspace"));painter.setClipPath(clip);
        const qreal elapsed=elapsedMs();
        if(!snapshot.isNull()&&elapsed<SnapshotFadeMs)paintOldFrame(painter,elapsed/SnapshotFadeMs);
        paintLightfront(painter,elapsed/DurationMs);paintParticles(painter,elapsed/1000.);
        costs.lastPaintNs=paintTimer.nsecsElapsed();costs.paintNs+=costs.lastPaintNs;costs.maxPaintNs=std::max(costs.maxPaintNs,costs.lastPaintNs);++costs.paints;
    }
    void hideEvent(QHideEvent *event)override{clearSnapshot();QWidget::hideEvent(event);}
    bool eventFilter(QObject *object,QEvent *event)override{
        if(object==host&&(event->type()==QEvent::Hide||event->type()==QEvent::Close||event->type()==QEvent::Resize))clearSnapshot();
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
        if(object==host&&event->type()==QEvent::DevicePixelRatioChange)clearSnapshot();
#endif
        if(object==qApp){
            if(event->type()==QEvent::ApplicationDeactivate)clearSnapshot();
            if(event->type()==QEvent::DynamicPropertyChange&&static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName()=="lixinReducedMotion"&&reducedMotion())clearSnapshot();
        }
        return QWidget::eventFilter(object,event);
    }
private:
    static qreal clamp01(qreal value){return std::clamp(value,qreal(0),qreal(1));}
    static qreal smooth(qreal value){value=clamp01(value);return value*value*(3-2*value);}
    static qreal dot(QPointF a,QPointF b){return a.x()*b.x()+a.y()*b.y();}
    static QPointF unit(QPointF value){const qreal length=std::hypot(value.x(),value.y());return length>1e-7?value/length:QPointF(1,0);}
    static QJsonArray pointJson(QPointF value){return {value.x(),value.y()};}
    static QJsonArray paletteJson(const std::array<QColor,3> &palette){QJsonArray result;for(const auto &ink:palette)result.append(ink.name(QColor::HexRgb));return result;}
    static QColor mixColor(const QColor &from,const QColor &to,qreal phase){
        phase=clamp01(phase);return QColor::fromRgbF(from.redF()*(1-phase)+to.redF()*phase,
            from.greenF()*(1-phase)+to.greenF()*phase,from.blueF()*(1-phase)+to.blueF()*phase);
    }
    std::array<QColor,3> particlePalette(qreal phase)const{
        std::array<QColor,3> result;phase=smooth(phase);
        static const std::array<QColor,3> crystal{QColor("#6c819c"),QColor("#a3b6ce"),QColor("#e6f5ff")};
        for(int i=0;i<3;++i)result[i]=fireMode?mixColor(firePaletteStart[i],firePaletteTarget[i],phase):mixColor(crystal[i],mixColor(analysisFrom.palette[i],analysisTo.palette[i],phase),.12);
        return result;
    }
    QWidget *workspace()const{return host?host->findChild<QWidget*>(QStringLiteral("dashboardWorkspace")):nullptr;}
    BackgroundCapture captureBackground()const{
        BackgroundCapture result;auto content=workspace();const auto fallback=color("silver");
        if(!content)return result;
        QElapsedTimer renderTimer;renderTimer.start();
        const qreal dpr=content->devicePixelRatioF();
        QImage image(QSize(qCeil(content->width()*dpr),qCeil(content->height()*dpr)),QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(dpr);image.fill(Qt::transparent);
        {QPainter painter(&image);content->render(&painter,QPoint(),QRegion(),QWidget::DrawWindowBackground);}
        costs.backgroundNs+=renderTimer.nsecsElapsed();QElapsedTimer analysisTimer;analysisTimer.start();result.profile=analyzeBackgroundMotion(image,fallback);costs.analysisNs+=analysisTimer.nsecsElapsed();
        result.source={{"draw_children",false},{"background_only",true},{"logical_size",QJsonArray{content->width(),content->height()}},
            {"pixel_size",QJsonArray{image.width(),image.height()}},{"device_pixel_ratio",dpr},{"fallback_color",fallback.name(QColor::HexArgb)}};
        return result; // The background image is destroyed; only aggregates remain.
    }
    QPointF toPosition(QPointF uv)const{return QPointF(clipRect.left()+uv.x()*clipRect.width(),clipRect.top()+uv.y()*clipRect.height());}
    void prepareFlow(){
        const auto axis=unit(analysisFrom.direction+analysisTo.direction*2);
        flowDirection=unit(QPointF(axis.x()*clipRect.width(),axis.y()*clipRect.height()));
        fireMode=skin()==QStringLiteral("kda_red")||(analysisTo.warmth>.055&&analysisTo.meanSaturation>.28);
        if(fireMode){
            // A warm target starts warm immediately, including rapid flips
            // from silver. The measured background still supplies its tint,
            // salience and curl, rather than tinting old silver fragments.
            firePaletteStart={QColor("#e83c43"),QColor("#ff7838"),QColor("#ffebb9")};
            const std::array<QColor,3> hot{QColor("#ed3c32"),QColor("#ff913d"),QColor("#fff2ce")};
            for(int i=0;i<3;++i)firePaletteTarget[i]=mixColor(hot[i],analysisTo.palette[i],.08);
            flowDirection=unit(QPointF(flowDirection.x()*.20,-.96-std::abs(flowDirection.y())*.04));
        }
        frontAspect=std::clamp(qreal(1.35)+analysisTo.contrast*2,qreal(1.35),qreal(1.85));
    }
    QPointF salientPoint(const BackgroundMotionProfile &profile,QRandomGenerator *random)const{
        QPointF candidate; // Rejection sampling follows the measured 12 x 8 field.
        for(int trial=0;trial<12;++trial){candidate={random->generateDouble(),random->generateDouble()};
            const qreal energy=profile.energyAt(candidate),outer=candidate.x()>.80||candidate.x()<.10||candidate.y()>.84?1.25:1.;
            const qreal acceptance=fireMode?(.015+.985*std::pow(energy,1.25))*outer:.08+.92*energy;
            if(random->generateDouble()<acceptance)break;}
        return candidate*.82+profile.focus*.18;
    }
    void makeParticles(){
        const qreal scale=std::sqrt(clipRect.width()*qreal(clipRect.height())/(840.*440.));
        const int count=std::clamp(qRound(900*scale),900,1200);particles.reserve(count);peakCount=count;
        auto random=QRandomGenerator::global();const auto number=[&]{return random->generateDouble();};
        const qreal focusBias=(std::clamp(analysisTo.focus.x(),qreal(.82),qreal(.96))-.89)*.32;
        const auto addStream=[&](QPointF a,QPointF b,QPointF c,QPointF d,qreal width){Particle stream;const auto locate=[&](QPointF uv){uv.rx()+=focusBias;return toPosition(uv);};stream.p0=locate(a);stream.p1=locate(b);stream.p2=locate(c);stream.p3=locate(d);stream.size=width*scale;stream.rotation=materialStreams.size()*1.71;stream.heat=analysisTo.energyAt((a+d)*.5);materialStreams.append(stream);};
        if(fireMode){
            addStream({.85,1.04},{1.02,.83},{.83,.60},{.94,.20},15);
            addStream({.99,1.02},{.85,.76},{1.05,.53},{.93,.35},8);
            addStream({.67,1.04},{.85,1.00},{.97,.82},{.88,.64},12);
            addStream({.94,.87},{.83,.73},{.99,.49},{.96,.10},6);
            addStream({.36,1.02},{.68,.94},{.81,1.00},{.94,.80},5);
        }else{
            addStream({.86,1.05},{1.04,.82},{.83,.58},{.95,.08},7);
            addStream({.99,1.02},{.84,.80},{1.02,.59},{.91,.29},3.5);
            addStream({.31,1.02},{.65,.93},{.78,1.01},{.97,.73},5);
        }
        streamPeakCount=materialStreams.size();for(auto &batch:particleBatches){batch.clear();batch.reserve(count/4);}
        for(int i=0;i<count;++i){
            Particle particle;const auto &stream=materialStreams[i%materialStreams.size()];
            // A few shared current lines carry all of the fine dust. Individual
            // grains vary within that current instead of falling across the UI.
            const qreal start=number()*.80,end=std::min(qreal(.995),start+.07+number()*.16);
            const QPointF a=bezier(stream,start),b=bezier(stream,end),direction=unit(b-a),normal(-direction.y(),direction.x());
            const qreal scatter=(number()+number()-1)*stream.size*(i%11==0?3.8:1.25),curl=(number()-.5)*stream.size*.65;
            particle.p0=a+normal*scatter;particle.p3=b+normal*scatter*.72;
            if(fireMode)particle.p3.setY(std::min(particle.p3.y(),particle.p0.y()-8));
            const auto distance=particle.p3-particle.p0;particle.p1=particle.p0+distance*.34+normal*curl;particle.p2=particle.p0+distance*.72-normal*curl*.45;
            particle.delay=.01+number()*.29+start*.06;particle.life=std::min(.65+number()*.30,DurationMs/1000.-particle.delay-.02);
            particle.size=.28+number()*.80;particle.rotation=number()*6.283185307179586;
            particle.shape=i%47==0?1:0;particle.opacity=particle.shape?.24:.065+number()*.12;
            particle.tone=particle.shape?2:int(random->bounded(2u));particles.append(particle);
        }
    }
    QBrush frontBrush(qreal radiusValue,const QGradientStops &stops)const{
        QRadialGradient gradient(QPointF(),std::max(qreal(1),radiusValue));gradient.setStops(stops);
        QTransform transform;const auto focus=toPosition(analysisTo.focus);transform.translate(focus.x(),focus.y());
        transform.rotate(std::atan2(flowDirection.y(),flowDirection.x())*180/3.141592653589793);transform.scale(frontAspect,1);
        QBrush brush(gradient);brush.setTransform(transform);return brush;
    }
    void paintOldFrame(QPainter &painter,qreal phase){
        // Each source pixel is drawn once at its original DPR and position.
        // Small opacity steps follow the rising material, with no ARGB copy,
        // destination-in pass, radial wash or resampling of captured text.
        for(int band=0;band<RevealBands;++band){
            const int top=clipRect.top()+band*clipRect.height()/RevealBands,bottom=clipRect.top()+(band+1)*clipRect.height()/RevealBands;
            const qreal arrival=(1-(band+.5)/RevealBands)*.18,opacity=1-smooth((phase-arrival)/.82);
            if(opacity<=.001)continue;painter.save();painter.setClipRect(QRect(clipRect.left(),top,clipRect.width(),bottom-top),Qt::IntersectClip);painter.setOpacity(opacity);painter.drawPixmap(QPointF(),snapshot);painter.restore();
        }
    }
    QRectF quietReadingRect()const{return {clipRect.left()+clipRect.width()*.11,clipRect.top()+clipRect.height()*.05,clipRect.width()*.76,clipRect.height()*.82};}
    static QPointF curveTangent(const Particle &stream,qreal t){const qreal rest=1-t;return unit((stream.p1-stream.p0)*(3*rest*rest)+(stream.p2-stream.p1)*(6*rest*t)+(stream.p3-stream.p2)*(3*t*t));}
    static QPainterPath ribbonPath(const Particle &stream,qreal phase,qreal widthScale=1.){
        constexpr int Samples=18;std::array<QPointF,Samples+1> left{},right{};
        const qreal extent=.32+.68*smooth(phase*2.5),breath=.9+.1*std::sin(phase*6+stream.rotation);
        for(int i=0;i<=Samples;++i){const qreal local=i/qreal(Samples),t=local*extent;const auto tangent=curveTangent(stream,t);const QPointF normal(-tangent.y(),tangent.x());
            const qreal shape=std::pow(std::max(qreal(0),std::sin(local*3.141592653589793)),.85);
            const auto center=bezier(stream,t)+normal*(std::sin(t*6.5-phase*4.4+stream.rotation)*shape*stream.size*.48);
            const qreal span=stream.size*widthScale*shape*(1-local*.57)*breath;left[i]=center+normal*span;right[i]=center-normal*span;
        }
        QPainterPath path(left[0]);for(int i=1;i<Samples;++i)path.quadTo(left[i],(left[i]+left[i+1])*.5);path.lineTo(left[Samples]);path.lineTo(right[Samples]);for(int i=Samples-1;i>0;--i)path.quadTo(right[i],(right[i]+right[i-1])*.5);path.lineTo(right[0]);path.closeSubpath();return path;
    }
    void paintLightfront(QPainter &painter,qreal phase){
        lastMaterialPaths=0;const qreal envelope=smooth(phase/.16)*smooth((1-phase)/.45);if(envelope<=0)return;
        const auto palette=particlePalette(phase);painter.save();painter.setClipRegion(QRegion(clipRect).subtracted(QRegion(quietReadingRect().toAlignedRect())),Qt::IntersectClip);
        painter.setOpacity(envelope);painter.setPen(Qt::NoPen);
        for(int i=0;i<materialStreams.size();++i){const auto &stream=materialStreams[i];
            if(fireMode){paintFlame(painter,stream,phase,palette);lastMaterialPaths+=3;}
            else{QLinearGradient refraction(stream.p0,stream.p3);auto ink=palette[1];ink.setAlpha(0);refraction.setColorAt(0,ink);ink.setAlpha(23);refraction.setColorAt(.26,ink);ink=palette[2];ink.setAlpha(47);refraction.setColorAt(.50,ink);ink=palette[0];ink.setAlpha(12);refraction.setColorAt(.62,ink);ink=palette[2];ink.setAlpha(31);refraction.setColorAt(.80,ink);ink.setAlpha(0);refraction.setColorAt(1,ink);painter.setBrush(refraction);painter.drawPath(ribbonPath(stream,phase));
                auto edge=palette[2];edge.setAlpha(62);painter.setPen(QPen(edge,.45));painter.setBrush(Qt::NoBrush);painter.drawPath(ribbonPath(stream,phase,.18));painter.setPen(Qt::NoPen);lastMaterialPaths+=2;}
        }
        painter.restore();
    }
    static qreal travelPhase(qreal life){return 1-std::pow(1-clamp01(life),2.4);}
    static QPointF bezier(const Particle &particle,qreal phase){const qreal rest=1-phase;return particle.p0*(rest*rest*rest)+particle.p1*(3*rest*rest*phase)+particle.p2*(3*rest*phase*phase)+particle.p3*(phase*phase*phase);}
    static QPointF particlePosition(const Particle &particle,qreal age){return bezier(particle,travelPhase(age/particle.life));}
    void paintFlame(QPainter &painter,const Particle &stream,qreal phase,const std::array<QColor,3> &palette)const{
        QLinearGradient outer(stream.p0,stream.p3);auto ink=palette[0];ink.setAlpha(0);outer.setColorAt(0,ink);ink.setAlpha(59);outer.setColorAt(.20,ink);ink=palette[1];ink.setAlpha(87);outer.setColorAt(.48,ink);ink=palette[0];ink.setAlpha(38);outer.setColorAt(.79,ink);ink.setAlpha(0);outer.setColorAt(1,ink);
        painter.setPen(Qt::NoPen);painter.setBrush(outer);painter.drawPath(ribbonPath(stream,phase));
        QLinearGradient core(stream.p0,stream.p3);ink=palette[1];ink.setAlpha(0);core.setColorAt(0,ink);ink.setAlpha(104);core.setColorAt(.27,ink);ink=palette[2];ink.setAlpha(172);core.setColorAt(.54,ink);ink=palette[1];ink.setAlpha(69);core.setColorAt(.79,ink);ink.setAlpha(0);core.setColorAt(1,ink);
        painter.setBrush(core);painter.drawPath(ribbonPath(stream,phase,.19));
        auto rim=palette[0];rim.setAlpha(52);painter.setBrush(Qt::NoBrush);painter.setPen(QPen(rim,.55));painter.drawPath(ribbonPath(stream,phase,.64));painter.setPen(Qt::NoPen);
    }
    void paintParticles(QPainter &painter,qreal time){
        const auto palette=particlePalette(time*1000/DurationMs);for(auto &batch:particleBatches)batch.clear();lastDrawnParticles=0;lastParticleBatches=0;
        const auto quiet=quietReadingRect();
        for(const auto &particle:particles){const qreal age=time-particle.delay;if(age<0||age>=particle.life)continue;
            const qreal life=age/particle.life,envelope=smooth(age/.12)*smooth((1-life)/.40),opacity=particle.opacity*envelope;if(opacity<.020)continue;
            const QPointF position=particlePosition(particle,age);if(!QRectF(clipRect).contains(position)||quiet.contains(position))continue;
            const auto tangent=curveTangent(particle,travelPhase(life));const qreal length=particle.size*(particle.shape?3.2:fireMode?1.45:1.05);
            const int alphaBucket=std::clamp(qRound(opacity*255/18)-1,0,3),bucket=particle.tone*4+alphaBucket;
            particleBatches[bucket].append(QLineF(position-tangent*length*.5,position+tangent*length*.5));++lastDrawnParticles;
        }
        // Only twelve possible painter batches, rather than a path, glow image
        // and painter-state changes for every grain. Fine dust never draws into
        // the reading area; the material currents remain the visual hierarchy.
        static constexpr int alpha[]{18,32,52,78};painter.save();painter.setClipRegion(QRegion(clipRect).subtracted(QRegion(quiet.toAlignedRect())),Qt::IntersectClip);painter.setOpacity(1);
        for(int bucket=0;bucket<int(particleBatches.size());++bucket){const auto &batch=particleBatches[bucket];if(batch.isEmpty())continue;auto ink=palette[bucket/4];ink.setAlpha(alpha[bucket%4]);painter.setPen(QPen(ink,bucket%4==3?.82:.62,Qt::SolidLine,Qt::RoundCap));painter.drawLines(batch.constData(),int(batch.size()));++lastParticleBatches;}
        painter.restore();
    }
    QRect workspaceRect()const{const auto content=workspace();return content&&content->isVisibleTo(host)?QRect(content->mapTo(host,QPoint()),content->size()).intersected(host->rect()):QRect();}
    int elapsedMs()const{return clock.isValid()?int(std::min<qint64>(clock.elapsed(),DurationMs)):0;}
    int aliveCount(bool visibleOnly=false)const{int count=0;if(active){const qreal time=elapsedMs()/1000.;for(const auto &particle:particles)if(time>=particle.delay&&time<particle.delay+particle.life&&(!visibleOnly||QRectF(clipRect).contains(particlePosition(particle,time-particle.delay))))++count;}return count;}
    int fireTongueCount()const{return fireMode?materialStreams.size():0;}
    bool fireUpwardPaths()const{if(!fireMode||particles.isEmpty())return false;for(const auto &particle:particles)if(particle.p3.y()>=particle.p0.y())return false;return true;}
    bool canCapture()const{return host&&host->isVisible()&&!host->isMinimized()&&host->testAttribute(Qt::WA_WState_Created)&&host->width()>0&&host->height()>0&&!reducedMotion()&&!workspaceRect().isEmpty();}
    bool frameFits()const{return canCapture()&&host->size()==capturedSize&&qFuzzyCompare(host->devicePixelRatioF(),capturedDpr)&&geometry()==host->rect()&&clipRect==workspaceRect();}
    void queueRelease(){
        if(releaseQueued)return;releaseQueued=true;const auto expectedGeneration=generation;
        QMetaObject::invokeMethod(this,[this,expectedGeneration]{if(generation!=expectedGeneration)return;releaseQueued=false;if(active&&(animation.state()!=QAbstractAnimation::Running||!frameFits()||reducedMotion()))clearSnapshot();},Qt::QueuedConnection);
    }
    void releaseOldFrame(){snapshot=QPixmap();revealBuffer=QImage();}
    void clearSnapshot(){
        if(clearing)return;clearing=true;++generation;releaseQueued=false;pending=false;active=false;
        animation.stop();clock.invalidate();releaseOldFrame();QVector<Particle>().swap(particles);QVector<Particle>().swap(materialStreams);for(auto &batch:particleBatches)QVector<QLineF>().swap(batch);capturedSize=QSize();capturedDpr=0;clipRect=QRect();
        if(isVisible())hide();clearing=false;
    }
    static constexpr int DurationMs=1250,SnapshotFadeMs=420,RevealBands=16;
    QPointer<QWidget> host;QVariantAnimation animation;QElapsedTimer clock;QPixmap snapshot;QImage revealBuffer;QVector<Particle> particles,materialStreams;
    BackgroundMotionProfile analysisFrom,analysisTo;QJsonObject captureFrom,captureTo;QPointF flowDirection{1,0};qreal frontAspect=1.35;
    std::array<QVector<QLineF>,12> particleBatches;mutable FrameCosts costs;
    std::array<QColor,3> firePaletteStart{},firePaletteTarget{};
    QSize capturedSize;QRect clipRect;qreal capturedDpr=0;quint64 generation=0;int peakCount=0,streamPeakCount=0,lastDrawnParticles=0,lastParticleBatches=0,lastMaterialPaths=0;
    bool pending=false,active=false,capturing=false,clearing=false,releaseQueued=false,fireMode=false;
};
}
