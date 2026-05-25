#ifndef UTILS_H
#define UTILS_H

#include <string>
#include <chrono>

// Разбор строки вида "yyyy.mm.dd-hh:mm:ss.msmsms" в time_point
std::chrono::system_clock::time_point parseTimestamp(const std::string& s);

#endif // UTILS_H