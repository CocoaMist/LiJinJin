#include "startup_notice.h"
#include "kda_theme.h"
#include <QApplication>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QShowEvent>
#include <QVBoxLayout>
#include <algorithm>

namespace {
constexpr int readingInset=22;

QLabel *copy(const QString &text,const char *name,int pixels=14){
    auto label=new QLabel(text);label->setObjectName(QString::fromLatin1(name));
    label->setTextFormat(Qt::PlainText);label->setWordWrap(true);
    label->setFont(Kda::font(Kda::FontRole::Body,pixels));label->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Minimum);
    label->setMinimumWidth(0);return label;
}
void addNoticeSection(QVBoxLayout &layout,const QString &heading,const QString &text,const char *bodyName){
    layout.addSpacing(12);
    auto label=copy(heading,"startupNoticeSection",13);
    label->setFont(Kda::font(Kda::FontRole::Label,13,QFont::Medium));
    layout.addWidget(label);
    layout.addWidget(copy(text,bodyName));
}

class NoticeLogo final : public QWidget {
public:
    NoticeLogo(){
        setObjectName(QStringLiteral("startupNoticeLogo"));
        setFixedSize(28,28);
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        Kda::configurePainter(painter);
        icon.paint(&painter,rect());
    }
private:
    const QIcon icon{QStringLiteral(":/lixin/kda/brand/lijinjin-app.png")};
};

QString noticeStyle(){
    return QStringLiteral(
        "QDialog#startupNotice,QWidget#startupNoticePage{background:%1;color:%2;}"
        "QLabel{background:transparent;border:0;color:%2;}"
        "QLabel#startupNoticeTitle{font-size:24px;font-weight:600;}"
        "QLabel#startupNoticeBrand{color:%3;}QLabel#startupNoticeEyebrow{color:%4;}"
        "QLabel#startupNoticeSubtitle,QLabel#startupNoticeSection{color:%5;}"
        "QLabel#startupNoticeEnding{color:%3;}"
        "QFrame#startupNoticeFooterLine{background:%7;border:0;}"
        "QScrollArea#startupNoticeScroll,QWidget#startupNoticeBody{background:transparent;border:0;}"
        "QScrollArea#startupNoticeScroll>QWidget{background:transparent;}"
        "QScrollBar:vertical{width:5px;background:transparent;margin:2px 0;}"
        "QScrollBar::handle:vertical{background:%7;min-height:24px;border-radius:2px;}"
        "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;}"
        "QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical{background:transparent;}"
        "QPushButton{border:1px solid %7;border-radius:4px;background:%6;color:%3;padding:0 16px;}"
        "QPushButton:hover{border-color:%5;background:%8;}"
        "QPushButton:pressed{background:%1;}"
        "QPushButton:focus{border:2px solid %5;}"
        "QPushButton#startupNoticeContinue{background:%3;color:%1;border-color:%3;}"
        "QPushButton#startupNoticeContinue:hover{background:%2;border-color:%5;}"
        "QPushButton#startupNoticeContinue:pressed{background:%5;}"
        "QPushButton#startupNoticeContinue:focus{border:2px solid %5;}"
    ).arg(Kda::color("bg").name(),Kda::color("text").name(),Kda::color("silver").name(),Kda::color("muted").name(),
          Kda::color("lilac").name(),Kda::color("surface").name(),Kda::color("border").name(),Kda::color("raised").name());
}
}

