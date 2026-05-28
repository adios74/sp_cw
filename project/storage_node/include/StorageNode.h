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
#include "Index.h"

struct PageHeader {
    uint64_t page_id;
    bool dirty;
};

struct PageConfig {
    static constexpr size_t PAGE_SIZE = 4096;
    static constexpr size_t HEADER_SIZE = sizeof(PageHeader);
    static constexpr size_t DATA_SIZE = PAGE_SIZE - HEADER_SIZE;
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
    
    const std::string& getFilePath() const {
        return filename_;
    }
    
    void mark_dirty(uint64_t page_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_.find(page_id);
        if (it != cache_.end()) {
            it->second->header.dirty = true;
        }
    }

    std::shared_ptr<Page> load_page(uint64_t page_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = cache_.find(page_id);
        if (it != cache_.end()) {
            return it->second;
        }
        
        auto page = std::make_shared<Page>();
        if (page_id == 0) {
            page->header.page_id = ++next_page_id_;
            cache_[page->header.page_id] = page;
            return page;
        }
        
        size_t offset = calculate_offset(page_id);
        file_.seekg(offset);
        file_.read(reinterpret_cast<char*>(page.get()), sizeof(Page));
        
        if (file_.fail()) {
            throw std::runtime_error("Failed to read page " + std::to_string(page_id));
        }
        
        cache_[page_id] = page;
        return page;
    }
    
    void write_page(uint64_t page_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = cache_.find(page_id);
        if (it == cache_.end()) {
            return;
        }
        
        size_t offset = calculate_offset(page_id);
        file_.seekp(offset);
        file_.write(reinterpret_cast<const char*>(it->second.get()), sizeof(Page));
        
        if (file_.fail()) {
            throw std::runtime_error("Failed to write page " + std::to_string(page_id));
        }
    }
    
    void flush_page(uint64_t page_id) {
        write_page(page_id);
        std::lock_guard<std::mutex> lock(mutex_);
        cache_.erase(page_id);
    }
    
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
    
    uint64_t allocate_page() {
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t new_id = ++next_page_id_;
        auto page = std::make_shared<Page>();
        page->header.page_id = new_id;
        cache_[new_id] = page;
        return new_id;
    }
    
