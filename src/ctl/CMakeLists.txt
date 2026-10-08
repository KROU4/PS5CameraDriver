add_executable(ps5cam-ctl main.cpp ../common/settings.cpp ../common/vcamreg.cpp ../common/camdefaults.cpp)
target_link_libraries(ps5cam-ctl PRIVATE mfplat mfsensorgroup mfuuid ole32 cfgmgr32 advapi32)
