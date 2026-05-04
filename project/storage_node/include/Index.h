#ifndef SYS_PROG_BP_TREE_H
#define SYS_PROG_BP_TREE_H

#include <iterator>
#include <utility>
#include <vector>
#include <boost/container/static_vector.hpp>
#include <pp_allocator.h>
#include <associative_container.h>
#include <initializer_list>
#include <optional>
#include <stack>
#include <algorithm>
#include <functional>
#include <unordered_map>
#include <memory>

template <typename tkey, typename tvalue, comparator<tkey> compare = std::less<tkey>, std::size_t t = 5>
class BP_tree final : private compare
{

    template<typename Key>
    friend class PageBasedIndex;

public:
    using tree_data_type = std::pair<tkey, tvalue>;
    using tree_data_type_const = std::pair<const tkey, tvalue>;
    using value_type = tree_data_type_const;
    using allocator_type = pp_allocator<value_type>;

private:
    static constexpr size_t max_keys = 2 * t - 1;
    static constexpr size_t min_keys = (2 * max_keys + 2) / 3;

    inline bool cmp(const tkey& a, const tkey& b) const noexcept {
		return compare::operator()(a, b);
	}
    inline bool eq(const tkey& a, const tkey& b) const noexcept {
		return !cmp(a, b) && !cmp(b, a);
	}

    struct node
    {
        bool leaf;
        size_t key_count;
        tkey keys[max_keys + 1];
        node* children[max_keys + 2];
        node* next;
        node(bool leaf) : leaf(leaf), key_count(0), next(nullptr) {}
        virtual ~node() = default;
    };

    struct leaf_node : node
    {
        tree_data_type values[max_keys + 1];
        leaf_node() : node(true) {}
    };

    struct internal_node : node
    {
        internal_node() : node(false) {}
    };

    using byte_alloc = typename std::allocator_traits<allocator_type>::template rebind_alloc<char>;
    allocator_type _alloc;
    node* _root;
    leaf_node* _leaf_head;
    size_t _size;

    byte_alloc get_byte_alloc() const noexcept {
		return byte_alloc(_alloc);
	}

    node* create_node(bool leaf)
    {
        byte_alloc ba = get_byte_alloc();
        size_t sz = leaf ? sizeof(leaf_node) : sizeof(internal_node);
        char* raw = ba.allocate(sz);
        if (leaf) {
			return new (raw) leaf_node();
		}
        else return new (raw) internal_node();
    }

    void destroy_node(node* n)
    {
        if (!n) return;
        byte_alloc ba = get_byte_alloc();
        size_t sz = n->leaf ? sizeof(leaf_node) : sizeof(internal_node);
        n->~node();
        ba.deallocate(reinterpret_cast<char*>(n), sz);
    }

    void clear(node* n)
    {
        if (!n) return;
        if (!n->leaf) {
            for (size_t i = 0; i <= n->key_count; ++i)
                clear(n->children[i]);
        }
        destroy_node(n);
    }

    template <typename Key, typename Cmp>
    static size_t lower_bound_binary(const Key* keys, size_t count, const Key& key, Cmp cmp)
    {
        return std::lower_bound(keys, keys + count, key, cmp) - keys;
    }

    size_t lower_pos_leaf(const leaf_node* leaf, const tkey& k) const noexcept
    {
        return lower_bound_binary(leaf->keys, leaf->key_count, k,
            [this](const tkey& a, const tkey& b) { return cmp(a, b); });
    }

    size_t lower_pos_internal(const internal_node* node, const tkey& k) const noexcept
    {
        return lower_bound_binary(node->keys, node->key_count, k,
            [this](const tkey& a, const tkey& b) { return cmp(a, b); });
    }

    size_t upper_pos_internal(const internal_node* node, const tkey& k) const noexcept
    {
        size_t i = lower_pos_internal(node, k);
        while (i < node->key_count && eq(node->keys[i], k)) ++i;
        return i;
    }

    void insert_into_leaf(leaf_node* leaf, const tkey& key, tree_data_type&& data, size_t pos)
    {
        for (size_t i = leaf->key_count; i > pos; --i) {
            leaf->values[i] = std::move(leaf->values[i - 1]);
            leaf->keys[i] = leaf->keys[i - 1];
        }
        leaf->values[pos] = std::move(data);
        leaf->keys[pos] = key;
        ++leaf->key_count;
    }

    void insert_into_internal(internal_node* parent, const tkey& key, node* right, size_t pos)
    {
        for (size_t i = parent->key_count; i > pos; --i) {
            parent->keys[i] = parent->keys[i - 1];
            parent->children[i + 1] = parent->children[i];
        }
        parent->keys[pos] = key;
        parent->children[pos + 1] = right;
        ++parent->key_count;
    }