    void free_page(uint64_t page_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_.erase(page_id);
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
public:
    using value_type = T;
    
    size_t max_size() const noexcept {
        return std::numeric_limits<size_t>::max() / sizeof(T);
    }
    
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
        
        if (n == 1) {
            return static_cast<T*>(::operator new(sizeof(T)));
        }
        
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
    using TreeType = BSP_tree<Key, uint64_t, std::less<Key>>;
    using LeafNodeType = typename TreeType::LeafNode;
    using InternalNodeType = typename TreeType::InternalNode;
    
public:
    explicit PageBasedIndex(PageManager& pm) 
        : page_manager_(pm), index_() {}
    
    ~PageBasedIndex() = default;
    
    void insert(const Key& key, const void* data, size_t size) {
        auto it = index_.find(key);
        
        if (it != index_.end()) {
            uint64_t old_page_id = it->second;
            page_manager_.free_page(old_page_id);
            uint64_t new_page_id = write_data_to_page(data, size);
            index_.erase(it);
            index_.insert({key, new_page_id});
        } else {
            uint64_t page_id = write_data_to_page(data, size);
            index_.insert({key, page_id});
        }
    }
    
    void insert_string(const Key& key, const std::string& value) {
        insert(key, value.data(), value.size());
    }
    
    template<typename T>
    void insert_value(const Key& key, const T& value) {
        static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
        insert(key, &value, sizeof(T));
    }
    
    std::optional<std::vector<char>> find(const Key& key) const {
        auto it = index_.find(key);
        if (it == index_.end()) {
            return std::nullopt;
        }
        return read_data_from_page(it->second);
    }
    
    std::optional<std::string> find_string(const Key& key) const {
        auto data = find(key);
        if (!data) {
            return std::nullopt;
        }
        return std::string(data->begin(), data->end());
    }
    
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
    
    bool remove(const Key& key) {
        auto it = index_.find(key);
        if (it == index_.end()) {
            return false;
        }
        
        page_manager_.free_page(it->second);
        index_.erase(it);
        return true;
    }
    
    bool contains(const Key& key) const {
        return index_.find(key) != index_.end();
    }
    
    size_t size() const {
        return index_.size();
    }
    
    void clear() {
        for (auto it = index_.begin(); it != index_.end(); ++it) {
            page_manager_.free_page(it->second);
        }
        index_.clear();
    }
    
    void save_metadata(uint64_t& root_page_id, uint64_t& leaf_head_page_id) {
        // Сначала находим первый лист
        LeafNodeType* first_leaf = nullptr;
        if (index_._root) {
            auto* node = index_._root;
            while (!node->is_leaf) {
                node = static_cast<InternalNodeType*>(node)->children[0];
            }
            first_leaf = static_cast<LeafNodeType*>(node);
        }
        
        leaf_head_page_id = 0;
        save_node(index_._root, root_page_id, leaf_head_page_id, first_leaf);
    }
    
    void load_metadata(uint64_t root_page_id, uint64_t leaf_head_page_id) {
        index_.clear();
        if (root_page_id != 0) {
            index_._root = load_node(root_page_id);
        }
        // leaf_head_page_id используется только для информации,
        // _leaf_head отсутствует в дереве, поэтому просто игнорируем
    }
    
    auto& tree() { return index_; }
    const auto& tree() const { return index_; }
    
private:
    uint64_t write_data_to_page(const void* data, size_t size) {
        uint64_t page_id = page_manager_.allocate_page();
        auto page = page_manager_.load_page(page_id);
        
        if (size + sizeof(size_t) > PageConfig::DATA_SIZE) {
            throw std::runtime_error("Data too large for single page");
        }
        
        *reinterpret_cast<size_t*>(page->data) = size;
        
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
    
    void save_node(typename TreeType::NodeBase* node, uint64_t& page_id_out, 
                   uint64_t& leaf_head_id_out, LeafNodeType* first_leaf) {
        if (!node) {
            page_id_out = 0;
            return;
        }
        
        uint64_t page_id = page_manager_.allocate_page();
        auto page = page_manager_.load_page(page_id);
        size_t offset = 0;
        
        bool is_leaf = node->is_leaf;
        memcpy(page->data + offset, &is_leaf, sizeof(bool));
        offset += sizeof(bool);
        
        if (is_leaf) {
            auto* leaf = static_cast<LeafNodeType*>(node);
            size_t key_count = leaf->data.size();
            memcpy(page->data + offset, &key_count, sizeof(size_t));
            offset += sizeof(size_t);
            
            for (size_t i = 0; i < key_count; ++i) {
                const auto& kv = leaf->data[i];
                Key k = kv.first;
                uint64_t v = kv.second;
                memcpy(page->data + offset, &k, sizeof(Key));
                offset += sizeof(Key);
                memcpy(page->data + offset, &v, sizeof(uint64_t));
                offset += sizeof(uint64_t);
            }
            
            uint64_t next_id = 0;
            if (leaf->next) {
                save_node(leaf->next, next_id, leaf_head_id_out, first_leaf);
            }
            memcpy(page->data + offset, &next_id, sizeof(uint64_t));
            offset += sizeof(uint64_t);
            
            if (leaf == first_leaf) {
                leaf_head_id_out = page_id;
            }
        } else {
            auto* internal = static_cast<InternalNodeType*>(node);
            size_t key_count = internal->keys.size();
            memcpy(page->data + offset, &key_count, sizeof(size_t));
            offset += sizeof(size_t);
            
            size_t keys_size = key_count * sizeof(Key);
            if (keys_size > 0) {
                memcpy(page->data + offset, internal->keys.data(), keys_size);
                offset += keys_size;
            }
            
            for (size_t i = 0; i < internal->children.size(); ++i) {
                uint64_t child_id = 0;
                save_node(internal->children[i], child_id, leaf_head_id_out, first_leaf);
                memcpy(page->data + offset, &child_id, sizeof(uint64_t));
                offset += sizeof(uint64_t);
            }
        }
        
        page_manager_.mark_dirty(page_id);
        page_id_out = page_id;
        node_page_map_[reinterpret_cast<uintptr_t>(node)] = page_id;
    }
    
    typename TreeType::NodeBase* load_node(uint64_t page_id) {
        if (page_id == 0) return nullptr;
        
        auto page = page_manager_.load_page(page_id);
        size_t offset = 0;
        
        bool is_leaf;
        memcpy(&is_leaf, page->data + offset, sizeof(bool));
        offset += sizeof(bool);
        
        typename TreeType::NodeBase* node = nullptr;
        if (is_leaf) {
            auto* leaf = index_.make_node_leaf();
            node = leaf;
            node_page_map_[reinterpret_cast<uintptr_t>(node)] = page_id;
            
            size_t key_count;
            memcpy(&key_count, page->data + offset, sizeof(size_t));
            offset += sizeof(size_t);
            
            for (size_t i = 0; i < key_count; ++i) {
                Key k;
                uint64_t v;
                memcpy(&k, page->data + offset, sizeof(Key));
                offset += sizeof(Key);
                memcpy(&v, page->data + offset, sizeof(uint64_t));
                offset += sizeof(uint64_t);
                leaf->data.push_back({k, v});
            }
            
            uint64_t next_id;
            memcpy(&next_id, page->data + offset, sizeof(uint64_t));
            offset += sizeof(uint64_t);
            leaf->next = static_cast<LeafNodeType*>(load_node(next_id));
        } else {
            auto* internal = index_.make_node_internal();
            node = internal;
            node_page_map_[reinterpret_cast<uintptr_t>(node)] = page_id;
            
            size_t key_count;
            memcpy(&key_count, page->data + offset, sizeof(size_t));
            offset += sizeof(size_t);
            
            if (key_count > 0) {
                internal->keys.resize(key_count);
                size_t keys_size = key_count * sizeof(Key);
                memcpy(internal->keys.data(), page->data + offset, keys_size);
                offset += keys_size;
            }
            
            for (size_t i = 0; i < key_count + 1; ++i) {
                uint64_t child_id;
                memcpy(&child_id, page->data + offset, sizeof(uint64_t));
                offset += sizeof(uint64_t);
                typename TreeType::NodeBase* child = load_node(child_id);
                internal->children.push_back(child);
            }
        }
        
        return node;
    }
    
    PageManager& page_manager_;
    TreeType index_;
    std::unordered_map<uintptr_t, uint64_t> node_page_map_;
};

#endif // PAGE_STORAGE_H