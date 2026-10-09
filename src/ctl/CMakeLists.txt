add_executable(ps5cam-ctl main.cpp ../common/settings.cpp ../common/vcamreg.cpp ../common/camdefaults.cpp)
target_link_libraries(ps5cam-ctl PRIVATE mfplat mfuuid ole32 cfgmgr32 advapi32)
ps5cam_version(ps5cam-ctl "PS5 HD Camera settings tool")