    bool remove_key(const tkey& key)
    {
        if (!_root) return false;

        struct frame { internal_node* node; size_t child_idx; };
        std::stack<frame> path;
        node* cur = _root;

        while (!cur->leaf) {
            internal_node* in = static_cast<internal_node*>(cur);
            size_t pos = upper_pos_internal(in, key);
            path.push({in, pos});
            cur = in->children[pos];
        }

        leaf_node* leaf = static_cast<leaf_node*>(cur);
        size_t leaf_pos = lower_pos_leaf(leaf, key);
        if (leaf_pos >= leaf->key_count || !eq(leaf->keys[leaf_pos], key))
            return false;

        // Удаляем из листа
        for (size_t i = leaf_pos; i < leaf->key_count - 1; ++i) {
            leaf->values[i] = std::move(leaf->values[i + 1]);
            leaf->keys[i] = leaf->keys[i + 1];
        }
        --leaf->key_count;
        --_size;
		if (leaf_pos == 0 && !path.empty() && leaf->key_count > 0) {
            frame f = path.top();
            internal_node* parent = f.node;
            size_t idx = f.child_idx;
            // keys[idx-1] ограничивает текущий лист слева
            if (idx > 0) parent->keys[idx - 1] = leaf->keys[0];
        }

        // Блок преждевременного обновления разделителя удалён!
        // (все обновления делаются в rebalance_leaf)

        while (!path.empty()) {
            frame f = path.top(); path.pop();
            internal_node* parent = f.node;
            size_t idx = f.child_idx;
            node* child = parent->children[idx];

            if (child->leaf) {
                leaf_node* leaf_child = static_cast<leaf_node*>(child);
                if (leaf_child->key_count < min_keys && child != _root)
                    rebalance_leaf(parent, idx);
            } else {
                internal_node* int_child = static_cast<internal_node*>(child);
                if (int_child->key_count < min_keys && child != _root)
                    rebalance_internal(parent, idx);
            }

            if (parent->key_count < min_keys && parent != _root)
                continue;
            else
                break;
        }

        if (_root->leaf && static_cast<leaf_node*>(_root)->key_count == 0) {
            destroy_node(_root);
            _root = nullptr;
            _leaf_head = nullptr;
        } else if (!_root->leaf && _root->key_count == 0) {
            node* new_root = _root->children[0];
            destroy_node(_root);
            _root = new_root;
        }
        return true;
    }

    void rebalance_leaf(internal_node* parent, size_t idx)
    {
        leaf_node* child = static_cast<leaf_node*>(parent->children[idx]);
        // Заимствование слева
        if (idx > 0) {
            leaf_node* left = static_cast<leaf_node*>(parent->children[idx - 1]);
            if (left->key_count > min_keys) {
                for (size_t i = child->key_count; i > 0; --i) {
                    child->values[i] = std::move(child->values[i - 1]);
                    child->keys[i] = child->keys[i - 1];
                }
                child->values[0] = std::move(left->values[left->key_count - 1]);
                child->keys[0] = child->values[0].first;
                ++child->key_count;
                --left->key_count;
                parent->keys[idx - 1] = child->keys[0];
                return;
            }
        }
        // Заимствование справа
        if (idx + 1 <= parent->key_count) {
            leaf_node* right = static_cast<leaf_node*>(parent->children[idx + 1]);
            if (right->key_count > min_keys) {
                child->values[child->key_count] = std::move(right->values[0]);
                child->keys[child->key_count] = child->values[child->key_count].first;
                ++child->key_count;
                for (size_t i = 0; i < right->key_count - 1; ++i) {
                    right->values[i] = std::move(right->values[i + 1]);
                    right->keys[i] = right->keys[i + 1];
                }
                --right->key_count;
                parent->keys[idx] = right->keys[0];
                return;
            }
        }

        // Слияние с левым соседом
        if (idx > 0) {
            leaf_node* left = static_cast<leaf_node*>(parent->children[idx - 1]);
            size_t total_keys = left->key_count + child->key_count;
            if (total_keys <= max_keys) {
                for (size_t i = 0; i < child->key_count; ++i) {
                    left->values[left->key_count + i] = std::move(child->values[i]);
                    left->keys[left->key_count + i] = child->keys[i];
                }
                left->key_count += child->key_count;
                left->next = child->next;
                if (_leaf_head == child) _leaf_head = left;
                destroy_node(child);
                for (size_t i = idx - 1; i < parent->key_count - 1; ++i) {
                    parent->keys[i] = parent->keys[i + 1];
                    parent->children[i + 1] = parent->children[i + 2];
                }
                --parent->key_count;
            } else {
                // Равномерное перераспределение с левым соседом
                leaf_node* orig_child_next = static_cast<leaf_node*>(child->next);
                std::vector<tree_data_type> temp;
                temp.reserve(total_keys);
                for (size_t i = 0; i < left->key_count; ++i) temp.emplace_back(std::move(left->values[i]));
                for (size_t i = 0; i < child->key_count; ++i) temp.emplace_back(std::move(child->values[i]));
                size_t mid = temp.size() / 2;
                left->key_count = 0;
                for (size_t i = 0; i < mid; ++i) {
                    left->values[i] = std::move(temp[i]);
                    left->keys[i] = left->values[i].first;
                    ++left->key_count;
                }
                child->key_count = 0;
                for (size_t i = mid; i < temp.size(); ++i) {
                    child->values[i - mid] = std::move(temp[i]);
                    child->keys[i - mid] = child->values[i - mid].first;
                    ++child->key_count;
                }
                child->next = orig_child_next;
                left->next = child;
                parent->keys[idx - 1] = child->keys[0];
            }
        }
        // Слияние с правым соседом
        else if (idx + 1 <= parent->key_count) {
            leaf_node* right = static_cast<leaf_node*>(parent->children[idx + 1]);
            size_t total_keys = child->key_count + right->key_count;
            if (total_keys <= max_keys) {
                for (size_t i = 0; i < right->key_count; ++i) {
                    child->values[child->key_count + i] = std::move(right->values[i]);
                    child->keys[child->key_count + i] = right->keys[i];
                }
                child->key_count += right->key_count;
                child->next = right->next;
                destroy_node(right);
                for (size_t i = idx; i < parent->key_count - 1; ++i) {
                    parent->keys[i] = parent->keys[i + 1];
                    parent->children[i + 1] = parent->children[i + 2];
                }
                --parent->key_count;
            } else {
                // Равномерное перераспределение с правым соседом
                leaf_node* orig_right_next = static_cast<leaf_node*>(right->next);
                std::vector<tree_data_type> temp;
                temp.reserve(total_keys);
                for (size_t i = 0; i < child->key_count; ++i)
                    temp.emplace_back(std::move(child->values[i]));
                for (size_t i = 0; i < right->key_count; ++i)
                    temp.emplace_back(std::move(right->values[i]));
                size_t mid = temp.size() / 2;
                child->key_count = 0;
                for (size_t i = 0; i < mid; ++i) {
                    child->values[i] = std::move(temp[i]);
                    child->keys[i] = child->values[i].first;
                    ++child->key_count;
                }
                right->key_count = 0;
                for (size_t i = mid; i < temp.size(); ++i) {
                    right->values[i - mid] = std::move(temp[i]);
                    right->keys[i - mid] = right->values[i - mid].first;
                    ++right->key_count;
                }
                right->next = orig_right_next;
                child->next = right;
                parent->keys[idx] = right->keys[0];
            }
        }
    }

