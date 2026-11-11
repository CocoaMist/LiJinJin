#pragma once

#include "kda_theme.h"
#include <QJsonObject>
#include <QWidget>

class Catalog;
class HudAssets;
struct GameState;

class CornerCombatHud final : public QWidget {
public:
    static constexpr int compactHeight=194;
    static constexpr int expandedHeight=317;

    explicit CornerCombatHud(HudAssets *assets);
    void setCombatState(const Catalog &,const GameState &,const QJsonObject &summary,bool demo);
    bool ready() const {return snapshot.value("available").toBool();}
    bool selfDead() const {return snapshot.value("self_dead").toBool();}
    bool wantsDetails() const {return selfDead()&&snapshot.value("focus").toInt(-1)>=0;}
    void present(const QRect &,bool details,double opacity);
    QJsonObject report() const;

protected:
    void paintEvent(QPaintEvent *) override;

private:
    HudAssets *assets;
    QJsonObject snapshot;
    bool expanded=false;
};
