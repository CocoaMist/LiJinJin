#pragma once

#include <QString>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>

// One immutable snapshot gets one shared, monotonic work budget. Cancellation
// never turns an incomplete enumeration into an authorized recommendation.
class RecommendationWork final {
public:
    explicit RecommendationWork(qint64 maximumMilliseconds=4000)
        : started(std::chrono::steady_clock::now()),maximumMs(std::max<qint64>(0,maximumMilliseconds)) {}
    void cancel(){cancelled.store(true,std::memory_order_relaxed);}
    qint64 elapsedMs()const{return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();}
    qint64 remainingMs()const{return cancelled.load(std::memory_order_relaxed)?0:std::max<qint64>(0,maximumMs-elapsedMs());}
    bool stopped()const{return cancelled.load(std::memory_order_relaxed)||elapsedMs()>=maximumMs;}
    QString stopReason()const{
        if(cancelled.load(std::memory_order_relaxed))return QStringLiteral("cancelled");
        return elapsedMs()>=maximumMs?QStringLiteral("time_budget_exceeded"):QString();
    }
private:
    const std::chrono::steady_clock::time_point started;
    const qint64 maximumMs;
    std::atomic_bool cancelled{false};
};
using RecommendationWorkPtr=std::shared_ptr<RecommendationWork>;
