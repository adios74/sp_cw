#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <variant>

#include "../include/DbEngine.h"
#include "../../common/include/AST.h"
#include "../../common/include/Parser.h"

namespace fs = std::filesystem;

class DBEngineTest : public ::testing::Test {
protected:
    const std::string test_dir = "test_db_data";
    
    void SetUp() override {
        if (fs::exists(test_dir)) {
            fs::remove_all(test_dir);
        }
        fs::create_directories(test_dir);
    }
    
    void TearDown() override {
        if (fs::exists(test_dir)) {
            fs::remove_all(test_dir);
        }
    }
    
    CreateTableStmt createSimpleTableStmt(const std::string& table_name = "users") {
        CreateTableStmt stmt;
        stmt.table.name = table_name;
        
        ColumnDef col1;
        col1.name = "id";
        col1.type = "INT";
        
        ColumnDef col2;
        col2.name = "name";
        col2.type = "STRING";
        
        stmt.columns = {col1, col2};
        return stmt;
    }
    
    Row createRow(int id, const std::string& name) {
        Row row;
        row.push_back(id);
        row.push_back(name);
        return row;
    }
    
    ColumnRef makeColumnRef(const std::string& column) {
        ColumnRef ref;
        ref.column = column;
        return ref;
    }
    
    // Исправлено: принимаем по значению и перемещаем
    Statement makeStatement(UpdateStmt stmt) {
        return Statement{std::move(stmt)};
    }
    
    // Исправлено: принимаем по значению и перемещаем
    Statement makeStatement(DeleteStmt stmt) {
        return Statement{std::move(stmt)};
    }
    
    Statement makeStatement(const InsertStmt& stmt) {
        return Statement{stmt};
    }
    
    Statement makeStatement(const CreateTableStmt& stmt) {
        return Statement{stmt};
    }
    
    Statement makeStatement(const CreateDatabaseStmt& stmt) {
        return Statement{stmt};
    }
    
    Statement makeStatement(const DropTableStmt& stmt) {
        return Statement{stmt};
    }
    
    Statement makeStatement(const DropDatabaseStmt& stmt) {
        return Statement{stmt};
    }
    
    Statement makeStatement(const UseStmt& stmt) {
        return Statement{stmt};
    }
};

// ==================== Database Tests ====================

TEST_F(DBEngineTest, CreateDatabase) {
    DBMS dbms(test_dir);
    
    EXPECT_TRUE(dbms.createDatabase("test_db"));
    EXPECT_TRUE(dbms.createDatabase("test_db2"));
    EXPECT_FALSE(dbms.createDatabase("test_db")); // Duplicate
    
    EXPECT_TRUE(fs::exists(test_dir + "/test_db"));
    EXPECT_TRUE(fs::exists(test_dir + "/test_db2"));
}

TEST_F(DBEngineTest, DropDatabase) {
    DBMS dbms(test_dir);
    
    dbms.createDatabase("test_db");
    EXPECT_TRUE(fs::exists(test_dir + "/test_db"));
    
    EXPECT_TRUE(dbms.dropDatabase("test_db"));
    EXPECT_FALSE(fs::exists(test_dir + "/test_db"));
    
    EXPECT_FALSE(dbms.dropDatabase("non_existent"));
}

TEST_F(DBEngineTest, UseDatabase) {
    DBMS dbms(test_dir);
    
    dbms.createDatabase("test_db");
    
    EXPECT_TRUE(dbms.useDatabase("test_db"));
    EXPECT_NE(dbms.currentDatabase(), nullptr);
    EXPECT_EQ(dbms.currentDatabase()->name(), "test_db");
    
    EXPECT_FALSE(dbms.useDatabase("non_existent"));
}

TEST_F(DBEngineTest, CreateTable) {
    DBMS dbms(test_dir);
    dbms.createDatabase("test_db");
    dbms.useDatabase("test_db");
    
    auto db = dbms.currentDatabase();
    
    EXPECT_TRUE(db->createTable(createSimpleTableStmt("users")));
    EXPECT_FALSE(db->createTable(createSimpleTableStmt("users"))); // Duplicate
    
    auto table = db->getTable("users");
    EXPECT_NE(table, nullptr);
    EXPECT_EQ(table->metadata().name, "users");
    EXPECT_EQ(table->metadata().columns.size(), 2);
}

