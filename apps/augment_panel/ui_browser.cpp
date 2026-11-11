#include "ui_browser.h"
#include "kda_controls.h"
#include <QCloseEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QPainter>
#include <QResizeEvent>
#include <QShortcut>
#include <QTextLayout>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

// The explanation has no scroll area. Every page holds an exact, contiguous
// slice of the original text; actual text shaping determines all line breaks.
class UiBrowserExplanation final : public QWidget {
public:
    explicit UiBrowserExplanation(QWidget *parent=nullptr):QWidget(parent){
        setObjectName(QStringLiteral("uiBrowserExplanation"));setAccessibleName(QStringLiteral("当前场景说明分页"));
        setFont(Kda::font(Kda::FontRole::Body,12));setFocusPolicy(Qt::StrongFocus);setMinimumHeight(72);setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Expanding);
    }
    void setText(const QString &value){if(value==full)return;full=value;index=0;reflow(false);}
    QString fullText()const{return full;}
    QString pageText()const{const auto &page=pages.at(index);return full.mid(page.start,page.end-page.start);}
    int pageCount()const{return pages.size();}
    int pageIndex()const{return index;}
    void setPage(int value){value=std::clamp(value,0,int(pages.size())-1);if(value==index)return;index=value;setAccessibleDescription(pageText());update();if(onPagesChanged)onPagesChanged();}
    bool textFits()const{
        const auto content=rect().adjusted(8,7,-8,-7);const auto &page=pages.at(index);
        if(page.lines.size()*lineHeight>content.height()+.5)return false;
        for(const auto &line:page.lines)if(line.width>content.width()+.5)return false;
        return true;
    }
    std::function<void()> onPagesChanged;
protected:
    void resizeEvent(QResizeEvent *event)override{QWidget::resizeEvent(event);reflow(true);}
    bool event(QEvent *event)override{
        const bool handled=QWidget::event(event);
        if(event->type()==QEvent::FontChange||event->type()==QEvent::ApplicationFontChange||event->type()==QEvent::StyleChange)reflow(true);
        return handled;
    }
    void paintEvent(QPaintEvent *)override{
        QPainter painter(this);Kda::configurePainter(painter);const auto face=QRectF(rect()).adjusted(.5,.5,-.5,-.5);
        painter.setPen(QPen(Kda::color(hasFocus()?"ice":"border"),1));painter.setBrush(Kda::color("surface"));painter.drawRoundedRect(face,Kda::radius("control"),Kda::radius("control"));
        const auto content=rect().adjusted(8,7,-8,-7);painter.setClipRect(content);painter.setFont(font());painter.setPen(Kda::color("silver"));
        qreal y=content.top();const auto ascent=QFontMetricsF(font()).ascent();
        for(const auto &line:pages.at(index).lines){painter.drawText(QPointF(content.left(),y+ascent),line.text);y+=lineHeight;}
    }
