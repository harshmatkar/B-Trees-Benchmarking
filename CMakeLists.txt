cmake_minimum_required(VERSION 3.10)
project(BPlusTreeIndexEngine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O2")
add_compile_options(-Wall -Wextra)

include_directories(${CMAKE_SOURCE_DIR}/include)

# Core engine as a static library: Page (layout) + BufferManager (disk/LRU)
# + BPlusTree (search/insert/range). No third-party dependencies at all -
# everything here is standard C++17 / STL.
add_library(bptree_core STATIC
    src/Page.cpp
    src/BufferManager.cpp
    src/BPlusTree.cpp
)

# CLI + benchmark executable
add_executable(bptree_cli src/main.cpp)
target_link_libraries(bptree_cli PRIVATE bptree_core)

# Optional: place the built binary at the project root for convenience
set_target_properties(bptree_cli PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
