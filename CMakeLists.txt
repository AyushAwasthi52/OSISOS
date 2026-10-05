cmake_minimum_required(VERSION 3.10)
project(OSISOS VERSION 1.0 DESCRIPTION "Resource-constrained Linux-based operating environment")

# Set C standards
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# Add API include directory
include_directories(${PROJECT_SOURCE_DIR}/api)

# Add OSISOS shell executable and core modules
add_executable(osisos 
    shell/main.c
    core/process/process.c
)

# We will add other components here as we implement them
# target_link_libraries(osisos ... )

# Enable testing
enable_testing()
add_subdirectory(tests/unit)
