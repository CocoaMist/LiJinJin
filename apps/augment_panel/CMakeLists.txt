set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Network Concurrent)
foreach(module IN ITEMS Core Gui Widgets Network Concurrent)
  get_target_property(qt_module_type Qt6::${module} TYPE)
  if(NOT qt_module_type STREQUAL "SHARED_LIBRARY")
    message(FATAL_ERROR "LiJinJin's public build uses shared Qt 6 libraries. Install a dynamic Qt SDK.")
  endif()
endforeach()
add_library(kda_theme STATIC kda_theme.cpp kda_theme.h overlay_backdrop.cpp overlay_backdrop.h)
if(WIN32)
  add_executable(lixin_backdrop_helper native_backdrop_host.cpp)
  target_link_libraries(lixin_backdrop_helper PRIVATE user32 gdi32)
endif()
target_link_libraries(kda_theme PUBLIC Qt6::Widgets)
target_include_directories(kda_theme PUBLIC .)

set(KDA_BRAND_ASSETS
  ../../assets/brand/lijinjin-app.ico
  ../../assets/brand/lijinjin-app.png
  ../../assets/brand/lijinjin-wordmark.png)
foreach(asset IN LISTS KDA_BRAND_ASSETS)
  get_filename_component(name "${asset}" NAME)
  set_source_files_properties("${asset}" PROPERTIES QT_RESOURCE_ALIAS "brand/${name}")
endforeach()
qt_add_resources(kda_theme "kda_assets"
  PREFIX "/lixin/kda"
  BASE "${CMAKE_CURRENT_SOURCE_DIR}/../../assets/themes/kda"
  FILES ${KDA_BRAND_ASSETS}
        ../../assets/themes/kda/theme.json
        ../../assets/themes/kda/fonts/BarlowSemiCondensed-SemiBold.ttf
        ../../assets/themes/kda/fonts/BarlowSemiCondensed-Bold.ttf
        ../../assets/themes/kda/fonts/Barlow-OFL.txt
        ../../assets/themes/kda/fonts/NotoSansSC-Variable.ttf
        ../../assets/themes/kda/fonts/NotoSansSC-OFL.txt
        ../../assets/themes/kda/fonts/Inter-Variable.ttf
        ../../assets/themes/kda/fonts/Inter-OFL.txt
        ../../assets/themes/kda/textures/pearl-satin.png
        ../../assets/themes/kda/textures/all-out-prism.png
        ../../assets/themes/kda/textures/more-foil.png
        ../../assets/themes/kda/textures/home-fire-ribbon-v2.png
        ../../assets/themes/kda/textures/home-silver-ribbon-v2.png
        ../../assets/themes/kda/textures/home-platinum-stage-v1.jpg
        ../../assets/themes/kda/textures/home-fire-mirror-v2.png
)

if(WIN32)
  target_link_libraries(kda_theme PRIVATE user32 gdi32)
endif()

if(MSVC)
  target_compile_options(kda_theme PRIVATE /utf-8 /W4)
endif()

add_library(panel_state panel_state.cpp offer_match_binding.cpp auto_advice.cpp card_advice.cpp coach_presentation.cpp)
target_link_libraries(panel_state PUBLIC Qt6::Core)
target_include_directories(panel_state PUBLIC . ../native)
set(NATIVE ../native)
add_library(coach_core ${NATIVE}/catalog.cpp ${NATIVE}/analysis.cpp ${NATIVE}/combat.cpp
  ${NATIVE}/mechanic_triggers.cpp ${NATIVE}/mechanic_actions.cpp ${NATIVE}/growth.cpp
  ${NATIVE}/ai_context.cpp ${NATIVE}/settings.cpp ${NATIVE}/coach_model.cpp ${NATIVE}/augment_strategy.cpp ${NATIVE}/learned_state_model.cpp ${NATIVE}/headliner.cpp ${NATIVE}/decision_context.cpp)
target_sources(coach_core PRIVATE ${NATIVE}/coach_knowledge.cpp)
target_sources(coach_core PRIVATE ${NATIVE}/augment_advisory.cpp)
target_sources(coach_core PRIVATE ${NATIVE}/local_choice_evidence.cpp ${NATIVE}/local_choice_v2.cpp)
target_sources(coach_core PRIVATE ${NATIVE}/card_reference_cache.cpp)
target_sources(coach_core PRIVATE ${NATIVE}/augment_context_advice.cpp)
target_sources(coach_core PRIVATE ${NATIVE}/mode_context_rules.cpp)

