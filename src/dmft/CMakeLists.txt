add_library(ps5cam-dmft SHARED
    dllmain.cpp dmft.cpp processor.cpp ps5cam-dmft.def
    ../common/log.cpp ../common/settings.cpp)
target_link_libraries(ps5cam-dmft PRIVATE ps5cam-core
    mfplat mfuuid ole32 oleaut32 shell32 advapi32)
