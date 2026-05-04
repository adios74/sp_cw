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