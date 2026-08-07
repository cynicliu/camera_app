set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

get_filename_component(ONVIF_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(ONVIF_TOOLCHAIN "${ONVIF_ROOT}/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin")
set(ONVIF_STAGING "${ONVIF_ROOT}/sysdrv/source/buildroot/buildroot-2023.02.6/output/staging")

set(CMAKE_C_COMPILER "${ONVIF_TOOLCHAIN}/arm-rockchip830-linux-uclibcgnueabihf-gcc")
set(CMAKE_CXX_COMPILER "${ONVIF_TOOLCHAIN}/arm-rockchip830-linux-uclibcgnueabihf-g++")
set(CMAKE_FIND_ROOT_PATH "${ONVIF_STAGING}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

add_compile_options("-I${ONVIF_STAGING}/usr/include")
add_compile_options("-DWITH_NO_C_LOCALE")
add_link_options("-L${ONVIF_STAGING}/usr/lib")
add_link_options("-Wl,-rpath-link,${ONVIF_STAGING}/usr/lib")
