#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <algorithm>
#include <random>
#include <stdexcept>

#include "Index.h"

using Tree = BP_tree<int, std::string>;

TEST(BPTreeTest, EmptyTree) {
    Tree t;
    EXPECT_TRUE(t.empty());
    EXPECT_EQ(t.size(), 0u);
    EXPECT_EQ(t.begin(), t.end());
    EXPECT_FALSE(t.contains(42));
    EXPECT_EQ(t.find(42), t.end());
    EXPECT_EQ(t.lower_bound(42), t.end());
    EXPECT_EQ(t.upper_bound(42), t.end());
}

TEST(BPTreeTest, InsertAndFind) {
    Tree t;
    auto [it, inserted] = t.insert({10, "ten"});
    EXPECT_TRUE(inserted);
    EXPECT_EQ(it->first, 10);
    EXPECT_EQ(it->second, "ten");

    EXPECT_EQ(t.size(), 1u);
    EXPECT_FALSE(t.empty());
    EXPECT_TRUE(t.contains(10));
    EXPECT_EQ(t.at(10), "ten");
    EXPECT_THROW(t.at(99), std::out_of_range);

    auto it2 = t.find(10);
    ASSERT_NE(it2, t.end());
    EXPECT_EQ(it2->second, "ten");

    auto [dup_it, dup_ins] = t.insert({10, "different"});
    EXPECT_FALSE(dup_ins);
    EXPECT_EQ(dup_it->second, "ten");
    EXPECT_EQ(t.size(), 1u);
}

TEST(BPTreeTest, Iteration) {
    Tree t;
    std::vector<std::pair<int, std::string>> data = {
        {3, "three"}, {1, "one"}, {4, "four"}, {1, "another"}, {2, "two"}
    };
    for (auto& p : data) t.insert(p);

    auto it = t.begin();
    ASSERT_NE(it, t.end());
    EXPECT_EQ(it->first, 1); EXPECT_EQ(it->second, "one"); ++it;
    ASSERT_NE(it, t.end());
    EXPECT_EQ(it->first, 2); EXPECT_EQ(it->second, "two"); ++it;
    ASSERT_NE(it, t.end());
    EXPECT_EQ(it->first, 3); EXPECT_EQ(it->second, "three"); ++it;
    ASSERT_NE(it, t.end());
    EXPECT_EQ(it->first, 4); EXPECT_EQ(it->second, "four"); ++it;
    EXPECT_EQ(it, t.end());

    const Tree& ct = t;
    auto cit = ct.begin();
    EXPECT_EQ(cit->first, 1);
}

TEST(BPTreeTest, LowerUpperBound) {
    Tree t;
    for (int k : {2, 5, 8, 12, 17}) t.insert({k, std::to_string(k)});

    EXPECT_EQ(t.lower_bound(1)->first, 2);
    EXPECT_EQ(t.lower_bound(2)->first, 2);
    EXPECT_EQ(t.lower_bound(5)->first, 5);
    EXPECT_EQ(t.lower_bound(18), t.end());
    EXPECT_EQ(t.upper_bound(5)->first, 8);
    EXPECT_EQ(t.upper_bound(17), t.end());
}

TEST(BPTreeTest, EraseByKey) {
    Tree t;
    t.insert({1, "one"}); t.insert({2, "two"}); t.insert({3, "three"});
    EXPECT_EQ(t.size(), 3u);

    auto it = t.erase(2);
    ASSERT_NE(it, t.end());
    EXPECT_EQ(it->first, 3);
    EXPECT_EQ(t.size(), 2u);
    EXPECT_FALSE(t.contains(2));

    auto it2 = t.erase(100);
    EXPECT_EQ(it2, t.end());

    t.erase(1); t.erase(3);
    EXPECT_TRUE(t.empty());
    EXPECT_EQ(t.begin(), t.end());
}

TEST(BPTreeTest, EraseIterator) {
    Tree t;
    for (int k = 1; k <= 5; ++k) t.insert({k, ""});
    auto it = t.find(3);
    auto next = t.erase(it);
    ASSERT_NE(next, t.end());
    EXPECT_EQ(next->first, 4);
    EXPECT_EQ(t.size(), 4u);

    it = t.find(5);
    next = t.erase(it);
    EXPECT_EQ(next, t.end());
    EXPECT_EQ(t.size(), 3u);
}

TEST(BPTreeTest, InsertOrAssign) {
    Tree t;
    // insert_or_assign возвращает итератор, а не пару
    auto it1 = t.insert_or_assign({10, "ten"});
    EXPECT_EQ(it1->second, "ten");

    auto it2 = t.insert_or_assign({10, "new"});
    EXPECT_EQ(it2->second, "new");
    EXPECT_EQ(t.size(), 1u);
}

TEST(BPTreeTest, Emplace) {
    Tree t;
    auto [it, inserted] = t.emplace(42, "forty two");
    EXPECT_TRUE(inserted);
    EXPECT_EQ(it->first, 42);
    EXPECT_EQ(it->second, "forty two");
}

TEST(BPTreeTest, CopyAndMove) {
    Tree t;
    t.insert({1, "a"}); t.insert({3, "b"});
    Tree t2(t);
    EXPECT_EQ(t2.size(), 2u);
    EXPECT_TRUE(t2.contains(1));
    EXPECT_TRUE(t2.contains(3));

    Tree t3 = std::move(t2);
    EXPECT_EQ(t3.size(), 2u);
    EXPECT_TRUE(t2.empty());

    Tree t4;
    t4.insert({10, "x"});
    swap(t3, t4);
    EXPECT_EQ(t3.size(), 1u);
    EXPECT_EQ(t4.size(), 2u);
}

TEST(BPTreeTest, CustomComparator) {
    BP_tree<int, std::string, std::greater<int>> t;
    t.insert({5, "five"}); t.insert({2, "two"}); t.insert({8, "eight"});
    auto it = t.begin();
    EXPECT_EQ(it->first, 8); ++it;
    EXPECT_EQ(it->first, 5); ++it;
    EXPECT_EQ(it->first, 2); ++it;
    EXPECT_EQ(it, t.end());
}

TEST(BPTreeTest, LeafLinkIntegrity) {
    Tree t;
    for (int k = 0; k < 15; ++k) t.insert({k, ""});
    auto it = t.begin();
    int count = 0;
    int prev = -999;
    while (it != t.end()) {
        EXPECT_GT(it->first, prev);
        prev = it->first;
        ++count;
        ++it;
    }
    EXPECT_EQ(count, 15);
}