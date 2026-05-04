#ifndef PAGE_STORAGE_H
#define PAGE_STORAGE_H

#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <cstring>
#include "Index.h" // btree here
// We are using pages as a nodes of btree for storaging them at disk


struct PageConfig {
    static constexpr size_t PAGE_SIZE = 4096;  // 4KB страницы
    static constexpr size_t HEADER_SIZE = sizeof(uint64_t);  // ID страницы в заголовке
    static constexpr size_t DATA_SIZE = PAGE_SIZE - HEADER_SIZE;
};

struct PageHeader {
    uint64_t page_id;  // Уникальный идентификатор страницы
    // можно добавить: флаги, контрольную сумму, LSN и т.д.
};

struct Page {
    PageHeader header;
    char data[PageConfig::DATA_SIZE];
    
    Page() {
        memset(&header, 0, sizeof(header));
        memset(data, 0, sizeof(data));
    }
};

class PageManager {
public:
    PageManager(const std::string& filename) 
        : filename_(filename), next_page_id_(0) {
        file_.open(filename_, std::ios::in | std::ios::out | std::ios::binary);
        if (!file_.is_open()) {
            // Создаем новый файл
            file_.open(filename_, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
            if (!file_.is_open()) {
                throw std::runtime_error("Cannot create database file: " + filename);
            }
            write_metadata();
        } else {
            read_metadata();
        }
    }
    
    ~PageManager() {
        flush_all();
        file_.close();
    }
    
    // Загружает страницу в память
    std::shared_ptr<Page> load_page(uint64_t page_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // Проверяем кэш (пока простой, потом заменим на LRU)
        auto it = cache_.find(page_id);
        if (it != cache_.end()) {
            return it->second;
        }
        
        auto page = std::make_shared<Page>();
        if (page_id == 0) {
            // Новая страница
            page->header.page_id = ++next_page_id_;
            cache_[page->header.page_id] = page;
            return page;
        }
        
        // Читаем существующую страницу
        size_t offset = calculate_offset(page_id);
        file_.seekg(offset);
        file_.read(reinterpret_cast<char*>(page.get()), sizeof(Page));
        
        if (file_.fail()) {
            throw std::runtime_error("Failed to read page " + std::to_string(page_id));
        }
        
        cache_[page_id] = page;
        return page;
    }
    
    // Сохраняет страницу на диск
    void write_page(uint64_t page_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = cache_.find(page_id);
        if (it == cache_.end()) {
            return; // Страница не в кэше
        }
        
        size_t offset = calculate_offset(page_id);
        file_.seekp(offset);
        file_.write(reinterpret_cast<const char*>(it->second.get()), sizeof(Page));
        
        if (file_.fail()) {
            throw std::runtime_error("Failed to write page " + std::to_string(page_id));
        }
    }
    
    // Сбрасывает страницу на диск и удаляет из кэша
    void flush_page(uint64_t page_id) {
        write_page(page_id);
        std::lock_guard<std::mutex> lock(mutex_);
        cache_.erase(page_id);
    }
    
    // Сбрасывает все страницы
    void flush_all() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [page_id, page] : cache_) {
            size_t offset = calculate_offset(page_id);
            file_.seekp(offset);
            file_.write(reinterpret_cast<const char*>(page.get()), sizeof(Page));
        }
        cache_.clear();
        write_metadata();
    }
    
    // Выделяет новую страницу
    uint64_t allocate_page() {
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t new_id = ++next_page_id_;
        auto page = std::make_shared<Page>();
        page->header.page_id = new_id;
        cache_[new_id] = page;
        return new_id;
    }
    
    // Освобождает страницу (пометка как свободной)
    void free_page(uint64_t page_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_.erase(page_id);
        // maybe better to add free-list logic?
    }
    
private:
    void write_metadata() {
        file_.seekp(0);
        file_.write(reinterpret_cast<const char*>(&next_page_id_), sizeof(next_page_id_));
    }
    
    void read_metadata() {
        file_.seekg(0);
        file_.read(reinterpret_cast<char*>(&next_page_id_), sizeof(next_page_id_));
        if (file_.fail()) {
            next_page_id_ = 0;
        }
    }
    
    size_t calculate_offset(uint64_t page_id) {
        // Первые 8 байт - метаданные, дальше страницы
        return sizeof(uint64_t) + (page_id - 1) * sizeof(Page);
    }
    
    std::string filename_;
    std::fstream file_;
    uint64_t next_page_id_;
    std::unordered_map<uint64_t, std::shared_ptr<Page>> cache_;
    std::mutex mutex_;
};

template<typename T>
class page_aware_allocator {
public: // idk about that. all this allocator shit is dubious, if something - i will change it
// i just hope after ill do tests they will work and it will be happy time for us
    using value_type = T;
    
    page_aware_allocator(PageManager& pm) noexcept : page_manager_(&pm) {}
    
    template<typename U>
    page_aware_allocator(const page_aware_allocator<U>& other) noexcept 
        : page_manager_(other.page_manager_) {}
    
    T* allocate(size_t n) {
        if (n > max_size()) {
            throw std::bad_array_new_length();
        }
        
        // Для единичных объектов используем обычный new
        if (n == 1) {
            return static_cast<T*>(::operator new(sizeof(T)));
        }
        
        // Для массивов/блоков используем страницы
        size_t bytes = n * sizeof(T);
        if (bytes <= PageConfig::DATA_SIZE) {
            uint64_t page_id = page_manager_->allocate_page();
            auto page = page_manager_->load_page(page_id);
            
            PageAllocation alloc;
            alloc.page_id = page_id;
            alloc.ptr = page->data;
            allocations_[page_id] = alloc;
            
            return reinterpret_cast<T*>(alloc.ptr);
        }
        
        throw std::bad_alloc();
    }
    
    void deallocate(T* p, size_t n) {
        if (!p) return;
        
        if (n == 1) {
            ::operator delete(p);
            return;
        }
        
        // Ищем аллокацию по странице
        for (auto it = allocations_.begin(); it != allocations_.end(); ++it) {
            if (it->second.ptr == reinterpret_cast<char*>(p)) {
                page_manager_->free_page(it->first);
                allocations_.erase(it);
                return;
            }
        }
        
        throw std::runtime_error("Invalid deallocation pointer");
    }
    
    template<typename U>
    bool operator==(const page_aware_allocator<U>& other) const noexcept {
        return page_manager_ == other.page_manager_;
    }
    
    template<typename U>
    bool operator!=(const page_aware_allocator<U>& other) const noexcept {
        return !(*this == other);
    }
    
private:
    struct PageAllocation {
        uint64_t page_id;
        char* ptr;
    };
    
    PageManager* page_manager_;
    std::unordered_map<uint64_t, PageAllocation> allocations_;
    
    template<typename U>
    friend class page_aware_allocator;
};
 