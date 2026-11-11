#pragma once
#include <QFutureWatcher>
#include <QJsonObject>
#include <QString>
#include <utility>

// GUI-thread ownership outlives QFuture::isRunning(): a finished future still
// owns its watcher and request metadata until its completion has been consumed.
class PendingFutureCompletion {
public:
    PendingFutureCompletion()=default;
    PendingFutureCompletion(const PendingFutureCompletion &)=delete;
    PendingFutureCompletion &operator=(const PendingFutureCompletion &)=delete;
    class Consumption {
    public:
        Consumption(const Consumption &)=delete;
        Consumption &operator=(const Consumption &)=delete;
        ~Consumption(){release();}
        explicit operator bool() const noexcept{return owner_!=nullptr;}
        void release() noexcept{
            if(auto owner=std::exchange(owner_,nullptr)){owner->pending_=false;owner->consuming_=false;}
        }
    private:
        friend class PendingFutureCompletion;
        explicit Consumption(PendingFutureCompletion *owner):owner_(owner){}
        PendingFutureCompletion *owner_;
    };
    bool tryStart() noexcept{if(pending_)return false;pending_=true;return true;}
    bool pending() const noexcept{return pending_;}
    Consumption consume() noexcept{
        if(!pending_||consuming_)return Consumption(nullptr);
        consuming_=true;return Consumption(this);
    }
private:
    bool pending_=false,consuming_=false;
};

struct CompletedJsonFuture {
    QJsonObject value;
    QString error;
    bool available() const noexcept{return error.isEmpty();}
    bool interrupted() const{
        return value.value(QStringLiteral("status")).toString()==QStringLiteral("interrupted")
            ||value.value(QStringLiteral("computation_complete"))==QJsonValue(false);
    }
};

inline CompletedJsonFuture completedJsonFuture(const QFutureWatcher<QJsonObject> &watcher){
    // Never enter a blocking result() path, including an unexpected callback.
    if(!watcher.isFinished())return {{},QStringLiteral("worker_completion_not_finished")};
    try{
        const auto future=watcher.future();
        if(future.isCanceled()||future.resultCount()!=1)return {{},QStringLiteral("worker_result_unavailable")};
        const auto value=watcher.result();
        if(value.isEmpty())return {{},QStringLiteral("worker_result_empty")};
        return {value,{}};
    }catch(...){return {{},QStringLiteral("worker_result_exception")};}
}
