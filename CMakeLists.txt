# logcollectd.cpp with sqlite3
cmake_minimum_required(VERSION 3.22)
project(logcollectd)

set(CMAKE_CXX_STANDARD 14)

add_executable(logcollectd logcollectd.cpp)
target_link_libraries(logcollectd sqlite3)

# verify if sqlite3 present, for ubuntu its libsqlite3-dev
find_package(SQLite3 REQUIRED)
