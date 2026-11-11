#pragma once
#include "kda_theme.h"
#include "kda_hud_style.h"
#include "hud_symbols.h"
#include <QApplication>
#include <QDateTime>
#include <QDynamicPropertyChangeEvent>
#include <QFontMetrics>
#include <QJsonArray>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QTimer>
#include <QWidget>
#include <algorithm>
#include <functional>
#include <limits>
#include <utility>

namespace Kda {
enum class NoticeKind {Info,Progress,Success,Warning,Error};
struct Notice {
    QString key,title,detail;
    NoticeKind kind=NoticeKind::Info;
    int durationMs=4500;
};
namespace NoticeDetail {
inline QString kindName(NoticeKind kind){
    switch(kind){case NoticeKind::Progress:return QStringLiteral("progress");case NoticeKind::Success:return QStringLiteral("success");case NoticeKind::Warning:return QStringLiteral("warning");case NoticeKind::Error:return QStringLiteral("error");default:return QStringLiteral("info");}
}
inline QColor accent(NoticeKind kind){return color(kind==NoticeKind::Error?"danger":kind==NoticeKind::Warning?"lilac":kind==NoticeKind::Success?"ice":"silver");}

// A child of the existing surface: no extra desktop window, activation,
// focus request, perpetual animation or global mouse/keyboard capture.
class Card final : public QWidget {
public:
    explicit Card(QWidget *parent):QWidget(parent){
        setObjectName(QStringLiteral("globalNotice"));setFocusPolicy(Qt::NoFocus);setAttribute(Qt::WA_ShowWithoutActivating);setAttribute(Qt::WA_TranslucentBackground);
        close=new QPushButton(this);close->setObjectName(QStringLiteral("dismissGlobalNotice"));close->setAccessibleName(QStringLiteral("关闭提醒"));close->setToolTip(QStringLiteral("关闭提醒"));close->setFocusPolicy(Qt::StrongFocus);close->setCursor(Qt::PointingHandCursor);
        QObject::connect(close,&QPushButton::clicked,this,[this]{if(onDismiss)onDismiss();});
        applySkin();
    }
    Notice notice;
    std::function<void()> onDismiss;
    void setNotice(const Notice &value){notice=value;setAccessibleName(value.title);setAccessibleDescription(value.detail);setToolTip(value.detail.isEmpty()?value.title:value.title+QStringLiteral("\n")+value.detail);applySkin();update();}
    void applySkin(){
        close->setText(QStringLiteral("×"));close->setFont(Kda::font(FontRole::Body,16));
        const auto muted=color("muted"),bright=color("text"),raised=color("raised"),focus=color("ice");
        close->setStyleSheet(QStringLiteral("QPushButton{background:transparent;border:0;border-radius:6px;color:rgba(%1,%2,%3,92);padding:0;}QPushButton:hover{background:rgba(%4,%5,%6,105);color:rgba(%7,%8,%9,225);}QPushButton:focus{border:1px solid rgba(%10,%11,%12,150);color:rgba(%7,%8,%9,225);}")
            .arg(muted.red()).arg(muted.green()).arg(muted.blue()).arg(raised.red()).arg(raised.green()).arg(raised.blue()).arg(bright.red()).arg(bright.green()).arg(bright.blue()).arg(focus.red()).arg(focus.green()).arg(focus.blue()));
        setProperty("noticeSkin",skin());update();
    }
protected:
    void resizeEvent(QResizeEvent *event)override{QWidget::resizeEvent(event);close->setGeometry(std::max(0,width()-33),(height()-24)/2-1,24,24);}
    void paintEvent(QPaintEvent *)override{
        QPainter painter(this);configurePainter(painter);const bool ember=skin()==QStringLiteral("kda_red");
        const auto bounds=QRectF(rect()).adjusted(3,2,-3,-5);
        painter.setPen(Qt::NoPen);for(int i=3;i>=1;--i){painter.setBrush(QColor(0,0,0,7+(3-i)*4));painter.drawRoundedRect(bounds.adjusted(-i*.35,-i*.2,i*.35,i*.2).translated(0,1+i*.35),12+i*.3,12+i*.3);}
        QPainterPath shape;shape.addRoundedRect(bounds,12,12);
        QLinearGradient face(bounds.topLeft(),bounds.bottomRight());face.setColorAt(0,ember?QColor("#2e2329"):QColor("#2a2e37"));face.setColorAt(.45,ember?QColor("#241a21"):QColor("#21252d"));face.setColorAt(1,ember?QColor("#1b141b"):QColor("#181c23"));painter.fillPath(shape,face);
        auto edge=color(ember?"lilac":"silver");edge.setAlpha(ember?55:52);painter.setPen(QPen(edge,.7));painter.setBrush(Qt::NoBrush);painter.drawPath(shape);
        painter.save();painter.setClipPath(shape);QLinearGradient highlight(bounds.topLeft(),bounds.topRight());auto pearl=color(ember?"ice":"silver");pearl.setAlpha(ember?40:72);highlight.setColorAt(0,pearl);pearl.setAlpha(0);highlight.setColorAt(.75,pearl);highlight.setColorAt(1,pearl);painter.setPen(QPen(highlight,.65));painter.drawLine(bounds.topLeft()+QPointF(12,.6),bounds.topRight()+QPointF(-12,.6));painter.restore();
        painter.save();painter.setClipPath(shape,Qt::IntersectClip);HudStyle::paintSignature(painter,bounds,.3);painter.restore();
        const auto ink=accent(notice.kind);const QRectF medallion(12,(height()-20)/2.-1,20,20);auto disc=ink;disc.setAlpha(15);painter.setBrush(disc);auto discEdge=ink;discEdge.setAlpha(28);painter.setPen(QPen(discEdge,.65));painter.drawEllipse(medallion);
        const auto symbol=notice.kind==NoticeKind::Success?HudSymbols::Symbol::Check:notice.kind==NoticeKind::Warning||notice.kind==NoticeKind::Error?HudSymbols::Symbol::Warning:notice.kind==NoticeKind::Progress?HudSymbols::Symbol::Clock:HudSymbols::Symbol::Info;
        HudSymbols::draw(painter,symbol,medallion.adjusted(4,4,-4,-4),ink,1.45);
        const int textWidth=std::max(0,width()-84);const auto titleFont=Kda::font(FontRole::Body,13,QFont::DemiBold);painter.setFont(titleFont);painter.setPen(color("text"));painter.drawText(QRect(42,(height()-24)/2-1,textWidth,24),Qt::AlignLeft|Qt::AlignVCenter|Qt::TextSingleLine,QFontMetrics(titleFont).elidedText(notice.title.simplified(),Qt::ElideRight,textWidth));
    }
private:QPushButton *close=nullptr;
};

class Manager final : public QObject {
public:
    explicit Manager(QObject *parent):QObject(parent){
        expiry.setSingleShot(true);connect(&expiry,&QTimer::timeout,this,[this]{dismiss();});if(qApp)qApp->installEventFilter(this);
    }
    void install(QWidget *host,int priority){
        if(!host)return;for(auto &entry:hosts)if(entry.widget==host){entry.priority=priority;present();return;}
        hosts.append({host,priority});present();
    }
    void post(Notice value){
        if(value.key.isEmpty()||value.title.trimmed().isEmpty())return;
        const auto signature=value.title+QChar(0x1f)+value.detail+QChar(0x1f)+kindName(value.kind);
        const auto activeSignature=latest.title+QChar(0x1f)+latest.detail+QChar(0x1f)+kindName(latest.kind);
        if(pending&&latest.key==value.key&&activeSignature==signature){++deduplicated;return;}
        if(pending)++replaced;latest=std::move(value);pending=true;expiresAt=0;expiry.stop();++accepted;present();
    }
    void dismiss(const QString &key={}){
        if(!pending||(!key.isEmpty()&&latest.key!=key))return;
        expiry.stop();pending=false;expiresAt=0;++dismissed;if(card)card->hide();
    }
    QJsonObject report()const{
        const auto host=card?card->parentWidget():nullptr;const QRect bounds=card?card->geometry():QRect();
        return {{"registered_hosts",hosts.size()},{"pending",pending},{"visible",card&&card->isVisibleTo(host)&&host&&host->isVisible()},{"active_key",pending?latest.key:QString()},{"kind",pending?kindName(latest.kind):QString()},
            {"accepted",accepted},{"deduplicated",deduplicated},{"replaced",replaced},{"dismissed",dismissed},{"host_priority",activePriority},{"geometry",QJsonArray{bounds.x(),bounds.y(),bounds.width(),bounds.height()}},{"within_host",host&&host->rect().contains(bounds)},
            {"skin",card?card->property("noticeSkin").toString():QString()},{"focus_requested",false},{"desktop_window",card&&card->isWindow()},{"spinner_timer",false},{"content_exported",false}};
    }
protected:
    bool eventFilter(QObject *watched,QEvent *event)override{
        if(watched==qApp&&event->type()==QEvent::DynamicPropertyChange){const auto name=static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName();if(name=="lixinSkin"||name=="lixinReducedMotion"){if(card)card->applySkin();schedule();}}
        else if(event->type()==QEvent::Show||event->type()==QEvent::Hide||event->type()==QEvent::Resize||event->type()==QEvent::WindowStateChange||event->type()==QEvent::Destroy){for(const auto &entry:hosts)if(entry.widget==watched){schedule();break;}}
        return QObject::eventFilter(watched,event);
    }
private:
    struct Host{QPointer<QWidget> widget;int priority=0;};
    QList<Host> hosts;QPointer<Card> card;QTimer expiry;Notice latest;
    bool pending=false,scheduled=false;int accepted=0,deduplicated=0,replaced=0,dismissed=0,activePriority=0;qint64 expiresAt=0;
    void schedule(){if(scheduled)return;scheduled=true;QTimer::singleShot(0,this,[this]{scheduled=false;present();});}
    void present(){
        hosts.erase(std::remove_if(hosts.begin(),hosts.end(),[](const Host &entry){return entry.widget.isNull();}),hosts.end());
        if(!pending){if(card)card->hide();return;}
        QWidget *host=nullptr;int priority=std::numeric_limits<int>::min();
        for(const auto &entry:hosts)if(entry.widget&&entry.widget->isVisible()&&!entry.widget->window()->isMinimized()&&entry.priority>=priority){host=entry.widget;priority=entry.priority;}
        if(!host){if(card)card->hide();return;}
        const auto now=QDateTime::currentMSecsSinceEpoch();if(expiresAt>0&&now>=expiresAt){dismiss();return;}
        if(!card){card=new Card(host);card->onDismiss=[this]{dismiss();};}
        if(card->parentWidget()!=host){card->hide();card->setParent(host);}
        activePriority=priority;card->setNotice(latest);
        int top=12;if(const auto bar=host->findChild<QWidget*>(QStringLiteral("topbar")))top=bar->mapTo(host,QPoint()).y()+bar->height()+10;
        else if(const auto bar=host->findChild<QWidget*>(QStringLiteral("titlebar")))top=bar->mapTo(host,QPoint()).y()+bar->height()+6;
        top=std::min(top,std::max(0,host->height()-58));
        const int margin=host->width()>=360?18:8;
        const int intrinsic=std::clamp(QFontMetrics(Kda::font(FontRole::Body,13,QFont::DemiBold)).horizontalAdvance(latest.title.simplified())+100,180,380);
        const int width=std::min(intrinsic,std::max(0,host->width()-2*margin));
        const int height=std::min(48,std::max(0,host->height()-top-8));
        if(width<130||height<42){card->hide();return;}
        card->setGeometry(host->width()-margin-width,top,width,height);card->show();card->raise();
        if(expiresAt==0){expiresAt=now+std::clamp(latest.durationMs,500,30000);expiry.start(int(expiresAt-now));}
    }
};
inline Manager *manager(){static QPointer<Manager> value;if(qApp&&!value)value=new Manager(qApp);return value;}
}
inline void postNotice(const Notice &notice){if(auto manager=NoticeDetail::manager())manager->post(notice);}
inline void installNoticeHost(QWidget *host,int priority=0){if(auto manager=NoticeDetail::manager())manager->install(host,priority);}
inline void dismissNotice(const QString &key={}){if(auto manager=NoticeDetail::manager())manager->dismiss(key);}
inline QJsonObject noticeReport(){if(auto manager=NoticeDetail::manager())return manager->report();return {};}
}