    void rebalance_internal(internal_node* parent, size_t idx)
    {
        internal_node* child = static_cast<internal_node*>(parent->children[idx]);
        // Заимствование слева
        if (idx > 0) {
            internal_node* left = static_cast<internal_node*>(parent->children[idx - 1]);
            if (left->key_count > min_keys) {
                for (size_t i = child->key_count; i > 0; --i) {
                    child->keys[i] = child->keys[i - 1];
                    child->children[i + 1] = child->children[i];
                }
                child->children[1] = child->children[0];
                child->keys[0] = parent->keys[idx - 1];
                child->children[0] = left->children[left->key_count];
                left->children[left->key_count] = nullptr;
                --left->key_count;
                ++child->key_count;
                parent->keys[idx - 1] = left->keys[left->key_count];
                return;
            }
        }
        // Заимствование справа
        if (idx + 1 <= parent->key_count) {
            internal_node* right = static_cast<internal_node*>(parent->children[idx + 1]);
            if (right->key_count > min_keys) {
                child->keys[child->key_count] = parent->keys[idx];
                child->children[child->key_count + 1] = right->children[0];
                ++child->key_count;
                parent->keys[idx] = right->keys[0];
                for (size_t i = 0; i < right->key_count - 1; ++i) {
                    right->keys[i] = right->keys[i + 1];
                    right->children[i] = right->children[i + 1];
                }
                right->children[right->key_count - 1] = right->children[right->key_count];
                right->children[right->key_count] = nullptr;
                --right->key_count;
                return;
            }
        }

        // Слияние с левым соседом
        if (idx > 0) {
            internal_node* left = static_cast<internal_node*>(parent->children[idx - 1]);
            size_t total_keys = left->key_count + 1 + child->key_count;
            if (total_keys <= max_keys) {
                left->keys[left->key_count] = parent->keys[idx - 1];
                left->children[left->key_count + 1] = child->children[0];
                ++left->key_count;
                for (size_t i = 0; i < child->key_count; ++i) {
                    left->keys[left->key_count + i] = child->keys[i];
                    left->children[left->key_count + i + 1] = child->children[i + 1];
                }
                left->key_count += child->key_count;
                destroy_node(child);
                for (size_t i = idx - 1; i < parent->key_count - 1; ++i) {
                    parent->keys[i] = parent->keys[i + 1];
                    parent->children[i + 1] = parent->children[i + 2];
                }
                --parent->key_count;
            } else {
                std::vector<tkey> keys_temp;
                std::vector<node*> children_temp;
                keys_temp.reserve(total_keys);
                children_temp.reserve(total_keys + 1);
                for (size_t i = 0; i < left->key_count; ++i) {
                    keys_temp.push_back(left->keys[i]);
                    children_temp.push_back(left->children[i]);
                }
                children_temp.push_back(left->children[left->key_count]);
                keys_temp.push_back(parent->keys[idx - 1]);
                children_temp.push_back(child->children[0]);
                for (size_t i = 0; i < child->key_count; ++i) {
                    keys_temp.push_back(child->keys[i]);
                    children_temp.push_back(child->children[i + 1]);
                }
                size_t mid = total_keys / 2;
                left->key_count = 0;
                for (size_t i = 0; i < mid; ++i) {
                    left->keys[i] = keys_temp[i];
                    left->children[i] = children_temp[i];
                    ++left->key_count;
                }
                left->children[mid] = children_temp[mid];
                child->key_count = 0;
                for (size_t i = mid + 1; i < total_keys; ++i) {
                    child->keys[i - (mid + 1)] = keys_temp[i];
                    child->children[i - (mid + 1)] = children_temp[i];
                    ++child->key_count;
                }
                child->children[child->key_count] = children_temp[total_keys];
                parent->keys[idx - 1] = keys_temp[mid];
            }
        }
        // Слияние с правым соседом
        else if (idx + 1 <= parent->key_count) {
            internal_node* right = static_cast<internal_node*>(parent->children[idx + 1]);
            size_t total_keys = child->key_count + 1 + right->key_count;
            if (total_keys <= max_keys) {
                child->keys[child->key_count] = parent->keys[idx];
                child->children[child->key_count + 1] = right->children[0];
                ++child->key_count;
                for (size_t i = 0; i < right->key_count; ++i) {
                    child->keys[child->key_count + i] = right->keys[i];
                    child->children[child->key_count + i + 1] = right->children[i + 1];
                }
                child->key_count += right->key_count;
                destroy_node(right);
                for (size_t i = idx; i < parent->key_count - 1; ++i) {
                    parent->keys[i] = parent->keys[i + 1];
                    parent->children[i + 1] = parent->children[i + 2];
                }
                --parent->key_count;
            } else {
                std::vector<tkey> keys_temp;
                std::vector<node*> children_temp;
                keys_temp.reserve(total_keys);
                children_temp.reserve(total_keys + 1);
                for (size_t i = 0; i < child->key_count; ++i) {
                    keys_temp.push_back(child->keys[i]);
                    children_temp.push_back(child->children[i]);
                }
                children_temp.push_back(child->children[child->key_count]);
                keys_temp.push_back(parent->keys[idx]);
                children_temp.push_back(right->children[0]);
                for (size_t i = 0; i < right->key_count; ++i) {
                    keys_temp.push_back(right->keys[i]);
                    children_temp.push_back(right->children[i + 1]);
                }
                size_t mid = total_keys / 2;
                child->key_count = 0;
                for (size_t i = 0; i < mid; ++i) {
                    child->keys[i] = keys_temp[i];
                    child->children[i] = children_temp[i];
                    ++child->key_count;
                }
                child->children[mid] = children_temp[mid];
                right->key_count = 0;
                for (size_t i = mid + 1; i < total_keys; ++i) {
                    right->keys[i - (mid + 1)] = keys_temp[i];
                    right->children[i - (mid + 1)] = children_temp[i];
                    ++right->key_count;
                }
                right->children[right->key_count] = children_temp[total_keys];
                parent->keys[idx] = keys_temp[mid];
            }
        }
    }

