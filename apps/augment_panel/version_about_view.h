#pragma once

#include "app_version.h"
#include "kda_theme.h"
#include "kda_reading_surface.h"
#include "baddest_about_paint.h"
#include <QApplication>
#include <QDynamicPropertyChangeEvent>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QShowEvent>
#include <QVBoxLayout>
#include <QWidget>
#include <QVariantAnimation>

namespace VersionAbout {
class Portrait final : public QWidget {
public:
    Portrait(QString resource,const QString &name,QWidget *parent=nullptr)
        :QWidget(parent),source(std::move(resource)){
        setFixedSize(168,252);
        setAccessibleName(name);
        setToolTip(name);
    }
    void setEntrance(qreal milliseconds,bool running,int index){
        const bool animated=running&&milliseconds<530.;
        const auto next=std::max(milliseconds,index*52.);
        if(active==animated&&(!animated||(time==next&&ordinal==index)))return;
        if(!animated)animationFrame={};
        time=next;active=animated;ordinal=index;update();
    }
protected:
    bool event(QEvent *event)override{
        if(event->type()==QEvent::DevicePixelRatioChange){artworkDpr=0;update();}
        return QWidget::event(event);
    }
    void showEvent(QShowEvent *event)override{
        QWidget::showEvent(event);
        ensureArtwork();
        update();
    }
    void paintEvent(QPaintEvent *)override{
        ensureArtwork();
        QPainter painter(this);Kda::configurePainter(painter);
        if(active&&!artwork.isNull()){
            if(animationFrame.size()!=artworkPixels||animationFrame.devicePixelRatio()!=artworkDpr)animationFrame=surface();
            else animationFrame.fill(QColor(15,14,23));
            {QPainter framePainter(&animationFrame);Kda::configurePainter(framePainter);
                paintPortraitEntrance(framePainter,artwork,QRectF(rect()),time,ordinal);}
            applyMask(animationFrame);
            painter.drawImage(QPointF(),animationFrame);
        }else{
            painter.drawPixmap(QPointF(),roundedArtwork);
        }
    }
private:
    QImage surface()const{
        QImage image(artworkPixels,QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(artworkDpr);image.fill(QColor(15,14,23));return image;
    }
    void applyMask(QImage &image)const{
        image.setDevicePixelRatio(1.);
        {QPainter painter(&image);painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);painter.drawImage(0,0,alphaMask);}
        image.setDevicePixelRatio(artworkDpr);
    }
    void ensureArtwork(){
        const qreal dpr=devicePixelRatioF();
        const QSize pixels(std::max(1,qRound(width()*dpr)),std::max(1,qRound(height()*dpr)));
        if(artworkPixels==pixels&&qFuzzyCompare(artworkDpr,dpr))return;
        if(!sourceRead){QImageReader reader(source);original=reader.read();sourceRead=true;}
        artworkDpr=dpr;artworkPixels=pixels;
        // Keep the source so moving to a denser screen never enlarges an old,
        // low-resolution decode. The frame and its alpha mask share this DPR.
        artwork=QPixmap::fromImage(original.scaled(pixels,Qt::IgnoreAspectRatio,Qt::SmoothTransformation));
        QPixmap white(1,1);white.fill(Qt::white);
        alphaMask=Kda::roundedPixmap(white,size(),3,dpr).toImage();alphaMask.setDevicePixelRatio(1.);
        auto frame=surface();
        if(!artwork.isNull()){QPainter framePainter(&frame);Kda::configurePainter(framePainter);framePainter.drawPixmap(rect(),artwork);}
        applyMask(frame);
        roundedArtwork=QPixmap::fromImage(std::move(frame));
        setProperty("artworkLoaded",!artwork.isNull());
        setProperty("artworkDevicePixelRatio",dpr);setProperty("artworkPixelSize",pixels);
        setProperty("artworkCacheBuilds",++cacheBuilds);
    }
    QString source;
    QImage original,alphaMask,animationFrame;
    QPixmap artwork,roundedArtwork;
    QSize artworkPixels;
    qreal artworkDpr=0;
    bool sourceRead=false;
    int cacheBuilds=0;
    qreal time=EntranceDuration;bool active=false;int ordinal=0;
};

class CoffeeMark final : public QWidget {
public:
    CoffeeMark(){setFixedSize(28,30);setAttribute(Qt::WA_TransparentForMouseEvents);}
protected:
    void paintEvent(QPaintEvent *)override{
        QPainter painter(this);Kda::configurePainter(painter);
        painter.setPen(QPen(Kda::color("silver"),1.2,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        QPainterPath cup;cup.moveTo(4,12);cup.lineTo(19,12);cup.lineTo(18,21);cup.quadTo(17.5,24,14,24);cup.lineTo(9,24);cup.quadTo(5.5,24,5,21);cup.closeSubpath();painter.drawPath(cup);
        QPainterPath handle;handle.moveTo(19,14);handle.cubicTo(27,12,26,22,19,21);painter.drawPath(handle);
        painter.drawLine(QPointF(3,27),QPointF(22,27));
        QPainterPath steam;steam.moveTo(9,8);steam.cubicTo(5,5,12,4,9,1);steam.moveTo(15,8);steam.cubicTo(11,5,18,4,15,1);painter.drawPath(steam);
    }
};

class View final : public QWidget {
public:
    View():entrance(this){
        setObjectName(QStringLiteral("versionAboutPage"));setMinimumWidth(0);Kda::markSharedGlassSurface(this);
        auto root=new QVBoxLayout(this);
        constexpr int inset=Kda::Density::pageInset;
        root->setContentsMargins(inset,inset,inset,inset);root->setSpacing(0);
        auto heading=new QWidget;heading->setObjectName(QStringLiteral("aboutHeading"));
        auto masthead=new QHBoxLayout(heading);masthead->setContentsMargins(0,0,0,0);masthead->setSpacing(12);
        auto brand=label("aboutTitle",QString());
        brand->setAccessibleName(QStringLiteral("李金金 · 金克丝组合标识"));brand->setFixedSize(164,45);
        brand->setPixmap(QPixmap(QStringLiteral(":/lixin/kda/brand/lijinjin-wordmark.png")));
        brand->setScaledContents(true);masthead->addWidget(brand);
        masthead->addStretch(1);
        auto version=label("settingsVersion",QStringLiteral("v%1").arg(QString::fromLatin1(LIXIN_APP_VERSION)));
        version->setAccessibleName(QStringLiteral("版本 %1").arg(version->text()));version->setTextInteractionFlags(Qt::TextSelectableByMouse);
        masthead->addWidget(version,0,Qt::AlignVCenter);

        auto body=new QHBoxLayout;body->setContentsMargins(0,0,0,0);body->setSpacing(2*Kda::Density::sectionGap);
        auto portraits=new QWidget;portraits->setObjectName(QStringLiteral("aboutPortraits"));
        auto duo=new QHBoxLayout(portraits);duo->setContentsMargins(0,0,0,0);duo->setSpacing(6);
        const auto portrait=[&](const char *object,const QString &file,const QString &accessible){
            auto image=new Portrait(QStringLiteral(":/lixin/kda/illustrations/")+file,accessible);image->setObjectName(QString::fromLatin1(object));
            duo->addWidget(image);portraitsToReveal.append(image);
        };
        portrait("aboutSoraka","soraka-star-guardian-black-evening-halfbody-v3.png",QStringLiteral("星之守护者索拉卡"));
        portrait("aboutCodex","codex-the-baddest-v1.png",QStringLiteral("Codex 原创虚拟舞台形象"));
        body->addWidget(section(portraits,20,1),0,Qt::AlignTop);

        auto author=new QWidget;author->setObjectName(QStringLiteral("aboutAuthor"));author->setMinimumWidth(0);
        auto copy=new QVBoxLayout(author);copy->setContentsMargins(0,1,0,0);copy->setSpacing(7);
        copy->addWidget(label("aboutEyebrow",QStringLiteral("作者")));
        authorName=new Name(QStringLiteral("RAIONNYAN"));authorName->setObjectName(QStringLiteral("aboutAuthorName"));copy->addWidget(authorName);
        auto bio=label("aboutBody",QStringLiteral("喜欢游戏，也在意好用和好看。\n把想法一点点做成李金金。"));
        bio->setWordWrap(true);bio->setTextInteractionFlags(Qt::TextSelectableByMouse);copy->addWidget(bio);
        copy->addWidget(label("aboutCredit",QStringLiteral("与 Codex 共创")));
        copy->addStretch(1);
        body->addWidget(section(author,118,-1),1);root->addLayout(body);root->addStretch(1);
        root->addSpacing(2*Kda::Density::sectionGap);

        auto coffee=new QWidget;coffee->setObjectName(QStringLiteral("aboutCoffee"));
        auto coffeeRow=new QHBoxLayout(coffee);coffeeRow->setContentsMargins(0,12,0,0);coffeeRow->setSpacing(12);coffeeRow->addWidget(new CoffeeMark);
        auto coffeeCopy=new QVBoxLayout;coffeeCopy->setContentsMargins(0,0,0,0);coffeeCopy->setSpacing(4);
        coffeeCopy->addWidget(label("aboutCoffeeTitle",QStringLiteral("Raionnyan")));
        coffeeRow->addLayout(coffeeCopy,1);
        auto coffeeContact=label("aboutCoffeeContact",QStringLiteral("QQ：1157158067"));
        coffeeContact->setTextInteractionFlags(Qt::TextSelectableByMouse);
        coffeeRow->addWidget(coffeeContact,0,Qt::AlignVCenter);root->addWidget(section(heading,258,1));
        root->addSpacing(2*Kda::Density::sectionGap);root->addWidget(section(coffee,316,1));
        strike=new PanelStrike(this);strike->setObjectName(QStringLiteral("aboutPanelStrike"));
        Kda::bindThemeStyle(this,[]{return Kda::themedStyleSheet(QStringLiteral(R"(
            QLabel#settingsVersion{font-family:'Inter';font-size:12px;color:#bcc5d7;padding-bottom:3px;}
            QLabel#aboutEyebrow{font-size:10px;color:#939ba9;}
            QLabel#aboutAuthorName{font-family:'Barlow Semi Condensed','Inter';font-size:23px;font-weight:600;color:#eceef3;}
            QLabel#aboutBody{font-size:12px;color:#c4c9d3;}
            QLabel#aboutCredit{font-size:11px;color:#939ba9;}
            QLabel#aboutCoffeeTitle{font-size:13px;font-weight:500;color:#e7e9ef;}
            QLabel#aboutCoffeeContact{font-size:12px;color:#bcc5d7;}
            QWidget#aboutCoffee{background:transparent;border:0;border-top:1px solid #343b49;}
        )"));});
        entrance.setDuration(EntranceDuration);entrance.setStartValue(0.);entrance.setEndValue(qreal(EntranceDuration));entrance.setEasingCurve(QEasingCurve::Linear);
        connect(&entrance,&QVariantAnimation::valueChanged,this,[this](const QVariant &value){time=value.toReal();++frames;revealFrame(true);});
        connect(&entrance,&QVariantAnimation::finished,this,[this]{finishEntrance();});
        if(qApp)qApp->installEventFilter(this);
    }
    ~View()override{if(qApp)qApp->removeEventFilter(this);}
    void playEntrance(){
        finishEntrance();if(!isVisible()||Kda::reducedMotion())return;
        time=0;++starts;revealFrame(true);entrance.start();
    }
    void finishEntrance(){entrance.stop();time=EntranceDuration;revealFrame(false);}
    void setInspectionTime(qreal milliseconds){entrance.stop();time=std::clamp(milliseconds,qreal(0),qreal(EntranceDuration));revealFrame(time<EntranceDuration);}
    QJsonObject entranceReport()const{
        const bool running=entrance.state()==QAbstractAnimation::Running;
        bool restored=true;for(auto slot:sections)restored&=slot->restored();
        return {{"effect","baddest_panel_three_cut_entrance"},{"running",running},{"duration_ms",EntranceDuration},{"progress",time/EntranceDuration},
            {"stage",!running?"settled":time<152?"first_cut":time<282?"second_cut":time<390?"third_cut":"settle"},
            {"starts",starts},{"frames",frames},{"loops",1},{"portrait_count",portraitsToReveal.size()},{"section_count",sections.size()},
            {"panel_wide",true},{"section_geometry_restored",restored},{"strike_visible",strike&&strike->isVisible()},
            {"background_animated",false},{"snapshot_count",0}};
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        Kda::configurePainter(painter);
        Kda::paintReadingSurface(painter, QRectF(rect()), this);
    }
    void hideEvent(QHideEvent *event)override{finishEntrance();QWidget::hideEvent(event);}
    void resizeEvent(QResizeEvent *event)override{QWidget::resizeEvent(event);revealFrame(entrance.state()==QAbstractAnimation::Running);}
    bool eventFilter(QObject *object,QEvent *event)override{
        if(event->type()==QEvent::MouseButtonPress){auto widget=qobject_cast<QWidget*>(object);if(widget&&(widget==this||isAncestorOf(widget)))finishEntrance();}
        if(object==qApp&&event->type()==QEvent::ApplicationDeactivate)finishEntrance();
        if(object==qApp&&event->type()==QEvent::DynamicPropertyChange&&static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName()=="lixinReducedMotion"&&Kda::reducedMotion())finishEntrance();
        return QWidget::eventFilter(object,event);
    }
private:
    EntranceSlot *section(QWidget *content,qreal delay,int direction){auto slot=new EntranceSlot(content,delay,direction);sections.append(slot);return slot;}
    void revealFrame(bool running){
        for(int i=0;i<portraitsToReveal.size();++i)portraitsToReveal[i]->setEntrance(time,running,i);
        if(authorName)authorName->setEntrance(time,running);for(auto slot:sections)slot->setEntrance(time,running);
        if(strike){if(strike->geometry()!=rect())strike->setGeometry(rect());strike->setEntrance(time,running);}
    }
    static QLabel *label(const char *name,const QString &text){
        auto value=new QLabel(text);value->setObjectName(QString::fromLatin1(name));value->setTextFormat(Qt::PlainText);value->setMinimumWidth(0);return value;
    }
    QVariantAnimation entrance;QList<Portrait*> portraitsToReveal;QList<EntranceSlot*> sections;Name *authorName=nullptr;PanelStrike *strike=nullptr;
    qreal time=EntranceDuration;int starts=0,frames=0;
};
}
