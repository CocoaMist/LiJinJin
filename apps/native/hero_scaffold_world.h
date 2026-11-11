#pragma once
#include "catalog.h"
#include <memory>

// One source-backed actor in a declared empty plane. This is an input/visual
// alignment scaffold, not a certified Garen skill or full combat simulator.
class HeroScaffoldWorld {
public:
    explicit HeroScaffoldWorld(const Catalog &catalog);
    ~HeroScaffoldWorld();
    HeroScaffoldWorld(const HeroScaffoldWorld &) = delete;
    HeroScaffoldWorld &operator=(const HeroScaffoldWorld &) = delete;
    QJsonObject reset(const QJsonObject &request = {});
    QJsonObject step(const QJsonObject &request);
    QJsonObject observe() const;
private:
    struct Data;
    std::unique_ptr<Data> m_data;
};
