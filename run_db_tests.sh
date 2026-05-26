#!/bin/bash

# Цвета для вывода
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Настройки
BUILD_DIR="build"
TEST_DIR="test_db_data"
VERBOSE=false
FILTER=""

# Парсинг аргументов командной строки
while [[ $# -gt 0 ]]; do
    case $1 in
        -v|--verbose)
            VERBOSE=true
            shift
            ;;
        -f|--filter)
            FILTER="$2"
            shift 2
            ;;
        -c|--clean)
            CLEAN=true
            shift
            ;;
        --db-only)
            FILTER="DBEngineTest.*"
            shift
            ;;
        --table-only)
            FILTER="*Table*:*Insert*:*Select*:*Update*:*Delete*"
            shift
            ;;
        -h|--help)
            echo "Usage: $0 [options]"
            echo "Options:"
            echo "  -v, --verbose     Verbose output"
            echo "  -f, --filter      Filter tests (e.g., 'DBEngineTest.*')"
            echo "  -c, --clean       Clean build directory before building"
            echo "  --db-only         Run only database engine tests"
            echo "  --table-only      Run only table operation tests"
            echo "  -h, --help        Show this help"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}  Database Engine Test Runner${NC}"
echo -e "${BLUE}========================================${NC}"

# Очистка старых тестовых данных
if [ -d "$TEST_DIR" ]; then
    echo -e "${YELLOW}Cleaning old test data...${NC}"
    rm -rf "$TEST_DIR"
fi

# Очистка билда если нужно
if [ "$CLEAN" = true ]; then
    echo -e "${YELLOW}Cleaning build directory...${NC}"
    rm -rf "$BUILD_DIR"
fi

# Создание директории для сборки
if [ ! -d "$BUILD_DIR" ]; then
    echo -e "${YELLOW}Creating build directory...${NC}"
    mkdir -p "$BUILD_DIR"
fi

# Переход в директорию сборки
cd "$BUILD_DIR" || exit 1

# Конфигурация CMake
echo -e "${YELLOW}Configuring CMake...${NC}"
cmake .. -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug

if [ $? -ne 0 ]; then
    echo -e "${RED}CMake configuration failed!${NC}"
    exit 1
fi

# Сборка
echo -e "${YELLOW}Building tests...${NC}"
cmake --build . --target test_page_storage -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

if [ $? -ne 0 ]; then
    echo -e "${RED}Build failed!${NC}"
    exit 1
fi

echo -e "${GREEN}Build successful!${NC}"

# Формирование команды запуска
TEST_CMD="./project/storage_node/tests/test_page_storage"

if [ "$VERBOSE" = true ]; then
    TEST_CMD="$TEST_CMD --gtest_verbose=true --gtest_print_time=true"
fi

TEST_CMD="$TEST_CMD --gtest_color=yes"

if [ -n "$FILTER" ]; then
    TEST_CMD="$TEST_CMD --gtest_filter=$FILTER"
    echo -e "${BLUE}Filter: $FILTER${NC}"
fi

# Запуск тестов
echo -e "${YELLOW}Running tests...${NC}"
echo -e "${BLUE}Command: $TEST_CMD${NC}"
echo ""

$TEST_CMD
TEST_RESULT=$?

echo ""
if [ $TEST_RESULT -eq 0 ]; then
    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN}  All tests passed!${NC}"
    echo -e "${GREEN}========================================${NC}"
else
    echo -e "${RED}========================================${NC}"
    echo -e "${RED}  Some tests failed!${NC}"
    echo -e "${RED}========================================${NC}"
fi

# Очистка тестовых данных
if [ -d "../$TEST_DIR" ]; then
    echo -e "${YELLOW}Cleaning test data...${NC}"
    rm -rf "../$TEST_DIR"
fi

cd ..
exit $TEST_RESULT