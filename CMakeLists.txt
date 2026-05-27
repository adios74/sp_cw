cmake_minimum_required(VERSION 3.14)
project(StorageNode)

# C++20 required for the project
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Опции компиляции
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -g -O2)
endif()

# ============================================================
# Поиск установленной nlohmann/json
# ============================================================
find_package(nlohmann_json REQUIRED)

# ============================================================
# Добавляем все include директории
# ============================================================
set(INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/project/storage_node/include
    ${CMAKE_CURRENT_SOURCE_DIR}/project/common/include
    ${CMAKE_CURRENT_SOURCE_DIR}/project/client
    ${CMAKE_CURRENT_SOURCE_DIR}/project/client/Text\ handler/include
    ${CMAKE_CURRENT_SOURCE_DIR}/project/entrypoint/include
)

# ============================================================
# Собираем все исходники
# ============================================================
file(GLOB_RECURSE STORAGE_SOURCES 
    "project/storage_node/src/*.cpp"
    "project/entrypoint/src/*.cpp"
)

file(GLOB_RECURSE COMMON_SOURCES 
    "project/common/src/*.cpp"
)

list(REMOVE_ITEM COMMON_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/project/common/src/Json.cpp")

file(GLOB_RECURSE TEXT_HANDLER_SOURCES 
    "project/client/Text\ handler/src/*.cpp"
)

set(LIBRARY_SOURCES
    ${STORAGE_SOURCES}
    ${COMMON_SOURCES}
    ${TEXT_HANDLER_SOURCES}
)

list(REMOVE_ITEM LIBRARY_SOURCES 
    "${CMAKE_CURRENT_SOURCE_DIR}/project/client/main.cpp"
)

if(LIBRARY_SOURCES)
    add_library(storage_node ${LIBRARY_SOURCES})
    target_include_directories(storage_node PUBLIC ${INCLUDE_DIRS})
    target_link_libraries(storage_node PUBLIC nlohmann_json::nlohmann_json)
endif()

# ============================================================
# Создаем исполняемый файл из main.cpp
# ============================================================
add_executable(storage_client 
    "project/client/main.cpp"
)

target_link_libraries(storage_client PRIVATE storage_node nlohmann_json::nlohmann_json)
target_include_directories(storage_client PRIVATE ${INCLUDE_DIRS})

# ============================================================
# Тесты
# ============================================================
option(BUILD_TESTS "Build tests" OFF)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(project/storage_node/tests)
endif()

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