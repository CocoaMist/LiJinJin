#pragma once
#include <QCheckBox>
#include "kda_motion.h"

// Retain QCheckBox's state, keyboard and accessibility behavior while drawing
// a full preference row. Pointer handling stays in the Qt base class.
class PreferenceSwitch final : public QCheckBox {
public:
    explicit PreferenceSwitch(const QString &text, QWidget *parent = nullptr);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    QJsonObject motionReport() const{return {{"thumb",thumbMotion.report()},{"hover",hoverMotion.report()}};}

protected:
    bool hitButton(const QPoint &position) const override;
    void paintEvent(QPaintEvent *) override;
    bool event(QEvent *) override;
private:
    Kda::MotionValue thumbMotion,hoverMotion;
};
