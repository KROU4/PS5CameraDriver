add_executable(ps5cam-tray WIN32 main.cpp ../common/settings.cpp)
target_link_libraries(ps5cam-tray PRIVATE shell32 user32 gdi32 advapi32)
ps5cam_version(ps5cam-tray "PS5 HD Camera tray icon")
