cmake_minimum_required(VERSION 3.8)
project(multi_threaded_chat_server VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Set output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Compiler-specific options
if (MSVC)
    # Enable multithreading support for MSVC
    add_compile_options(/W3 /EHsc)
else()
    # Enable multithreading support for GCC/Clang
    add_compile_options(-Wall -Wextra -pthread)
endif()

add_executable(chat_server src/server.cpp)

# Link libraries
if (WIN32)
    target_link_libraries(chat_server PRIVATE ws2_32)
elseif(UNIX)
    target_link_libraries(chat_server PRIVATE pthread)
endif()

# Installation rules
install(TARGETS chat_server
        RUNTIME DESTINATION bin)
