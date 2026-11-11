#include "hud_paged_text.h"
#include "kda_controls.h"
#include "kda_surface.h"
#include "kda_hud_style.h"
#include <QAbstractScrollArea>
#include <QAbstractTextDocumentLayout>
#include <QFontInfo>
#include <QFontMetrics>
#include <QGlyphRun>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSet>
#include <QStringList>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextLayout>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr int minimumBodyPixels=12;

// Read the shaped runs, including CJK fallback, rather than the requested
// family or the document's default font. HTML may use points or relative sizes.
qreal fragmentGlyphPixels(const QTextBlock &block,const QTextFragment &fragment,QSet<QString> *families=nullptr){
    qreal minimum=std::numeric_limits<qreal>::max();
    if(const auto layout=block.layout())for(const auto &run:layout->glyphRuns(fragment.position()-block.position(),fragment.length())){
        const auto raw=run.rawFont();if(!raw.isValid()||run.glyphIndexes().isEmpty())continue;
        if(raw.pixelSize()>0)minimum=std::min(minimum,raw.pixelSize());
        if(families&&!raw.familyName().isEmpty())families->insert(raw.familyName());
    }
    return minimum==std::numeric_limits<qreal>::max()?0:minimum;
}

void normalizeRichTextFonts(QTextDocument &document){
    (void)document.documentLayout()->documentSize();
    struct Edit {int start,length;QTextCharFormat format;};QList<Edit> edits;
    const auto body=Kda::hudFont(Kda::FontRole::Body,minimumBodyPixels,QFont::Normal);
    for(auto block=document.begin();block.isValid();block=block.next())for(auto it=block.begin();!it.atEnd();++it){
        const auto fragment=it.fragment();if(!fragment.isValid()||fragment.charFormat().isImageFormat())continue;
        auto format=fragment.charFormat();const qreal shapedPixels=fragmentGlyphPixels(block,fragment);
        const auto requested=format.font().resolve(document.defaultFont());
        const qreal pixels=shapedPixels>0?shapedPixels:QFontInfo(requested).pixelSize();
        if(pixels>0&&pixels<minimumBodyPixels-.01){
            // Replace only the size properties. Keep links, weight, colors and
            // larger heading sizes; removing a relative small-size adjustment
            // prevents it being applied again after the absolute lower bound.
            format.clearProperty(QTextFormat::FontPointSize);
            format.clearProperty(QTextFormat::FontSizeAdjustment);
            format.setProperty(QTextFormat::FontPixelSize,minimumBodyPixels);
        }
        format.setFontStyleStrategy(body.styleStrategy());
        format.setFontHintingPreference(body.hintingPreference());
        edits.append({fragment.position(),fragment.length(),format});
    }
    // Gather first: changing a format invalidates layouts used by later runs.
    QTextCursor cursor(&document);cursor.beginEditBlock();
    for(const auto &edit:edits){cursor.setPosition(edit.start);cursor.setPosition(edit.start+edit.length,QTextCursor::KeepAnchor);cursor.setCharFormat(edit.format);}
    cursor.endEditBlock();
}
}

class HudPagedTextCanvas final : public QWidget {
public:
    explicit HudPagedTextCanvas(HudPagedText *owner):QWidget(owner),owner(owner){
        setObjectName(QStringLiteral("hudPagedViewport"));setMinimumSize(0,0);
        setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Ignored);setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);setAccessibleName(QStringLiteral("当前页内容"));
    }