    // ----- Операции с листьями -----
    void redistribute_leaf(leaf_node* full_leaf, leaf_node* neighbor, bool left,
                           internal_node* parent, size_t idx)
    {
        size_t can_move = max_keys - neighbor->key_count;
        size_t need_move = full_leaf->key_count - min_keys;
        size_t move = std::min(can_move, need_move);
        if (move == 0) return;
        if (left) {
            for (size_t i = 0; i < move; ++i) {
                neighbor->values[neighbor->key_count + i] = std::move(full_leaf->values[i]);
                neighbor->keys[neighbor->key_count + i] = full_leaf->keys[i];
            }
            neighbor->key_count += move;
            for (size_t i = 0; i < full_leaf->key_count - move; ++i) {
                full_leaf->values[i] = std::move(full_leaf->values[i + move]);
                full_leaf->keys[i] = full_leaf->keys[i + move];
            }
            full_leaf->key_count -= move;
            parent->keys[idx - 1] = full_leaf->keys[0];
        } else {
            for (size_t i = neighbor->key_count; i > 0; --i) {
                neighbor->values[move + i - 1] = std::move(neighbor->values[i - 1]);
                neighbor->keys[move + i - 1]   = neighbor->keys[i - 1];
            }
            for (size_t i = 0; i < move; ++i) {
                neighbor->values[i] = std::move(full_leaf->values[full_leaf->key_count - move + i]);
                neighbor->keys[i]   = full_leaf->keys[full_leaf->key_count - move + i];
            }
            neighbor->key_count += move;
            full_leaf->key_count -= move;
            parent->keys[idx] = neighbor->keys[0];
        }
    }

    void split_root_leaf(leaf_node* leaf, const tkey& key, tree_data_type&& data, size_t insert_pos)
    {
        leaf_node* new_leaf = static_cast<leaf_node*>(create_node(true));
        for (size_t i = leaf->key_count; i > insert_pos; --i) {
            leaf->values[i] = std::move(leaf->values[i - 1]);
            leaf->keys[i] = leaf->keys[i - 1];
        }
        leaf->values[insert_pos] = std::move(data);
        leaf->keys[insert_pos] = key;
        ++leaf->key_count;

        size_t mid = leaf->key_count / 2;
        for (size_t i = mid; i < leaf->key_count; ++i) {
            new_leaf->values[i - mid] = std::move(leaf->values[i]);
            new_leaf->keys[i - mid] = leaf->keys[i];
        }
        new_leaf->key_count = leaf->key_count - mid;
        leaf->key_count = mid;
        new_leaf->next = leaf->next;
        leaf->next = new_leaf;

        tkey promote = new_leaf->keys[0];
        internal_node* new_root = static_cast<internal_node*>(create_node(false));
        new_root->key_count = 1;
        new_root->keys[0] = promote;
        new_root->children[0] = leaf;
        new_root->children[1] = new_leaf;
        _root = new_root;
        if (leaf == _leaf_head) _leaf_head = leaf;
        ++_size;
    }

    void split_2_3_leaf(leaf_node* leaf, leaf_node* neighbor, bool left,
                        internal_node* parent, size_t idx,
                        const tkey& insert_key, tree_data_type&& insert_data,
                        leaf_node*& new_leaf_out, tkey& promote_key)
    {
        boost::container::static_vector<tree_data_type, 3 * max_keys + 3> all;
        auto push_leaf = [&](leaf_node* ln) {
            for (size_t i = 0; i < ln->key_count; ++i)
                all.push_back(tree_data_type(ln->keys[i], std::move(ln->values[i].second)));
        };
        if (left) { push_leaf(neighbor); push_leaf(leaf); }
        else { push_leaf(leaf); push_leaf(neighbor); }

        auto it = std::lower_bound(all.begin(), all.end(), tree_data_type(insert_key, tvalue()),
            [this](const tree_data_type& a, const tree_data_type& b) { return cmp(a.first, b.first); });
        all.insert(it, std::move(insert_data));

        size_t total = all.size();
        // Гарантированные размеры, чтобы каждый узел имел минимум min_keys
        size_t sz1 = min_keys;
        size_t sz2 = total - 2 * min_keys;
        size_t sz3 = min_keys;

        leaf_node* new_leaf = static_cast<leaf_node*>(create_node(true));
        new_leaf->key_count = 0;
        if (left) { neighbor->key_count = 0; leaf->key_count = 0; }
        else { leaf->key_count = 0; neighbor->key_count = 0; }

        size_t pos = 0;
        auto fill_leaf = [&](leaf_node* ln, size_t count) {
            for (size_t i = 0; i < count; ++i) {
                ln->values[ln->key_count + i] = std::move(all[pos]);
                ln->keys[ln->key_count + i] = ln->values[ln->key_count + i].first;
                ++pos;
            }
            ln->key_count += count;
        };

        if (left) { fill_leaf(neighbor, sz1); fill_leaf(leaf, sz2); }
        else { fill_leaf(leaf, sz1); fill_leaf(neighbor, sz2); }
        fill_leaf(new_leaf, sz3);

        leaf_node* first, *second, *third;
        if (left) { first = neighbor; second = leaf; third = new_leaf; }
        else { first = leaf; second = neighbor; third = new_leaf; }

        leaf_node* next_after = static_cast<leaf_node*>((left ? leaf : neighbor)->next);
        first->next = second;
        second->next = third;
        third->next = next_after;

        if (parent) {
            if (left) parent->keys[idx - 1] = second->keys[0];
            else parent->keys[idx] = second->keys[0];
        }
        promote_key = third->keys[0];
        new_leaf_out = new_leaf;
    }

