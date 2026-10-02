cmake_minimum_required(VERSION 3.15)
project(colorassist CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV REQUIRED)
find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)

add_executable(colorassist colorassist.cpp)
target_link_libraries(colorassist PRIVATE ${OpenCV_LIBS} CURL::libcurl nlohmann_json::nlohmann_json)
if(WIN32)
  target_link_libraries(colorassist PRIVATE comdlg32)
endif()