TEST_F(DBEngineTest, DropTable) {
    DBMS dbms(test_dir);
    dbms.createDatabase("test_db");
    dbms.useDatabase("test_db");
    
    auto db = dbms.currentDatabase();
    db->createTable(createSimpleTableStmt("users"));
    
    EXPECT_TRUE(db->dropTable("users"));
    EXPECT_EQ(db->getTable("users"), nullptr);
    EXPECT_FALSE(db->dropTable("users")); // Already dropped
}

TEST_F(DBEngineTest, InsertAndSelectAllRows) {
    DBMS dbms(test_dir);
    dbms.createDatabase("test_db");
    dbms.useDatabase("test_db");
    
    auto db = dbms.currentDatabase();
    db->createTable(createSimpleTableStmt("users"));
    
    auto table = db->getTable("users");
    table->insertRow(createRow(1, "Alice"));
    table->insertRow(createRow(2, "Bob"));
    table->insertRow(createRow(3, "Charlie"));
    
    EXPECT_EQ(table->metadata().row_count, 3);
    
    auto rows = table->selectRows();
    EXPECT_EQ(rows.size(), 3);
    
    // Verify first row
    EXPECT_EQ(std::get<int>(rows[0][0]), 1);
    EXPECT_EQ(std::get<std::string>(rows[0][1]), "Alice");
}

TEST_F(DBEngineTest, SelectWithCondition) {
    DBMS dbms(test_dir);
    dbms.createDatabase("test_db");
    dbms.useDatabase("test_db");
    
    auto db = dbms.currentDatabase();
    db->createTable(createSimpleTableStmt("users"));
    
    auto table = db->getTable("users");
    table->insertRow(createRow(1, "Alice"));
    table->insertRow(createRow(2, "Bob"));
    table->insertRow(createRow(3, "Alice"));
    
    auto condition = std::make_unique<Expr>();
    condition->type = Expr::COMPARISON;
    condition->comparison.op = ComparisonOp::EQUAL;
    condition->comparison.left = makeColumnRef("name");
    condition->comparison.right = std::string("Alice");
    
    auto rows = table->selectRows(condition.get());
    EXPECT_EQ(rows.size(), 2);
}

TEST_F(DBEngineTest, SelectWithIntComparison) {
    DBMS dbms(test_dir);
    dbms.createDatabase("test_db");
    dbms.useDatabase("test_db");
    
    auto db = dbms.currentDatabase();
    db->createTable(createSimpleTableStmt("users"));
    
    auto table = db->getTable("users");
    table->insertRow(createRow(1, "Alice"));
    table->insertRow(createRow(2, "Bob"));
    table->insertRow(createRow(3, "Charlie"));
    
    auto condition = std::make_unique<Expr>();
    condition->type = Expr::COMPARISON;
    condition->comparison.op = ComparisonOp::GREATER;
    condition->comparison.left = makeColumnRef("id");
    condition->comparison.right = 1;
    
    auto rows = table->selectRows(condition.get());
    EXPECT_EQ(rows.size(), 2);
}

TEST_F(DBEngineTest, UpdateRows) {
    DBMS dbms(test_dir);
    dbms.createDatabase("test_db");
    dbms.useDatabase("test_db");
    
    auto db = dbms.currentDatabase();
    db->createTable(createSimpleTableStmt("users"));
    
    auto table = db->getTable("users");
    table->insertRow(createRow(1, "Alice"));
    table->insertRow(createRow(2, "Bob"));
    table->insertRow(createRow(3, "Alice"));
    
    auto condition = std::make_unique<Expr>();
    condition->type = Expr::COMPARISON;
    condition->comparison.op = ComparisonOp::EQUAL;
    condition->comparison.left = makeColumnRef("name");
    condition->comparison.right = std::string("Alice");
    
    std::vector<std::pair<std::string, Value>> assignments = {
        {"name", std::string("Updated")}
    };
    
    size_t updated = table->updateRows(assignments, condition.get());
    EXPECT_EQ(updated, 2);
    
    auto rows = table->selectRows();
    int updated_count = 0;
    for (const auto& row : rows) {
        if (std::get<std::string>(row[1]) == "Updated") {
            updated_count++;
        }
    }
    EXPECT_EQ(updated_count, 2);
}