    // ----- Исправленные операции с внутренними узлами -----
    void redistribute_internal(internal_node* full, internal_node* neighbor, bool left,
                               internal_node* parent, size_t idx)
    {
        if (left) {
            neighbor->keys[neighbor->key_count] = parent->keys[idx - 1];
            neighbor->children[neighbor->key_count + 1] = full->children[0];
            ++neighbor->key_count;
            parent->keys[idx - 1] = full->keys[0];
            for (size_t i = 0; i < full->key_count - 1; ++i) {
                full->keys[i] = full->keys[i + 1];
                full->children[i + 1] = full->children[i + 2];
            }
            --full->key_count;
        } else {
            for (size_t i = neighbor->key_count; i > 0; --i) {
                neighbor->keys[i] = neighbor->keys[i - 1];
                neighbor->children[i + 1] = neighbor->children[i];
            }
            neighbor->children[1] = neighbor->children[0];
            neighbor->keys[0] = parent->keys[idx];
            neighbor->children[0] = full->children[full->key_count];
            ++neighbor->key_count;
            parent->keys[idx] = full->keys[full->key_count - 1];
            --full->key_count;
        }
    }

    void split_2_3_internal(internal_node* node, internal_node* neighbor, bool left,
                            internal_node* parent, size_t idx,
                            internal_node*& new_node_out, tkey& promote_key)
    {
        if (!neighbor) {
            size_t mid = node->key_count / 2;
            internal_node* new_node = static_cast<internal_node*>(create_node(false));
            new_node->key_count = node->key_count - mid - 1;
            for (size_t i = 0; i < new_node->key_count; ++i) {
                new_node->keys[i] = node->keys[mid + 1 + i];
                new_node->children[i] = node->children[mid + 1 + i];
            }
            new_node->children[new_node->key_count] = node->children[node->key_count];
            promote_key = node->keys[mid];
            node->key_count = mid;
            new_node_out = new_node;
            return;
        }

        internal_node* first_node  = left ? neighbor : node;
        internal_node* second_node = left ? node     : neighbor;
        tkey separator = left ? parent->keys[idx - 1] : parent->keys[idx];

        std::vector<tkey>             km;
        std::vector<BP_tree::node*>   cm;
        km.reserve(2 * max_keys + 2);
        cm.reserve(2 * max_keys + 3);

        for (size_t i = 0; i < first_node->key_count; ++i) {
            km.push_back(first_node->keys[i]);
            cm.push_back(first_node->children[i]);
        }
        cm.push_back(first_node->children[first_node->key_count]);
        km.push_back(separator);
        for (size_t i = 0; i < second_node->key_count; ++i) {
            km.push_back(second_node->keys[i]);
            cm.push_back(second_node->children[i]);
        }
        cm.push_back(second_node->children[second_node->key_count]);

        size_t total_keys = km.size();
        // Гарантированные размеры, чтобы каждый узел имел минимум min_keys
        size_t sz1 = min_keys;
        size_t sz2 = total_keys - 2 * min_keys;
        size_t sz3 = min_keys;

        tkey promote1 = km[sz1];
        tkey promote2 = km[sz1 + 1 + sz2];

        internal_node* third = static_cast<internal_node*>(create_node(false));

        first_node->key_count = 0;
        for (size_t i = 0; i < sz1; ++i) {
            first_node->keys[i]     = km[i];
            first_node->children[i] = cm[i];
        }
        first_node->children[sz1] = cm[sz1];
        first_node->key_count = sz1;

        size_t s2 = sz1 + 1;
        second_node->key_count = 0;
        for (size_t i = 0; i < sz2; ++i) {
            second_node->keys[i]     = km[s2 + i];
            second_node->children[i] = cm[s2 + i];
        }
        second_node->children[sz2] = cm[s2 + sz2];
        second_node->key_count = sz2;

        size_t s3 = s2 + sz2 + 1;
        third->key_count = 0;
        for (size_t i = 0; i < sz3; ++i) {
            third->keys[i]     = km[s3 + i];
            third->children[i] = cm[s3 + i];
        }
        third->children[sz3] = cm[s3 + sz3];
        third->key_count = sz3;

        if (parent) {
            if (left) parent->keys[idx - 1] = promote1;
            else      parent->keys[idx]     = promote1;
        }

        promote_key  = promote2;
        new_node_out = third;
    }

public:
    // ----- Итераторы -----
    class bptree_iterator
    {
        leaf_node* _leaf;
        size_t _idx;
    public:
        using value_type = tree_data_type_const;
        using reference = const value_type&;
        using pointer = const value_type*;
        using iterator_category = std::forward_iterator_tag;
        using difference_type = ptrdiff_t;

        friend class bptree_const_iterator;
        friend class BP_tree;

        bptree_iterator(leaf_node* leaf = nullptr, size_t idx = 0) : _leaf(leaf), _idx(idx) {}
        reference operator*() const noexcept {
            return *reinterpret_cast<const value_type*>(&_leaf->values[_idx]);
        }
        pointer operator->() const noexcept { return &**this; }
        bptree_iterator& operator++() noexcept {
            if (_idx + 1 < _leaf->key_count) ++_idx;
            else { _leaf = static_cast<leaf_node*>(_leaf->next); _idx = 0; }
            return *this;
        }
        bptree_iterator operator++(int) noexcept { auto tmp = *this; ++*this; return tmp; }
        bool operator==(const bptree_iterator& o) const noexcept { return _leaf == o._leaf && _idx == o._idx; }
        bool operator!=(const bptree_iterator& o) const noexcept { return !(*this == o); }
        size_t current_node_keys_count() const noexcept { return _leaf ? _leaf->key_count : 0; }
        size_t index() const noexcept { return _idx; }
    };

