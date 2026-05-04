#!/bin/bash

# ============================================
# Storage Node Test Runner
# ============================================

set -e  # Останавливаем выполнение при ошибке

# Цвета для вывода
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
MAGENTA='\033[0;35m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Функции для форматированного вывода
print_header() {
    echo -e "${BLUE}============================================${NC}"
    echo -e "${BLUE}$1${NC}"
    echo -e "${BLUE}============================================${NC}"
}

print_success() {
    echo -e "${GREEN}✓ $1${NC}"
}

print_error() {
    echo -e "${RED}✗ $1${NC}"
}

print_info() {
    echo -e "${CYAN}ℹ $1${NC}"
}

print_warning() {
    echo -e "${YELLOW}⚠ $1${NC}"
}

# Переходим в директорию sp_cw
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SP_CW_DIR="$SCRIPT_DIR"

if [ ! -d "$SP_CW_DIR" ]; then
    print_error "Directory sp_cw not found!"
    exit 1
fi

cd "$SP_CW_DIR" || exit 1

# Обработка аргументов командной строки
BUILD_TYPE="Debug"
CLEAN_BUILD=false
RUN_TESTS=true
VERBOSE=false
FILTER=""
REPEAT=1
PARALLEL=1
GENERATOR=""

show_help() {
    echo "Usage: ./build_tests.sh [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  -h, --help              Show this help message"
    echo "  -c, --clean             Clean build directory before building"
    echo "  -d, --debug             Build in Debug mode (default)"
    echo "  -r, --release           Build in Release mode"
    echo "  -b, --build-only        Only build, don't run tests"
    echo "  -v, --verbose           Enable verbose output"
    echo "  -f, --filter FILTER     Run specific tests (gtest filter)"
    echo "  -n, --repeat N          Repeat tests N times"
    echo "  -j, --jobs N            Number of parallel build jobs"
    echo "  -m, --makefiles         Use Unix Makefiles generator"
    echo "  -n, --ninja             Use Ninja generator"
    echo "  --performance           Run performance tests only"
    echo "  --stress                Run stress tests"
    echo "  --unit                  Run unit tests only"
    echo "  --integration           Run integration tests only"
    echo ""
    echo "Examples:"
    echo "  ./build_tests.sh -c -v                     # Clean build with verbose output"
    echo "  ./build_tests.sh -f PageStorageTest.*      # Run specific tests"
    echo "  ./build_tests.sh --release -j 8            # Release build with 8 jobs"
    echo "  ./build_tests.sh --unit                    # Run unit tests only"
}

# Парсинг аргументов
while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--help)
            show_help
            exit 0
            ;;
        -c|--clean)
            CLEAN_BUILD=true
            shift
            ;;
        -d|--debug)
            BUILD_TYPE="Debug"
            shift
            ;;
        -r|--release)
            BUILD_TYPE="Release"
            shift
            ;;
        -b|--build-only)
            RUN_TESTS=false
            shift
            ;;
        -v|--verbose)
            VERBOSE=true
            shift
            ;;
        -f|--filter)
            FILTER="$2"
            shift 2
            ;;
        -n|--repeat)
            REPEAT="$2"
            shift 2
            ;;
        -j|--jobs)
            PARALLEL="$2"
            shift 2
            ;;
        -m|--makefiles)
            GENERATOR="Unix Makefiles"
            shift
            ;;
        -n|--ninja)
            GENERATOR="Ninja"
            shift
            ;;
        --performance)
            FILTER="*Performance*"
            shift
            ;;
        --stress)
            FILTER="*Stress*:*Concurrent*:*Performance*"
            REPEAT=3
            shift
            ;;
        --unit)
            FILTER="PageStorageTest.*:PageAwareAllocatorTest.*:PageBasedIndexTest.*:PageEdgeCase*"
            shift
            ;;
        --integration)
            FILTER="IntegrationTest.*"
            shift
            ;;
        *)
            print_error "Unknown option: $1"
            show_help
            exit 1
            ;;
    esac
done

print_header "Storage Node Test Suite"

# Создаем директорию для сборки
BUILD_DIR="$SP_CW_DIR/build"
mkdir -p "$BUILD_DIR"

