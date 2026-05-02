#include <iostream>
#include <fstream>
#include "Text handler/include/CommandReader.h"
#include "../common/include/AST.h"

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

        // Тест AST
    std::cout << "\n=== AST Tests ===" << std::endl;

    // SELECT *
    SelectStmt selectAll;
    selectAll.table.name = "users";
    Statement stmt1 = std::move(selectAll);          // ← перемещение
    std::cout << astToString(stmt1) << std::endl;

    // SELECT с алиасами и COUNT(*)
    SelectStmt selectComplex;
    selectComplex.table.name = "users";
    SelectColumn col1;
    col1.expr = ColumnRef{"name"};
    col1.alias = "username";
    selectComplex.columns.push_back(col1);

    SelectColumn col2;
    AggCall countCall;
    countCall.func = AggFunc::COUNT;
    countCall.arg = nullptr;
    col2.expr = countCall;
    col2.alias = "total";
    selectComplex.columns.push_back(col2);

    auto cond = makeComparison(ComparisonOp::EQUAL,
                            Value(ColumnRef{"id"}),
                            Value(5));
    selectComplex.condition = std::move(cond);
    Statement stmt2 = std::move(selectComplex);      // ← перемещение
    std::cout << astToString(stmt2) << std::endl;

    // INSERT — InsertStmt не содержит unique_ptr, можно копировать
    InsertStmt ins;
    ins.table.name = "users";
    ins.columns = {"name", "age"};
    ins.values.push_back({Value(std::string("Alice")), Value(30)});
    ins.values.push_back({Value(std::string("Bob")), Value(25)});
    Statement stmt3 = ins;                          // ← копирование (работает)
    std::cout << astToString(stmt3) << std::endl;

    while (auto command = reader.nextCommand()) {
        std::cout << "Executing: " << *command << std::endl;
    }

    return 0;
}