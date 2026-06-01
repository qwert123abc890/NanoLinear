# 1. Minimum CMake version required by modern standard
cmake_minimum_required(VERSION 3.15)

# 2. Project Name and internal metadata metadata
project(TwoLevelBitmapAllocator 
        VERSION 2.0 
        LANGUAGES CXX)

# 3. Force CMake to adhere to your strict ISO C++23 standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD_EXTENSIONS OFF)

# 4. Configure compiler optimizing parameters for BOTH Windows (MSVC) and Linux (GCC)
if(MSVC)
    # /O2: Aggressive Speed Optimization
    # /W4: Display production grade engineering warnings
    add_compile_options(/O2 /W4)
else()
    # -O3: Maximum performance compilation tier for GCC/Clang
    # -Wall -Wextra: Enable rich static review diagnostics
    add_compile_options(-O3 -Wall -Wextra -march=native)
endif()

# 5. Define the target executive production unit
# add_executable(bitmap_benchmark src/main.cpp)
#  FIXED: Change the source file from the empty main.cpp to your real performance_test.cpp
add_executable(bitmap_benchmark src/performance_test.cpp)


# 6. Bind the include directories directly to the compilation scope
target_include_directories(bitmap_benchmark PRIVATE ${PROJECT_SOURCE_DIR}/include)