private:
    struct Line {int start=0,end=0;QString text;qreal width=0;};
    struct Page {int start=0,end=0;QList<Line> lines;};
    QString full;QList<Page> pages{Page{}};int index=0;qreal lineHeight=1;
    void reflow(bool retainAnchor){
        const int anchor=retainAnchor&&!pages.isEmpty()?pages.at(std::clamp(index,0,int(pages.size())-1)).start:0;
        const auto content=rect().adjusted(8,7,-8,-7);const int width=std::max(1,content.width());
        lineHeight=std::ceil(QFontMetricsF(font()).height())+1;QList<Line> lines;
        for(int start=0;start<full.size();){
            const int newline=full.indexOf(QChar(u'\n'),start);const int end=newline<0?full.size():newline;
            const auto paragraph=full.mid(start,end-start);QTextLayout layout(paragraph.isEmpty()?QStringLiteral(" "):paragraph,font());
            QTextOption option;option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);layout.setTextOption(option);layout.beginLayout();
            const int before=lines.size();
            while(true){auto shaped=layout.createLine();if(!shaped.isValid())break;shaped.setLineWidth(width);
                lineHeight=std::max(lineHeight,std::ceil(shaped.height())+1);
                const int offset=paragraph.isEmpty()?0:shaped.textStart(),length=paragraph.isEmpty()?0:shaped.textLength();
                auto text=paragraph.mid(offset,length);if(text.endsWith(QChar(u'\r')))text.chop(1);
                lines.append(Line{start+offset,start+offset+length,text,paragraph.isEmpty()?0:shaped.naturalTextWidth()});
            }
            layout.endLayout();
            if(lines.size()==before)lines.append(Line{start,end,QString(),0});
            lines.last().end=newline<0?end:newline+1;
            start=newline<0?full.size():newline+1;
        }
        pages.clear();const int capacity=std::max(1,int(std::floor(std::max(0,content.height())/lineHeight)));
        for(int begin=0;begin<lines.size();begin+=capacity){Page page;page.start=lines[begin].start;
            const int end=std::min(begin+capacity,int(lines.size()));for(int row=begin;row<end;++row)page.lines.append(lines[row]);
            page.end=page.lines.last().end;pages.append(page);
        }
        if(pages.isEmpty())pages.append(Page{});index=0;
        if(retainAnchor)for(int i=0;i<pages.size();++i)if(anchor>=pages[i].start&&(anchor<pages[i].end||i==pages.size()-1)){index=i;break;}
        setAccessibleDescription(pageText());update();if(onPagesChanged)onPagesChanged();
    }
};

const QList<UiBrowserScene> &uiBrowserScenes(){
    static const QList<UiBrowserScene> scenes{
        {QStringLiteral("idle"),QStringLiteral("等待对局"),QStringLiteral("查看未进入游戏时的连接状态与浮层。")},
        {QStringLiteral("champ_select"),QStringLiteral("选人 · 阵容"),QStringLiteral("查看已公开的队友；尚未公开的对手保留未知。")},
        {QStringLiteral("loading_pending"),QStringLiteral("加载 · 阵容载入"),QStringLiteral("查看部分席位尚未公开时的载入动效。")},
        {QStringLiteral("loading_ready"),QStringLiteral("加载 · 阵容就绪"),QStringLiteral("查看十个席位就绪后停住的 HUD。")},
        {QStringLiteral("alive_purchase"),QStringLiteral("存活 · 出装"),QStringLiteral("查看左下角紧凑出装提示，检查信息层级与组件显示。")},
        {QStringLiteral("alive_roster"),QStringLiteral("存活 · 阵容"),QStringLiteral("十人同屏查看头像、装备与状态。")},
        {QStringLiteral("dead_purchase"),QStringLiteral("阵亡 · 出装"),QStringLiteral("查看阵亡时展开的装备详情，并检查出装与阵容切换。")},
        {QStringLiteral("dead_roster"),QStringLiteral("阵亡 · 阵容"),QStringLiteral("十人同屏对照双方装备与状态。")},
        {QStringLiteral("cards"),QStringLiteral("海克斯 · 选牌"),QStringLiteral("查看卡面下沿的评价。原有标题、说明与重随控件留给游戏。")},
        {QStringLiteral("cards_incomplete"),QStringLiteral("海克斯 · 信息不足"),QStringLiteral("查看机制或触发信息缺失时的解释，避免把未知当成零收益。")},
        {QStringLiteral("death_tips"),QStringLiteral("阵亡 · TIPS"),QStringLiteral("查看上方居中的阵亡建议；可重播进入、切换等元素动效。")},
        {QStringLiteral("scoreboard"),QStringLiteral("计分板 · 让位"),QStringLiteral("模拟按住 Tab 查看计分板时，游戏浮层立即隐藏。")},
        {QStringLiteral("stale_live"),QStringLiteral("局内 · 等待重连"),QStringLiteral("查看实时数据过期时的界面；旧结果不能伪装成当前建议。")}
    };
    return scenes;
}

