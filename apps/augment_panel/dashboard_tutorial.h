#pragma once

#include "dashboard_tutorial_diagram.h"
#include "dashboard_window_frame.h"
#include "kda_controls.h"
#include <QApplication>
#include <QCloseEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QShortcut>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <functional>

class DashboardTutorial final : public QWidget {
public:
    explicit DashboardTutorial(QWidget *host):QWidget(host){
        setObjectName(QStringLiteral("dashboardTutorial"));
        setAccessibleName(QStringLiteral("李金金 · 快速上手"));
        setProperty("dashboardDragBlock",true);
        setFont(Kda::font(Kda::FontRole::Body,13));
        hide();
        host->installEventFilter(this);

        auto shell=new QVBoxLayout(this);
        auto lesson=new QWidget;
        lesson->setObjectName(QStringLiteral("tutorialLesson"));
        Kda::markSharedGlassSurface(lesson);
        shell->addWidget(lesson);

        auto surfaceLayout=new QVBoxLayout(lesson);
        surfaceLayout->setContentsMargins(0,0,0,0);
        content=new QWidget;
        surfaceLayout->addWidget(content,0,Qt::AlignCenter);
        auto column=new QVBoxLayout(content);
        column->setContentsMargins(0,0,0,0);
        column->setSpacing(14);
        auto header=new QHBoxLayout;
        header->setContentsMargins(0,0,0,0);
        header->setSpacing(14);
        header->addWidget(new Masthead);
        header->addWidget(rule(),1);
        progress=new ProgressRail;
        progress->setFixedSize(168,18);
        progress->setAccessibleName(QStringLiteral("教程进度"));
        header->addWidget(progress);
        closeButton=action(QStringLiteral("跳过"),"closeDashboardTutorial");
        closeButton->setFixedSize(60,32);
        closeButton->setProperty("toolbarAction",false);
        closeButton->setAccessibleName(QStringLiteral("跳过教程"));
        closeButton->setToolTip(QStringLiteral("跳过教程 · Esc"));
        header->addWidget(closeButton);
        column->addLayout(header);

        auto stageRow=new QWidget;
        stageRow->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Expanding);
        auto scene=new QHBoxLayout(stageRow);
        scene->setContentsMargins(0,0,0,0);
        scene->setSpacing(22);
        intro=new QWidget;
        auto story=new QVBoxLayout(intro);
        story->setContentsMargins(0,0,0,0);
        story->setSpacing(10);
        counter=label("index");
        counter->setObjectName(QStringLiteral("tutorialStepCounter"));
        counter->setFont(Kda::font(Kda::FontRole::Numeric,13,QFont::DemiBold));
        story->addWidget(counter);
        stageTitle=label("title");
        stageTitle->setObjectName(QStringLiteral("tutorialStageTitle"));
        story->addWidget(stageTitle);
        dialogue=new Dialogue;
        story->addWidget(dialogue);
        shortcut=label("shortcut",QStringLiteral("按住 Ctrl + Alt\nF7 归位 · F8 显隐 · F9 鼠标穿透"));
        shortcut->setWordWrap(true);
        story->addWidget(shortcut);
        scene->addWidget(intro,0,Qt::AlignVCenter);

        diagram=new DashboardTutorialDiagram;
        scene->addWidget(diagram,1,Qt::AlignVCenter);
        column->addWidget(stageRow,1);
        column->addWidget(rule());

        auto footer=new QHBoxLayout;
        footer->setSpacing(10);
        previous=action(QStringLiteral("上一步"),"tutorialPrevious");
        previous->setSymbol(HudSymbols::Symbol::Back);
        previous->setMinimumHeight(34);
        footer->addWidget(previous);
        hud=action(QStringLiteral("HUD 管理"),"tutorialOpenHudEditor");
        preview=action(QStringLiteral("界面浏览"),"tutorialOpenPreview");
        preview->setToolTip(QStringLiteral("预览载入、出装、选牌和阵亡浮层"));
        footer->addWidget(hud);
        footer->addWidget(preview);
        footer->addStretch(1);
        next=action(QStringLiteral("下一步"),"tutorialNext");
        next->setMinimumSize(116,34);
        next->setAutoDefault(true);
        next->setProperty("toolbarAction",false);
        next->setProperty("primaryAction",true);
        footer->addWidget(next);
        column->addLayout(footer);

