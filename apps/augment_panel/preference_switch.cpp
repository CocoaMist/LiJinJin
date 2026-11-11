#include "preference_switch.h"
#include "kda_theme.h"
#include "kda_surface.h"
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>

PreferenceSwitch::PreferenceSwitch(const QString &text, QWidget *parent)
    : QCheckBox(text, parent),thumbMotion(this,260),hoverMotion(this,220) {
    setFixedHeight(Kda::Density::switchHeight);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover);
    connect(this,&QCheckBox::toggled,this,[this](bool checked){thumbMotion.retarget(checked?1:0);});
}
bool PreferenceSwitch::event(QEvent *event){
    const auto type=event->type();const bool accepted=QCheckBox::event(event);
    if(type==QEvent::Enter||type==QEvent::FocusIn)hoverMotion.retarget(1);
    if(type==QEvent::Leave||type==QEvent::FocusOut)hoverMotion.retarget(underMouse()||hasFocus()?1:0);
    return accepted;
}

QSize PreferenceSwitch::sizeHint() const { return {260, Kda::Density::switchHeight}; }
QSize PreferenceSwitch::minimumSizeHint() const {
    return {QFontMetrics(Kda::font(Kda::FontRole::Body,12)).horizontalAdvance(text())+56, Kda::Density::switchHeight};
}

bool PreferenceSwitch::hitButton(const QPoint &position) const {
    return contentsRect().contains(position);
}

void PreferenceSwitch::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    Kda::configurePainter(painter);
    const auto bounds = contentsRect();
    if (!isEnabled()) painter.setOpacity(.45);

    const QRectF track(bounds.right() - 35, bounds.top() + (bounds.height() - 18) / 2., 32, 18);
    const qreal phase=thumbMotion.value();
    const QColor trackColor = Kda::mixColor(Kda::color("raised"),Kda::color("silver").darker(118),phase);
    QLinearGradient enamel(track.topLeft(),track.bottomLeft());enamel.setColorAt(0,trackColor.lighter(110));enamel.setColorAt(1,trackColor.darker(110));painter.setBrush(enamel);
    painter.setPen(QPen(Kda::mixColor(Kda::color("border"),Kda::color("ice"),hoverMotion.value()), 1));
    painter.drawRoundedRect(track.adjusted(.5, .5, -.5, -.5), Kda::radius("control"), Kda::radius("control"));

    const QRectF thumb(track.left() + 3+14*phase, track.top() + 3, 12, 12);
    painter.setPen(Qt::NoPen);
    auto thumbColor = Kda::mixColor(Kda::color("silver"),Kda::color("bg"),phase);
    if (isDown()) thumbColor = thumbColor.darker(115);
    painter.setBrush(thumbColor);
    QPainterPath crystal;crystal.moveTo(thumb.center().x(),thumb.top());crystal.lineTo(thumb.right(),thumb.center().y());crystal.lineTo(thumb.center().x(),thumb.bottom());crystal.lineTo(thumb.left(),thumb.center().y());crystal.closeSubpath();painter.drawPath(crystal);
    auto glint=Kda::color("silver");glint.setAlpha(qRound(180-80*phase));painter.setPen(QPen(glint,.65));painter.drawLine(thumb.topLeft()+QPointF(0,thumb.height()/2),QPointF(thumb.center().x(),thumb.top()));

    auto labelFont = Kda::font(Kda::FontRole::Body, 12);
    labelFont.setWeight(QFont::Normal);
    const int labelLeft = bounds.left() + 6;
    const QRect label(labelLeft, bounds.top(), qMax(0, int(track.left()) - labelLeft - 12), bounds.height());
    painter.setFont(labelFont);
    painter.setPen(Kda::color("text"));
    painter.drawText(label, Qt::AlignLeft | Qt::AlignVCenter,
        QFontMetrics(labelFont).elidedText(text(), Qt::ElideRight, label.width()));

    if (hasFocus()) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(Kda::color("ice"), 1));
        painter.drawRoundedRect(QRectF(bounds).adjusted(1.5, 1.5, -1.5, -1.5), Kda::radius("tag"), Kda::radius("tag"));
    }
}
