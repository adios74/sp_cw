#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <fstream>
#include <filesystem>
#include <thread>
#include <random>
#include <algorithm>
#include "../include/StorageNode.h"

namespace fs = std::filesystem;

// ==================== Test Fixtures ====================

class PageStorageTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Удаляем тестовую БД перед каждым тестом
        fs::remove("test_db.bin");
        fs::remove("test_db_mt.bin");
        fs::remove("test_db_perf.bin");
    }
    
    void TearDown() override {
        // Очистка после тестов
        fs::remove("test_db.bin");
        fs::remove("test_db_mt.bin");
        fs::remove("test_db_perf.bin");
    }
    
    // Helper для создания PageManager
    std::unique_ptr<PageManager> createPageManager(const std::string& filename = "test_db.bin") {
        return std::make_unique<PageManager>(filename);
    }
};

// ==================== Unit Tests: PageConfig ====================

TEST_F(PageStorageTest, PageConfigConstants) {
    EXPECT_EQ(PageConfig::PAGE_SIZE, 4096);
    EXPECT_EQ(PageConfig::HEADER_SIZE, sizeof(uint64_t));
    EXPECT_EQ(PageConfig::DATA_SIZE, 4096 - sizeof(uint64_t));
    EXPECT_GT(PageConfig::DATA_SIZE, 0);
}

TEST_F(PageStorageTest, PageSizeIsPowerOfTwo) {
    EXPECT_EQ(PageConfig::PAGE_SIZE & (PageConfig::PAGE_SIZE - 1), 0);
}

// ==================== Unit Tests: Page Structure ====================

TEST_F(PageStorageTest, PageDefaultConstruction) {
    Page page;
    EXPECT_EQ(page.header.page_id, 0);
    
    // Проверяем, что data инициализирована нулями
    for (size_t i = 0; i < PageConfig::DATA_SIZE; ++i) {
        EXPECT_EQ(page.data[i], 0);
    }
}

TEST_F(PageStorageTest, PageSize) {
    EXPECT_EQ(sizeof(Page), PageConfig::PAGE_SIZE);
    EXPECT_EQ(sizeof(PageHeader) + PageConfig::DATA_SIZE, PageConfig::PAGE_SIZE);
}

// ==================== Unit Tests: PageManager Creation ====================

TEST_F(PageStorageTest, CreateNewDatabase) {
    EXPECT_NO_THROW({
        auto pm = createPageManager();
    });
    EXPECT_TRUE(fs::exists("test_db.bin"));
}

TEST_F(PageStorageTest, OpenExistingDatabase) {
    // Создаем и закрываем
    {
        auto pm = createPageManager();
        pm->allocate_page();
    }
    
    // Открываем существующую
    EXPECT_NO_THROW({
        auto pm = createPageManager();
    });
}

TEST_F(PageStorageTest, CreateDatabaseInvalidPath) {
    EXPECT_THROW({
        PageManager pm("/invalid/path/test.bin");
    }, std::runtime_error);
}

TEST_F(PageStorageTest, DatabaseFileNotEmpty) {
    auto pm = createPageManager();
    EXPECT_GT(fs::file_size("test_db.bin"), 0);
}

// ==================== Unit Tests: Page Allocation ====================

TEST_F(PageStorageTest, AllocateSinglePage) {
    auto pm = createPageManager();
    uint64_t page_id = pm->allocate_page();
    
    EXPECT_EQ(page_id, 1);
}

TEST_F(PageStorageTest, AllocateMultiplePages) {
    auto pm = createPageManager();
    
    for (uint64_t i = 1; i <= 100; ++i) {
        uint64_t page_id = pm->allocate_page();
        EXPECT_EQ(page_id, i);
    }
}

TEST_F(PageStorageTest, AllocatePageReturnsUniqueIds) {
    auto pm = createPageManager();
    std::set<uint64_t> ids;
    
    for (int i = 0; i < 1000; ++i) {
        uint64_t id = pm->allocate_page();
        EXPECT_TRUE(ids.insert(id).second);
    }
}

TEST_F(PageStorageTest, AllocatedPageHasCorrectId) {
    auto pm = createPageManager();
    uint64_t page_id = pm->allocate_page();
    
    auto page = pm->load_page(page_id);
    EXPECT_EQ(page->header.page_id, page_id);
}

