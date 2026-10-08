add_library(ps5cam-vcam SHARED
    dllmain.cpp source.cpp capture.cpp ps5cam-vcam.def
    ../common/log.cpp ../common/settings.cpp)
target_link_libraries(ps5cam-vcam PRIVATE ps5cam-core
    mfplat mf mfuuid mfreadwrite mfsensorgroup ole32 oleaut32 cfgmgr32 shell32 advapi32 ksuser)
