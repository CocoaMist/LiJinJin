#pragma once
#include "hud_layout.h"
#include <QDialog>
#include <QImage>
#include <QMap>
class HudLayoutCanvas;
class QComboBox;
class QSpinBox;
class QLabel;
class QResizeEvent;

class HudLayoutEditor final : public QDialog {
public:
    HudLayoutEditor(const QRect &viewport,const HudLayout &,const QMap<QString,QRect> &defaults,
                    const QMap<QString,QImage> &previews={},QWidget *parent=nullptr);
    const HudLayout &editedLayout()const;
    // Persistence is owned by the caller; rejecting never touches preferences.
protected:
    void resizeEvent(QResizeEvent *)override;
private:
    HudLayoutCanvas *canvas;
    QComboBox *selection;
    QSpinBox *widthInput,*heightInput;
    QLabel *detail;
    QWidget *topBar,*bottomBar;
    void refreshControls();
    void positionControls();
};
