cmake_minimum_required(VERSION 3.10)

project(RetroTransfer VERSION 1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

include_directories(include)

add_executable(RetroTransfer 
    src/main.cpp 
    src/scanner.cpp
)