UiBrowserWindow::UiBrowserWindow(QWidget *parent):QWidget(parent){
    setWindowFlag(Qt::Window,true);setObjectName(QStringLiteral("uiBrowserWindow"));
    setWindowTitle(QStringLiteral("李金金 · 界面浏览"));setMinimumSize(360,420);resize(380,476);
    setFont(Kda::font(Kda::FontRole::Body,13));
    Kda::bindThemeStyle(this,[]{return Kda::overlayStyleSheet()+Kda::themedStyleSheet(QStringLiteral("\nQWidget#uiBrowserWindow{background:#101017;color:#edeef4;}"));});
    auto layout=new QVBoxLayout(this);layout->setContentsMargins(14,12,14,12);layout->setSpacing(6);
    auto heading=new QLabel(QStringLiteral("界面浏览"));heading->setObjectName(QStringLiteral("uiBrowserHeading"));
    heading->setFont(Kda::font(Kda::FontRole::Title,20,QFont::DemiBold));
    auto badge=new QLabel(QStringLiteral("演示数据 · 独立预览"));badge->setObjectName(QStringLiteral("uiBrowserDemoBadge"));
    Kda::bindThemeStyle(badge,[]{return QStringLiteral("color:%1;").arg(Kda::color("lilac").name());});badge->setFont(Kda::font(Kda::FontRole::Label,12));auto headingRow=new QHBoxLayout;headingRow->addWidget(heading);headingRow->addStretch(1);headingRow->addWidget(badge);layout->addLayout(headingRow);
    auto scope=new QLabel(QStringLiteral("浏览不同对局场景，实际浮层会随选择切换。"));scope->setObjectName(QStringLiteral("muted"));scope->setWordWrap(true);layout->addWidget(scope);
    auto sceneLabel=new QLabel(QStringLiteral("场景"));scenes=new Kda::ComboBox;scenes->setObjectName(QStringLiteral("uiBrowserScene"));
    scenes->setAccessibleName(QStringLiteral("浏览场景"));scenes->setFocusPolicy(Qt::StrongFocus);scenes->setFixedHeight(30);scenes->setMinimumWidth(0);scenes->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);sceneLabel->setBuddy(scenes);
    for(const auto &scene:uiBrowserScenes())scenes->addItem(scene.title,scene.id);
    auto sceneRow=new QHBoxLayout;sceneRow->setSpacing(10);sceneRow->addWidget(sceneLabel);sceneRow->addWidget(scenes,1);layout->addLayout(sceneRow);
    auto paging=new QHBoxLayout;paging->setSpacing(8);previous=new Kda::Button(QStringLiteral("上一项"));previous->setObjectName(QStringLiteral("uiBrowserPrevious"));
    next=new Kda::Button(QStringLiteral("下一项"));next->setObjectName(QStringLiteral("uiBrowserNext"));positionLabel=new QLabel;positionLabel->setObjectName(QStringLiteral("uiBrowserPosition"));positionLabel->setAlignment(Qt::AlignCenter);positionLabel->setMinimumWidth(44);
    paging->addWidget(previous,1);paging->addWidget(positionLabel);paging->addWidget(next,1);layout->addLayout(paging);
    sceneDescription=new QLabel;sceneDescription->setObjectName(QStringLiteral("uiBrowserDescription"));sceneDescription->setWordWrap(true);sceneDescription->setMinimumHeight(44);sceneDescription->setAlignment(Qt::AlignLeft|Qt::AlignTop);sceneDescription->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);layout->addWidget(sceneDescription);
    explanation=new UiBrowserExplanation;layout->addWidget(explanation,1);
    explanationPrevious=new Kda::Button(QStringLiteral("上一页"));explanationPrevious->setObjectName(QStringLiteral("uiBrowserExplanationPrevious"));explanationPrevious->setAccessibleName(QStringLiteral("说明上一页"));
    explanationNext=new Kda::Button(QStringLiteral("下一页"));explanationNext->setObjectName(QStringLiteral("uiBrowserExplanationNext"));explanationNext->setAccessibleName(QStringLiteral("说明下一页"));
    explanationPosition=new QLabel;explanationPosition->setObjectName(QStringLiteral("uiBrowserExplanationPosition"));explanationPosition->setAlignment(Qt::AlignCenter);explanationPosition->setMinimumWidth(58);
    auto explanationPaging=new QHBoxLayout;explanationPaging->setSpacing(8);explanationPaging->addWidget(explanationPrevious,1);explanationPaging->addWidget(explanationPosition);explanationPaging->addWidget(explanationNext,1);layout->addLayout(explanationPaging);
    explanation->onPagesChanged=[this]{refreshExplanationControls();};
    auto viewportLabel=new QLabel(QStringLiteral("显示范围"));viewports=new Kda::ComboBox;viewports->setObjectName(QStringLiteral("uiBrowserViewport"));viewports->setAccessibleName(QStringLiteral("预览显示范围"));viewports->setFixedHeight(30);viewports->setMinimumWidth(0);viewportLabel->setBuddy(viewports);
    viewports->addItem(QStringLiteral("随屏幕"),QSize());
    for(const auto size:{QSize(1600,900),QSize(1024,768),QSize(1920,1080),QSize(2560,1080),QSize(2560,1440),QSize(3440,1440),QSize(3840,1600),QSize(3840,2160)})
        viewports->addItem(QStringLiteral("%1 × %2").arg(size.width()).arg(size.height()),size);
    auto viewportRow=new QHBoxLayout;viewportRow->setSpacing(10);viewportRow->addWidget(viewportLabel);viewportRow->addWidget(viewports,1);layout->addLayout(viewportRow);
    replay=new Kda::Button(QStringLiteral("重播动效"));replay->setObjectName(QStringLiteral("uiBrowserReplay"));replay->setToolTip(QStringLiteral("重新展示当前场景的元素动效"));
    quit=new Kda::Button(QStringLiteral("退出浏览"));quit->setObjectName(QStringLiteral("uiBrowserQuit"));auto actions=new QHBoxLayout;actions->setSpacing(8);actions->addWidget(replay,1);actions->addWidget(quit,1);layout->addLayout(actions);
    interactive={scenes,previous,next,explanation,explanationPrevious,explanationNext,viewports,replay,quit};
    setTabOrder(scenes,previous);setTabOrder(previous,next);setTabOrder(next,explanation);setTabOrder(explanation,explanationPrevious);setTabOrder(explanationPrevious,explanationNext);setTabOrder(explanationNext,viewports);setTabOrder(viewports,replay);setTabOrder(replay,quit);
    connect(scenes,&QComboBox::currentIndexChanged,this,[this]{refreshScene();if(onScene)onScene(sceneId());});
    connect(viewports,&QComboBox::currentIndexChanged,this,[this]{if(onViewport)onViewport(viewportSize());});
    connect(previous,&QPushButton::clicked,this,[this]{step(-1);});connect(next,&QPushButton::clicked,this,[this]{step(1);});
    connect(replay,&QPushButton::clicked,this,[this]{if(onReplay)onReplay();});connect(quit,&QPushButton::clicked,this,[this]{close();});
    connect(explanationPrevious,&QPushButton::clicked,this,[this]{explanation->setPage(explanation->pageIndex()-1);});connect(explanationNext,&QPushButton::clicked,this,[this]{explanation->setPage(explanation->pageIndex()+1);});
    auto previousKey=new QShortcut(QKeySequence(QStringLiteral("Alt+Left")),this);auto nextKey=new QShortcut(QKeySequence(QStringLiteral("Alt+Right")),this);
    connect(previousKey,&QShortcut::activated,this,[this]{step(-1);});connect(nextKey,&QShortcut::activated,this,[this]{step(1);});
    previous->setToolTip(QStringLiteral("上一项 · Alt + ←"));next->setToolTip(QStringLiteral("下一项 · Alt + →"));
    refreshScene();refreshExplanationControls();
}