target_sources(coach_core PRIVATE ${NATIVE}/ds_engine_contract.cpp)
target_sources(coach_core PRIVATE ${NATIVE}/external_strategy_reference.cpp)
target_sources(coach_core PRIVATE ${NATIVE}/semantic_evidence.cpp)
target_sources(coach_core PRIVATE ${NATIVE}/build_reasoning.cpp ${NATIVE}/growth_objective.cpp)
target_sources(coach_core PRIVATE ${NATIVE}/ally_utility.cpp)
target_sources(coach_core PRIVATE ${NATIVE}/growth_observations.cpp)
target_sources(coach_core PRIVATE ${NATIVE}/role_guidance.cpp ${NATIVE}/hero_execution.cpp ${NATIVE}/recommendation_trace.cpp)
target_sources(coach_core PRIVATE ${NATIVE}/combat_sandbox.cpp ${NATIVE}/purchase_planner.cpp ${NATIVE}/chain_purchase_planner.cpp)
target_sources(coach_core PRIVATE ${NATIVE}/match_context.cpp)
target_sources(coach_core PRIVATE ${NATIVE}/tactical_context.cpp)
target_sources(coach_core PRIVATE ${NATIVE}/lifecycle_sandbox.cpp)
target_sources(coach_core PRIVATE ${NATIVE}/teamfight_sandbox.cpp)
target_sources(coach_core PRIVATE ${NATIVE}/support_sandbox.cpp)
target_sources(coach_core PRIVATE ${NATIVE}/continuous_action_state.cpp ${NATIVE}/continuous_control_state.cpp ${NATIVE}/continuous_sandbox.cpp)
target_sources(coach_core PRIVATE ${NATIVE}/item_zone_ledger.cpp)
target_sources(coach_core PRIVATE ${NATIVE}/combat_sustain.cpp ${NATIVE}/chain_equipment_comparison.cpp ${NATIVE}/chain_mechanism_contract.cpp)
target_sources(coach_core PRIVATE ${NATIVE}/hextech_mechanism_state.cpp ${NATIVE}/hextech_conversion.cpp ${NATIVE}/augment_3116.cpp)
target_sources(coach_core PRIVATE ${NATIVE}/world_attack_probe_v1.cpp)
target_sources(coach_core PRIVATE ${NATIVE}/hero_scaffold_world.cpp)
target_sources(coach_core PRIVATE ${NATIVE}/form_stats_v4.cpp ${NATIVE}/progression_v4.cpp)
target_sources(coach_core PRIVATE ${NATIVE}/mechanic_delivery_v4.cpp)
target_sources(coach_core PRIVATE ${NATIVE}/stateful_skill_v7.cpp)
target_sources(coach_core PRIVATE ${NATIVE}/combat_units_v8.cpp)
target_sources(coach_core PRIVATE ${NATIVE}/live_combat_summary.cpp)
target_sources(coach_core PRIVATE ${NATIVE}/live_equipment_cues.cpp)
target_sources(coach_core PRIVATE ${NATIVE}/coach_refresh_policy.cpp ${NATIVE}/coach_recommendation_quality.cpp)
target_sources(coach_core PRIVATE ${NATIVE}/recommendation_history.cpp ${NATIVE}/loading_preparation.cpp)
target_sources(coach_core PRIVATE dashboard_match_view.cpp roster_recent_results_view.cpp)
target_include_directories(coach_core PUBLIC ${NATIVE})
target_link_libraries(coach_core PUBLIC Qt6::Core)
add_library(coach_client ${NATIVE}/live_client.cpp ${NATIVE}/lcu_discovery.cpp ${NATIVE}/custom_game_probe.cpp ${NATIVE}/deepseek.cpp ${NATIVE}/sgp_history.cpp ${NATIVE}/sgp_client.cpp)
target_sources(coach_client PRIVATE ${NATIVE}/ds_forecast.cpp ${NATIVE}/ds_usage_meter.cpp)
target_link_libraries(coach_client PUBLIC Qt6::Concurrent)
target_sources(coach_client PRIVATE ${NATIVE}/roster_analysis_bridge.cpp)
target_sources(coach_client PRIVATE ${NATIVE}/growth_capture_bridge.cpp)
target_link_libraries(coach_client PUBLIC coach_core Qt6::Network)