StartupNoticePage::StartupNoticePage(QWidget *parent):QWidget(parent){
    setObjectName(QStringLiteral("startupNoticePage"));setMinimumSize(0,0);
    setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
    setFont(Kda::font(Kda::FontRole::Body,14));
    Kda::bindThemeStyle(this,noticeStyle);
    auto layout=new QVBoxLayout(this);
    layout->setContentsMargins(readingInset,18,readingInset,16);
    layout->setSpacing(10);

    auto scroll=new QScrollArea;scroll->setObjectName(QStringLiteral("startupNoticeScroll"));
    scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setMinimumSize(0,0);scroll->setFocusPolicy(Qt::NoFocus);
    auto body=new QWidget;body->setObjectName(QStringLiteral("startupNoticeBody"));
    auto content=new QVBoxLayout(body);content->setContentsMargins(0,2,8,6);content->setSpacing(5);
    auto title=copy(QStringLiteral("使用前，请先了解"),"startupNoticeTitle",27);
    title->setFont(Kda::font(Kda::FontRole::Title,27,QFont::DemiBold));content->addWidget(title);
    content->addWidget(copy(QStringLiteral("玩家自制 · 学习与研究"),"startupNoticeSubtitle",13));
    addNoticeSection(*content,QStringLiteral("关于工具"),
        QStringLiteral("本工具由玩家制作，仅供学习与研究，不代表腾讯、Riot Games 或《英雄联盟》官方，未获官方认证。"),"startupNoticeOrigin");
    addNoticeSection(*content,QStringLiteral("运行边界"),
        QStringLiteral("本工具不读取或修改游戏进程内存，不向游戏进程注入代码，也不代替玩家自动操作。启用游戏商店出装页时，仅通过游戏支持的装备方案配置展示推荐。"),"startupNoticeOperation");
    addNoticeSection(*content,QStringLiteral("账号风险"),
        QStringLiteral("使用第三方工具仍有账号处罚风险。本工具无法保证不会受到限制登录、临时封禁或其他处罚。玩家举报可能引发进一步审查，是否处罚及处罚时长由官方规则与判定决定。"),"startupNoticeRisk");
    content->addSpacing(8);content->addWidget(copy(QStringLiteral("请了解并自行评估风险后继续使用。"),"startupNoticeEnding",13));content->addStretch(1);
    scroll->setWidget(body);layout->addWidget(scroll,1);

    auto footerLine=new QFrame;footerLine->setObjectName(QStringLiteral("startupNoticeFooterLine"));footerLine->setFixedHeight(1);layout->addWidget(footerLine);
    footerLayout=new QBoxLayout(QBoxLayout::LeftToRight);
    footerLayout->setSpacing(12);
    auto brandRow=new QHBoxLayout;
    brandRow->setSpacing(8);
    brandRow->addWidget(new NoticeLogo);
    auto brand=copy(QStringLiteral("李金金"),"startupNoticeBrand",13);
    brand->setFont(Kda::font(Kda::FontRole::Label,13,QFont::Medium));
    brand->setWordWrap(false);
    brandRow->addWidget(brand);
    auto reminder=copy(QStringLiteral("使用提醒"),"startupNoticeEyebrow",12);
    reminder->setWordWrap(false);
    brandRow->addWidget(reminder);
    brandRow->addStretch();
    footerLayout->addLayout(brandRow);

    auto actions=new QHBoxLayout;
    actions->setSpacing(12);
    actions->addStretch();
    exitButton=new QPushButton(QStringLiteral("退出"));exitButton->setObjectName(QStringLiteral("startupNoticeExit"));
    continueButton=new QPushButton(QStringLiteral("同意并继续"));continueButton->setObjectName(QStringLiteral("startupNoticeContinue"));
    for(auto button:{exitButton,continueButton}){button->setFont(Kda::font(Kda::FontRole::Label,14,QFont::Medium));button->setAutoDefault(false);button->setDefault(false);button->setFixedHeight(40);button->setCursor(Qt::PointingHandCursor);}
    exitButton->setMinimumWidth(88);continueButton->setMinimumWidth(146);
    actions->addWidget(exitButton);actions->addWidget(continueButton);
    footerLayout->addLayout(actions);
    layout->addLayout(footerLayout);
    connect(exitButton,&QPushButton::clicked,this,[this]{decide(false);});connect(continueButton,&QPushButton::clicked,this,[this]{decide(true);});
    setTabOrder(exitButton,continueButton);
}
void StartupNoticePage::focusExit(){if(!decided&&exitButton)exitButton->setFocus(Qt::OtherFocusReason);}
void StartupNoticePage::reject(){decide(false);}
void StartupNoticePage::decide(bool accepted){
    if(decided)return;decided=true;
    exitButton->setEnabled(false);continueButton->setEnabled(false);
    // The owner may remove this page during the callback. Do not touch members
    // after invoking it, and make reentrant click/close handling a single choice.
    const auto callback=accepted?onAccepted:onRejected;if(callback)callback();
}
void StartupNoticePage::showEvent(QShowEvent *event){QWidget::showEvent(event);focusExit();}
void StartupNoticePage::resizeEvent(QResizeEvent *event){
    QWidget::resizeEvent(event);
    if(!footerLayout)return;
    const int minimumRowWidth=footerLayout->itemAt(0)->minimumSize().width()
        +footerLayout->itemAt(1)->minimumSize().width()+footerLayout->spacing();
    footerLayout->setDirection(event->size().width()-2*readingInset<minimumRowWidth
        ?QBoxLayout::TopToBottom:QBoxLayout::LeftToRight);
}
void StartupNoticePage::keyPressEvent(QKeyEvent *event){
    if(event->key()==Qt::Key_Return||event->key()==Qt::Key_Enter){
        decide(continueButton->hasFocus());event->accept();return;
    }
    if(event->key()==Qt::Key_Escape){reject();event->accept();return;}
    QWidget::keyPressEvent(event);
}