protected:
    void resizeEvent(QResizeEvent *event) override {QWidget::resizeEvent(event);owner->layoutDocument();}
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);Kda::configurePainter(painter);
        painter.setClipRect(rect());painter.translate(0,-owner->page*owner->text.pageSize().height());
        QAbstractTextDocumentLayout::PaintContext context;
        context.clip=QRectF(0,owner->page*owner->text.pageSize().height(),width(),height());
        context.palette=palette();context.palette.setColor(QPalette::Text,Kda::color("text"));
        owner->text.documentLayout()->draw(&painter,context);
    }
    void mouseMoveEvent(QMouseEvent *event) override {
        const auto url=owner->anchorAt(event->position());
        setCursor(url.isEmpty()?Qt::ArrowCursor:Qt::PointingHandCursor);
        if(owner->onHighlighted)owner->onHighlighted(url);
        QWidget::mouseMoveEvent(event);
    }
    void leaveEvent(QEvent *event) override {
        if(owner->onHighlighted)owner->onHighlighted(QUrl());QWidget::leaveEvent(event);
    }
    void mousePressEvent(QMouseEvent *event) override {
        if(event->button()==Qt::LeftButton){const auto url=owner->anchorAt(event->position());
            if(!url.isEmpty()&&owner->onAnchor){owner->onAnchor(url);event->accept();return;}}
        QWidget::mousePressEvent(event);
    }
    void keyPressEvent(QKeyEvent *event) override {
        if(event->key()==Qt::Key_PageDown||event->key()==Qt::Key_Right){owner->setPage(owner->pageIndex()+1);event->accept();return;}
        if(event->key()==Qt::Key_PageUp||event->key()==Qt::Key_Left){owner->setPage(owner->pageIndex()-1);event->accept();return;}
        QWidget::keyPressEvent(event);
    }
private:HudPagedText *owner;
};