TEST_F(DBEngineTest, DeleteRows) {
    DBMS dbms(test_dir);
    dbms.createDatabase("test_db");
    dbms.useDatabase("test_db");
    
    auto db = dbms.currentDatabase();
    db->createTable(createSimpleTableStmt("users"));
    
    auto table = db->getTable("users");
    table->insertRow(createRow(1, "Alice"));
    table->insertRow(createRow(2, "Bob"));
    table->insertRow(createRow(3, "Charlie"));
    
    auto condition = std::make_unique<Expr>();
    condition->type = Expr::COMPARISON;
    condition->comparison.op = ComparisonOp::EQUAL;
    condition->comparison.left = makeColumnRef("id");
    condition->comparison.right = 2;
    
    size_t deleted = table->deleteRows(condition.get());
    EXPECT_EQ(deleted, 1);
    
    auto rows = table->selectRows();
    EXPECT_EQ(rows.size(), 2);
}

TEST_F(DBEngineTest, DeleteAllRows) {
    DBMS dbms(test_dir);
    dbms.createDatabase("test_db");
    dbms.useDatabase("test_db");
    
    auto db = dbms.currentDatabase();
    db->createTable(createSimpleTableStmt("users"));
    
    auto table = db->getTable("users");
    table->insertRow(createRow(1, "Alice"));
    table->insertRow(createRow(2, "Bob"));
    
    size_t deleted = table->deleteRows();
    EXPECT_EQ(deleted, 2);
    
    auto rows = table->selectRows();
    EXPECT_EQ(rows.size(), 0);
}

// ==================== SQL Executor Tests ====================

TEST_F(DBEngineTest, ExecuteInsertAndSelect) {
    DBMS dbms(test_dir);
    SQLExecutor executor(dbms);
    
    CreateDatabaseStmt create_db;
    create_db.name = "test_db";
    executor.execute(makeStatement(create_db));
    
    UseStmt use_db;
    use_db.name = "test_db";
    executor.execute(makeStatement(use_db));
    
    executor.execute(makeStatement(createSimpleTableStmt("users")));
    
    InsertStmt insert;
    insert.table.name = "users";
    insert.values.push_back(createRow(1, "Alice"));
    insert.values.push_back(createRow(2, "Bob"));
    executor.execute(makeStatement(insert));
    
    auto db = dbms.currentDatabase();
    auto table = db->getTable("users");
    auto rows = table->selectRows();
    EXPECT_EQ(rows.size(), 2);
}

TEST_F(DBEngineTest, ExecuteUpdate) {
    DBMS dbms(test_dir);
    SQLExecutor executor(dbms);
    
    CreateDatabaseStmt create_db;
    create_db.name = "test_db";
    executor.execute(makeStatement(create_db));
    
    UseStmt use_db;
    use_db.name = "test_db";
    executor.execute(makeStatement(use_db));
    
    executor.execute(makeStatement(createSimpleTableStmt("users")));
    
    InsertStmt insert;
    insert.table.name = "users";
    insert.values.push_back(createRow(1, "Alice"));
    executor.execute(makeStatement(insert));
    
    UpdateStmt update;
    update.table.name = "users";
    update.assignments = {{"name", std::string("Updated")}};
    
    auto condition = std::make_unique<Expr>();
    condition->type = Expr::COMPARISON;
    condition->comparison.op = ComparisonOp::EQUAL;
    condition->comparison.left = makeColumnRef("id");
    condition->comparison.right = 1;
    update.condition = std::move(condition);
    
    // Перемещаем объект, так как он содержит unique_ptr
    executor.execute(makeStatement(std::move(update)));
    
    auto db = dbms.currentDatabase();
    auto table = db->getTable("users");
    auto rows = table->selectRows();
    EXPECT_EQ(std::get<std::string>(rows[0][1]), "Updated");
}

