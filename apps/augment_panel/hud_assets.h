#pragma once
#include <QObject>
#include <QJsonObject>
#include <QPixmap>
#include <functional>
#include <memory>

class Catalog;

// Public artwork only. First requests return a null pixmap while local files are
// verified/decoded off the GUI thread. The HUD owns its honest missing-icon glyph.
class HudAssets : public QObject {
public:
    explicit HudAssets(const Catalog &catalog, QObject *parent = nullptr);
    ~HudAssets() override;
    QPixmap item(int id);
    QPixmap champion(int id);
    QPixmap augment(int id);
    QJsonObject report() const;
    std::function<void()> onChanged;
private:
    struct State;
    std::unique_ptr<State> state;
    QPixmap request(const QString &kind, int id);
};
