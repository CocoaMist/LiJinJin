#include "game_window.h"
#include "overlay_layout.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScreen>
#include <QThread>
#include <QWidget>
#include <cmath>
#include <cstdio>
#include <limits>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
void settle(){for(int i=0;i<12;++i){QApplication::processEvents();QThread::msleep(5);}}
}
int main(int argc,char **argv){
    QApplication app(argc,argv);QJsonArray checks,frames;int failures=0;
    const auto check=[&](const QString &name,bool passed){checks.append(QJsonObject{{"name",name},{"passed",passed}});if(!passed){++failures;std::fprintf(stderr,"FAIL %s\n",name.toUtf8().constData());}};
    check("only_game_executable_is_admitted",GameWindow::isGameExecutable(QStringLiteral("C:/Riot Games/League of Legends/Game/League of Legends.exe"))&&GameWindow::isGameExecutable(QStringLiteral("C:\\Riot Games\\league OF legends.EXE"))&&!GameWindow::isGameExecutable(QStringLiteral("LeagueClientUx.exe"))&&!GameWindow::isGameExecutable(QStringLiteral("League of Legends.exe.backup")));
    check("only_client_ux_executable_is_admitted",GameWindow::isClientExecutable(QStringLiteral("C:/Riot Games/League of Legends/LeagueClientUx.exe"))&&GameWindow::isClientExecutable(QStringLiteral("C:\\Riot Games\\LEAGUECLIENTUX.EXE"))&&!GameWindow::isClientExecutable(QStringLiteral("LeagueClient.exe"))&&!GameWindow::isClientExecutable(QStringLiteral("LeagueClientUxRender.exe"))&&!GameWindow::isClientExecutable(QStringLiteral("LeagueClientUx.exe.backup"))&&!GameWindow::isClientExecutable(QStringLiteral("League of Legends.exe")));
    for(const auto scale:{1.,1.25,1.5,2.})for(const auto origin:{QPoint(0,0),QPoint(-2560,-120),QPoint(1920,300)})for(const auto size:{QSize(1024,768),QSize(1920,1080),QSize(2560,1080),QSize(3440,1440),QSize(3840,1600)}){
        const QRect nativeMonitor(origin,QSize(3840,2160)),qtScreen(origin,QSize(qRound(3840/scale),qRound(2160/scale)));
        const QRect pixels(origin+QPoint(90,60),size);const auto logical=GameWindow::logicalClient(pixels,nativeMonitor,qtScreen,scale);
        const auto lane=safeOverlayPlacement(logical);
        const bool mapping=logical.topLeft()==origin+QPoint(qRound(90/scale),qRound(60/scale))&&logical.size()==QSize(qRound(size.width()/scale),qRound(size.height()/scale));
        const bool safeLane=logical.width()<954?lane.right.isEmpty():logical.contains(lane.right)&&overlayAvoidsProtectedAreas(lane);
        check(QStringLiteral("client_mapping_%1x%2_%3x_%4_%5").arg(size.width()).arg(size.height()).arg(scale).arg(origin.x()).arg(origin.y()),mapping&&safeLane);
        const auto moved=GameWindow::logicalClient(pixels.translated(120,80),nativeMonitor,qtScreen,scale);
        check(QStringLiteral("client_movement_%1x%2_%3x_%4_%5").arg(size.width()).arg(size.height()).arg(scale).arg(origin.x()).arg(origin.y()),moved.topLeft()-logical.topLeft()==QPoint(qRound(210/scale)-qRound(90/scale),qRound(140/scale)-qRound(60/scale)));
    }
    const QRect compactViewport(-1024,40,1024,768);
    const auto compactLane=safeOverlayPlacement(compactViewport);
    check("compact_1024_lane_keeps_shop_and_all_protected_areas_clear",compactLane.right==QRect(-1023,140,150,414)
          &&!compactLane.collapsed&&compactViewport.contains(compactLane.right)&&overlayAvoidsProtectedAreas(compactLane)
          &&shopReferenceArea(compactViewport)==QRect(-871,101,819,591)&&combatCornerPlacement(compactLane,{},194).isEmpty());
    check("compact_lane_preserves_width_and_height_safety_limits",safeOverlayPlacement(QRect(0,0,953,768)).right.isEmpty()
          &&safeOverlayPlacement(QRect(0,0,1024,160)).right.isEmpty()
          &&safeOverlayPlacement(QRect(0,0,954,768)).right==QRect(1,100,142,414)
          &&safeOverlayPlacement(QRect(0,0,1023,768)).right==QRect(1,100,150,414)
          &&safeOverlayPlacement(QRect(0,0,1119,768)).right==QRect(1,100,150,414)
          &&safeOverlayPlacement(QRect(0,0,1120,768)).right==QRect(10,100,150,414)
          &&safeOverlayPlacement(QRect(0,0,1549,982)).right==QRect(10,125,214,534));
    const QRect highDpiPixels(-1920,300,1920,1080),highDpiScreen(-1920,300,960,540);
    const auto highDpiClient=GameWindow::logicalClient(highDpiPixels,highDpiPixels,highDpiScreen,2.);
    const auto highDpiLane=safeOverlayPlacement(highDpiClient);
    check("full_hd_at_200_percent_leaves_a_safe_coach_lane_on_an_offset_screen",
          highDpiClient==highDpiScreen&&highDpiLane.right==QRect(-1919,372,143,287)
          &&highDpiClient.contains(highDpiLane.right)&&overlayAvoidsProtectedAreas(highDpiLane)
          &&highDpiLane.right.right()<shopReferenceArea(highDpiClient).left());
    check("unmapped_or_invalid_scale_never_becomes_a_primary_screen_guess",GameWindow::logicalClient(QRect(0,0,1920,1080),QRect(),QRect(0,0,1920,1080),1).isEmpty()&&GameWindow::logicalClient(QRect(0,0,1920,1080),QRect(0,0,1920,1080),QRect(0,0,1920,1080),std::numeric_limits<double>::quiet_NaN()).isEmpty());
    GameWindow::Surface surface;surface.valid=true;surface.visible=true;surface.clientPixels=QRect(120,140,1280,720);surface.monitorPixels=QRect(0,0,2560,1080);surface.framed=true;
    check("windowed_mode_uses_client_instead_of_frame",GameWindow::surfaceMode(surface)==QStringLiteral("windowed"));surface.framed=false;
    check("borderless_window_is_distinct_from_monitor_covering_surface",GameWindow::surfaceMode(surface)==QStringLiteral("borderless_windowed"));surface.clientPixels=surface.monitorPixels;
    check("monitor_covering_mode_does_not_claim_exclusive_fullscreen_detection",GameWindow::surfaceMode(surface)==QStringLiteral("fullscreen_or_borderless"));
    surface.monitorName=QStringLiteral("\\\\.\\DISPLAY1");
    check("friendly_qt_monitor_names_map_by_native_geometry",GameWindow::screenIndex(surface,{{QStringLiteral("model-name"),QRect(0,0,2560,1080),1.}})==0);
    check("mixed_dpi_monitor_mapping_uses_native_origin_and_pixel_size",GameWindow::screenIndex(surface,{{QStringLiteral("remote"),QRect(2560,0,1920,1080),1.},{QStringLiteral("ultrawide"),QRect(0,0,1280,540),2.}})==1);
    check("unmatched_remote_screen_is_not_used",GameWindow::screenIndex(surface,{{QStringLiteral("remote"),QRect(0,0,3840,2160),1.}})==-1);
    GameWindow::Candidate first{1,100,QStringLiteral("League of Legends.exe"),surface,false},second=first;second.window=2;second.process=200;
    check("stable_window_kept_when_enum_order_changes",GameWindow::selectCandidate({second,first},1)==1);second.foreground=true;
    check("foreground_game_wins_over_cached_background_game",GameWindow::selectCandidate({first,second},1)==1);
    second.executable=QStringLiteral("LeagueClient.exe");check("foreground_launcher_cannot_steal_game_geometry",GameWindow::selectCandidate({second,first},1)==1);
    first.surface.minimized=true;check("minimized_and_non_game_candidates_not_selected",GameWindow::selectCandidate({first,second},1)==-1);first.surface.minimized=false;first.surface.visible=false;
    check("hidden_game_window_not_selected",GameWindow::selectCandidate({first},1)==-1);
    GameWindow::Candidate ux{11,310,QStringLiteral("LeagueClientUx.exe"),surface,false},otherUx=ux;
    otherUx.window=12;otherUx.process=311;
    check("client_only_select_phase_does_not_require_a_game_executable",GameWindow::selectClientCandidate({ux},310)==0&&GameWindow::selectCandidate({ux})==-1);
    check("missing_connected_client_process_fails_closed",GameWindow::selectClientCandidate({ux},0)==-1);
    check("different_account_client_process_cannot_supply_geometry",GameWindow::selectClientCandidate({otherUx,ux},310)==1&&GameWindow::selectClientCandidate({ux},311)==-1);
    check("bound_pid_still_requires_exact_client_executable",GameWindow::selectClientCandidate({{21,310,QStringLiteral("LeagueClientUxRender.exe"),surface,true},{22,310,QStringLiteral("League of Legends.exe"),surface,true}},310)==-1);
    otherUx=ux;otherUx.window=12;otherUx.surface.clientPixels=QRect(0,0,3840,2160);otherUx.foreground=true;
    check("multiple_usable_bound_client_windows_are_rejected_instead_of_area_or_focus_guess",GameWindow::selectClientCandidate({ux,otherUx},310)==-1&&GameWindow::selectClientCandidate({otherUx,ux},310)==-1);
    otherUx.surface.visible=false;
    check("hidden_auxiliary_client_does_not_make_one_visible_client_ambiguous",GameWindow::selectClientCandidate({otherUx,ux},310)==1);
    ux.surface.minimized=true;check("minimized_client_never_supplies_overlay_geometry",GameWindow::selectClientCandidate({ux},310)==-1);ux.surface.minimized=false;
    ux.surface.visible=false;check("hidden_client_never_supplies_overlay_geometry",GameWindow::selectClientCandidate({ux},310)==-1);ux.surface.visible=true;
    ux.surface.clientPixels=QRect(0,0,100,80);check("tiny_client_auxiliary_is_not_a_selection_surface",GameWindow::selectClientCandidate({ux},310)==-1);
    GameWindow::ClientTracker clientTracker;
    check("unbound_client_tracker_is_inactive_and_reports_no_identity",!clientTracker.state().overlayAllowed()&&clientTracker.state().report().value("position_basis")==QStringLiteral("league_client_area")&&!clientTracker.state().report().contains("process")&&!clientTracker.state().report().contains("window"));
    clientTracker.setExpectedProcess(310);clientTracker.setExpectedProcess(311);clientTracker.setExpectedProcess(0);
    check("client_process_changes_revoke_observation_without_fallback",!clientTracker.state().found&&!clientTracker.refresh().found&&!clientTracker.state().overlayAllowed());
    GameWindow::State state;state.found=true;state.visible=true;state.foreground=true;state.client=QRect(-1920,40,1280,720);
    check("foreground_window_allows_overlay",state.overlayAllowed());state.foreground=false;
    check("alt_tab_hides_overlay_even_with_fresh_game_data",!state.overlayAllowed());state.foreground=true;state.minimized=true;
    check("minimize_hides_overlay",!state.overlayAllowed());state.minimized=false;state.visible=false;
    check("hidden_window_hides_overlay",!state.overlayAllowed());state.visible=true;state.found=false;
    check("destroyed_window_hides_overlay",!state.overlayAllowed());state.found=true;state.client={};
    check("unmapped_screen_hides_overlay_instead_of_showing_on_remote_screen",!state.overlayAllowed());
    state.client=QRect(60,80,1280,720);check("restore_recovers_overlay_after_valid_window_returns",state.overlayAllowed());
    check("diagnostic_report_has_no_process_or_window_identity",!state.report().contains("process")&&!state.report().contains("window")&&!state.report().value("exclusive_fullscreen_verified").toBool(true));

    const bool native=QGuiApplication::platformName()==QStringLiteral("windows");
    bool nativeCloakFixture=false;
    if(native){
        QWidget fixture;fixture.setWindowTitle(QStringLiteral("LiXin window geometry fixture"));fixture.resize(900,600);fixture.move(QGuiApplication::primaryScreen()->availableGeometry().topLeft()+QPoint(80,80));fixture.show();settle();
        const auto inspect=[&](const QString &name,const QString &mode){
            const auto surface=GameWindow::nativeSurface(fixture.winId());QScreen *mapped=nullptr;QVector<GameWindow::Screen> mappings;
            const auto qtScreens=QGuiApplication::screens();for(auto screen:qtScreens)mappings.append({screen->name(),screen->geometry(),screen->devicePixelRatio()});
            const int index=GameWindow::screenIndex(surface,mappings);if(index>=0)mapped=qtScreens[index];
            const auto logical=mapped?GameWindow::logicalClient(surface.clientPixels,surface.monitorPixels,mapped->geometry(),mapped->devicePixelRatio()):QRect();
            check(name+QStringLiteral("_native_content_rect_matches_qt_client"),surface.valid&&mapped&&logical.size()==fixture.size()&&(logical.topLeft()-fixture.mapToGlobal(QPoint())).manhattanLength()<=2);
            check(name+QStringLiteral("_native_mode"),GameWindow::surfaceMode(surface)==mode);
            QJsonArray screens;for(auto screen:QGuiApplication::screens())screens.append(QJsonObject{{"name",screen->name()},{"origin",QJsonArray{screen->geometry().x(),screen->geometry().y()}},{"scale",screen->devicePixelRatio()}});
            frames.append(QJsonObject{{"name",name},{"mode",GameWindow::surfaceMode(surface)},{"native_monitor",surface.monitorName},{"qt_screens",screens},{"qt_size",QJsonArray{fixture.width(),fixture.height()}},{"client",QJsonArray{logical.x(),logical.y(),logical.width(),logical.height()}},{"scale",mapped?mapped->devicePixelRatio():0.}});
        };
        inspect("framed",QStringLiteral("windowed"));
        fixture.move(fixture.pos()+QPoint(130,90));fixture.resize(1024,700);settle();inspect("moved_resized",QStringLiteral("windowed"));
        fixture.setWindowFlags(Qt::Window|Qt::FramelessWindowHint);fixture.resize(900,600);fixture.show();settle();inspect("borderless",QStringLiteral("borderless_windowed"));
#ifdef Q_OS_WIN
        const auto nativeWindow=reinterpret_cast<HWND>(fixture.winId());
        const auto dwm=GetModuleHandleW(L"dwmapi.dll");
        using SetDwmAttribute=HRESULT(WINAPI *)(HWND,DWORD,LPCVOID,DWORD);
        const auto setDwm=dwm?reinterpret_cast<SetDwmAttribute>(GetProcAddress(dwm,"DwmSetWindowAttribute")):nullptr;
        constexpr DWORD cloakAttribute=13; // DWMWA_CLOAK
        const BOOL cloaked=TRUE,uncloaked=FALSE;
        if(setDwm&&SUCCEEDED(setDwm(nativeWindow,cloakAttribute,&cloaked,sizeof(cloaked)))){
            nativeCloakFixture=true;settle();
            check("dwm_cloaked_window_is_not_visible_even_with_visible_style",IsWindowVisible(nativeWindow)&&!GameWindow::nativeSurface(fixture.winId()).visible);
            const bool restored=SUCCEEDED(setDwm(nativeWindow,cloakAttribute,&uncloaked,sizeof(uncloaked)));settle();
            check("dwm_uncloaked_window_is_visible_again",restored&&GameWindow::nativeSurface(fixture.winId()).visible);
        }
#endif
        fixture.showMinimized();settle();check("native_minimization_is_observed_without_stale_rectangle",GameWindow::nativeSurface(fixture.winId()).minimized&&!GameWindow::nativeSurface(fixture.winId()).valid);
        fixture.showNormal();settle();inspect("restored",QStringLiteral("borderless_windowed"));
        for(auto screen:QGuiApplication::screens()){
            fixture.move(screen->availableGeometry().topLeft()+QPoint(40,40));settle();inspect(QStringLiteral("monitor_%1").arg(frames.size()),QStringLiteral("borderless_windowed"));
        }
        fixture.showFullScreen();settle();inspect("qt_fullscreen_composited",QStringLiteral("fullscreen_or_borderless"));fixture.hide();
    }
    const QJsonObject result{{"passed",failures==0},{"total",checks.size()},{"failures",failures},{"checks",checks},{"native_fixture",native},{"native_cloak_fixture",nativeCloakFixture},{"frames",frames},{"real_league_game",false},{"exclusive_fullscreen_verified",false}};
    if(argc>1){const auto folder=QString::fromLocal8Bit(argv[1]);QDir().mkpath(folder);QFile file(folder+QStringLiteral("/report.json"));if(!file.open(QIODevice::WriteOnly))return 2;file.write(QJsonDocument(result).toJson(QJsonDocument::Indented));}
    std::printf("%lld game window checks, %d failures; native fixture=%s\n",qint64(checks.size()),failures,native?"true":"false");return failures?1:0;
}