TEST_F(PageStorageTest, AllocatedPageIsZeroed) {
    auto pm = createPageManager();
    uint64_t page_id = pm->allocate_page();
    
    auto page = pm->load_page(page_id);
    for (size_t i = 0; i < PageConfig::DATA_SIZE; ++i) {
        EXPECT_EQ(page->data[i], 0);
    }
}

// ==================== Unit Tests: Page Loading ====================

TEST_F(PageStorageTest, LoadNonExistentPage) {
    auto pm = createPageManager();
    EXPECT_THROW({
        pm->load_page(999);
    }, std::runtime_error);
}

TEST_F(PageStorageTest, LoadPageAfterAllocation) {
    auto pm = createPageManager();
    uint64_t page_id = pm->allocate_page();
    
    EXPECT_NO_THROW({
        auto page = pm->load_page(page_id);
    });
}

TEST_F(PageStorageTest, LoadPageReturnsCorrectData) {
    auto pm = createPageManager();
    uint64_t page_id = pm->allocate_page();
    
    auto page = pm->load_page(page_id);
    page->data[0] = 'A';
    page->data[1] = 'B';
    page->data[2] = 'C';
    
    pm->write_page(page_id);
    pm->flush_page(page_id);
    
    auto loaded = pm->load_page(page_id);
    EXPECT_EQ(loaded->data[0], 'A');
    EXPECT_EQ(loaded->data[1], 'B');
    EXPECT_EQ(loaded->data[2], 'C');
}

// ==================== Unit Tests: Page Writing ====================

TEST_F(PageStorageTest, WritePageData) {
    auto pm = createPageManager();
    uint64_t page_id = pm->allocate_page();
    
    auto page = pm->load_page(page_id);
    const char* test_data = "Hello, World!";
    memcpy(page->data, test_data, strlen(test_data) + 1);
    
    EXPECT_NO_THROW(pm->write_page(page_id));
}

TEST_F(PageStorageTest, WriteAndReadRoundtrip) {
    auto pm = createPageManager();
    uint64_t page_id = pm->allocate_page();
    
    // Записываем данные
    auto page = pm->load_page(page_id);
    std::string test_string = "Test data for roundtrip validation";
    memcpy(page->data, test_string.c_str(), test_string.size() + 1);
    pm->write_page(page_id);
    pm->flush_page(page_id);
    
    // Читаем обратно
    auto loaded = pm->load_page(page_id);
    std::string loaded_string(loaded->data);
    EXPECT_EQ(test_string, loaded_string);
}

TEST_F(PageStorageTest, WriteMultiplePages) {
    auto pm = createPageManager();
    const int num_pages = 100;
    std::vector<uint64_t> ids;
    
    for (int i = 0; i < num_pages; ++i) {
        uint64_t id = pm->allocate_page();
        auto page = pm->load_page(id);
        
        // Записываем номер страницы в начало данных
        *reinterpret_cast<int*>(page->data) = i;
        ids.push_back(id);
    }
    
    pm->flush_all();
    
    // Проверяем
    for (int i = 0; i < num_pages; ++i) {
        auto page = pm->load_page(ids[i]);
        EXPECT_EQ(*reinterpret_cast<int*>(page->data), i);
    }
}

// ==================== Unit Tests: Page Cache ====================

TEST_F(PageStorageTest, CacheHitReturnsSamePointer) {
    auto pm = createPageManager();
    uint64_t page_id = pm->allocate_page();
    
    auto page1 = pm->load_page(page_id);
    auto page2 = pm->load_page(page_id);
    
    EXPECT_EQ(page1.get(), page2.get());
}

TEST_F(PageStorageTest, CacheMissLoadsFromDisk) {
    auto pm = createPageManager();
    uint64_t page_id = pm->allocate_page();
    
    auto page1 = pm->load_page(page_id);
    page1->data[0] = 'X';
    pm->write_page(page_id);
    pm->flush_page(page_id); // Удаляем из кэша
    
    auto page2 = pm->load_page(page_id);
    EXPECT_NE(page1.get(), page2.get());
    EXPECT_EQ(page2->data[0], 'X');
}

// ==================== Unit Tests: Page Flushing ====================

TEST_F(PageStorageTest, FlushPageRemovesFromCache) {
    auto pm = createPageManager();
    uint64_t page_id = pm->allocate_page();
    
    auto page1 = pm->load_page(page_id);
    pm->flush_page(page_id);
    
    auto page2 = pm->load_page(page_id);
    EXPECT_NE(page1.get(), page2.get());
}

