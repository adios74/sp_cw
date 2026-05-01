#pragma once
#include <string>
#include <istream>
#include <optional>

class CommandReader {
public:
    explicit CommandReader(std::istream& input, bool interactive = false);

    // Возвращает следующую команду без завершающей ';'
    // или std::nullopt при конце ввода.
    std::optional<std::string> nextCommand();

private:
    std::istream& input_;
    bool interactive_;
    std::string buffer_;   // накопленные символы незавершённой команды
    bool eof_ = false;
};