        connect(closeButton,&QPushButton::clicked,this,&QWidget::close);
        connect(previous,&QPushButton::clicked,this,[this]{setStage(stage-1);});
        connect(next,&QPushButton::clicked,this,[this]{
            if(stage==stageCount-1){finished=true;close();}
            else setStage(stage+1);
        });
        connect(hud,&QPushButton::clicked,this,[this]{if(onOpenHudEditor)onOpenHudEditor();});
        connect(preview,&QPushButton::clicked,this,[this]{if(onOpenPreview)onOpenPreview();});
        auto escape=new QShortcut(QKeySequence(Qt::Key_Escape),this);
        escape->setContext(Qt::WidgetWithChildrenShortcut);
        connect(escape,&QShortcut::activated,this,&QWidget::close);
        auto goBack=new QShortcut(QKeySequence(Qt::Key_Left),this);
        auto goNext=new QShortcut(QKeySequence(Qt::Key_Right),this);
        for(auto key:{goBack,goNext})key->setContext(Qt::WidgetWithChildrenShortcut);
        connect(goBack,&QShortcut::activated,this,[this]{setStage(stage-1);});
        connect(goNext,&QShortcut::activated,this,[this]{setStage(stage+1);});

        Kda::bindThemeStyle(this,[]{
            return QStringLiteral(
                "QLabel[tutorialRole]{background:transparent;border:0;color:%1;}"
                "QLabel[tutorialRole=\"title\"]{font-size:32px;font-weight:600;}"
                "QLabel[tutorialRole=\"index\"]{color:%2;border-left:2px solid %2;padding-left:9px;font-size:13px;font-weight:600;}"
                "QLabel[tutorialRole=\"speaker\"]{color:%5;font-size:12px;font-weight:600;}"
                "QLabel[tutorialRole=\"body\"]{color:%3;font-size:14px;font-weight:500;}"
                "QLabel[tutorialRole=\"shortcut\"]{color:%3;font-size:11px;font-weight:500;}"
                "QFrame#tutorialRule{border:0;background:qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 %4,stop:0.72 %4,stop:1 transparent);}"
            ).arg(Kda::color("text").name(),Kda::color("ice").name(),Kda::color("silver").name(),
                  Kda::color("border").name(),Kda::color(Kda::skin()==QStringLiteral("kda_red")?"text":"bg").name());
        });
        setStage(0);
        reposition();
    }

    void present(){
        if(finished){finished=false;setStage(0);}
        reposition();
        raise();
        show();
        dialogue->pulseAccent();
        next->setFocus(Qt::OtherFocusReason);
    }

    void reposition(){
        setGeometry(parentWidget()->rect());
        const int horizontal=std::clamp(width()/22,26,64);
        const int vertical=std::clamp(height()/28,16,30);
        layout()->setContentsMargins(horizontal,vertical,horizontal,vertical);
        content->setFixedSize(std::clamp(width()-2*horizontal,1,1360),
                              std::clamp(height()-2*vertical,1,740));
        intro->setFixedWidth(std::clamp(width()*27/100,200,320));
        diagram->setFixedHeight(std::clamp(width()*34/100,210,std::clamp(height()-155,210,430)));
    }

    std::function<void()> onDismiss,onOpenHudEditor,onOpenPreview;

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        const qreal radius=Kda::radius("workspace");
        DashboardFrame::paint(painter,size(),devicePixelRatioF(),radius,Kda::color("bg"),Kda::color("border"));
        Kda::configurePainter(painter);
        QPainterPath surface;
        surface.addRoundedRect(QRectF(rect()).adjusted(1,1,-1,-1),radius,radius);
        if(Kda::highContrastEnabled())return;
        painter.setClipPath(surface);
        const auto area=surface.boundingRect();
        const auto &art=backdrop();
        if(art.isNull()){
            Kda::paintFacetedMarble(painter,area,.62);
            return;
        }
        const qreal scale=std::max(area.width()/art.width(),area.height()/art.height());
        const QSizeF sourceSize=area.size()/scale;
        const QRectF source(QPointF((art.width()-sourceSize.width())*.72,
                                   (art.height()-sourceSize.height())*.5),sourceSize);
        painter.drawPixmap(area,art,source);
        QLinearGradient veil(area.topLeft(),area.bottomLeft());
        veil.setColorAt(0,QColor(0,0,0,78));
        veil.setColorAt(.29,QColor(0,0,0,13));
        veil.setColorAt(.7,QColor(0,0,0,12));
        veil.setColorAt(1,QColor(0,0,0,110));
        painter.fillRect(area,veil);
    }

    bool eventFilter(QObject *watched,QEvent *event) override {
        if(watched==parentWidget()&&event->type()==QEvent::Resize)reposition();
        return QWidget::eventFilter(watched,event);
    }

    bool event(QEvent *event) override {
        switch(event->type()){
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
        case QEvent::Wheel:
            event->accept();
            return true;
        default:
            return QWidget::event(event);
        }
    }

    void closeEvent(QCloseEvent *event) override {
        const bool wasVisible=isVisible();
        event->accept();
        hide();
        if(wasVisible&&onDismiss)onDismiss();
    }

    bool focusNextPrevChild(bool forward) override {
        if(!isVisible())return QWidget::focusNextPrevChild(forward);
        const std::array controls{closeButton,previous,hud,preview,next};
        const auto focused=std::find(controls.begin(),controls.end(),QApplication::focusWidget());
        const int count=int(controls.size());
        int index=focused==controls.end()?(forward?count-1:0):int(focused-controls.begin());
        for(int step=0;step<count;++step){
            index=(index+(forward?1:count-1))%count;
            auto control=controls[index];
            if(control->isVisible()&&control->isEnabled()){
                control->setFocus(forward?Qt::TabFocusReason:Qt::BacktabFocusReason);
                return true;
            }
        }
        return true;
    }

