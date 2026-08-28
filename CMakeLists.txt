cmake_minimum_required(VERSION 3.10)

# 设置交叉编译
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
# 这里指定的gec6818交叉编译器路径，必须是绝对路径
#set(CMAKE_C_COMPILER /usr/local/arm/5.4.0/usr/bin/arm-linux-gcc)
#set(CMAKE_CXX_COMPILER /usr/local/arm/5.4.0/usr/bin/arm-linux-g++)
# 这里指定的rk3568交叉编译器路径，必须是绝对路径 如果18.04 
# 看你的交叉编译工具链配置在哪 对应修改绝对路径
set(CMAKE_C_COMPILER /usr/local/arm-linux/bin/aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER /usr/local/arm-linux/bin/aarch64-linux-gnu-g++)


project(lvgl)

set(CMAKE_C_STANDARD 99)#C99 # lvgl officially support C99 and above
set(CMAKE_CXX_STANDARD 17)#C17
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/bin)

link_directories(./)
include_directories(
    freetype-2.13.3/include/freetype  # 必须放在首位
    freetype-2.13.3/include/
)

add_subdirectory(freetype-2.13.3) 
add_subdirectory(lvgl)
target_include_directories(lvgl PUBLIC ${PROJECT_SOURCE_DIR} PRIVATE freetype-2.13.3/include/freetype)

aux_source_directory(mycode SRC_FILE)
include_directories(mycode)
add_executable(main main.c mouse_cursor_icon.c ${SRC_FILE})

# 注释掉这行，如果要使用DRM驱动，则需要放开这行
# include(${CMAKE_CURRENT_LIST_DIR}/lvgl/tests/FindLibDRM.cmake)
# include_directories(${Libdrm_INCLUDE_DIRS})

# 注释掉下面四个，这个是在PC端使用的
# find_package(SDL2)
# find_library(NAME SDL2_image REQUIRED)
# link_libraries(SDL2_image)
# include_directories(${SDL2_INCLUDE_DIRS} ${SDL2_IMAGE_INCLUDE_DIRS})
# set(SDL2_LIBRARIES "-L ${SDL2_LIBDIR}  -lSDL2")

target_link_libraries(main lvgl lvgl::examples lvgl::demos lvgl::thorvg ${SDL2_LIBRARIES} ${SDL2_IMAGE_LIBRARIES} ${Libdrm_LIBRARIES} m pthread freetype)
add_custom_target (run COMMAND ${EXECUTABLE_OUTPUT_PATH}/main DEPENDS main)