#include "../include/Utils.h"
#include <sstream>
#include <iomanip>
#include <stdexcept>

std::chrono::system_clock::time_point parseTimestamp(const std::string& s) {
	// формат: yyyy.mm.dd-hh:mm:ss.msmsms
	int year, month, day, hour, minute, second, millisecond;
	char dot1, dot2, dash, colon1, colon2, dot3;

	std::istringstream iss(s);
	iss >> year >> dot1 >> month >> dot2 >> day >> dash
		>> hour >> colon1 >> minute >> colon2 >> second >> dot3 >> millisecond;

	if (iss.fail() || dot1 != '.' || dot2 != '.' || dash != '-' ||
		colon1 != ':' || colon2 != ':' || dot3 != '.') {
		throw std::runtime_error("Invalid timestamp format: " + s +
								 " (expected yyyy.mm.dd-hh:mm:ss.msmsms)");
		}

	std::tm tm = {};
	tm.tm_year = year - 1900;
	tm.tm_mon  = month - 1;
	tm.tm_mday = day;
	tm.tm_hour = hour;
	tm.tm_min  = minute;
	tm.tm_sec  = second;

	auto tp = std::chrono::system_clock::from_time_t(std::mktime(&tm));
	tp += std::chrono::milliseconds(millisecond);
	return tp;
}