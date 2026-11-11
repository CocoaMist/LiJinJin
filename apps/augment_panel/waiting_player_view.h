#pragma once
#include "kda_home_material.h"
#include "waiting_type_motion.h"
#include <QFontDatabase>
#include <QFontMetrics>
#include <QJsonArray>
#include <QJsonObject>
#include <QLinearGradient>
#include <QPainterPath>
#include <QPainter>
#include <QTransform>
#include <QVector>
#include <QFontMetricsF>

// Native lettering and status copy sit over a static stage backdrop.
class WaitingPlayerView final : public QWidget {
public:
    explicit WaitingPlayerView(QWidget *parent=nullptr):QWidget(parent),motion(this){
        setObjectName(QStringLiteral("waitingPlayerView"));setMinimumHeight(260);
        setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Expanding);
        setAccessibleName(QStringLiteral("等待玩家。等你入场。登录英雄联盟客户端后自动进入主页。"));
    }
    void setConnected(bool value){if(connected==value)return;connected=value;update();}
    // Deterministic native rendering for previews/self-checks, never a UI control.
    void setInspectionTime(qreal value){inspectionTime=value;motion.setEnabled(value<0);update();}
    QJsonObject report()const{
        const bool red=Kda::HomeMaterialDetail::fire();
        const auto asset=Kda::HomeMaterialDetail::backdropAsset();
        return {{"visible",isVisibleTo(window())},{"connected",connected},{"motion",motion.report()},
            {"composition","full_width_kinetic_type_v7"},{"background_rendering",red?"native_vector":"cached_bitmap"},
            {"backdrop_asset",red?QStringLiteral("native:crimson_kinetic_type_v7"):asset},
            {"backdrops_ready",red||Kda::HomeMaterialDetail::sources().loaded(asset)},
            {"display_font",displayFamily()},{"display_font_loaded",!displayFamily().isEmpty()},
            {"headline_treatment","interlocking_cut_type_outline_and_solid"},{"text_rasterized",false},{"physical_materials",!red},
            {"type_animation",QJsonObject{{"loop_seconds",WaitingType::loopSeconds},{"phase_seconds",currentTime()},{"stage",WaitingType::stage(currentTime())},
                {"effects",QJsonArray{"character_stagger","masked_rise","width_snap_back","temporal_echo","contour_repeater","slice_displacement","chromatic_registration","outline_fill_matte"}},
                {"status_and_instructions_static",true},{"whole_screen_flash",false},{"live_text_geometry_animated",false}}},
            {"decorative_shapes",false},{"headline_bounds",QJsonArray{headlineBounds.x(),headlineBounds.y(),headlineBounds.width(),headlineBounds.height()}},
            {"fake_progress",false},{"content_fits",width()>=560&&height()>=260&&(headlineBounds.isEmpty()||QRectF(rect()).contains(headlineBounds.adjusted(-2,-2,2,2)))}};
    }
