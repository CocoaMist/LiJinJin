#pragma once
#include <QCoreApplication>
#include <QEvent>
#include <QObject>
#include <QPointer>
#include <QSettings>
#include <QTimer>
#include <functional>

namespace Kda {
// Keep settings visible in memory immediately; finish disk writes after the
// finite skin animation. Qt's own posted flush must obey the same deadline.
class AppearanceSave final : public QObject {
public:
    AppearanceSave(QSettings &settings,QObject *owner,std::function<void(bool)> completed)
        :QObject(owner),store(&settings),completion(std::move(completed)){
        timer.setSingleShot(true);timer.setInterval(1400);store->installEventFilter(this);
        connect(&timer,&QTimer::timeout,this,[this]{flush();});
    }
    ~AppearanceSave()override{
        completion={};flush();if(store)store->removeEventFilter(this);
    }
    void defer(){pending=true;timer.start();}
    bool isPending()const{return pending;}
    int flushes()const{return completedFlushes;}
    void flush(){
        if(!pending)return;pending=false;timer.stop();if(!store)return;
        // Deliver the suppressed native request too, so QSettings clears its
        // internal pending flag and future ordinary writes retain Qt behavior.
        QEvent request(QEvent::UpdateRequest);QCoreApplication::sendEvent(store,&request);store->sync();++completedFlushes;
        if(completion)completion(store->status()==QSettings::NoError);
    }
protected:
    bool eventFilter(QObject *object,QEvent *event)override{
        if(object==store&&pending&&event->type()==QEvent::UpdateRequest)return true;
        return QObject::eventFilter(object,event);
    }
private:
    QPointer<QSettings> store;QTimer timer;std::function<void(bool)> completion;
    bool pending=false;int completedFlushes=0;
};
}
