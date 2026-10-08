add_executable(ps5cam-svc main.cpp ../common/firmware.cpp ../common/log.cpp ../common/vcamreg.cpp
    ../common/settings.cpp ../common/camdefaults.cpp)
target_link_libraries(ps5cam-svc PRIVATE winusb cfgmgr32 advapi32 shell32 ole32 mfplat mfsensorgroup mfuuid wtsapi32 userenv)
# The service uploads the firmware.bin next to its exe. Keep the exe in this directory for every
# generator (the generator expression stops multi-config generators adding a per-config folder).
set_target_properties(ps5cam-svc PROPERTIES RUNTIME_OUTPUT_DIRECTORY $<1:${CMAKE_CURRENT_BINARY_DIR}>)
# Developer tree: a built firmware/firmware.bin is copied next to the exe for quick deploys. The
# installers build firmware.bin themselves from Sony's original and firmware/ps5cam-firmware.json.
if(EXISTS ${PROJECT_SOURCE_DIR}/firmware/firmware.bin)
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/firmware.bin
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PROJECT_SOURCE_DIR}/firmware/firmware.bin ${CMAKE_CURRENT_BINARY_DIR}/firmware.bin
        DEPENDS ${PROJECT_SOURCE_DIR}/firmware/firmware.bin)
    add_custom_target(ps5cam-firmware ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/firmware.bin)
    add_dependencies(ps5cam-svc ps5cam-firmware)
endif()
ps5cam_version(ps5cam-svc "PS5 HD Camera service")
