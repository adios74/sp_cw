#include <iostream>
#include <fstream>
#include "Text handler/include/CommandReader.h"

int main(int argc, char* argv[]) {
    std::ifstream file;           // владеет файлом, если открыт
    std::istream* input = nullptr; // невладеющий указатель
    bool interactive = false;

    if (argc == 1) {
        input = &std::cin;
        interactive = true;
        std::cout << "Interactive mode. Type SQL commands terminated by ';'." << std::endl;
    } else if (argc == 2) {
        file.open(argv[1]);
        if (!file.is_open()) {
            std::cerr << "Error: cannot open file " << argv[1] << std::endl;
            return 1;
        }
        input = &file;
        std::cout << "Batch mode. Executing commands from " << argv[1] << std::endl;
    } else {
        std::cerr << "Usage: " << argv[0] << " [script_file]" << std::endl;
        return 1;
    }

    CommandReader reader(*input, interactive);
    while (auto command = reader.nextCommand()) {
        std::cout << "Executing: " << *command << std::endl;
    }

    return 0;
}