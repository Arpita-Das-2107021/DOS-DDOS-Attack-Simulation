cmake_minimum_required(VERSION 3.16)
project(CppDosDdosLab)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(server server.cpp)
add_executable(client client.cpp)

target_link_libraries(server ws2_32)
target_link_libraries(client ws2_32)