    class bptree_const_iterator
    {
        const leaf_node* _leaf;
        size_t _idx;
    public:
        using value_type = tree_data_type_const;
        using reference = const value_type&;
        using pointer = const value_type*;
        using iterator_category = std::forward_iterator_tag;
        using difference_type = ptrdiff_t;

        bptree_const_iterator(const bptree_iterator& it) : _leaf(it._leaf), _idx(it._idx) {}
        bptree_const_iterator(const leaf_node* leaf = nullptr, size_t idx = 0) : _leaf(leaf), _idx(idx) {}
        reference operator*() const noexcept {
            return *reinterpret_cast<const value_type*>(&_leaf->values[_idx]);
        }
        pointer operator->() const noexcept { return &**this; }
        bptree_const_iterator& operator++() noexcept {
            if (_idx + 1 < _leaf->key_count) ++_idx;
            else { _leaf = static_cast<const leaf_node*>(_leaf->next); _idx = 0; }
            return *this;
        }
        bptree_const_iterator operator++(int) noexcept { auto tmp = *this; ++*this; return tmp; }
        bool operator==(const bptree_const_iterator& o) const noexcept { return _leaf == o._leaf && _idx == o._idx; }
        bool operator!=(const bptree_const_iterator& o) const noexcept { return !(*this == o); }
        size_t current_node_keys_count() const noexcept { return _leaf ? _leaf->key_count : 0; }
        size_t index() const noexcept { return _idx; }
    };

private:
    std::pair<typename BP_tree::bptree_iterator, bool>
    insert_impl(const tkey& key, tree_data_type&& data)
    {
        if (!_root) {
            _root = _leaf_head = static_cast<leaf_node*>(create_node(true));
            insert_into_leaf(_leaf_head, key, std::move(data), 0);
            ++_size;
            return {bptree_iterator(_leaf_head, 0), true};
        }

        struct frame { internal_node* node; size_t child_idx; };
        std::stack<frame> path;
        node* cur = _root;
        while (!cur->leaf) {
            internal_node* in = static_cast<internal_node*>(cur);
            size_t pos = upper_pos_internal(in, key);
            path.push({in, pos});
            cur = in->children[pos];
        }
        leaf_node* leaf = static_cast<leaf_node*>(cur);
        size_t insert_pos = lower_pos_leaf(leaf, key);
        if (insert_pos < leaf->key_count && eq(leaf->keys[insert_pos], key))
            return {bptree_iterator(leaf, insert_pos), false};

        tkey promote;
        node* promoted_node = nullptr;
        bool need_promote = false;
        bool leaf_left_neighbor = false;
        size_t leaf_parent_idx = 0;

        if (leaf->key_count < max_keys) {
            insert_into_leaf(leaf, key, std::move(data), insert_pos);
            ++_size;
        } else {
            internal_node* parent = nullptr;
            leaf_node* neighbor = nullptr;
            if (!path.empty()) {
                frame f = path.top();
                parent = f.node;
                leaf_parent_idx = f.child_idx;
                if (leaf_parent_idx > 0) {
                    neighbor = static_cast<leaf_node*>(parent->children[leaf_parent_idx - 1]);
                    leaf_left_neighbor = true;
                } else if (leaf_parent_idx + 1 <= parent->key_count) {
                    neighbor = static_cast<leaf_node*>(parent->children[leaf_parent_idx + 1]);
                    leaf_left_neighbor = false;
                }
            }

            if (neighbor && neighbor->key_count < max_keys) {
                redistribute_leaf(leaf, neighbor, leaf_left_neighbor, parent, leaf_parent_idx);
                leaf_node* target = leaf;
                if (leaf_left_neighbor) {
                    if (leaf->key_count > 0 && cmp(key, leaf->keys[0]))
                        target = neighbor;
                } else {
                    if (!cmp(key, parent->keys[leaf_parent_idx]))
                        target = neighbor;
                }
                insert_into_leaf(target, key, std::move(data), lower_pos_leaf(target, key));
                ++_size;
            } else {
                if (!neighbor) {
                    split_root_leaf(leaf, key, std::move(data), insert_pos);
                    return {find(key), true};
                }
                leaf_node* new_leaf = nullptr;
                split_2_3_leaf(leaf, neighbor, leaf_left_neighbor, parent, leaf_parent_idx,
                               key, std::move(data), new_leaf, promote);
                promoted_node = new_leaf;
                need_promote = true;
                ++_size;
            }
        }

        if (!need_promote) return {find(key), true};

        frame f = path.top(); path.pop();
        internal_node* current_parent = f.node;
        size_t ins_pos = leaf_left_neighbor ? f.child_idx : f.child_idx + 1;
        insert_into_internal(current_parent, promote, promoted_node, ins_pos);

        while (current_parent->key_count > max_keys) {
            if (path.empty()) {
                internal_node* new_internal = nullptr;
                tkey new_promote;
                split_2_3_internal(current_parent, nullptr, false, nullptr, 0, new_internal, new_promote);
                internal_node* new_root = static_cast<internal_node*>(create_node(false));
                new_root->key_count = 1;
                new_root->keys[0] = new_promote;
                new_root->children[0] = current_parent;
                new_root->children[1] = new_internal;
                _root = new_root;
                return {find(key), true};
            }

            frame grand_frame = path.top(); path.pop();
            internal_node* grandparent = grand_frame.node;
            size_t parent_idx = grand_frame.child_idx;
            internal_node* neighbor = nullptr;
            bool left_nb = false;
            if (parent_idx > 0) {
                neighbor = static_cast<internal_node*>(grandparent->children[parent_idx - 1]);
                left_nb = true;
            } else if (parent_idx + 1 <= grandparent->key_count) {
                neighbor = static_cast<internal_node*>(grandparent->children[parent_idx + 1]);
                left_nb = false;
            }

            if (neighbor && neighbor->key_count < max_keys) {
                redistribute_internal(current_parent, neighbor, left_nb, grandparent, parent_idx);
                return {find(key), true};
            }

            internal_node* new_internal = nullptr;
            tkey new_promote;
            split_2_3_internal(current_parent, neighbor, left_nb, grandparent,
                               parent_idx, new_internal, new_promote);
            promote = new_promote;
            promoted_node = new_internal;
            size_t ins = left_nb ? parent_idx : parent_idx + 1;
            insert_into_internal(grandparent, promote, promoted_node, ins);
            current_parent = grandparent;
        }

        return {find(key), true};
    }

public:
    BP_tree(const compare& cmp = compare(), const allocator_type& alloc = allocator_type())
        : compare(cmp), _alloc(alloc), _root(nullptr), _leaf_head(nullptr), _size(0) {}

