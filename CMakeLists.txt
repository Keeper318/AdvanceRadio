cmake_minimum_required(VERSION 3.22)
project(AdvanceRadio C)

set(CMAKE_C_STANDARD 11)
add_compile_options(-Wall -Wvla -Werror)
link_libraries(curl)

add_executable(AdvanceRadio main.c)
