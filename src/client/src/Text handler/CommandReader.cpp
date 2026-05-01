#include "CommandReader.h"
#include <iostream>
#include <ostream>

CommandReader::CommandReader(std::istream& input, bool interactive)
    : input_(input), interactive_(interactive)
{}

std::optional<std::string> CommandReader::nextCommand() {
    if (eof_)
        return std::nullopt;

    while (true) {
        if (interactive_) {
            std::cout << "sql> " << std::flush;   // приглашение
        }

        std::string line;
        if (!std::getline(input_, line)) {
            eof_ = true;
            // Если в буфере что-то осталось без ';' — ошибка
            if (!buffer_.empty()) {
                std::cerr << "Error: missing semicolon at end of input" << std::endl;
                return std::nullopt;
            }
            return std::nullopt;
        }

        // Ищем ';' в прочитанной строке
        size_t pos = line.find(';');
        if (pos != std::string::npos) {
            // Присоединяем часть до ';' к буферу и возвращаем
            buffer_ += line.substr(0, pos);

            // Остаток строки после ';' кладём обратно в буфер для следующей команды
            std::string rest = line.substr(pos + 1);
            if (!rest.empty()) {
                // Это упрощённый вариант: остаток после ';' на той же строке
                // будет считан при следующем вызове. Поскольку мы не можем
                // положить обратно в istream, сохраним во внутренний буфер
                // и будем использовать его перед чтением новых строк.
                // Для упрощения тут используем отдельную переменную.
                // Можно реализовать через вспомогательный stringstream,
                // но для начала оставим так.
                buffer_ += ";"; // временный костыль: лучше переделать
            }

            std::string command = buffer_;
            buffer_.clear();
            if (!rest.empty()) {
                buffer_ = rest;
            }
            return command;
        } else {
            // Вся строка — часть многострочной команды
            buffer_ += line + " ";   // можно сохранять \n, но для парсера пробел тоже подойдёт
        }
    }
}