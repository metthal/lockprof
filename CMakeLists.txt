cmake_minimum_required(VERSION 3.6)
project(proflock CXX C)

add_library(lockprof SHARED lockprof.cpp)
set_property(TARGET lockprof PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET lockprof PROPERTY CXX_STANDARD 14)
target_link_libraries(lockprof dl)