add_library(overlay_layout overlay_layout.cpp)
target_link_libraries(overlay_layout PUBLIC Qt6::Core)
target_include_directories(overlay_layout PUBLIC .)
add_library(game_window STATIC game_window.cpp)
target_link_libraries(game_window PUBLIC Qt6::Gui)
target_include_directories(game_window PUBLIC .)
if(WIN32)
  target_link_libraries(game_window PRIVATE user32)
endif()

qt_add_executable(lixin_augment_panel WIN32 coach_main.cpp overlay_window.cpp history_pane.cpp player_match_view.cpp card_hint_window.cpp death_tips_window.cpp coach_action_hud.cpp coach_live_hud.cpp hud_assets.cpp coach_replay.cpp coach_dashboard.cpp coach_dashboard.h dashboard_chrome.cpp preference_switch.cpp ui_browser.cpp hud_paged_text.cpp)
target_sources(lixin_augment_panel PRIVATE live_summary_worker.cpp execution_reference_worker.cpp diagnostic_report_writer.cpp runtime_log.cpp)
target_sources(lixin_augment_panel PRIVATE growth_capture_settings.cpp startup_notice.cpp)
target_sources(lixin_augment_panel PRIVATE corner_combat_hud.cpp)
target_sources(lixin_augment_panel PRIVATE hud_layout.cpp hud_layout_editor.cpp)
target_sources(lixin_augment_panel PRIVATE champ_select_view.cpp shop_recommendations.cpp shop_recommendation_projection.cpp)
target_link_libraries(lixin_augment_panel PRIVATE panel_state coach_client overlay_layout game_window kda_theme Qt6::Gui Qt6::Widgets Qt6::Concurrent)
if(WIN32)
  enable_language(RC)
  configure_file(app_icon.rc.in "${CMAKE_CURRENT_BINARY_DIR}/app_icon.rc" @ONLY)
  set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/app_icon.rc" PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/app_version.h;${CMAKE_CURRENT_SOURCE_DIR}/../../assets/brand/lijinjin-app.ico")
  target_sources(lixin_augment_panel PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/app_icon.rc")
  target_link_libraries(lixin_augment_panel PRIVATE user32 gdi32 dwmapi)
  target_link_libraries(coach_client PRIVATE iphlpapi ws2_32 shell32)
endif()
if(MSVC)
  target_compile_options(panel_state PRIVATE /utf-8 /W4)
  target_compile_options(lixin_augment_panel PRIVATE /utf-8 /W4)
  target_compile_options(coach_core PRIVATE /utf-8 /W4)
  target_compile_options(coach_client PRIVATE /utf-8 /W4)
  target_compile_options(overlay_layout PRIVATE /utf-8 /W4)
endif()
if(MSVC)
  target_compile_options(game_window PRIVATE /utf-8 /W4)
endif()

if(BUILD_TESTING)
  add_executable(game_window_checks ../../tests/game_window_checks.cpp)
  target_link_libraries(game_window_checks PRIVATE game_window overlay_layout Qt6::Widgets)
  add_test(NAME game_window_checks COMMAND game_window_checks)

  add_executable(offer_match_binding_checks ../../tests/offer_match_binding_checks.cpp)
  target_link_libraries(offer_match_binding_checks PRIVATE panel_state)
  add_test(NAME offer_match_binding_checks COMMAND offer_match_binding_checks)

  add_executable(runtime_log_checks ../../tests/runtime_log_checks.cpp runtime_log.cpp)
  target_include_directories(runtime_log_checks PRIVATE .)
  target_link_libraries(runtime_log_checks PRIVATE Qt6::Core)
  add_test(NAME runtime_log_checks COMMAND runtime_log_checks)

  if(MSVC)
    foreach(target IN ITEMS game_window_checks offer_match_binding_checks runtime_log_checks)
      target_compile_options(${target} PRIVATE /utf-8 /W4)
    endforeach()
  endif()
endif()
