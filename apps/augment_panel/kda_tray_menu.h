#pragma once

#include "kda_surface.h"
#include <QMenu>
#include <QStyleOption>

namespace Kda {
class TrayMenu final : public QMenu {
public:
    explicit TrayMenu(QWidget *parent=nullptr):QMenu(parent){
        auto face=Kda::font(FontRole::Body,15,QFont::Medium);
        face.setHintingPreference(QFont::PreferNoHinting);
        setFont(face);
        // The dashboard's QWidget rule also matches its popup windows.
        setStyleSheet(QStringLiteral("QMenu { font-size:15px; font-weight:500; }"));
    }

protected:
    void paintEvent(QPaintEvent *event) override {
        // Qt still owns layout, the active action and input. Keep its painter
        // for screen-constrained menus that need native scroll controls.
        if(sizeHint().height()>height()||columnCount()>1){
            QMenu::paintEvent(event);
            return;
        }

        OverlayCanvas canvas(this);
        auto &p=canvas.paint();
        QStyleOptionMenuItem panel;
        panel.initFrom(this);
        panel.state=QStyle::State_None;
        style()->drawPrimitive(QStyle::PE_PanelMenu,&panel,&p,this);
        for(auto action:actions()){
            const auto bounds=actionGeometry(action);
            if(bounds.isEmpty())continue;
            QStyleOptionMenuItem item;
            initStyleOption(&item,action);
            item.rect=bounds;
            p.save();
            p.setClipRect(bounds);
            p.setFont(item.font);
            style()->drawControl(QStyle::CE_MenuItem,&item,&p,this);
            p.restore();
        }
    }
};
}