HudPagedText::HudPagedText(QWidget *parent):QWidget(parent),text(this){
    setMinimumSize(0,0);setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Ignored);
    text.setDefaultFont(Kda::hudFont(Kda::FontRole::Body,minimumBodyPixels,QFont::Normal));text.setDocumentMargin(4);
    auto option=text.defaultTextOption();option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);text.setDefaultTextOption(option);
    auto layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);layout->setSpacing(2);
    canvas=new HudPagedTextCanvas(this);layout->addWidget(canvas,1);
    pager=new QWidget;pager->setObjectName(QStringLiteral("hudDocumentPager"));pager->setFixedHeight(22);
    auto row=new QHBoxLayout(pager);row->setContentsMargins(0,0,0,0);row->setSpacing(4);
    previous=new Kda::Button(QStringLiteral("‹"));previous->setObjectName(QStringLiteral("hudDocumentPrevious"));previous->setAccessibleName(QStringLiteral("上一页"));
    next=new Kda::Button(QStringLiteral("›"));next->setObjectName(QStringLiteral("hudDocumentNext"));next->setAccessibleName(QStringLiteral("下一页"));
    for(auto button:{previous,next}){button->setMinimumSize(24,22);button->setFixedSize(24,22);button->setProperty("labelPixels",12);button->setProperty("labelRegular",true);button->setProperty("toolbarAction",true);}
    position=new QLabel;position->setObjectName(QStringLiteral("hudDocumentPosition"));position->setAlignment(Qt::AlignCenter);position->setMinimumWidth(0);position->setFont(Kda::font(Kda::FontRole::Numeric,12));
    row->addWidget(previous);row->addWidget(position,1);row->addWidget(next);layout->addWidget(pager);
    connect(previous,&QPushButton::clicked,this,[this]{setPage(page-1);});connect(next,&QPushButton::clicked,this,[this]{setPage(page+1);});
    connect(text.documentLayout(),&QAbstractTextDocumentLayout::documentSizeChanged,this,[this]{if(!layingOut){pages=std::max(1,text.pageCount());page=std::clamp(page,0,pages-1);syncNavigation();canvas->update();}});
    syncNavigation();layoutDocument();
}
void HudPagedText::setHtml(const QString &value){
    if(html==value)return;html=value;text.setHtml(value);normalizeRichTextFonts(text);layoutDocument();
}
QString HudPagedText::toPlainText()const{return text.toPlainText();}
QTextDocument *HudPagedText::document(){return &text;}
const QTextDocument *HudPagedText::document()const{return &text;}
QWidget *HudPagedText::viewport()const{return canvas;}
int HudPagedText::pageIndex()const{return page;}
int HudPagedText::pageCount()const{return pages;}
void HudPagedText::setPage(int index){
    const int bounded=std::clamp(index,0,pages-1);if(page!=bounded){page=bounded;canvas->update();}
    syncNavigation();
}
void HudPagedText::layoutDocument(){
    if(layingOut)return;layingOut=true;
    text.setPageSize(QSizeF(std::max(1,canvas->width()),std::max(1,canvas->height())));
    // Force layout now, so page bounds, buttons, hit testing and reports agree.
    (void)text.documentLayout()->documentSize();pages=std::max(1,text.pageCount());page=std::clamp(page,0,pages-1);
    layingOut=false;syncNavigation();canvas->update();
}
void HudPagedText::syncNavigation(){
    position->setText(QStringLiteral("%1 / %2").arg(page+1).arg(pages));
    position->setAccessibleName(QStringLiteral("第 %1 页，共 %2 页").arg(page+1).arg(pages));
    previous->setEnabled(page>0);next->setEnabled(page+1<pages);
    canvas->setAccessibleDescription(pageText(page));
}
QPointF HudPagedText::documentPoint(const QPointF &point)const{return point+QPointF(0,page*text.pageSize().height());}
QUrl HudPagedText::anchorAt(const QPointF &point)const{
    return canvas->rect().contains(point.toPoint())?QUrl(text.documentLayout()->anchorAt(documentPoint(point))):QUrl();
}
QString HudPagedText::pageText(int index)const{
    QStringList lines;const qreal top=index*text.pageSize().height(),bottom=top+text.pageSize().height();
    for(auto block=text.begin();block.isValid();block=block.next()){
        const auto layout=block.layout();if(!layout)continue;const qreal y=text.documentLayout()->blockBoundingRect(block).top();
        for(int i=0;i<layout->lineCount();++i){const auto line=layout->lineAt(i);const qreal start=y+line.y();
            if(start+line.height()>top+.1&&start<bottom-.1)lines.append(block.text().mid(line.textStart(),line.textLength()));}
    }return lines.join('\n');
}
QJsonObject HudPagedText::report()const{
    bool textFits=canvas->width()>0&&canvas->height()>=text.defaultFont().pixelSize()+2;
    qreal minimumPixels=std::numeric_limits<qreal>::max();QSet<QString> families;
    const qreal top=page*text.pageSize().height(),bottom=top+text.pageSize().height();int visibleLines=0,horizontalOverflowLines=0;
    for(auto block=text.begin();block.isValid();block=block.next()){
        const auto layout=block.layout();if(!layout)continue;const auto blockRect=text.documentLayout()->blockBoundingRect(block);
        for(auto it=block.begin();!it.atEnd();++it){const auto fragment=it.fragment();if(!fragment.isValid()||fragment.charFormat().isImageFormat())continue;
            const auto pixels=fragmentGlyphPixels(block,fragment,&families);if(pixels>0)minimumPixels=std::min(minimumPixels,pixels);}
        for(int i=0;i<layout->lineCount();++i){const auto line=layout->lineAt(i);const qreal y=blockRect.top()+line.y();
            if(y+line.height()<=top+.1||y>=bottom-.1)continue;++visibleLines;
            const auto ink=line.naturalTextRect().translated(blockRect.topLeft());
            const bool horizontalFit=ink.left()>=-.5&&ink.right()<=canvas->width()+.5;
            horizontalOverflowLines+=!horizontalFit;
            textFits&=y>=top-.5&&y+line.height()<=bottom+.5&&horizontalFit;}
    }
    const bool hasTextMetrics=minimumPixels!=std::numeric_limits<qreal>::max();
    QStringList familyNames=families.values();familyNames.sort(Qt::CaseInsensitive);
    const QRect navigation(pager->mapTo(this,QPoint()),pager->size());
    return {{"page_count",pages},{"page_index",page},{"page_text",pageText(page)},{"full_text",toPlainText()},{"theme",Kda::HudStyle::identity()},{"navigation_style","quiet_inline_actions"},
        {"no_scrollbars",findChildren<QScrollBar*>().isEmpty()&&findChildren<QAbstractScrollArea*>().isEmpty()},
        {"text_fits",textFits},{"visible_lines",visibleLines},{"horizontal_overflow_lines",horizontalOverflowLines},{"navigation_fits",rect().contains(navigation)},
        {"all_content_pageable",true},{"uses_document_pagination",true},{"font_shrunk",hasTextMetrics&&minimumPixels<minimumBodyPixels-.01},
        {"effective_min_font_px",hasTextMetrics?QJsonValue(minimumPixels):QJsonValue()},
        {"glyph_fallback_families",QJsonArray::fromStringList(familyNames)},
        {"viewport_width",canvas->width()},{"viewport_height",canvas->height()}};
}
