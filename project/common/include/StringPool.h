#ifndef STRING_POOL_H
#define STRING_POOL_H

#include <string>
#include <memory>
#include <unordered_map>
#include <mutex>

class StringPool {
public:
	static StringPool& instance();

	std::shared_ptr<const std::string> intern(const std::string& str);
	// Для строковых литералов
	std::shared_ptr<const std::string> intern(const char* str) {
		return intern(std::string(str));
	}

	// Очистка (для тестов)
	void clear();

	size_t size() const;

	void dumpStats() const;//udali

private:
	StringPool() = default;
	~StringPool() = default;
	StringPool(const StringPool&) = delete;
	StringPool& operator=(const StringPool&) = delete;

	std::unordered_map<std::string, std::shared_ptr<const std::string>> pool_;
	mutable std::mutex mutex_;
};

#endif // STRING_POOL_H