TEST_F(PageStorageTest, FlushAllWritesAllPages) {
    auto pm = createPageManager();
    std::vector<uint64_t> ids;
    
    for (int i = 0; i < 50; ++i) {
        uint64_t id = pm->allocate_page();
        auto page = pm->load_page(id);
        *reinterpret_cast<int*>(page->data) = i * 100;
        ids.push_back(id);
    }
    
    pm->flush_all();
    
    // Переоткрываем файл
    pm.reset();
    auto pm2 = createPageManager();
    
    for (int i = 0; i < 50; ++i) {
        auto page = pm2->load_page(ids[i]);
        EXPECT_EQ(*reinterpret_cast<int*>(page->data), i * 100);
    }
}

// ==================== Unit Tests: Free Page ====================

TEST_F(PageStorageTest, FreePageRemovesFromCache) {
    auto pm = createPageManager();
    uint64_t page_id = pm->allocate_page();
    
    pm->load_page(page_id);
    pm->free_page(page_id);
    
    // После освобождения, попытка загрузки должна создать новую страницу
    EXPECT_THROW({
        pm->load_page(page_id);
    }, std::runtime_error);
}

// ==================== Unit Tests: Metadata Persistence ====================

TEST_F(PageStorageTest, MetadataSavedOnFlush) {
    uint64_t last_id;
    
    {
        auto pm = createPageManager();
        for (int i = 0; i < 10; ++i) {
            pm->allocate_page();
        }
        last_id = 10;
        pm->flush_all();
    }
    
    {
        auto pm = createPageManager();
        uint64_t new_id = pm->allocate_page();
        EXPECT_EQ(new_id, last_id + 1);
    }
}

// ==================== Unit Tests: PageAwareAllocator ====================

class PageAwareAllocatorTest : public PageStorageTest {};

TEST_F(PageAwareAllocatorTest, AllocateSingleObject) {
    auto pm = createPageManager();
    page_aware_allocator<int> allocator(*pm);
    
    int* ptr = allocator.allocate(1);
    EXPECT_NE(ptr, nullptr);
    *ptr = 42;
    EXPECT_EQ(*ptr, 42);
    allocator.deallocate(ptr, 1);
}

TEST_F(PageAwareAllocatorTest, AllocateArray) {
    auto pm = createPageManager();
    page_aware_allocator<int> allocator(*pm);
    
    int* arr = allocator.allocate(10);
    EXPECT_NE(arr, nullptr);
    
    for (int i = 0; i < 10; ++i) {
        arr[i] = i * i;
    }
    
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(arr[i], i * i);
    }
    
    allocator.deallocate(arr, 10);
}

TEST_F(PageAwareAllocatorTest, AllocateLargeData) {
    auto pm = createPageManager();
    page_aware_allocator<char> allocator(*pm);
    
    // Тест на выделение данных больше размера страницы
    EXPECT_THROW({
        allocator.allocate(PageConfig::DATA_SIZE + 1);
    }, std::bad_alloc);
}

TEST_F(PageAwareAllocatorTest, AllocatorComparison) {
    auto pm1 = createPageManager("test_db_mt.bin");
    page_aware_allocator<int> alloc1(*pm1);
    page_aware_allocator<int> alloc2(*pm1);
    
    EXPECT_TRUE(alloc1 == alloc2);
}

// ==================== Unit Tests: PageBasedIndex ====================

class PageBasedIndexTest : public PageStorageTest {
protected:
    std::unique_ptr<PageManager> pm;
    std::unique_ptr<PageBasedIndex<int>> index;
    
    void SetUp() override {
        PageStorageTest::SetUp();
        pm = createPageManager();
        index = std::make_unique<PageBasedIndex<int>>(*pm);
    }
};

TEST_F(PageBasedIndexTest, InsertAndFindString) {
    index->insert_string(1, "Hello, World!");
    auto result = index->find_string(1);
    
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "Hello, World!");
}

TEST_F(PageBasedIndexTest, InsertAndFindInt) {
    int value = 42;
    index->insert_value(1, value);
    auto result = index->find_value<int>(1);
    
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 42);
}

TEST_F(PageBasedIndexTest, InsertAndFindDouble) {
    double value = 3.14159;
    index->insert_value(1, value);
    auto result = index->find_value<double>(1);
    
    EXPECT_TRUE(result.has_value());
    EXPECT_DOUBLE_EQ(result.value(), 3.14159);
}