private:
    static constexpr int stageCount=5;

    class Dialogue final : public QWidget {
    public:
        Dialogue():accent(this,240){
            setObjectName(QStringLiteral("tutorialDialogue"));
            auto column=new QVBoxLayout(this);
            column->setContentsMargins(13,0,13,12);
            column->setSpacing(8);
            speaker=label("speaker");
            speaker->setObjectName(QStringLiteral("tutorialSpeaker"));
            speaker->setContentsMargins(11,0,0,0);
            speaker->setFixedHeight(25);
            column->addWidget(speaker,0,Qt::AlignLeft);
            body=label("body");
            body->setObjectName(QStringLiteral("tutorialStageInstructions"));
            body->setWordWrap(true);
            body->setAlignment(Qt::AlignLeft|Qt::AlignTop);
            column->addWidget(body);
        }

        void setCopy(const QString &copy,bool opening){
            if(greeting==opening&&body->text()==copy)return;
            greeting=opening;
            speaker->setText(greeting?QStringLiteral("Ciallo～"):QStringLiteral("李金金"));
            speaker->setFixedWidth(greeting?102:82);
            body->setText(copy);
            pulseAccent();
        }

        void pulseAccent(){
            accent.finish();
            if(!Kda::highContrastEnabled())accent.trigger();
        }

    protected:
        void paintEvent(QPaintEvent *) override {
            QPainter painter(this);
            Kda::configurePainter(painter);
            const QRectF plate=QRectF(rect()).adjusted(.5,speaker->height()/2.+.5,-.5,-.5);
            const qreal radius=Kda::radius("section");
            QPainterPath shape;
            shape.addRoundedRect(plate,radius,radius);
            const bool highContrast=Kda::highContrastEnabled();
            auto upper=Kda::mixColor(Kda::color("raised"),Kda::color("lilac"),greeting?.15:.08);
            auto lower=Kda::color("bg");
            if(!highContrast){
                upper.setAlpha(226);
                lower.setAlpha(239);
            }
            QLinearGradient lacquer(plate.topLeft(),plate.bottomRight());
            lacquer.setColorAt(0,upper);
            lacquer.setColorAt(1,lower);
            painter.fillPath(shape,lacquer);
            Kda::paintPolishedEdge(painter,shape,plate,radius);

            const auto badge=QRectF(speaker->geometry()).adjusted(.5,.5,-.5,-.5);
            const auto tag=Kda::facet(badge,6);
            const bool red=Kda::skin()==QStringLiteral("kda_red");
            const auto silver=Kda::color("silver");
            const auto accentColor=Kda::color(red?"ice":"lilac");

            // The cut corner continues under the nameplate like a folded ribbon.
            if(!highContrast){
                const qreal x=badge.left(),y=badge.bottom();
                QPainterPath fold;
                fold.moveTo(x,y-6);
                fold.lineTo(x+8,y);
                fold.lineTo(x+3,y+5);
                fold.closeSubpath();
                painter.fillPath(fold,Kda::mixColor(Kda::color("raised"),accentColor,.57));
            }
            QLinearGradient foil(badge.topLeft(),badge.bottomRight());
            foil.setColorAt(0,red?Kda::color("raised").lighter(150):silver.lighter(109));
            foil.setColorAt(1,Kda::mixColor(red?Kda::color("raised"):silver,Kda::color("lilac"),.39));
            painter.fillPath(tag,foil);
            painter.setPen(QPen(Kda::color(red?"border":"silver"),.75));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(tag);
            const qreal starSize=greeting?13.:9.;
            const QRectF star(badge.right()-starSize-7,badge.center().y()-starSize/2,starSize,starSize);
            HudSymbols::draw(painter,HudSymbols::Symbol::Star,star,Kda::color(red?"text":"bg"),1.25);

            if(!highContrast){
                const qreal phase=accent.progress();
                const qreal offset=accent.running()?phase*4:4;
                const QRectF gleam(badge.right()+4+offset,badge.top()+2,6,6);
                auto ink=accentColor;
                ink.setAlphaF(accent.running()?.85-.45*phase:.4);
                HudSymbols::draw(painter,HudSymbols::Symbol::Star,gleam,ink,2.0);
                if(greeting){
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(ink);
                    painter.drawEllipse(QPointF(gleam.right()+3,gleam.bottom()+3),1.,1.);
                }
            }
        }

    private:
        QLabel *speaker=nullptr,*body=nullptr;
        Kda::Transition accent;
        bool greeting=false;
    };

    const QPixmap &backdrop() const {
        if(Kda::skin()==QStringLiteral("kda_red")){
            static const QPixmap fire(QStringLiteral(":/lixin/kda/tutorial/stage-firestarter-v1.jpg"));
            return fire;
        }
        if(stage<2){
            static const QPixmap gate(QStringLiteral(":/lixin/kda/tutorial/stage-gate-v1.jpg"));
            return gate;
        }
        static const QPixmap mirrors(QStringLiteral(":/lixin/kda/tutorial/stage-mirrors-v1.jpg"));
        return mirrors;
    }

    class Masthead final : public QLabel {
    public:
        Masthead():QLabel(QStringLiteral("快速上手")){
            setFixedSize(172,34);
            setIndent(14);
            setAlignment(Qt::AlignLeft|Qt::AlignVCenter);
        }
    protected:
        void paintEvent(QPaintEvent *) override {
            QPainter painter(this);
            Kda::configurePainter(painter);
            const QRectF bounds=QRectF(rect()).adjusted(.5,.5,-.5,-.5);
            const auto shape=Kda::facet(bounds,13);
            const bool red=Kda::skin()==QStringLiteral("kda_red");
            const auto silver=Kda::color("silver");
            QLinearGradient metal(bounds.topLeft(),bounds.bottomRight());
            metal.setColorAt(0,red?Kda::color("raised").lighter(140):silver.lighter(109));
            metal.setColorAt(1,red?Kda::color("surface"):Kda::mixColor(silver,Kda::color("lilac"),.45));
            painter.fillPath(shape,metal);

            painter.save();
            painter.setClipPath(shape);
            const QRectF wing=bounds.adjusted(bounds.width()*.74,0,0,0);
            QPainterPath facet;
            facet.moveTo(wing.left()+17,wing.top());
            facet.lineTo(wing.topRight());
            facet.lineTo(wing.bottomRight());
            facet.lineTo(wing.bottomLeft());
            facet.closeSubpath();
            painter.setClipPath(facet,Qt::IntersectClip);
            Kda::paintFoil(painter,wing,.72);
            painter.restore();

            painter.setPen(QPen(red?Kda::color("border"):silver,.75));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(shape);
            painter.setFont(Kda::font(Kda::FontRole::Title,17,QFont::DemiBold));
            painter.setPen(red?Kda::color("text"):Kda::color("bg"));
            painter.drawText(rect().adjusted(indent(),0,0,0),alignment(),text());
        }
    };

    class ProgressRail final : public QWidget {
    public:
        ProgressRail():sheen(this,260){}
        void setCurrent(int value){
            if(current==value)return;
            current=value;
            setAccessibleDescription(QStringLiteral("第 %1 步，共 %2 步").arg(current+1).arg(stageCount));
            sheen.finish();
            sheen.trigger();
        }
    protected:
        void paintEvent(QPaintEvent *) override {
            QPainter painter(this);
            Kda::configurePainter(painter);
            constexpr int gap=6;
            constexpr int inset=7;
            const qreal segmentWidth=(width()-2*inset-gap*(stageCount-1))/qreal(stageCount);
            auto glass=Kda::color("bg");
            if(!Kda::highContrastEnabled())glass.setAlpha(190);
            painter.setPen(Qt::NoPen);
            painter.setBrush(glass);
            painter.drawRoundedRect(QRectF(rect()),Kda::radius("control"),Kda::radius("control"));
            for(int i=0;i<stageCount;++i){
                const qreal thickness=i==current?4.:2.;
                const QRectF segment(inset+i*(segmentWidth+gap),(height()-thickness)/2,segmentWidth,thickness);
                QPainterPath shape;
                shape.moveTo(segment.topLeft()+QPointF(2,0));
                shape.lineTo(segment.topRight());
                shape.lineTo(segment.bottomRight()-QPointF(2,0));
                shape.lineTo(segment.bottomLeft());
                shape.closeSubpath();
                if(i!=current){
                    auto ink=Kda::color(i<current?"silver":"border");
                    if(i<current)ink.setAlpha(150);
                    painter.fillPath(shape,ink);
                    continue;
                }
                QLinearGradient metal(segment.topLeft(),segment.topRight());
                metal.setColorAt(0,Kda::color("lilac"));
                metal.setColorAt(.55,Kda::color("silver"));
                metal.setColorAt(1,Kda::color("ice"));
                painter.fillPath(shape,metal);
                if(sheen.running()){
                    const qreal x=segment.left()-5+sheen.progress()*(segmentWidth+10);
                    QLinearGradient light(x-5,0,x+5,0);
                    light.setColorAt(0,Qt::transparent);
                    light.setColorAt(.5,QColor(255,255,255,165));
                    light.setColorAt(1,Qt::transparent);
                    painter.fillPath(shape,light);
                }
            }
        }
    private:
        Kda::Transition sheen;
        int current=-1;
    };

    struct Step {const char *title,*copy;};
    static constexpr Step steps[stageCount]{
        {"连接客户端","先看底栏。亮起「客户端已连接」，就能进海克斯大乱斗啦。"},
        {"选人与载入","先看看队友和可换英雄。进载入画面，再看阵容浮层。"},
        {"海克斯与装备","选牌时看看海克斯建议。进对局后，装备推荐也会跟上。"},
        {"阵亡与复活","阵亡时可以看看本局战况。复活后，详细对照会自动收起来。"},
        {"调整浮层","拖卡片挪位置，拉边框改大小。调顺手了，点「保存并退出」。"}
    };

    static QLabel *label(const char *role,const QString &copy={}){
        auto result=new QLabel(copy);
        result->setProperty("tutorialRole",QString::fromLatin1(role));
        result->setTextFormat(Qt::PlainText);
        return result;
    }
    static Kda::Button *action(const QString &text,const char *name){
        auto result=new Kda::Button(text);
        result->setObjectName(QString::fromLatin1(name));
        result->setProperty("toolbarAction",true);
        result->setProperty("labelPixels",12);
        return result;
    }
    static QFrame *rule(){
        auto line=new QFrame;
        line->setObjectName(QStringLiteral("tutorialRule"));
        line->setFixedHeight(1);
        return line;
    }
    void setStage(int index){
        const bool wasInMatch=stage>=2;
        stage=std::clamp(index,0,stageCount-1);
        if(wasInMatch!=(stage>=2))update();
        setProperty("tutorialStage",stage);
        const auto &step=steps[stage];
        stageTitle->setText(QString::fromUtf8(step.title));
        counter->setText(QStringLiteral("%1 / %2").arg(stage+1,2,10,QLatin1Char('0')).arg(stageCount,2,10,QLatin1Char('0')));
        dialogue->setCopy(QString::fromUtf8(step.copy),stage==0);
        diagram->setStage(stage);
        progress->setCurrent(stage);
        const bool last=stage==stageCount-1;
        shortcut->setVisible(last);
        hud->setVisible(last);
        preview->setVisible(last);
        next->setText(last?QStringLiteral("完成"):QStringLiteral("下一步"));
        previous->setEnabled(stage>0);
    }
    DashboardTutorialDiagram *diagram=nullptr;
    ProgressRail *progress=nullptr;
    Dialogue *dialogue=nullptr;
    QWidget *content=nullptr,*intro=nullptr;
    QLabel *stageTitle=nullptr,*counter=nullptr,*shortcut=nullptr;
    Kda::Button *closeButton=nullptr,*previous=nullptr,*next=nullptr,*hud=nullptr,*preview=nullptr;
    int stage=0;
    bool finished=false;
};
