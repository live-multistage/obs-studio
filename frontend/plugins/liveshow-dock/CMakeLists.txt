cmake_minimum_required(VERSION 3.28...3.30)

find_package(Qt6 REQUIRED Widgets)

add_library(liveshow-dock MODULE)
add_library(OBS::liveshow-dock ALIAS liveshow-dock)

target_sources(liveshow-dock PRIVATE liveshow-dock.cpp)

target_link_libraries(
  liveshow-dock
  PRIVATE
    OBS::libobs
    OBS::frontend-api
    OBS::browser-panels
    Qt::Widgets
)

set_target_properties_obs(
  liveshow-dock
  PROPERTIES FOLDER frontend
             PREFIX ""
             AUTOMOC ON
)
