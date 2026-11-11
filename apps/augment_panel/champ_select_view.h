#pragma once
#include <QJsonObject>
#include <QWidget>

class Catalog;
class HudAssets;
struct GameState;
namespace Kda { class SparkleField; }

// Pure, bounded projections of a sanitized current champion-select snapshot.
// No artwork access, clocks, network, ranking or GUI work runs in this model.
// JSON input: active, stage="champ_select", self_champion_id, roster (ALLY,
// champion_id, self, optional visible), bench_champion_ids, optional bench_known.
// A missing bench is unknown; an explicitly known empty bench is unavailable.
QJsonObject buildChampSelectModel(const Catalog &,const QJsonObject &);
// The owner gates championSelectFresh() before supplying GameState. Projection
// never samples a clock and never extends an observation's lifetime.
QJsonObject buildChampSelectModel(const Catalog &,const GameState &);

// Passive content only. The owner retains its window/input-transparency policy
// and owns the shared HudAssets redraw notification, as with the other HUDs.
class ChampSelectView final : public QWidget {
public:
    ChampSelectView(const Catalog &,HudAssets *,QWidget *parent=nullptr);
    void setState(const GameState &);
    void setState(const QJsonObject &);
    QJsonObject model() const;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    QJsonObject report() const;
protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
private:
    const Catalog &catalog;
    HudAssets *assets;
    QJsonObject snapshot;
    Kda::SparkleField *flow;
    void applyModel(const QJsonObject &);
    void syncFlowRegion();
};