# Очистка если нужно
if [ "$CLEAN_BUILD" = true ]; then
    print_info "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
    mkdir -p "$BUILD_DIR"
    print_success "Build directory cleaned"
fi

# Настройка CMake
print_info "Configuring CMake (${BUILD_TYPE} mode)..."

CMAKE_ARGS="-DCMAKE_BUILD_TYPE=${BUILD_TYPE} -DBUILD_TESTS=ON"

if [ ! -z "$GENERATOR" ]; then
    CMAKE_ARGS="$CMAKE_ARGS -G \"$GENERATOR\""
fi

cd "$BUILD_DIR" || exit 1

if ! cmake $CMAKE_ARGS .. 2>&1; then
    print_error "CMake configuration failed!"
    exit 1
fi
print_success "CMake configuration completed"

# Сборка
print_info "Building tests (using $PARALLEL jobs)..."

if ! cmake --build . --target test_page_storage -j "$PARALLEL" 2>&1; then
    print_error "Build failed!"
    exit 1
fi
print_success "Build completed successfully"

# Запуск тестов
if [ "$RUN_TESTS" = true ]; then
    print_header "Running Tests"
    
    # Находим исполняемый файл тестов
    TEST_EXECUTABLE=$(find "$BUILD_DIR" -name "test_page_storage" -type f -executable | head -n 1)
    
    if [ -z "$TEST_EXECUTABLE" ]; then
        print_error "Test executable not found!"
        exit 1
    fi
    
    GTEST_ARGS=""
    
    if [ ! -z "$FILTER" ]; then
        GTEST_ARGS="$GTEST_ARGS --gtest_filter=$FILTER"
        print_info "Filter: $FILTER"
    fi
    
    if [ "$VERBOSE" = true ]; then
        GTEST_ARGS="$GTEST_ARGS --gtest_verbose=true --gtest_print_time=true"
    fi
    
    if [ $REPEAT -gt 1 ]; then
        GTEST_ARGS="$GTEST_ARGS --gtest_repeat=$REPEAT"
        print_info "Repeating tests $REPEAT times"
    fi
    
    # Цветной вывод
    GTEST_ARGS="$GTEST_ARGS --gtest_color=yes"
    
    # Вывод результатов в XML
    GTEST_ARGS="$GTEST_ARGS --gtest_output=xml:${BUILD_DIR}/test_results.xml"
    
    print_info "Command: $TEST_EXECUTABLE $GTEST_ARGS"
    echo ""
    
    # Запускаем тесты
    if $TEST_EXECUTABLE $GTEST_ARGS; then
        print_header "All tests passed! 🎉"
        
        # Парсим результаты если есть xml
        if [ -f "${BUILD_DIR}/test_results.xml" ]; then
            TOTAL_TESTS=$(grep -o 'tests="[0-9]*"' "${BUILD_DIR}/test_results.xml" | head -1 | grep -o '[0-9]*')
            FAILURES=$(grep -o 'failures="[0-9]*"' "${BUILD_DIR}/test_results.xml" | head -1 | grep -o '[0-9]*')
            DISABLED=$(grep -o 'disabled="[0-9]*"' "${BUILD_DIR}/test_results.xml" | head -1 | grep -o '[0-9]*')
            ERRORS=$(grep -o 'errors="[0-9]*"' "${BUILD_DIR}/test_results.xml" | head -1 | grep -o '[0-9]*')
            
            print_info "Total tests: $TOTAL_TESTS"
            print_success "Passed: $((TOTAL_TESTS - FAILURES - ERRORS))"
            
            if [ "$FAILURES" -gt 0 ]; then
                print_error "Failures: $FAILURES"
            fi
            if [ "$ERRORS" -gt 0 ]; then
                print_error "Errors: $ERRORS"
            fi
            if [ "$DISABLED" -gt 0 ]; then
                print_warning "Disabled: $DISABLED"
            fi
        fi
        
        exit 0
    else
        print_header "Tests failed! 💥"
        
        # Показываем логи если есть
        if [ -f "${BUILD_DIR}/test_results.xml" ]; then
            print_info "Check detailed results in: ${BUILD_DIR}/test_results.xml"
        fi
        
        exit 1
    fi
else
    print_success "Build completed (tests not run)"
fi

cd "$SP_CW_DIR" || exit 1