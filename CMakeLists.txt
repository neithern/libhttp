cmake_minimum_required(VERSION 3.0.0)
project(libhttp VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)

add_subdirectory(libuv)

aux_source_directory(include src_files)
aux_source_directory(src src_files)
aux_source_directory(src/pico src_files)

add_library(http SHARED ${src_files})
target_include_directories(http PUBLIC include libuv/include)
target_link_libraries(http uv)

add_library(http_a STATIC ${src_files})
target_include_directories(http_a PUBLIC include libuv/include)
target_link_libraries(http_a uv_a)

add_executable(client
    test/test-client.cpp
)
target_link_libraries(client http_a)

add_executable(server
    test/test-server.cpp
)
target_link_libraries(server http_a)