    explicit BP_tree(const allocator_type& alloc, const compare& comp = compare())
        : BP_tree(comp, alloc) {}

    BP_tree(const compare& cmp, std::nullptr_t) : BP_tree(cmp) {}

    template<input_iterator_for_pair<tkey, tvalue> It>
    BP_tree(It begin, It end, const compare& cmp = compare(), const allocator_type& alloc = allocator_type())
        : BP_tree(cmp, alloc)
    {
        for (auto it = begin; it != end; ++it) insert(*it);
    }

    BP_tree(std::initializer_list<std::pair<tkey, tvalue>> data, const compare& cmp = compare(), const allocator_type& alloc = allocator_type())
        : BP_tree(cmp, alloc)
    {
        for (const auto& item : data) insert(item);
    }

    BP_tree(const BP_tree& other)
        : compare(other), _alloc(other._alloc), _root(nullptr), _leaf_head(nullptr), _size(0)
    {
        if (!other._root) return;
        std::unordered_map<node*, node*> copy_map;
        std::function<node*(node*)> copy = [&](node* src) -> node* {
            if (!src) return nullptr;
            if (copy_map.count(src)) return copy_map[src];
            node* dst = create_node(src->leaf);
            dst->key_count = src->key_count;
            for (size_t i = 0; i < src->key_count; ++i) {
                dst->keys[i] = src->keys[i];
                if (src->leaf)
                    static_cast<leaf_node*>(dst)->values[i] = static_cast<leaf_node*>(src)->values[i];
            }
            if (!src->leaf) {
                for (size_t i = 0; i <= src->key_count; ++i)
                    dst->children[i] = copy(src->children[i]);
            }
            copy_map[src] = dst;
            return dst;
        };
        _root = copy(other._root);
        leaf_node* prev = nullptr;
        node* cur = _root;
        while (cur && !cur->leaf) cur = cur->children[0];
        while (cur) {
            leaf_node* leaf = static_cast<leaf_node*>(cur);
            if (!_leaf_head) _leaf_head = leaf;
            if (prev) prev->next = leaf;
            prev = leaf;
            cur = leaf->next;
        }
        _size = other._size;
    }

    BP_tree(BP_tree&& other) noexcept
        : compare(std::move(other)), _alloc(std::move(other._alloc)),
          _root(other._root), _leaf_head(other._leaf_head), _size(other._size)
    {
        other._root = nullptr;
        other._leaf_head = nullptr;
        other._size = 0;
    }

    BP_tree& operator=(const BP_tree& other)
    {
        if (this != &other) {
            BP_tree temp(other);
            swap(temp);
        }
        return *this;
    }

    BP_tree& operator=(BP_tree&& other) noexcept
    {
        if (this != &other) {
            clear();
            compare::operator=(std::move(other));
            _alloc = std::move(other._alloc);
            _root = other._root;
            _leaf_head = other._leaf_head;
            _size = other._size;
            other._root = nullptr;
            other._leaf_head = nullptr;
            other._size = 0;
        }
        return *this;
    }

    ~BP_tree() noexcept {
		clear();
	}

    friend void swap(BP_tree& a, BP_tree& b) noexcept
    {
        using std::swap;
        swap(static_cast<compare&>(a), static_cast<compare&>(b));
        swap(a._alloc, b._alloc);
        swap(a._root, b._root);
        swap(a._leaf_head, b._leaf_head);
        swap(a._size, b._size);
    }

    void swap(BP_tree& other) noexcept {
		using std::swap;
		swap(*this, other);
	}

    allocator_type get_allocator() const noexcept {
		return _alloc;
	}

    bptree_iterator begin() noexcept
    {
        if (!_leaf_head) return end();
        return bptree_iterator(_leaf_head, 0);
    }
    bptree_iterator end() noexcept {
		return bptree_iterator(nullptr, 0);
	}
    bptree_const_iterator begin() const noexcept {
		return bptree_const_iterator(_leaf_head, 0);
	}
    bptree_const_iterator end() const noexcept {
		return bptree_const_iterator(nullptr, 0);
	}
    bptree_const_iterator cbegin() const noexcept {
		return begin();
	}
    bptree_const_iterator cend() const noexcept {
		return end();
	}

    size_t size() const noexcept {
		return _size;
	}
    bool empty() const noexcept {
		return _size == 0;
	}

    tvalue& at(const tkey& key)
    {
        auto it = find(key);
        if (it == end()) {
			throw std::out_of_range("key not found");
        }
		return const_cast<tvalue&>(it->second);
    }
    const tvalue& at(const tkey& key) const
    {
        auto it = find(key);
        if (it == end()) {
			throw std::out_of_range("key not found");
        }
		return it->second;
    }

