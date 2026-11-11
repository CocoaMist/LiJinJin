#pragma once
#include "kda_theme.h"
#include <QApplication>
#include <QCursor>
#include <QDynamicPropertyChangeEvent>
#include <QLabel>
#include <QPalette>
#include <QScreen>
#include <QToolTip>
#include <QTextDocument>
#include <algorithm>

namespace Kda::Detail {
inline QString toolTipSheet(){
    const bool red=skin()==QStringLiteral("kda_red");
    return QStringLiteral("QToolTip,QLabel#qtooltip_label{color:%1;"
        "background:qlineargradient(x1:0,y1:0,x2:0.15,y2:1,stop:0 %2,stop:0.18 %3,stop:1 %4);"
        "border:1px solid %5;border-top-color:%6;border-radius:7px;padding:11px 14px;"
        "font-family:'Inter','Noto Sans SC','Segoe UI';font-size:13px;font-weight:400;}")
        .arg(color("text").name(),red?QStringLiteral("#392129"):QStringLiteral("#30303d"),
             red?QStringLiteral("#24171e"):QStringLiteral("#22232f"),red?QStringLiteral("#170f15"):QStringLiteral("#171922"),
             red?QStringLiteral("#724450"):QStringLiteral("#73798e"),red?QStringLiteral("#a87268"):QStringLiteral("#b8c1d3"));
}
class ToolTipStyle final:public QObject {
public:
    explicit ToolTipStyle(QObject *parent):QObject(parent){qApp->installEventFilter(this);defaults();qApp->setProperty("kdaToolTipStyleInstalled",true);}
private:
    bool adapting=false;
    void defaults(){
        QToolTip::setFont(font(FontRole::Body,13));auto palette=QToolTip::palette();
        const auto base=skin()==QStringLiteral("kda_red")?QColor("#24171e"):QColor("#22232f");
        for(const auto group:{QPalette::Active,QPalette::Inactive,QPalette::Disabled}){palette.setColor(group,QPalette::ToolTipBase,base);palette.setColor(group,QPalette::Window,base);palette.setColor(group,QPalette::ToolTipText,color("text"));palette.setColor(group,QPalette::WindowText,color("text"));}
        QToolTip::setPalette(palette);
    }
    void adapt(QLabel *label){
        if(adapting)return;adapting=true;
        label->setAttribute(Qt::WA_TransparentForMouseEvents,true);label->setFocusPolicy(Qt::NoFocus);label->setTextInteractionFlags(Qt::NoTextInteraction);
        label->setFont(font(FontRole::Body,13));label->setPalette(QToolTip::palette());label->setMargin(0);label->setWordWrap(true);
        const auto sheet=toolTipSheet();if(label->styleSheet()!=sheet)label->setStyleSheet(sheet);
        label->setProperty("kdaTooltipSkin",skin());label->setProperty("kdaTooltipText",label->text());label->setProperty("kdaTooltipPreferredWidth",360);
        auto screen=QGuiApplication::screenAt(label->frameGeometry().center());if(!screen)screen=QGuiApplication::screenAt(QCursor::pos());if(!screen)screen=QGuiApplication::primaryScreen();
        if(screen){
            const auto available=screen->availableGeometry().adjusted(8,8,-8,-8);
            const int frameExtra=std::max(0,label->frameGeometry().width()-label->width());const int maximum=std::max(1,available.width()-frameExtra);
            // Measure both candidates before changing constraints. An already
            // expanded tooltip must not shrink to 360 and grow again on repaint.
            label->ensurePolished();const auto hint=label->sizeHint();const int preferred=std::min(360,maximum);
            qreal natural=0;
            if(Qt::mightBeRichText(label->text())){QTextDocument document;document.setDefaultFont(label->font());document.setDocumentMargin(0);document.setHtml(label->text());natural=document.idealWidth();}
            else for(const auto &line:label->text().split(QChar(u'\n')))natural=std::max(natural,qreal(label->fontMetrics().horizontalAdvance(line)));
            // QLabel's word-wrapped sizeHint deliberately chooses a narrow
            // heuristic width. A short hint should use its actual text width.
            int finalMaximum=preferred,width=std::max(1,std::min(preferred,int(std::ceil(natural))+32)),height=label->heightForWidth(width);if(height<0)height=hint.height();
            // Exceptional long source explanations may use more screen width;
            // never cut the text or impose a maximum height on its contents.
            if(height>available.height()&&width<maximum){width=maximum;finalMaximum=maximum;height=label->heightForWidth(width);}
            if(label->maximumWidth()!=finalMaximum)label->setMaximumWidth(finalMaximum);
            const QSize finalSize(std::max(1,width),std::max(1,height));if(label->size()!=finalSize)label->resize(finalSize);
            const auto frame=label->frameGeometry();const int x=std::clamp(frame.x(),available.left(),std::max(available.left(),available.right()-frame.width()+1));const int y=std::clamp(frame.y(),available.top(),std::max(available.top(),available.bottom()-frame.height()+1));if(label->pos()!=QPoint(x,y))label->move(x,y);
        }
        label->setProperty("kdaTooltipNeedsLayout",false);
        adapting=false;
    }
    bool eventFilter(QObject *watched,QEvent *event)override{
        if(watched==qApp&&event->type()==QEvent::DynamicPropertyChange&&static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName()=="lixinSkin"){
            defaults();for(auto widget:QApplication::topLevelWidgets())if(widget->windowType()==Qt::ToolTip)if(auto label=qobject_cast<QLabel*>(widget))adapt(label);
        }else if(event->type()==QEvent::Show||event->type()==QEvent::StyleChange||event->type()==QEvent::UpdateRequest){
            if(auto label=qobject_cast<QLabel*>(watched);label&&label->isWindow()&&label->windowType()==Qt::ToolTip){
                if(event->type()==QEvent::StyleChange){if(!adapting)label->setProperty("kdaTooltipNeedsLayout",true);}
                else if(event->type()==QEvent::Show||!label->wordWrap()||label->property("kdaTooltipSkin").toString()!=skin()||label->property("kdaTooltipText").toString()!=label->text()||label->property("kdaTooltipNeedsLayout").toBool())adapt(label);
            }
        }
        return QObject::eventFilter(watched,event);
    }
};
inline void installToolTipStyle(){static QPointer<ToolTipStyle> instance;if(qApp&&!instance)instance=new ToolTipStyle(qApp);}
}
