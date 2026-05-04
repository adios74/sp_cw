#ifndef PAGE_STORAGE_H
#define PAGE_STORAGE_H

#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <cstring>
#include <optional>
#include <limits> 
#include "Index.h" // btree here
// We are using pages as a nodes of btree for storaging them at disk
struct PageHeader {
    uint64_t page_id;  // Уникальный идентификатор страницы
    bool dirty;
    // можно добавить: флаги, контрольную сумму, LSN и т.д.
};


struct PageConfig {
    static constexpr size_t PAGE_SIZE = 4096;  // 4KB страницы
    // todo: оптимизация под узлы маленького размера
    static constexpr size_t HEADER_SIZE = sizeof(PageHeader);  // ID страницы в заголовке
    static constexpr size_t DATA_SIZE = PAGE_SIZE - HEADER_SIZE;
};

#pragma pack(push, 1)


#pragma pack(pop)

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
            file_.flush();
        } else {
            read_metadata();
        }
    }
    
    ~PageManager() {
        flush_all();
        file_.close();
    }
    
    void mark_dirty(uint64_t page_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_.find(page_id);
        if (it != cache_.end()) {
            it->second->header.dirty = true;
        }
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
    
        size_t max_size() const noexcept {
        return std::numeric_limits<size_t>::max() / sizeof(T);
    }
    
    // Добавьте construct/destroy для C++17 совместимости
    template<typename U, typename... Args>
    void construct(U* p, Args&&... args) {
        ::new((void*)p) U(std::forward<Args>(args)...);
    }
    
    template<typename U>
    void destroy(U* p) {
        p->~U();
    }

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
 
template<typename Key>
class PageBasedIndex {
private:
    // Вспомогательные структуры для хранения типов
    using TreeType = BP_tree<Key, uint64_t, std::less<Key>>;
    using NodeType = typename TreeType::node;
    using LeafNodeType = typename TreeType::leaf_node;
    
public:
    explicit PageBasedIndex(PageManager& pm) 
        : page_manager_(pm), index_() {}
    
    ~PageBasedIndex() = default;
    
    // Вставка сырых данных
    void insert(const Key& key, const void* data, size_t size) {
        auto it = index_.find(key);
        
        if (it != index_.end()) {
            // Ключ существует — обновляем данные
            uint64_t old_page_id = it->second;
            page_manager_.free_page(old_page_id);
            
            uint64_t new_page_id = write_data_to_page(data, size);
            const_cast<uint64_t&>(it->second) = new_page_id;
        } else {
            // Новый ключ
            uint64_t page_id = write_data_to_page(data, size);
            index_.insert({key, page_id});
        }
    }
    
    // Вставка строки
    void insert_string(const Key& key, const std::string& value) {
        insert(key, value.data(), value.size());
    }
    
    // Вставка POD типа
    template<typename T>
    void insert_value(const Key& key, const T& value) {
        static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
        insert(key, &value, sizeof(T));
    }
    
    // Поиск — возвращает сырые данные
    std::optional<std::vector<char>> find(const Key& key) const {
        auto it = index_.find(key);
        if (it == index_.end()) {
            return std::nullopt;
        }
        
        return read_data_from_page(it->second);
    }
    
    // Поиск с десериализацией в строку
    std::optional<std::string> find_string(const Key& key) const {
        auto data = find(key);
        if (!data) {
            return std::nullopt;
        }
        return std::string(data->begin(), data->end());
    }
    
    // Поиск с десериализацией в POD тип
    template<typename T>
    std::optional<T> find_value(const Key& key) const {
        static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
        
        auto data = find(key);
        if (!data || data->size() != sizeof(T)) {
            return std::nullopt;
        }
        
        T result;
        memcpy(&result, data->data(), sizeof(T));
        return result;
    }
    
    // Удаление
    bool remove(const Key& key) {
        auto it = index_.find(key);
        if (it == index_.end()) {
            return false;
        }
        
        // Освобождаем страницу с данными
        page_manager_.free_page(it->second);
        
        // Удаляем из индекса
        index_.erase(it);
        return true;
    }
    
    // Проверка наличия
    bool contains(const Key& key) const {
        return index_.find(key) != index_.end();
    }
    
    // Размер индекса
    size_t size() const {
        return index_.size();
    }
    
    // Очистка
    void clear() {
        // Обходим все записи и освобождаем страницы
        for (auto it = index_.begin(); it != index_.end(); ++it) {
            page_manager_.free_page(it->second);
        }
        index_.clear();
    }
    
    // Сохранение метаданных индекса
    void save_metadata(uint64_t& root_page_id, uint64_t& leaf_head_page_id) {
        // Здесь нужно сохранить узлы B-дерева в страницы
        // и вернуть ID страниц для корня и головы листьев
        save_node(index_._root, root_page_id, leaf_head_page_id);
    }
    
    // Загрузка метаданных индекса
    void load_metadata(uint64_t root_page_id, uint64_t leaf_head_page_id) {
        index_.clear();
        if (root_page_id != 0) {
            index_._root = load_node(root_page_id);
        }
        if (leaf_head_page_id != 0) {
            index_._leaf_head = static_cast<typename decltype(index_)::leaf_node*>(
                load_node(leaf_head_page_id)
            );
        }
    }
    
    // Прямой доступ к B-дереву (для продвинутых операций)
    auto& tree() { return index_; }
    const auto& tree() const { return index_; }
    
private:
    // Формат страницы данных:
    // [8 байт: size_t размер данных][данные...]
    
    uint64_t write_data_to_page(const void* data, size_t size) {
        uint64_t page_id = page_manager_.allocate_page();
        auto page = page_manager_.load_page(page_id);
        
        // Проверяем, что данные помещаются в страницу
        if (size + sizeof(size_t) > PageConfig::DATA_SIZE) {
            throw std::runtime_error("Data too large for single page");
        }
        
        // Пишем размер
        *reinterpret_cast<size_t*>(page->data) = size;
        
        // Пишем данные
        if (size > 0) {
            memcpy(page->data + sizeof(size_t), data, size);
        }
        
        page_manager_.mark_dirty(page_id);
        return page_id;
    }
    
    std::vector<char> read_data_from_page(uint64_t page_id) const {
        auto page = page_manager_.load_page(page_id);
        
        size_t size = *reinterpret_cast<size_t*>(page->data);
        std::vector<char> result(size);
        
        if (size > 0) {
            memcpy(result.data(), page->data + sizeof(size_t), size);
        }
        
        return result;
    }
    
    // Сериализация узла B-дерева в страницу
    void save_node(NodeType* node, uint64_t& page_id_out, uint64_t& leaf_head_id_out) {
        if (!node) {
            page_id_out = 0;
            return;
        }
        
        uint64_t page_id = page_manager_.allocate_page();
        auto page = page_manager_.load_page(page_id);
        
        size_t offset = 0;
        
        // Флаг листа
        bool is_leaf = node->leaf;
        memcpy(page->data + offset, &is_leaf, sizeof(bool));
        offset += sizeof(bool);
        
        // Количество ключей
        memcpy(page->data + offset, &node->key_count, sizeof(size_t));
        offset += sizeof(size_t);
        
        // Ключи
        size_t keys_size = node->key_count * sizeof(Key);
        memcpy(page->data + offset, node->keys, keys_size);
        offset += keys_size;
        
        if (is_leaf) {
            auto* leaf = static_cast<LeafNodeType*>(node);
            
            // Значения (page_id для каждого ключа)
            for (size_t i = 0; i < node->key_count; ++i) {
                uint64_t value_page_id = leaf->values[i].second;
                memcpy(page->data + offset, &value_page_id, sizeof(uint64_t));
                offset += sizeof(uint64_t);
            }
            
            // Указатель на следующий лист
            uint64_t next_id = 0;
            if (leaf->next) {
                save_node(leaf->next, next_id, leaf_head_id_out);
            }
            memcpy(page->data + offset, &next_id, sizeof(uint64_t));
            
            if (!index_._leaf_head || leaf == index_._leaf_head) {
                leaf_head_id_out = page_id;
            }
        } else {
            // Дочерние узлы
            for (size_t i = 0; i <= node->key_count; ++i) {
                uint64_t child_id = 0;
                save_node(node->children[i], child_id, leaf_head_id_out);
                memcpy(page->data + offset, &child_id, sizeof(uint64_t));
                offset += sizeof(uint64_t);
            }
        }
        
        page_manager_.mark_dirty(page_id);
        page_id_out = page_id;
        node_page_map_[reinterpret_cast<uintptr_t>(node)] = page_id;
    }
    
    // Десериализация узла из страницы
    NodeType* load_node(uint64_t page_id) {
        if (page_id == 0) return nullptr;
        
        auto page = page_manager_.load_page(page_id);
        size_t offset = 0;
        
        bool is_leaf;
        memcpy(&is_leaf, page->data + offset, sizeof(bool));
        offset += sizeof(bool);
        
        auto* node = index_.create_node(is_leaf);
        node_page_map_[reinterpret_cast<uintptr_t>(node)] = page_id;
        
        size_t key_count;
        memcpy(&key_count, page->data + offset, sizeof(size_t));
        offset += sizeof(size_t);
        node->key_count = key_count;
        
        size_t keys_size = key_count * sizeof(Key);
        memcpy(node->keys, page->data + offset, keys_size);
        offset += keys_size;
        
        if (is_leaf) {
            auto* leaf = static_cast<LeafNodeType*>(node);
            
            for (size_t i = 0; i < key_count; ++i) {
                uint64_t value_page_id;
                memcpy(&value_page_id, page->data + offset, sizeof(uint64_t));
                offset += sizeof(uint64_t);
                leaf->values[i] = {node->keys[i], value_page_id};
            }
            
            uint64_t next_id;
            memcpy(&next_id, page->data + offset, sizeof(uint64_t));
            leaf->next = static_cast<LeafNodeType*>(load_node(next_id));
            
            if (!index_._leaf_head) {
                index_._leaf_head = leaf;
            }
        } else {
            for (size_t i = 0; i <= key_count; ++i) {
                uint64_t child_id;
                memcpy(&child_id, page->data + offset, sizeof(uint64_t));
                offset += sizeof(uint64_t);
                node->children[i] = load_node(child_id);
            }
        }
        
        return node;
    }
    
    PageManager& page_manager_;
    TreeType index_;
    std::unordered_map<uintptr_t, uint64_t> node_page_map_;
};

#endif // PAGE_STORAGE_H