protected:
    void paintEvent(QPaintEvent *)override{
        QPainter p(this);Kda::configurePainter(p);
        const qreal rounding=Kda::radius("section");
        QPainterPath clip;clip.addRoundedRect(QRectF(rect()),rounding,rounding);p.setClipPath(clip);
        const bool red=Kda::skin()==QStringLiteral("kda_red");
        const qreal t=currentTime();
        p.fillRect(rect(),Kda::color("bg"));
        if(!red){
            const bool resizing=window()->property("dashboardInteractiveMoveSize").toBool();
            const auto &backdrop=stageBackdrop.forSize(Kda::HomeMaterialDetail::backdropAsset(),size(),devicePixelRatioF(),resizing);
            p.save();
            p.setOpacity(.52);
            p.drawPixmap(rect(),backdrop);
            p.restore();
        }
        const auto frame=readingFrame();const int inset=frame.left(),usable=frame.width();
        auto label=Kda::font(Kda::FontRole::Label,11,QFont::Medium);label.setLetterSpacing(QFont::AbsoluteSpacing,1.2);
        text(p,{inset,frame.top()+19,usable/2,22},QStringLiteral("等待玩家"),label,Kda::color("silver"));
        auto index=displayFont(11);index.setLetterSpacing(QFont::AbsoluteSpacing,2);
        p.setFont(index);p.setPen(Kda::color("muted"));p.drawText(QRect(frame.center().x(),frame.top()+19,usable/2,22),Qt::AlignRight|Qt::AlignVCenter,QStringLiteral("PLAYER / STANDBY"));
        prepareType();
        paintWord(p,ready,0,red,t);paintWord(p,play,1,red,t);
        const bool compact=frame.height()<380;
        const int captionY=frame.top()+std::min(qRound(frame.height()*.75),frame.height()-80);
        auto chinese=Kda::font(Kda::FontRole::Title,compact?17:20,QFont::Medium);chinese.setLetterSpacing(QFont::AbsoluteSpacing,2);
        text(p,{inset,captionY,usable,29},QStringLiteral("等你入场"),chinese,Kda::color("text"));
        if(!compact)text(p,{inset,captionY+34,usable,21},QStringLiteral("登录英雄联盟客户端后，自动进入主页"),Kda::font(Kda::FontRole::Body,12),Kda::color("muted"));
        const int y=frame.bottom()-38;
        p.setPen(Qt::NoPen);p.setBrush(Kda::color(connected?"ice":"muted"));p.drawEllipse(QPointF(inset+3,y+15),2.5,2.5);
        text(p,{inset+15,y+2,usable-15,26},connected?QStringLiteral("客户端已连接 · 正在识别玩家"):QStringLiteral("助手已就绪 · 等待客户端"),Kda::font(Kda::FontRole::Body,12),Kda::color("silver"));
    }
