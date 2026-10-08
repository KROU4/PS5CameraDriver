add_library(ps5cam-vcam SHARED
    dllmain.cpp source.cpp capture.cpp depthengine.cpp ps5cam-vcam.def
    ../common/log.cpp ../common/settings.cpp ../common/camctl.cpp ../common/depthshare.cpp)
target_link_libraries(ps5cam-vcam PRIVATE ps5cam-core
    mfplat mf mfuuid mfreadwrite mfsensorgroup ole32 oleaut32 cfgmgr32 shell32 advapi32 ksuser)
ps5cam_version(ps5cam-vcam "PS5 HD Camera virtual camera")
