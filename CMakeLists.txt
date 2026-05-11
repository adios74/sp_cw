cmake_minimum_required(VERSION 3.14)
project(StorageNode)

# C++17 required for the project
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Опции компиляции
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -g -O2)
endif()

# ============================================================
# Автоматическая загрузка nlohmann/json
# ============================================================
include(FetchContent)

FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(json)

# ============================================================
# Добавляем include директории
# ============================================================
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/project/storage_node/include)

# ============================================================
# Библиотека storage_node
# ============================================================
file(GLOB_RECURSE STORAGE_SOURCES 
    "project/storage_node/src/*.cpp"
)

if(STORAGE_SOURCES)
    add_library(storage_node ${STORAGE_SOURCES})
    target_include_directories(storage_node PUBLIC 
        ${CMAKE_CURRENT_SOURCE_DIR}/project/storage_node/include
    )
    # Подключаем JSON-библиотеку к storage_node
    target_link_libraries(storage_node PUBLIC nlohmann_json::nlohmann_json)
endif()

# ============================================================
# Тесты
# ============================================================
option(BUILD_TESTS "Build tests" OFF)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(project/storage_node/tests)
endif()

# ============================================================
# Дополнительные цели
# ============================================================
add_custom_target(format
    COMMAND find ${CMAKE_CURRENT_SOURCE_DIR}/project -name "*.cpp" -o -name "*.h" | xargs clang-format -i
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Formatting code with clang-format"
)

add_custom_target(clean_all
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}
    COMMAND find ${CMAKE_CURRENT_SOURCE_DIR} -name "*.bin" -type f -delete
    COMMAND find ${CMAKE_CURRENT_SOURCE_DIR} -name "test_db*" -type f -delete
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Cleaning build directory and test databases"
)