private:
    QRect readingFrame()const{
        const int inset=std::clamp(width()/17,26,52);
        return QRect(inset,0,width()-inset*2,height());
    }
    static QString displayFamily(){
        static const QString family=[] {
            const int id=QFontDatabase::addApplicationFont(QStringLiteral(":/lixin/kda/fonts/BarlowSemiCondensed-Bold.ttf"));
            const auto names=QFontDatabase::applicationFontFamilies(id);return names.isEmpty()?QString():names.first();
        }();return family;
    }
    static QFont displayFont(int pixels){
        auto face=Kda::font(Kda::FontRole::Display,pixels,QFont::Bold);
        if(!displayFamily().isEmpty())face.setFamily(displayFamily());
        return face;
    }
    static QLinearGradient spectrum(QRectF area,bool red,qreal t){
        const qreal shift=std::sin(t*.23)*area.width()*.12;
        QLinearGradient gradient(area.left()+shift,area.top(),area.right()+shift,area.bottom());
        if(red){gradient.setColorAt(0,QColor("#ff718b"));gradient.setColorAt(.31,QColor("#ffe3ed"));gradient.setColorAt(.55,QColor("#ff8a68"));gradient.setColorAt(.79,QColor("#ff4d88"));gradient.setColorAt(1,QColor("#eac0fc"));}
        else{gradient.setColorAt(0,QColor("#aef5ff"));gradient.setColorAt(.24,QColor("#e9f0ff"));gradient.setColorAt(.47,QColor("#b2a9ff"));gradient.setColorAt(.69,QColor("#f4b8e9"));gradient.setColorAt(1,QColor("#b8edee"));}
        return gradient;
    }
    struct Echo{QPixmap first,second;QRectF bounds;};
    struct Word{QVector<QPainterPath> glyphs;QRectF area;QVector<Echo> echoes;QPixmap contour;QRectF contourBounds;qreal echoDpr=0;bool echoRed=false;};
    qreal currentTime()const{return Kda::reducedMotion()?1.4:WaitingType::phase(inspectionTime>=0?inspectionTime:motion.elapsedSeconds());}
    static Word lettering(const QString &copy,QRectF area){
        auto face=displayFont(100);face.setLetterSpacing(QFont::AbsoluteSpacing,1.7);
        QPainterPath raw;raw.addText(QPointF(),face,copy);
        QTransform slant;slant.shear(-.18,0);raw=slant.map(raw);
        const auto bounds=raw.boundingRect();if(bounds.isEmpty())return {};
        // The decorative display lettering fills the poster; body text keeps its metrics.
        QTransform place;place.translate(area.left(),area.top());place.scale(area.width()/bounds.width(),area.height()/bounds.height());place.translate(-bounds.left(),-bounds.top());
        QPainterPath cut;cut.addRect(QRectF(area.left()-2,area.top()+area.height()*.60,area.width()+4,std::max(1.2,area.height()*.014)));
        Word word;word.area=area;const QFontMetricsF metrics(face);
        for(int i=0;i<copy.size();++i){if(copy[i].isSpace())continue;
            QPainterPath glyph;glyph.addText(QPointF(metrics.horizontalAdvance(copy.left(i+1))-metrics.horizontalAdvance(copy.mid(i,1)),0),face,copy.mid(i,1));
            word.glyphs.append(place.map(slant.map(glyph)).subtracted(cut));
        }
        return word;
    }
    static QPixmap outlineImage(const QVector<QPainterPath> &paths,QRectF bounds,qreal dpr,QColor color){
        QImage image(QSize(qCeil(bounds.width()*dpr),qCeil(bounds.height()*dpr)),QImage::Format_ARGB32_Premultiplied);image.fill(Qt::transparent);image.setDevicePixelRatio(dpr);
        {QPainter p(&image);Kda::configurePainter(p);p.translate(-bounds.topLeft());p.setBrush(Qt::NoBrush);p.setPen(QPen(color,.85));for(const auto &path:paths)p.drawPath(path);}
        return QPixmap::fromImage(image);
    }
    void prepareEchoes(Word &word,bool red){
        const qreal dpr=devicePixelRatioF();if(qFuzzyCompare(word.echoDpr,dpr)&&word.echoRed==red&&!word.echoes.isEmpty())return;
        word.echoes.clear();word.echoDpr=dpr;word.echoRed=red;
        for(const auto &glyph:word.glyphs){const auto bounds=glyph.boundingRect().adjusted(-2,-2,2,2);
            word.echoes.append({outlineImage({glyph},bounds,dpr,QColor(red?"#ffa377":"#9ef1ff")),outlineImage({glyph},bounds,dpr,QColor(red?"#df9cff":"#cf9cff")),bounds});
        }
        word.contourBounds=word.area.adjusted(-2,-2,2,2);word.contour=outlineImage(word.glyphs,word.contourBounds,dpr,QColor(red?"#ff93ba":"#bcbbff"));
    }
    void paintWord(QPainter &p,Word &word,int row,bool red,qreal t){
        prepareEchoes(word,red);
        const qreal kick=WaitingType::impact(t);const QColor solid(red?"#ff3d69":"#e9edff"),bg(red?"#100b10":"#0b0f16");
        const auto ink=spectrum(headlineBounds,red,t);const int bands=qFuzzyIsNull(kick)?1:3;
        const qreal bandOffsets[3]={-.2,1.,-.55};
        p.save();p.setClipRect(word.area.adjusted(-12,-3,12,3),Qt::IntersectClip);
        // Two true temporal samples create a graphic AE-style echo, without a
        // blur texture or an unbounded trail/history buffer.
        if(t<1.0||t>5.40){
            for(int sample=2;sample>=1;--sample)for(int i=0;i<word.glyphs.size();++i){
                const auto ghost=WaitingType::pose(std::max(qreal(0),t-sample*.055),i,word.glyphs.size(),row,word.area.height());
                if(ghost.opacity<.01)continue;const auto center=word.glyphs[i].boundingRect().center();
                p.save();p.setOpacity(ghost.opacity*(sample==1?.22:.10));p.translate(center+QPointF(ghost.x,ghost.y));p.scale(ghost.sx,ghost.sy);p.translate(-center);
                const auto &echo=word.echoes[i];p.drawPixmap(echo.bounds.topLeft(),sample==1?echo.first:echo.second);p.restore();
            }
        }
        const qreal rebound=1-WaitingType::smooth(WaitingType::progress(t,.82,.38));
        if(t>.82&&t<1.2)for(int repeat=3;repeat>=1;--repeat){
            p.save();p.setOpacity(rebound*(.16-repeat*.025));
            p.drawPixmap(word.contourBounds.topLeft()+QPointF((row?-1:1)*repeat*rebound*6,repeat*rebound*2),word.contour);p.restore();
        }
        for(int band=0;band<bands;++band){
            p.save();if(bands>1)p.setClipRect(QRectF(word.area.left()-12,word.area.top()+word.area.height()*band/3.,word.area.width()+24,word.area.height()/3.),Qt::IntersectClip);
            const qreal dx=bands>1?kick*bandOffsets[band]*word.area.height()*.15*(row?-1:1):0;
            for(int i=0;i<word.glyphs.size();++i){
                const auto pose=WaitingType::pose(t,i,word.glyphs.size(),row,word.area.height());if(pose.opacity<.001)continue;
                p.save();p.setOpacity(pose.opacity);const auto center=word.glyphs[i].boundingRect().center();
                p.translate(center+QPointF(pose.x+dx,pose.y));p.scale(pose.sx,pose.sy);p.translate(-center);
                if(bands>1&&std::abs(dx)>.5){
                    p.setPen(Qt::NoPen);auto a=QColor(red?"#ff946d":"#a9ecff");a.setAlpha(100);p.fillPath(word.glyphs[i].translated(-2.2,0),a);
                    auto b=QColor(red?"#d7b4fb":"#efb3dd");b.setAlpha(100);p.fillPath(word.glyphs[i].translated(2.2,0),b);
                }
                p.setPen(QPen(QBrush(ink),1.2,Qt::SolidLine,Qt::FlatCap,Qt::MiterJoin));p.setBrush(Qt::NoBrush);
                const qreal hit=WaitingType::smooth(WaitingType::progress(t,.83,.08))*(1-WaitingType::smooth(WaitingType::progress(t,.99,.12)));
                if(row){p.fillPath(word.glyphs[i],solid);if(hit>.01){p.save();p.setOpacity(p.opacity()*hit);p.fillPath(word.glyphs[i],bg);p.drawPath(word.glyphs[i]);p.restore();}}
                else{if(hit>.01){p.save();p.setOpacity(p.opacity()*hit);p.fillPath(word.glyphs[i],solid);p.restore();}p.drawPath(word.glyphs[i]);}
                if(t>2.65&&t<4.55){
                    const qreal sweep=WaitingType::progress(t,2.65,1.90);
                    const qreal x=word.area.left()+(-.35+sweep*1.7)*word.area.width(),w=word.area.width()*.22,h=word.area.height();
                    QPainterPath matte;matte.moveTo(x+h*.18,word.area.top()-1);matte.lineTo(x+w+h*.18,word.area.top()-1);matte.lineTo(x+w,word.area.bottom()+1);matte.lineTo(x,word.area.bottom()+1);matte.closeSubpath();
                    p.save();p.setClipPath(matte,Qt::IntersectClip);
                    p.fillPath(word.glyphs[i],row?bg:solid);p.drawPath(word.glyphs[i]);p.restore();
                }
                p.restore();
            }
            p.restore();
        }
        p.restore();
    }
    void prepareType(){
        if(typeSize==size())return;typeSize=size();
        const auto frame=readingFrame();const qreal cap=frame.height()*.265;
        ready=lettering(QStringLiteral("READY"),QRectF(frame.left(),frame.height()*.155,width()*.76,cap));
        const qreal secondLeft=width()*.235;
        play=lettering(QStringLiteral("TO PLAY"),QRectF(secondLeft,frame.height()*.43,width()-frame.left()-secondLeft,cap));
        headlineBounds=ready.area.united(play.area);
    }
    static void text(QPainter &p,QRect slot,const QString &copy,const QFont &font,QColor ink){
        p.setFont(font);p.setPen(ink);p.drawText(slot,Qt::AlignLeft|Qt::AlignVCenter,QFontMetrics(font).elidedText(copy,Qt::ElideRight,slot.width()));
    }
    WaitingType::Clock motion;bool connected=false;qreal inspectionTime=-1;
    Kda::HomeMaterialDetail::CoverCache stageBackdrop{Kda::HomeMaterialDetail::backdropAssets(),0};
    Word ready,play;QRectF headlineBounds;QSize typeSize;
};
