#include "../include/StringPool.h"
#include <iostream>

StringPool& StringPool::instance() {
	static StringPool pool;
	return pool;
}

std::shared_ptr<const std::string> StringPool::intern(const std::string& str) {
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = pool_.find(str);
	if (it != pool_.end()) {
		return it->second;
	}
	auto ptr = std::make_shared<const std::string>(str);
	pool_[str] = ptr;
	return ptr;
}

void StringPool::clear() {
	std::lock_guard<std::mutex> lock(mutex_);
	pool_.clear();
}

size_t StringPool::size() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return pool_.size();
}

void StringPool::dumpStats() const {
	std::lock_guard<std::mutex> lock(mutex_);
	std::cout << "StringPool: " << pool_.size() << " unique strings\n";
	for (const auto& [str, ptr] : pool_) {
		std::cout << "  \"" << str << "\" use_count=" << ptr.use_count() << '\n';
	}
}