TEST_F(DBEngineTest, ExecuteDelete) {
    DBMS dbms(test_dir);
    SQLExecutor executor(dbms);
    
    CreateDatabaseStmt create_db;
    create_db.name = "test_db";
    executor.execute(makeStatement(create_db));
    
    UseStmt use_db;
    use_db.name = "test_db";
    executor.execute(makeStatement(use_db));
    
    executor.execute(makeStatement(createSimpleTableStmt("users")));
    
    InsertStmt insert;
    insert.table.name = "users";
    insert.values.push_back(createRow(1, "Alice"));
    insert.values.push_back(createRow(2, "Bob"));
    executor.execute(makeStatement(insert));
    
    DeleteStmt delete_stmt;
    delete_stmt.table.name = "users";
    
    auto condition = std::make_unique<Expr>();
    condition->type = Expr::COMPARISON;
    condition->comparison.op = ComparisonOp::EQUAL;
    condition->comparison.left = makeColumnRef("id");
    condition->comparison.right = 1;
    delete_stmt.condition = std::move(condition);
    
    // Перемещаем объект, так как он содержит unique_ptr
    executor.execute(makeStatement(std::move(delete_stmt)));
    
    auto db = dbms.currentDatabase();
    auto table = db->getTable("users");
    auto rows = table->selectRows();
    EXPECT_EQ(rows.size(), 1);
    EXPECT_EQ(std::get<int>(rows[0][0]), 2);
}

// ==================== Edge Cases ====================

TEST_F(DBEngineTest, EmptyTable) {
    DBMS dbms(test_dir);
    dbms.createDatabase("test_db");
    dbms.useDatabase("test_db");
    
    auto db = dbms.currentDatabase();
    db->createTable(createSimpleTableStmt("users"));
    
    auto table = db->getTable("users");
    auto rows = table->selectRows();
    EXPECT_TRUE(rows.empty());
}

TEST_F(DBEngineTest, UpdateNonExistentRows) {
    DBMS dbms(test_dir);
    dbms.createDatabase("test_db");
    dbms.useDatabase("test_db");
    
    auto db = dbms.currentDatabase();
    db->createTable(createSimpleTableStmt("users"));
    
    auto table = db->getTable("users");
    table->insertRow(createRow(1, "Alice"));
    
    auto condition = std::make_unique<Expr>();
    condition->type = Expr::COMPARISON;
    condition->comparison.op = ComparisonOp::EQUAL;
    condition->comparison.left = makeColumnRef("id");
    condition->comparison.right = 999;
    
    std::vector<std::pair<std::string, Value>> assignments = {
        {"name", std::string("Updated")}
    };
    
    size_t updated = table->updateRows(assignments, condition.get());
    EXPECT_EQ(updated, 0);
}

TEST_F(DBEngineTest, NotEqualComparison) {
    DBMS dbms(test_dir);
    dbms.createDatabase("test_db");
    dbms.useDatabase("test_db");
    
    auto db = dbms.currentDatabase();
    db->createTable(createSimpleTableStmt("users"));
    
    auto table = db->getTable("users");
    table->insertRow(createRow(1, "Alice"));
    table->insertRow(createRow(2, "Bob"));
    table->insertRow(createRow(3, "Alice"));
    
    auto condition = std::make_unique<Expr>();
    condition->type = Expr::COMPARISON;
    condition->comparison.op = ComparisonOp::NOT_EQUAL;
    condition->comparison.left = makeColumnRef("name");
    condition->comparison.right = std::string("Alice");
    
    auto rows = table->selectRows(condition.get());
    EXPECT_EQ(rows.size(), 1);
    EXPECT_EQ(std::get<std::string>(rows[0][1]), "Bob");
}

TEST_F(DBEngineTest, LessThanComparison) {
    DBMS dbms(test_dir);
    dbms.createDatabase("test_db");
    dbms.useDatabase("test_db");
    
    auto db = dbms.currentDatabase();
    db->createTable(createSimpleTableStmt("users"));
    
    auto table = db->getTable("users");
    table->insertRow(createRow(1, "Alice"));
    table->insertRow(createRow(2, "Bob"));
    table->insertRow(createRow(3, "Charlie"));
    
    auto condition = std::make_unique<Expr>();
    condition->type = Expr::COMPARISON;
    condition->comparison.op = ComparisonOp::LESS;
    condition->comparison.left = makeColumnRef("id");
    condition->comparison.right = 3;
    
    auto rows = table->selectRows(condition.get());
    EXPECT_EQ(rows.size(), 2);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}