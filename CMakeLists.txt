cmake_minimum_required(VERSION 3.10)
project(BTWin)

find_package(fmt CONFIG REQUIRED)
find_package(cppwinrt CONFIG REQUIRED)

set(SOURCES
    src/main.cpp
)

add_executable(${PROJECT_NAME} ${SOURCES})
target_link_libraries(${PROJECT_NAME} PRIVATE fmt::fmt Microsoft::CppWinRT)