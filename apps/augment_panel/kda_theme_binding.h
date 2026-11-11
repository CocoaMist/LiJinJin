#pragma once
#include "kda_theme.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QJsonObject>
#include <QPointer>
#include <QVector>
#include <QWidget>
#include <algorithm>
#include <functional>
#include <utility>

namespace Kda::Detail {
class ThemeStyleBinding final:public QObject {
public:
    ThemeStyleBinding(QWidget *widget,std::function<QString()> make):QObject(widget),owner(widget),builder(std::move(make)){}
    QPointer<QWidget> owner;std::function<QString()> builder;bool dirty=true;
};
class ThemeStyleRegistry final:public QObject {
public:
    explicit ThemeStyleRegistry(QObject *parent):QObject(parent){qApp->installEventFilter(this);}
    void bind(QWidget *owner,std::function<QString()> builder){
        if(!owner||!builder)return;
        auto binding=find(owner);
        if(binding){binding->builder=std::move(builder);binding->dirty=true;}
        else{binding=new ThemeStyleBinding(owner,std::move(builder));bindings.append(binding);}
        owner->setProperty("kdaThemeStyleManaged",true);apply(binding);
    }
    void refresh(){
        bindings.erase(std::remove_if(bindings.begin(),bindings.end(),[](const auto &entry){return entry.isNull();}),bindings.end());
        QElapsedTimer timer;timer.start();lastApplies=lastDeferred=lastUnchanged=0;switching=true;
        const auto current=bindings;for(const auto &binding:current)if(binding)binding->dirty=true;
        for(const auto &binding:current)if(binding&&binding->owner){if(binding->owner->isVisible())apply(binding);else{++lastDeferred;++deferred;}}
        switching=false;lastElapsedNs=timer.nsecsElapsed();++switches;
    }
    QJsonObject report()const{
        int count=0,dirty=0;for(const auto &binding:bindings)if(binding&&binding->owner){++count;dirty+=binding->dirty;}
        return {{"installed",true},{"bound_owners",count},{"dirty_owners",dirty},{"switch_count",qint64(switches)},
            {"apply_count",qint64(applies)},{"visible_apply_count",qint64(visibleApplies)},{"deferred_count",qint64(deferred)},
            {"unchanged_skip_count",qint64(unchanged)},{"last_switch_apply_count",lastApplies},{"last_switch_deferred_count",lastDeferred},
            {"last_switch_unchanged_count",lastUnchanged},{"last_switch_elapsed_ms",lastElapsedNs/1.e6},{"apply_total_ms",applyNs/1.e6},
            {"last_apply_ms",lastApplyNs/1.e6},{"max_apply_ms",maxApplyNs/1.e6},{"application_stylesheet_mutated",false},
            {"hidden_styles_apply_on_show",true},{"looping_timer",false}};
    }
private:
    QPointer<ThemeStyleBinding> find(QWidget *owner)const{for(const auto &binding:bindings)if(binding&&binding->owner==owner)return binding;return {};}
    void apply(QPointer<ThemeStyleBinding> binding){
        if(applying||!binding||!binding->owner||!binding->dirty||!binding->builder)return;
        applying=true;const auto owner=binding->owner;const auto sheet=binding->builder();
        if(!binding||!owner){applying=false;return;}
        const bool changed=owner->styleSheet()!=sheet;
        if(changed){QElapsedTimer timer;timer.start();owner->setStyleSheet(sheet);lastApplyNs=timer.nsecsElapsed();applyNs+=lastApplyNs;maxApplyNs=std::max(maxApplyNs,lastApplyNs);++applies;if(owner&&owner->isVisible())++visibleApplies;if(switching)++lastApplies;}
        else{++unchanged;if(switching)++lastUnchanged;}
        if(binding&&owner){binding->dirty=false;owner->setProperty("kdaThemeStyleAppliedSkin",skin());owner->update();}
        applying=false;
    }
    void adopt(QWidget *widget){
        if(!widget||!widget->isWindow()||widget->windowType()==Qt::ToolTip||widget->windowType()==Qt::Desktop||!widget->styleSheet().isEmpty())return;
        // Popups owned by an existing themed window inherit that owner's full
        // sheet. Do not raise generic font/padding rules above its custom rules.
        for(auto ancestor=widget->parentWidget();ancestor;ancestor=ancestor->parentWidget())if(auto binding=find(ancestor)){apply(binding);return;}
        bind(widget,[]{return overlayStyleSheet();});
    }
    bool eventFilter(QObject *watched,QEvent *event)override{
        if(!applying&&(event->type()==QEvent::Show||event->type()==QEvent::Polish))if(auto widget=qobject_cast<QWidget*>(watched)){
            if(auto binding=find(widget)){if(event->type()==QEvent::Show||widget->isVisible())apply(binding);}else adopt(widget);
        }
        return QObject::eventFilter(watched,event);
    }
    QVector<QPointer<ThemeStyleBinding>> bindings;bool applying=false,switching=false;
    quint64 applies=0,visibleApplies=0,deferred=0,unchanged=0,switches=0;
    int lastApplies=0,lastDeferred=0,lastUnchanged=0;qint64 applyNs=0,lastApplyNs=0,maxApplyNs=0,lastElapsedNs=0;
};
inline QPointer<ThemeStyleRegistry> &themeStyleInstance(){static QPointer<ThemeStyleRegistry> instance;return instance;}
inline ThemeStyleRegistry *themeStyleRegistry(){auto &instance=themeStyleInstance();if(qApp&&!instance)instance=new ThemeStyleRegistry(qApp);return instance;}
}