    tvalue& operator[](const tkey& key)
    {
        auto it = find(key);
        if (it == end()) {
			it = insert({key, tvalue{}}).first;
		}
        return const_cast<tvalue&>(it->second);
    }
    tvalue& operator[](tkey&& key) {
        auto it = find(key);
        if (it == end()) {
			it = insert({std::move(key), tvalue{}}).first;
		}
        return const_cast<tvalue&>(it->second);
    }

    bptree_iterator find(const tkey& key)
    {
        if (!_root) {
			return end();
        }
		node* cur = _root;
        while (!cur->leaf) {
            internal_node* in = static_cast<internal_node*>(cur);
            size_t pos = upper_pos_internal(in, key);
            cur = in->children[pos];
        }
        leaf_node* leaf = static_cast<leaf_node*>(cur);
        size_t pos = lower_pos_leaf(leaf, key);
        if (pos < leaf->key_count && eq(leaf->keys[pos], key)) {
            return bptree_iterator(leaf, pos);
		}
        return end();
    }
    bptree_const_iterator find(const tkey& key) const {
		return const_cast<BP_tree*>(this)->find(key);
	}

    bptree_iterator lower_bound(const tkey& key)
    {
        if (!_root) {
			return end();
        }
		node* cur = _root;
        while (!cur->leaf) {
            internal_node* in = static_cast<internal_node*>(cur);
            size_t pos = lower_pos_internal(in, key);
            cur = in->children[pos];
        }
        leaf_node* leaf = static_cast<leaf_node*>(cur);
        size_t pos = lower_pos_leaf(leaf, key);
        if (pos >= leaf->key_count) {
            leaf_node* nxt = static_cast<leaf_node*>(leaf->next);
            if (!nxt) return end();
            return bptree_iterator(nxt, 0);
        }
        return bptree_iterator(leaf, pos);
    }
    bptree_const_iterator lower_bound(const tkey& key) const {
		return const_cast<BP_tree*>(this)->lower_bound(key);
	}

    bptree_iterator upper_bound(const tkey& key)
    {
        auto it = lower_bound(key);
        if (it != end() && eq(it->first, key)) {
			++it;
        }
		return it;
    }
    bptree_const_iterator upper_bound(const tkey& key) const {
		return const_cast<BP_tree*>(this)->upper_bound(key);
	}

    bool contains(const tkey& key) const {
		return find(key) != end();
	}

    void clear() noexcept
    {
        clear(_root);
        _root = nullptr;
        _leaf_head = nullptr;
        _size = 0;
    }

    std::pair<bptree_iterator, bool> insert(const tree_data_type& data)
    {
        tkey key = data.first;
        tree_data_type copy = data;
        return insert_impl(key, std::move(copy));
    }

    std::pair<bptree_iterator, bool> insert(tree_data_type&& data)
    {
        tkey key = data.first;
        return insert_impl(key, std::move(data));
    }

    template<typename... Args>
    std::pair<bptree_iterator, bool> emplace(Args&&... args)
    {
        return insert(tree_data_type(std::forward<Args>(args)...));
    }

    bptree_iterator insert_or_assign(const tree_data_type& data)
    {
        auto it = find(data.first);
        if (it != end()) {
            const_cast<tvalue&>(it->second) = data.second;
            return it;
        }
        return insert(data).first;
    }

    bptree_iterator insert_or_assign(tree_data_type&& data)
    {
        auto it = find(data.first);
        if (it != end()) {
            const_cast<tvalue&>(it->second) = std::move(data.second);
            return it;
        }
        return insert(std::move(data)).first;
    }

    template<typename... Args>
    bptree_iterator emplace_or_assign(Args&&... args)
    {
        return insert_or_assign(tree_data_type(std::forward<Args>(args)...));
    }

    bptree_iterator erase(bptree_iterator pos)
    {
        if (pos == end()) {
			return end();
        }
		tkey current_key = pos->first;
        bptree_iterator next = pos;
        ++next;
        bool has_next = (next != end());
        tkey next_key = has_next ? next->first : tkey{};
        remove_key(current_key);
        if (has_next) {
            return find(next_key);
        }
        return end();
    }

    bptree_iterator erase(bptree_const_iterator pos)
    {
        return erase(bptree_iterator(const_cast<leaf_node*>(pos._leaf), pos._idx));
    }

    bptree_iterator erase(bptree_iterator beg, bptree_iterator en)
    {
        while (beg != en) beg = erase(beg);
        return beg;
    }

    bptree_iterator erase(bptree_const_iterator beg, bptree_const_iterator en)
    {
        return erase(bptree_iterator(const_cast<leaf_node*>(beg._leaf), beg._idx),
                     bptree_iterator(const_cast<leaf_node*>(en._leaf), en._idx));
    }

    bptree_iterator erase(const tkey& key)
    {
        auto it = find(key);
        if (it == end()) return end();
        auto next = it; ++next;
        bool has_next = (next != end());
        tkey next_key = has_next ? next->first : tkey{};

        remove_key(key);
        if (has_next) return find(next_key);
        return end();
    }
};

template<std::input_iterator It, comparator<typename std::iterator_traits<It>::value_type::first_type> Cmp, std::size_t T = 5>
BP_tree(It, It, const Cmp& = Cmp(), const pp_allocator<std::pair<const typename std::iterator_traits<It>::value_type::first_type,
                                                                        typename std::iterator_traits<It>::value_type::second_type>>& = {})
    -> BP_tree<typename std::iterator_traits<It>::value_type::first_type,
               typename std::iterator_traits<It>::value_type::second_type, Cmp, T>;

template<typename K, typename V, comparator<K> Cmp, std::size_t T = 5>
BP_tree(std::initializer_list<std::pair<K, V>>, const Cmp& = Cmp(), const pp_allocator<std::pair<const K, V>>& = {})
    -> BP_tree<K, V, Cmp, T>;

#endif