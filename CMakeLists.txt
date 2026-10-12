# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

# IDF v6.1 hardcodes FF_FS_EXFAT=0. Generate a project-local override from
# the installed SDK, preserving all upstream sources and license notices.
# Intermediate copies remain under ignored outputs; never patch the SDK.
set(box3_fatfs_dir "${CMAKE_CURRENT_LIST_DIR}/outputs/fatfs_exfat/fatfs")
file(COPY "$ENV{IDF_PATH}/components/fatfs/" DESTINATION "${box3_fatfs_dir}")
file(READ "${box3_fatfs_dir}/src/ffconf.h" box3_ffconf)
if(NOT box3_ffconf MATCHES "#define FF_FS_EXFAT[ \t]+0")
    message(FATAL_ERROR "SDK FatFs configuration changed; review exFAT override")
endif()
string(REGEX REPLACE "#define FF_FS_EXFAT[ \t]+0"
       "/* BOX3 project: enable exFAT without modifying SDK sources. */\n#define FF_FS_EXFAT 1"
       box3_ffconf "${box3_ffconf}")
# exFAT uses FF_USE_LABEL in a C expression, where undefined Kconfig booleans
# cannot be treated as zero as they can in preprocessor conditionals.
string(REGEX REPLACE "#define FF_USE_LABEL[ \t]+CONFIG_FATFS_USE_LABEL"
       "#ifdef CONFIG_FATFS_USE_LABEL\n#define FF_USE_LABEL 1\n#else\n#define FF_USE_LABEL 0\n#endif"
       box3_ffconf "${box3_ffconf}")
file(WRITE "${box3_fatfs_dir}/src/ffconf.h" "${box3_ffconf}")
set(EXTRA_COMPONENT_DIRS "${box3_fatfs_dir}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(box3_camera_preview)
