cmake_minimum_required(VERSION 3.10)
project(BTWin C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_C_STANDARD 23)

find_package(fmt CONFIG REQUIRED)
find_package(cppwinrt CONFIG REQUIRED)

set(LIB_SOURCES
        src/watcher.cpp
)


set(SOURCES
        src/main.c
)

add_library("watcher" SHARED ${LIB_SOURCES})
target_link_libraries(watcher
        PRIVATE
        fmt::fmt
        Microsoft::CppWinRT
        RuntimeObject
        WindowsApp
)

add_executable(${PROJECT_NAME} ${SOURCES})
target_link_libraries(${PROJECT_NAME}
        PRIVATE
            "watcher"
)