TEST_F(PageBasedIndexTest, InsertAndFindStruct) {
    struct TestStruct {
        int x;
        double y;
        char z[10];
    };
    
    TestStruct value{10, 20.5, "test"};
    index->insert_value(1, value);
    auto result = index->find_value<TestStruct>(1);
    
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result->x, 10);
    EXPECT_DOUBLE_EQ(result->y, 20.5);
    EXPECT_STREQ(result->z, "test");
}

TEST_F(PageBasedIndexTest, FindNonExistent) {
    auto result = index->find_string(999);
    EXPECT_FALSE(result.has_value());
}

TEST_F(PageBasedIndexTest, UpdateExistingKey) {
    index->insert_string(1, "Original");
    index->insert_string(1, "Updated");
    
    auto result = index->find_string(1);
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "Updated");
}

TEST_F(PageBasedIndexTest, MultipleInserts) {
    for (int i = 0; i < 100; ++i) {
        index->insert_value(i, i * 10);
    }
    
    for (int i = 0; i < 100; ++i) {
        auto result = index->find_value<int>(i);
        EXPECT_TRUE(result.has_value());
        EXPECT_EQ(result.value(), i * 10);
    }
}

TEST_F(PageBasedIndexTest, RemoveExisting) {
    index->insert_string(1, "Test");
    EXPECT_TRUE(index->remove(1));
    EXPECT_FALSE(index->contains(1));
}

TEST_F(PageBasedIndexTest, RemoveNonExistent) {
    EXPECT_FALSE(index->remove(999));
}

TEST_F(PageBasedIndexTest, Contains) {
    index->insert_string(1, "Test");
    EXPECT_TRUE(index->contains(1));
    EXPECT_FALSE(index->contains(2));
}

TEST_F(PageBasedIndexTest, Size) {
    EXPECT_EQ(index->size(), 0);
    
    index->insert_string(1, "One");
    EXPECT_EQ(index->size(), 1);
    
    index->insert_string(2, "Two");
    EXPECT_EQ(index->size(), 2);
    
    index->remove(1);
    EXPECT_EQ(index->size(), 1);
}

TEST_F(PageBasedIndexTest, Clear) {
    for (int i = 0; i < 50; ++i) {
        index->insert_value(i, i);
    }
    EXPECT_EQ(index->size(), 50);
    
    index->clear();
    EXPECT_EQ(index->size(), 0);
    
    for (int i = 0; i < 50; ++i) {
        EXPECT_FALSE(index->contains(i));
    }
}

TEST_F(PageBasedIndexTest, LargeData) {
    std::string large_data(PageConfig::DATA_SIZE - sizeof(size_t), 'X');
    EXPECT_NO_THROW({
        index->insert_string(1, large_data);
    });
    
    auto result = index->find_string(1);
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result->value(), large_data);
}

TEST_F(PageBasedIndexTest, DataTooLarge) {
    std::string too_large(PageConfig::DATA_SIZE + 1, 'X');
    EXPECT_THROW({
        index->insert_string(1, too_large);
    }, std::runtime_error);
}

// ==================== Integration Tests ====================

class IntegrationTest : public PageStorageTest {
protected:
    void SetUp() override {
        PageStorageTest::SetUp();
    }
};

TEST_F(IntegrationTest, FullWorkflow) {
    auto pm = createPageManager();
    
    // Тест 1: Работа с PageManager напрямую
    const int NUM_PAGES = 100;
    std::vector<uint64_t> page_ids;
    std::vector<std::string> test_data;
    
    for (int i = 0; i < NUM_PAGES; ++i) {
        uint64_t id = pm->allocate_page();
        page_ids.push_back(id);
        
        auto page = pm->load_page(id);
        std::string data = "Page data " + std::to_string(i);
        test_data.push_back(data);
        
        size_t len = data.size();
        memcpy(page->data, &len, sizeof(size_t));
        memcpy(page->data + sizeof(size_t), data.c_str(), len);
    }
    
    pm->flush_all();
    
    // Переоткрываем и проверяем
    pm.reset();
    pm = createPageManager();
    
    for (int i = 0; i < NUM_PAGES; ++i) {
        auto page = pm->load_page(page_ids[i]);
        
        size_t len;
        memcpy(&len, page->data, sizeof(size_t));
        
        std::string data(page->data + sizeof(size_t), len);
        EXPECT_EQ(data, test_data[i]);
    }
}

