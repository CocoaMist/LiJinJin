#pragma once
#include <QWidget>
#include <QJsonObject>

namespace Kda { class Transition; }

// A label surface only: no controls, game input or activation.
class CardHintWindow : public QWidget {
public:
    explicit CardHintWindow(int slot);
    void present(const QRect &, const QJsonObject &, double opacity);
    QJsonObject report() const;
protected:
    void paintEvent(QPaintEvent *) override;
    void hideEvent(QHideEvent *) override;
private:
    int slotIndex;
    QJsonObject content;
    Kda::Transition *highlight;
    int highlightCount = 0;
};
