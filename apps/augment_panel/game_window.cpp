#include "game_window.h"
#include <QGuiApplication>
#include <QHash>
#include <QJsonArray>
#include <QScreen>
#include <algorithm>
#include <cmath>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace GameWindow {
namespace {
QJsonArray rectangle(const QRect &r){return {r.x(),r.y(),r.width(),r.height()};}
bool usable(const Surface &s){return s.valid&&s.visible&&!s.minimized&&s.clientPixels.width()>=320&&s.clientPixels.height()>=200;}
#ifdef Q_OS_WIN
QString executableForProcess(quint32 pid){
    const auto process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if(!process)return {};
    wchar_t path[32768];DWORD length=32768;
    const bool okay=QueryFullProcessImageNameW(process,0,path,&length);CloseHandle(process);
    return okay?QString::fromWCharArray(path,int(length)):QString();
}
quint32 processId(HWND window){DWORD pid=0;GetWindowThreadProcessId(window,&pid);return pid;}
bool windowCloaked(HWND window){
    using QueryAttribute=HRESULT(WINAPI *)(HWND,DWORD,PVOID,DWORD);
    static const auto query=[] {
        const auto library=LoadLibraryExW(L"dwmapi.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        return library?reinterpret_cast<QueryAttribute>(GetProcAddress(library,"DwmGetWindowAttribute")):nullptr;
    }();
    constexpr DWORD cloakedAttribute=14; // DWMWA_CLOAKED
    DWORD cloaked=0;
    return query&&SUCCEEDED(query(window,cloakedAttribute,&cloaked,sizeof(cloaked)))&&cloaked!=0;
}
#endif
}
bool isGameExecutable(const QString &path){
    auto normalized=path;normalized.replace(QChar(u'\\'),QChar(u'/'));
    return normalized.section(QChar(u'/'),-1).compare(QStringLiteral("League of Legends.exe"),Qt::CaseInsensitive)==0;
}
bool isClientExecutable(const QString &path){
    auto normalized=path;normalized.replace(QChar(u'\\'),QChar(u'/'));
    return normalized.section(QChar(u'/'),-1).compare(QStringLiteral("LeagueClientUx.exe"),Qt::CaseInsensitive)==0;
}
QRect logicalClient(const QRect &pixels,const QRect &nativeMonitor,const QRect &qtScreen,qreal scale){
    if(pixels.isEmpty()||nativeMonitor.isEmpty()||qtScreen.isEmpty()||!std::isfinite(scale)||scale<=0)return {};
    const auto origin=qtScreen.topLeft()+QPoint(qRound((pixels.x()-nativeMonitor.x())/scale),qRound((pixels.y()-nativeMonitor.y())/scale));
    return {origin,QSize(qRound(pixels.width()/scale),qRound(pixels.height()/scale))};
}
QString surfaceMode(const Surface &s){
    if(s.minimized)return QStringLiteral("minimized");
    if(!s.valid)return QStringLiteral("unavailable");
    if(s.framed)return QStringLiteral("windowed");
    const auto a=s.clientPixels,b=s.monitorPixels;
    if(std::abs(a.left()-b.left())<=2&&std::abs(a.top()-b.top())<=2&&std::abs(a.right()-b.right())<=2&&std::abs(a.bottom()-b.bottom())<=2)
        return QStringLiteral("fullscreen_or_borderless");
    return QStringLiteral("borderless_windowed");
}
int screenIndex(const Surface &surface,const QVector<Screen> &screens){
    int match=-1;
    for(int i=0;i<screens.size();++i){
        const auto &s=screens[i];if(!std::isfinite(s.scale)||s.scale<=0)continue;
        // Windows Qt keeps native monitor origins while scaling monitor sizes.
        // QScreen::name may be a friendly model name instead of DISPLAY1.
        const auto n=surface.monitorPixels;
        const bool sameOrigin=(s.geometry.topLeft()-n.topLeft()).manhattanLength()<=2;
        const bool sameSize=std::abs(qRound(s.geometry.width()*s.scale)-n.width())<=2&&std::abs(qRound(s.geometry.height()*s.scale)-n.height())<=2;
        if(!sameOrigin||!sameSize)continue;
        if(match>=0&&(screens[match].geometry.topLeft()!=s.geometry.topLeft()||std::abs(screens[match].scale-s.scale)>.001))return -1;
        match=i;
    }
    return match;
}
int selectCandidate(const QVector<Candidate> &candidates,quintptr previous){
    int best=-1;
    for(int i=0;i<candidates.size();++i){
        const auto &c=candidates[i];if(!c.window||!c.process||!isGameExecutable(c.executable)||!usable(c.surface))continue;
        if(best<0){best=i;continue;}
        const auto &old=candidates[best];
        if(c.foreground!=old.foreground){if(c.foreground)best=i;continue;}
        if((c.window==previous)!=(old.window==previous)){if(c.window==previous)best=i;continue;}
        if(qint64(c.surface.clientPixels.width())*c.surface.clientPixels.height()>qint64(old.surface.clientPixels.width())*old.surface.clientPixels.height())best=i;
    }
    return best;
}
int selectClientCandidate(const QVector<Candidate> &candidates,quint32 expectedProcess){
    if(!expectedProcess)return -1;
    int selected=-1;
    for(int i=0;i<candidates.size();++i){
        const auto &c=candidates[i];
        if(!c.window||c.process!=expectedProcess||!isClientExecutable(c.executable)||!usable(c.surface))continue;
        // Two usable windows of the bound process are ambiguous. A previous
        // handle, enumeration order or window area must never pick one.
        if(selected>=0)return -1;
        selected=i;
    }
    return selected;
}
bool State::overlayAllowed()const{return found&&visible&&!minimized&&foreground&&!client.isEmpty();}
QJsonObject State::report()const{
    return {{"found",found},{"visible",visible},{"minimized",minimized},{"foreground",foreground},{"overlay_allowed",overlayAllowed()},
        {"mode",mode},{"client_pixels",rectangle(clientPixels)},{"client_logical",rectangle(client)},{"qt_scale",scale},
        {"position_basis",positionBasis},{"exclusive_fullscreen_verified",false},{"game_hooks",false}};
}
Surface nativeSurface(quintptr id){
    Surface result;
#ifdef Q_OS_WIN
    const auto window=reinterpret_cast<HWND>(id);if(!window||!IsWindow(window))return result;
    // Virtual desktops can cloak an HWND without clearing WS_VISIBLE.
    result.visible=IsWindowVisible(window)&&!windowCloaked(window);result.minimized=IsIconic(window);
    const auto style=GetWindowLongPtrW(window,GWL_STYLE);result.framed=(style&(WS_CAPTION|WS_THICKFRAME))!=0;
    if(result.minimized)return result;
    RECT client{};POINT origin{};MONITORINFOEXW monitor{};monitor.cbSize=sizeof(monitor);
    if(!GetClientRect(window,&client)||!ClientToScreen(window,&origin)||!GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor))return result;
    result.clientPixels=QRect(origin.x,origin.y,client.right-client.left,client.bottom-client.top);
    result.monitorPixels=QRect(monitor.rcMonitor.left,monitor.rcMonitor.top,monitor.rcMonitor.right-monitor.rcMonitor.left,monitor.rcMonitor.bottom-monitor.rcMonitor.top);
    result.monitorName=QString::fromWCharArray(monitor.szDevice);result.valid=!result.clientPixels.isEmpty();
#else
    Q_UNUSED(id);
#endif
    return result;
}
Tracker::Tracker(Target observationTarget):target(observationTarget){
    clock.start();
    if(target==Target::Client)current.positionBasis=QStringLiteral("league_client_area");
}
void Tracker::setExpectedProcess(quint32 process){
    if(target!=Target::Client||expectedProcess==process)return;
    expectedProcess=process;trackedWindow=0;trackedProcess=0;lastScan=-500;
    current={};current.positionBasis=QStringLiteral("league_client_area");
}
const State &Tracker::refresh(){
    current={};
    const bool client=target==Target::Client;
    if(client){
        current.positionBasis=QStringLiteral("league_client_area");
        if(!expectedProcess){trackedWindow=0;trackedProcess=0;return current;}
    }
#ifdef Q_OS_WIN
    const auto foreground=GetForegroundWindow();const auto foregroundProcess=foreground?processId(foreground):0;
    auto handle=reinterpret_cast<HWND>(trackedWindow);
    if(handle&&(!IsWindow(handle)||processId(handle)!=trackedProcess||(client&&trackedProcess!=expectedProcess))){trackedWindow=0;trackedProcess=0;handle=nullptr;lastScan=-500;}
    // Keep the verified handle on every 50 ms update; enumerate processes only
    // twice a second. This avoids repeated OpenProcess calls while dragging.
    if(clock.elapsed()-lastScan>=500){
        struct Search {QVector<Candidate> candidates;QHash<quint32,QString> executablePaths;quint32 foregroundProcess=0,expectedProcess=0;bool client=false;} search;
        search.foregroundProcess=foregroundProcess;search.expectedProcess=expectedProcess;search.client=client;
        EnumWindows([](HWND window,LPARAM value)->BOOL{
            auto &search=*reinterpret_cast<Search*>(value);
            if(!IsWindowVisible(window)||(!search.client&&IsIconic(window)))return TRUE;
            const auto pid=processId(window);
            if(!pid||(search.client&&pid!=search.expectedProcess))return TRUE;
            // A scan can see several top-level windows owned by the same PID.
            // Keep process identities only for this scan, so PID reuse is safe.
            auto path=search.executablePaths.constFind(pid);
            if(path==search.executablePaths.cend())path=search.executablePaths.insert(pid,executableForProcess(pid));
            if(search.client?!isClientExecutable(path.value()):!isGameExecutable(path.value()))return TRUE;
            search.candidates.append({reinterpret_cast<quintptr>(window),pid,path.value(),nativeSurface(reinterpret_cast<quintptr>(window)),pid==search.foregroundProcess});return TRUE;
        },reinterpret_cast<LPARAM>(&search));
        int selected=client?selectClientCandidate(search.candidates,expectedProcess):selectCandidate(search.candidates,trackedWindow);
        // A sole minimized, verified client can still report its state, but has
        // no client rectangle and cannot authorize an overlay.
        if(client&&selected<0&&search.candidates.size()==1&&search.candidates[0].surface.minimized)selected=0;
        if(selected>=0){trackedWindow=search.candidates[selected].window;trackedProcess=search.candidates[selected].process;handle=reinterpret_cast<HWND>(trackedWindow);}
        else if(client){trackedWindow=0;trackedProcess=0;handle=nullptr;}
        lastScan=clock.elapsed();
    }
    if(!handle)return current;
    const auto surface=nativeSurface(trackedWindow);current.found=true;current.visible=surface.visible;current.minimized=surface.minimized;
    current.foreground=client?foreground==handle:foregroundProcess==trackedProcess;current.mode=surfaceMode(surface);current.clientPixels=surface.clientPixels;
    if(!usable(surface))return current;
    QVector<Screen> mappings;const auto screens=QGuiApplication::screens();
    for(const auto screen:screens)mappings.append({screen->name(),screen->geometry(),screen->devicePixelRatio()});
    const int mapped=screenIndex(surface,mappings);
    if(mapped>=0){const auto &screen=mappings[mapped];current.scale=screen.scale;current.client=logicalClient(surface.clientPixels,surface.monitorPixels,screen.geometry,current.scale);}
#endif
    return current;
}
}