bool UiBrowserWindow::selectScene(const QString &id){
    const int index=scenes->findData(id);if(index<0)return false;
    if(index==scenes->currentIndex())return true;
    scenes->setCurrentIndex(index);return true;
}
QString UiBrowserWindow::sceneId()const{return scenes->currentData().toString();}
QSize UiBrowserWindow::viewportSize()const{return viewports->currentData().toSize();}
void UiBrowserWindow::setExplanation(const QString &text){explanation->setText(text);}
void UiBrowserWindow::refreshExplanationControls(){explanationPrevious->setEnabled(explanation->pageIndex()>0);explanationNext->setEnabled(explanation->pageIndex()+1<explanation->pageCount());explanationPosition->setText(QStringLiteral("说明 %1/%2").arg(explanation->pageIndex()+1).arg(explanation->pageCount()));}
void UiBrowserWindow::refreshScene(){
    const auto index=scenes->currentIndex();const auto &all=uiBrowserScenes();
    sceneDescription->setText(index>=0&&index<all.size()?all[index].description:QString());
    positionLabel->setText(QStringLiteral("%1 / %2").arg(index+1).arg(all.size()));
}
void UiBrowserWindow::step(int delta){const int size=scenes->count();if(size>0)scenes->setCurrentIndex((scenes->currentIndex()+delta+size)%size);}
void UiBrowserWindow::closeEvent(QCloseEvent *event){if(!quitting){quitting=true;if(onQuit)onQuit();}QWidget::closeEvent(event);}
QJsonObject UiBrowserWindow::report()const{
    QJsonArray controls;bool contained=true,nonOverlapping=true;
    QList<QRect> rectangles;
    for(const auto *control:interactive){const QRect r(control->mapTo(this,QPoint()),control->size());contained&=rect().contains(r);rectangles.append(r);
        controls.append(QJsonObject{{QStringLiteral("object_name"),control->objectName()},{QStringLiteral("rect"),QJsonArray{r.x(),r.y(),r.width(),r.height()}},
            {QStringLiteral("visible"),control->isVisibleTo(this)},{QStringLiteral("enabled"),control->isEnabled()},{QStringLiteral("keyboard_focusable"),control->focusPolicy()!=Qt::NoFocus}});}
    for(int i=0;i<rectangles.size();++i)for(int j=i+1;j<rectangles.size();++j)nonOverlapping&=!rectangles[i].intersects(rectangles[j]);
    const auto textRect=QFontMetrics(sceneDescription->font()).boundingRect(QRect(0,0,sceneDescription->contentsRect().width(),10000),Qt::TextWordWrap,sceneDescription->text());
    const auto viewport=viewportSize();
    return {{QStringLiteral("scene_id"),sceneId()},{QStringLiteral("scene_count"),scenes->count()},{QStringLiteral("scene_title"),scenes->currentText()},
        {QStringLiteral("scene_description"),sceneDescription->text()},{QStringLiteral("synthetic"),true},{QStringLiteral("demo_badge_visible"),findChild<QLabel*>(QStringLiteral("uiBrowserDemoBadge"))->isVisibleTo(this)},
        {QStringLiteral("full_explanation"),explanation->fullText()},{QStringLiteral("explanation_scrollable"),false},{QStringLiteral("explanation_read_only"),true},
        {QStringLiteral("explanation_page_count"),explanation->pageCount()},{QStringLiteral("explanation_page_index"),explanation->pageIndex()},{QStringLiteral("explanation_page_text"),explanation->pageText()},{QStringLiteral("explanation_text_fits"),explanation->textFits()},
        {QStringLiteral("explanation_horizontal_scroll_range"),0},{QStringLiteral("explanation_vertical_scroll_range"),0},
        {QStringLiteral("controls"),controls},{QStringLiteral("all_controls_contained"),contained},{QStringLiteral("controls_non_overlapping"),nonOverlapping},
        {QStringLiteral("description_text_fits"),textRect.height()<=sceneDescription->contentsRect().height()&&textRect.width()<=sceneDescription->contentsRect().width()},
        {QStringLiteral("viewport_size"),QJsonArray{viewport.width(),viewport.height()}},{QStringLiteral("viewport_follows_screen"),!viewport.isValid()},
        {QStringLiteral("geometry"),QJsonArray{x(),y(),width(),height()}},{QStringLiteral("real_game_verified"),false},
        {QStringLiteral("owns_live_client"),false},{QStringLiteral("owns_settings"),false},{QStringLiteral("paid_analysis_dispatches"),0}};
}