TEST_F(IntegrationTest, MultiplePageManagers) {
    auto pm1 = createPageManager("test_db1.bin");
    auto pm2 = createPageManager("test_db2.bin");
    
    auto id1 = pm1->allocate_page();
    auto id2 = pm2->allocate_page();
    
    EXPECT_EQ(id1, 1);
    EXPECT_EQ(id2, 1);
    
    auto page1 = pm1->load_page(id1);
    auto page2 = pm2->load_page(id2);
    
    page1->data[0] = 'A';
    page2->data[0] = 'B';
    
    pm1->flush_all();
    pm2->flush_all();
    
    auto loaded1 = pm1->load_page(id1);
    auto loaded2 = pm2->load_page(id2);
    
    EXPECT_EQ(loaded1->data[0], 'A');
    EXPECT_EQ(loaded2->data[0], 'B');
    
    fs::remove("test_db1.bin");
    fs::remove("test_db2.bin");
}

TEST_F(IntegrationTest, IndexAndPageManagerIntegration) {
    auto pm = createPageManager();
    PageBasedIndex<std::string> index(*pm);
    
    // Вставляем данные
    index.insert_string("key1", "value1");
    index.insert_string("key2", "value2");
    index.insert_string("key3", "value3");
    
    // Проверяем
    EXPECT_EQ(index.find_string("key1").value(), "value1");
    EXPECT_EQ(index.find_string("key2").value(), "value2");
    EXPECT_EQ(index.find_string("key3").value(), "value3");
    
    // Обновляем
    index.insert_string("key1", "updated_value1");
    EXPECT_EQ(index.find_string("key1").value(), "updated_value1");
    
    // Удаляем
    index.remove("key2");
    EXPECT_FALSE(index.contains("key2"));
}

TEST_F(IntegrationTest, ConcurrentPageAccess) {
    auto pm = std::make_shared<PageManager>("test_db_mt.bin");
    const int NUM_THREADS = 4;
    const int PAGES_PER_THREAD = 250;
    
    std::vector<std::thread> threads;
    std::mutex cout_mutex;
    
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([pm, t, PAGES_PER_THREAD, &cout_mutex]() {
            std::vector<uint64_t> ids;
            
            for (int i = 0; i < PAGES_PER_THREAD; ++i) {
                uint64_t id = pm->allocate_page();
                ids.push_back(id);
                
                auto page = pm->load_page(id);
                int value = t * PAGES_PER_THREAD + i;
                memcpy(page->data, &value, sizeof(int));
            }
            
            pm->flush_all();
            
            for (int i = 0; i < PAGES_PER_THREAD; ++i) {
                auto page = pm->load_page(ids[i]);
                int value;
                memcpy(&value, page->data, sizeof(int));
                EXPECT_EQ(value, t * PAGES_PER_THREAD + i);
            }
        });
    }
    
    for (auto& thread : threads) {
        thread.join();
    }
}

TEST_F(IntegrationTest, StressTest) {
    auto pm = createPageManager("test_db_perf.bin");
    const int NUM_OPERATIONS = 10000;
    
    PageBasedIndex<int> index(*pm);
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> key_dist(1, 5000);
    std::uniform_int_distribution<> value_dist(1, 1000000);
    
    std::map<int, int> reference;
    
    for (int i = 0; i < NUM_OPERATIONS; ++i) {
        int key = key_dist(gen);
        int value = value_dist(gen);
        
        index.insert_value(key, value);
        reference[key] = value;
    }
    
    // Проверяем случайную выборку
    for (int i = 0; i < 1000; ++i) {
        int key = key_dist(gen);
        auto expected = reference.find(key);
        auto actual = index.find_value<int>(key);
        
        if (expected != reference.end()) {
            EXPECT_TRUE(actual.has_value());
            EXPECT_EQ(actual.value(), expected->second);
        } else {
            // Ключ мог быть вставлен, но не входит в 5000
            if (key >= 1 && key <= 5000) {
                EXPECT_FALSE(actual.has_value());
            }
        }
    }
}

