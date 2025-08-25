cmake_minimum_required(VERSION 3.10)
project(fchess_server)

# Set C++ standard to 17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add /bigobj compile option for MSVC
if(MSVC)
    add_compile_options(/bigobj)
endif()

# Find Boost libraries
find_package(Boost REQUIRED COMPONENTS system)
find_package(nlohmann_json REQUIRED)

# Add include directory for this project
include_directories(include)

# Add executable
add_executable(fchess_server src/main.cpp)

# Link libraries
target_link_libraries(fchess_server PRIVATE Boost::system nlohmann_json::nlohmann_json)

# Add threading support
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(fchess_server PRIVATE -pthread)
    target_link_libraries(fchess_server PRIVATE -pthread)
endif()
