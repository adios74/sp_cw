#include "../include/CommandReader.h"
#include <iostream>
#include <algorithm>

CommandReader::CommandReader(std::istream& input, bool interactive)
    : input_(input), interactive_(interactive)
{}

static size_t findCommandEnd(const std::string& buffer) {
    bool in_string = false;
    char prev = '\0';
    for (size_t i = 0; i < buffer.size(); ++i) {
        char ch = buffer[i];
        if (in_string) {
            if (ch == '"' && prev != '\\') {
                in_string = false;
            }
        } else {
            if (ch == '"') {
                in_string = true;
            } else if (ch == ';') {
                return i;
            }
        }
        prev = ch;
    }
    return std::string::npos;
}

// Проверка: строка состоит только из пробельных символов?
static bool isBlank(const std::string& s) {
    return std::all_of(s.begin(), s.end(), 
                       [](unsigned char c) { return std::isspace(c); });
}

std::optional<std::string> CommandReader::nextCommand() {
    if (eof_)
        return std::nullopt;

    while (true) {
        size_t endPos = findCommandEnd(buffer_);
        if (endPos != std::string::npos) {
            std::string command = buffer_.substr(0, endPos);
            buffer_ = buffer_.substr(endPos + 1);
            
            // Убираем пробелы по краям команды
            size_t start = command.find_first_not_of(" \t\n\r");
            if (start == std::string::npos) {
                // Команда состоит только из пробелов — игнорируем
                continue;
            }
            size_t end = command.find_last_not_of(" \t\n\r");
            command = command.substr(start, end - start + 1);
            
            return command;
        }

        if (interactive_) {
            std::cout << "sql> " << std::flush;
        }

        std::string line;
        if (!std::getline(input_, line)) {
            eof_ = true;
            if (!buffer_.empty()) {
                std::cerr << "Error: missing semicolon at end of input." << std::endl;
                buffer_.clear();
            }
            return std::nullopt;
        }

        // Если строка пустая или состоит только из пробелов — игнорируем её
        if (isBlank(line)) {
            continue;
        }

        // Добавляем непустую строку к буферу
        if (!buffer_.empty()) {
            buffer_ += ' ';
        }
        buffer_ += line;
    }
}