TEST_F(IntegrationTest, PersistenceAcrossSessions) {
    const std::string filename = "test_db_persist.bin";
    
    // Сессия 1: запись данных
    {
        auto pm = createPageManager(filename);
        PageBasedIndex<int> index(*pm);
        
        for (int i = 0; i < 1000; ++i) {
            index.insert_value(i, i * 100);
        }
        
        pm->flush_all();
    }
    
    // Сессия 2: чтение данных
    {
        auto pm = std::make_unique<PageManager>(filename);
        PageBasedIndex<int> index(*pm);
        
        for (int i = 0; i < 1000; ++i) {
            auto result = index.find_value<int>(i);
            EXPECT_TRUE(result.has_value());
            if (result.has_value()) {
                EXPECT_EQ(result.value(), i * 100);
            }
        }
    }
    
    fs::remove(filename);
}

// ==================== Edge Case Tests ====================

TEST_F(PageStorageTest, ZeroPageId) {
    auto pm = createPageManager();
    auto page = pm->load_page(0);
    EXPECT_NE(page, nullptr);
    EXPECT_GT(page->header.page_id, 0);
}

TEST_F(PageStorageTest, MaxPageId) {
    auto pm = createPageManager();
    // Это может занять много памяти, поэтому тест концептуальный
    uint64_t max_id = std::numeric_limits<uint64_t>::max();
    
    // Просто проверяем, что система не падает при больших ID
    EXPECT_THROW({
        pm->load_page(max_id);
    }, std::runtime_error);
}

TEST_F(PageStorageTest, EmptyData) {
    auto pm = createPageManager();
    PageBasedIndex<int> index(*pm);
    
    index.insert_value(1, 42);
    
    // Поиск с неверным размером данных
    auto result = index.find_value<double>(1);
    EXPECT_FALSE(result.has_value());
}

TEST_F(PageBasedIndexTest, SameKeyDifferentTypes) {
    index->insert_string(1, "string");
    index->insert_value(1, 100);
    
    // После перезаписи ключа, тип данных меняется
    auto str_result = index->find_string(1);
    EXPECT_TRUE(str_result.has_value());
    EXPECT_NE(str_result.value(), "string");
}

// ==================== Performance Tests ====================

TEST_F(PageStorageTest, PerformanceSequentialWrites) {
    auto pm = createPageManager("test_db_perf.bin");
    const int NUM_PAGES = 1000;
    
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < NUM_PAGES; ++i) {
        uint64_t id = pm->allocate_page();
        auto page = pm->load_page(id);
        memset(page->data, i % 256, PageConfig::DATA_SIZE);
        pm->write_page(id);
    }
    
    pm->flush_all();
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    // Проверяем, что операция выполняется за разумное время (меньше 5 секунд)
    EXPECT_LT(duration.count(), 5000);
    
    std::cout << "Performance: " << NUM_PAGES << " pages written in " 
              << duration.count() << "ms" << std::endl;
}

TEST_F(PageStorageTest, PerformanceRandomReads) {
    auto pm = createPageManager("test_db_perf.bin");
    const int NUM_PAGES = 500;
    std::vector<uint64_t> ids;
    
    // Подготовка данных
    for (int i = 0; i < NUM_PAGES; ++i) {
        uint64_t id = pm->allocate_page();
        auto page = pm->load_page(id);
        *reinterpret_cast<int*>(page->data) = i;
        ids.push_back(id);
    }
    pm->flush_all();
    
    // Случайное чтение
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(0, NUM_PAGES - 1);
    
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < 10000; ++i) {
        int idx = dist(gen);
        auto page = pm->load_page(ids[idx]);
        volatile int value = *reinterpret_cast<int*>(page->data);
        (void)value;
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    EXPECT_LT(duration.count(), 5000);
    
    std::cout << "Performance: 10000 random reads in " 
              << duration.count() << "ms" << std::endl;
}

// ==================== Error Handling Tests ====================

TEST_F(PageStorageTest, CorruptedFile) {
    // Создаем нормальный файл
    {
        auto pm = createPageManager();
        pm->allocate_page();
        pm->flush_all();
    }
    
    // Повреждаем файл
    {
        std::fstream file("test_db.bin", std::ios::out | std::ios::binary);
        file.seekp(0);
        uint64_t garbage = 0xFFFFFFFFFFFFFFFF;
        file.write(reinterpret_cast<const char*>(&garbage), sizeof(garbage));
        file.close();
    }
    
    // Пробуем открыть поврежденный файл
    EXPECT_ANY_THROW({
        auto pm = createPageManager();
        pm->load_page(1);
    });
}

// ==================== Main ====================

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}