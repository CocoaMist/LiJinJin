#pragma once
#include "shop_recommendations.h"

namespace ShopRecommendations {

// Pure projection: no I/O, model work, clock reads, network or new ranking.
// gatedNextPurchase must already have passed the caller's fresh/context/hero/
// primary_purchase_visible gates. Passing an offline plan does not enable it:
// only a direct item_id and its verified buyable_components are consumed.
// Public records remain descriptive references with their original patch.
Request projectRecommendation(const Context &context, const QSet<int> &verifiedCatalogItems,
                              const QJsonObject &equipmentReference,
                              const QJsonObject &gatedNextPurchase = {});

} // namespace ShopRecommendations
