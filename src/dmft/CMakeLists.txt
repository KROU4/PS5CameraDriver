add_library(ps5cam-dmft SHARED
    dllmain.cpp dmft.cpp processor.cpp ps5cam-dmft.def
    ../common/log.cpp ../common/settings.cpp ../common/camctl.cpp ../common/camdefaults.cpp ../common/depthshare.cpp)
target_link_libraries(ps5cam-dmft PRIVATE ps5cam-core
    mfplat mfuuid ole32 oleaut32 shell32 advapi32 cfgmgr32)
ps5cam_version(ps5cam-dmft "PS5 HD Camera device MFT")

if(BUILD_TESTING)
    add_executable(ps5cam-frametiming-tests tests/frametiming_tests.cpp processor.cpp dmft.cpp
        ../common/log.cpp ../common/settings.cpp ../common/camctl.cpp ../common/camdefaults.cpp ../common/depthshare.cpp)
    target_link_libraries(ps5cam-frametiming-tests PRIVATE ps5cam-core
        mfplat mfuuid ole32 shell32 advapi32 cfgmgr32)
    add_test(NAME ps5cam-frametiming-tests COMMAND ps5cam-frametiming-tests)
    set_tests_properties(ps5cam-frametiming-tests PROPERTIES LABELS GPU TIMEOUT 180 ENVIRONMENT "PS5CAM_GPU=warp")
endif()