StartupNoticeDialog::StartupNoticeDialog(QWidget *parent):QDialog(parent){
    setObjectName(QStringLiteral("startupNotice"));setWindowTitle(QStringLiteral("李金金 · 使用提醒"));
    setWindowFlags(windowFlags()&~Qt::WindowContextHelpButtonHint);setModal(true);setAttribute(Qt::WA_QuitOnClose,false);
    setFont(Kda::font(Kda::FontRole::Body,14));setMinimumSize(300,240);resize(600,620);
    setWindowIcon(QIcon(QStringLiteral(":/lixin/kda/brand/lijinjin-app.png")));Kda::bindThemeStyle(this,noticeStyle);
    auto layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);layout->setSpacing(0);page=new StartupNoticePage(this);layout->addWidget(page);
    page->onAccepted=[this]{accept();};page->onRejected=[this]{reject();};fitAvailableScreen();
}
void StartupNoticeDialog::fitAvailableScreen(){
    auto target=parentWidget()?parentWidget()->screen():QGuiApplication::primaryScreen();if(!target)return;
    const auto available=target->availableGeometry();const auto usable=available.adjusted(12,12,-12,-44);
    // Only shrink an explicitly resized dialog; opening on a smaller monitor
    // leaves the reading area scrollable and both actions in the fixed footer.
    setMinimumSize(std::min(300,std::max(1,usable.width())),std::min(240,std::max(1,usable.height())));
    resize(std::min(width(),std::max(1,usable.width())),std::min(height(),std::max(1,usable.height())));
    const auto frame=frameGeometry();move(available.center()-QPoint(frame.width()/2,frame.height()/2));
}
void StartupNoticeDialog::showEvent(QShowEvent *event){
    QDialog::showEvent(event);fitAvailableScreen();page->focusExit();
}
void StartupNoticeDialog::keyPressEvent(QKeyEvent *event){
    if(event->key()==Qt::Key_Return||event->key()==Qt::Key_Enter||event->key()==Qt::Key_Escape){
        QApplication::sendEvent(page,event);return;
    }
    QDialog::keyPressEvent(event);
}
bool showStartupNotice(QWidget *parent){StartupNoticeDialog dialog(parent);return dialog.exec()==QDialog::Accepted;}
