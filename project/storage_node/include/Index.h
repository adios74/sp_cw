#include <iterator>
#include <utility>
#include <vector>
#include <boost/container/static_vector.hpp>
#include <concepts>
#include <stack>
#include <initializer_list>
#include "pp_allocator.h"
#include "associative_container.h"

#ifndef SYS_PROG_BS_PLUS_TREE_H
#define SYS_PROG_BS_PLUS_TREE_H

template <typename TKey, typename TValue, typename Compare = std::less<TKey>, std::size_t Order = 5>
class BSP_tree final : private Compare
{

    template<typename Key>
    friend class PageBasedIndex;

public:
    using key_type = TKey;
    using mapped_type = TValue;
    using tree_data_type = std::pair<TKey, TValue>;
    using tree_data_type_const = std::pair<const TKey, TValue>;
    using value_type = tree_data_type_const;
    using reference = value_type&;
    using const_reference = const value_type&;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using allocator_type = pp_allocator<value_type>;

private:
    static constexpr size_type minimum_keys_in_node = 2 * Order - 1;
    static constexpr size_type maximum_keys_in_node = 3 * Order - 1;
    static constexpr size_type maximum_keys_in_root = 4 * Order - 1;

    inline bool compare_keys(const TKey& lhs, const TKey& rhs) const;
    inline bool compare_pairs(const tree_data_type& lhs, const tree_data_type& rhs) const;
    inline bool keys_equal(const TKey& lhs, const TKey& rhs) const;

    struct NodeBase
    {
        bool is_leaf;
        NodeBase() noexcept = default;
        virtual ~NodeBase() = default;
    };

    struct LeafNode : public NodeBase
    {
        LeafNode* next = nullptr;
        boost::container::static_vector<tree_data_type, maximum_keys_in_root + 1> data;
        LeafNode() { this->is_leaf = true; }
    };

    struct InternalNode : public NodeBase
    {
        boost::container::static_vector<TKey, maximum_keys_in_root + 1> keys;
        boost::container::static_vector<NodeBase*, maximum_keys_in_root + 2> children;
        InternalNode() { this->is_leaf = false; }
    };

    allocator_type _allocator;
    NodeBase* _root;
    size_type _size;

    allocator_type get_allocator() const noexcept;
    pp_allocator<InternalNode> get_node_allocator_internal() const noexcept;
    pp_allocator<LeafNode> get_node_allocator_leaf() const noexcept;

    static value_type& as_value(tree_data_type& x) noexcept;
    static const value_type& as_value(const tree_data_type& x) noexcept;
    static bool iterators_are_equal(const LeafNode* node, size_type index, const LeafNode* other_node, size_type other_index);

    template <typename NodePtr>
    static void increment_iterator(NodePtr& node, size_type& index);

    LeafNode* get_leftmost_leaf();
    size_type find_key_index(InternalNode* node, const TKey& key);
    size_type find_key_index(LeafNode* node, const TKey& key);
    size_type upper_bound_key_index(InternalNode* node, const TKey& key);
    size_type upper_bound_key_index(LeafNode* node, const TKey& key);

    InternalNode* make_node_internal();
    LeafNode* make_node_leaf();
    void delete_node_internal(InternalNode* node);
    void delete_node_leaf(LeafNode* node);
    void destroy_tree(NodeBase* node);
    size_type get_keys_size(NodeBase* node);

    bool try_lend_key_to_neighbour(InternalNode* parent, size_type child_index);
    void split_child(InternalNode* parent, size_type child_index);
    void split_root();
    void insert_bottom_up(NodeBase* node, tree_data_type data);

    void borrow_from_left(InternalNode* parent, size_type child_index);
    void borrow_from_right(InternalNode* parent, size_type child_index);

    void merge_3_children_internal(InternalNode* parent, size_type mid_child_index);
    void merge_3_children_leaf(InternalNode* parent, size_type mid_child_index);
    void merge_3_children(InternalNode* parent, size_type mid_child_index);
    void merge_2_children(InternalNode* parent, size_type left_child_index);
    void ensure_child_has_enough_keys(InternalNode* node, size_type& idx);

    bool try_erase_from_node(NodeBase* node, const TKey& key);

public:
    explicit BSP_tree(const Compare& cmp = Compare(), allocator_type alloc = allocator_type());
    explicit BSP_tree(allocator_type alloc, const Compare& comp = Compare());

    template <std::input_iterator Iterator>
        requires std::is_same_v<typename std::iterator_traits<Iterator>::value_type, tree_data_type>
    explicit BSP_tree(Iterator begin, Iterator end, const Compare& cmp = 
		Compare(), allocator_type alloc = allocator_type());

    BSP_tree(std::initializer_list<tree_data_type> data, const Compare& cmp = 
		Compare(), allocator_type alloc = allocator_type());

    BSP_tree(const BSP_tree& other);
    BSP_tree(BSP_tree&& other) noexcept;
    BSP_tree& operator=(const BSP_tree& other);
    BSP_tree& operator=(BSP_tree&& other) noexcept;
    ~BSP_tree() noexcept;

    class bsptree_iterator;
    class bsptree_const_iterator;

    class bsptree_iterator final
    {
        LeafNode* _node;
        size_type _index;
    public:
        using value_type = tree_data_type_const;
        using reference = value_type&;
        using pointer = value_type*;
        using iterator_category = std::forward_iterator_tag;
        using difference_type = ptrdiff_t;
        using self = bsptree_iterator;

        friend class BSP_tree;
        friend class bsptree_const_iterator;

        reference operator*() const noexcept;
        pointer operator->() const noexcept;
        self& operator++();
        self operator++(int);
        bool operator==(const self& other) const noexcept;
        bool operator!=(const self& other) const noexcept;
        size_type current_node_keys_count() const noexcept;
        size_type index() const noexcept;
        explicit bsptree_iterator(LeafNode* node = nullptr, size_type index = 0);
    };

    class bsptree_const_iterator final
    {
        const LeafNode* _node;
        size_type _index;
    public:
        using value_type = tree_data_type_const;
        using reference = const value_type&;
        using pointer = const value_type*;
        using iterator_category = std::forward_iterator_tag;
        using difference_type = ptrdiff_t;
        using self = bsptree_const_iterator;

        friend class BSP_tree;
        friend class bsptree_iterator;

        bsptree_const_iterator(const bsptree_iterator& it) noexcept;
        reference operator*() const noexcept;
        pointer operator->() const noexcept;
        self& operator++();
        self operator++(int);
        bool operator==(const self& other) const noexcept;
        bool operator!=(const self& other) const noexcept;
        size_type current_node_keys_count() const noexcept;
        size_type index() const noexcept;
        explicit bsptree_const_iterator(const LeafNode* node = nullptr, size_type index = 0);
    };

    friend class bsptree_iterator;
    friend class bsptree_const_iterator;

    TValue& at(const TKey&);
    const TValue& at(const TKey&) const;
    TValue& operator[](const TKey& key);
    TValue& operator[](TKey&& key);

    bsptree_iterator begin();
    bsptree_iterator end();
    bsptree_const_iterator begin() const;
    bsptree_const_iterator end() const;
    bsptree_const_iterator cbegin() const;
    bsptree_const_iterator cend() const;

    size_type size() const noexcept;
    bool empty() const noexcept;
    bsptree_iterator find(const TKey& key);
    bsptree_const_iterator find(const TKey& key) const;
    bsptree_iterator lower_bound(const TKey& key);
    bsptree_const_iterator lower_bound(const TKey& key) const;
    bsptree_iterator upper_bound(const TKey& key);
    bsptree_const_iterator upper_bound(const TKey& key) const;
    bool contains(const TKey& key) const;

    void clear() noexcept;
    std::pair<bsptree_iterator, bool> insert(const tree_data_type& data);
    std::pair<bsptree_iterator, bool> insert(tree_data_type&& data);

    template <typename ...Args>
    std::pair<bsptree_iterator, bool> emplace(Args&&... args);

    bsptree_iterator insert_or_assign(const tree_data_type& data);
    bsptree_iterator insert_or_assign(tree_data_type&& data);

    template <typename ...Args>
    bsptree_iterator emplace_or_assign(Args&&... args);

    bsptree_iterator erase(bsptree_iterator pos);
    bsptree_iterator erase(bsptree_const_iterator pos);
    bsptree_iterator erase(bsptree_iterator beg, bsptree_iterator en);
    bsptree_iterator erase(bsptree_const_iterator beg, bsptree_const_iterator en);
    bsptree_iterator erase(const TKey& key);
};

template <std::input_iterator Iterator, typename Compare = 
	std::less<typename std::iterator_traits<Iterator>::value_type::first_type>, std::size_t Order = 5, typename U>
BSP_tree(Iterator begin, Iterator end, const Compare &cmp = 
	Compare(), pp_allocator<U> = pp_allocator<U>())
    -> BSP_tree<typename std::iterator_traits<Iterator>::value_type::first_type, typename std::iterator_traits<Iterator>::value_type::second_type, Compare, Order>;

template <typename TKey, typename TValue, typename Compare = std::less<TKey>, std::size_t Order = 5, typename U>
BSP_tree(std::initializer_list<std::pair<TKey, TValue>> data, const Compare &cmp = 
	Compare(), pp_allocator<U> = pp_allocator<U>())
    -> BSP_tree<TKey, TValue, Compare, Order>;

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
bool BSP_tree<TKey, TValue, Compare, Order>::compare_pairs(const tree_data_type &lhs, const tree_data_type &rhs) const
{
    return compare_keys(lhs.first, rhs.first);
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
bool BSP_tree<TKey, TValue, Compare, Order>::compare_keys(const TKey &lhs, const TKey &rhs) const
{
    return Compare::operator()(lhs, rhs);
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
bool BSP_tree<TKey, TValue, Compare, Order>::keys_equal(const TKey& lhs, const TKey& rhs) const
{
    return !compare_keys(lhs, rhs) && !compare_keys(rhs, lhs);
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
typename BSP_tree<TKey, TValue, Compare, Order>::allocator_type BSP_tree<TKey, TValue, Compare, Order>::get_allocator() const noexcept
{
    return _allocator;
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
pp_allocator<typename BSP_tree<TKey, TValue, Compare, Order>::InternalNode> BSP_tree<TKey, TValue, Compare, Order>::get_node_allocator_internal() const noexcept
{
    return pp_allocator<InternalNode>(_allocator);
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
pp_allocator<typename BSP_tree<TKey, TValue, Compare, Order>::LeafNode> BSP_tree<TKey, TValue, Compare, Order>::get_node_allocator_leaf() const noexcept
{
    return pp_allocator<LeafNode>(_allocator);
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
typename BSP_tree<TKey, TValue, Compare, Order>::value_type&
BSP_tree<TKey, TValue, Compare, Order>::as_value(tree_data_type& x) noexcept
{
    return reinterpret_cast<value_type&>(x);
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
const typename BSP_tree<TKey, TValue, Compare, Order>::value_type&
BSP_tree<TKey, TValue, Compare, Order>::as_value(const tree_data_type& x) noexcept
{
    return reinterpret_cast<const value_type&>(x);
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
BSP_tree<TKey, TValue, Compare, Order>::bsptree_const_iterator::bsptree_const_iterator(const LeafNode* node, size_type index)
    : _node(node), _index(index)
{
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
BSP_tree<TKey, TValue, Compare, Order>::BSP_tree(const Compare& cmp, allocator_type alloc)
    : Compare(cmp), _allocator(alloc), _root(nullptr), _size(0)
{
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
BSP_tree<TKey, TValue, Compare, Order>::BSP_tree(allocator_type alloc, const Compare& cmp)
    : BSP_tree(cmp, alloc)
{
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
template <std::input_iterator Iterator>
    requires std::is_same_v<typename std::iterator_traits<Iterator>::value_type, typename BSP_tree<TKey, TValue, Compare, Order>::tree_data_type>
BSP_tree<TKey, TValue, Compare, Order>::BSP_tree(Iterator begin, Iterator end, const Compare& cmp, allocator_type alloc)
    : BSP_tree(cmp, alloc)
{
    for (; begin != end; ++begin)
    {
        insert(tree_data_type(begin->first, begin->second));
    }
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
BSP_tree<TKey, TValue, Compare, Order>::BSP_tree(std::initializer_list<tree_data_type> data, const Compare& cmp, allocator_type alloc)
    : BSP_tree(data.begin(), data.end(), cmp, alloc)
{
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
BSP_tree<TKey, TValue, Compare, Order>::BSP_tree(const BSP_tree& other)
    : Compare(other), _allocator(other._allocator.select_on_container_copy_construction()), _root(nullptr), _size(0)
{
    for (auto it = other.begin(); it != other.end(); ++it)
    {
        insert(tree_data_type(it->first, it->second));
    }
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
BSP_tree<TKey, TValue, Compare, Order>::BSP_tree(BSP_tree&& other) noexcept
    : Compare(std::move(other)), _allocator(std::move(other._allocator)), _root(other._root), _size(other._size)
{
    other._root = nullptr;
    other._size = 0;
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
BSP_tree<TKey, TValue, Compare, Order>& BSP_tree<TKey, TValue, Compare, Order>::operator=(const BSP_tree& other)
{
    if (this != &other)
    {
        this->clear();
        Compare::operator=(other);
        _allocator = other._allocator.select_on_container_copy_construction();
        for (auto it = other.begin(); it != other.end(); ++it)
        {
            insert(tree_data_type(it->first, it->second));
        }
    }
    return *this;
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
BSP_tree<TKey, TValue, Compare, Order>& BSP_tree<TKey, TValue, Compare, Order>::operator=(BSP_tree&& other) noexcept
{
    if (this != &other)
    {
        clear();
        static_cast<Compare&>(*this) = std::move(static_cast<Compare&>(other));
        _allocator = std::move(other._allocator);
        _root = other._root;
        _size = other._size;
        other._root = nullptr;
        other._size = 0;
    }
    return *this;
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
BSP_tree<TKey, TValue, Compare, Order>::~BSP_tree() noexcept
{
    clear();
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
BSP_tree<TKey, TValue, Compare, Order>::bsptree_iterator::bsptree_iterator(LeafNode* node, size_type index)
    : _node(node), _index(index)
{
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
typename BSP_tree<TKey, TValue, Compare, Order>::bsptree_iterator::reference BSP_tree<TKey, TValue, Compare, Order>::bsptree_iterator::operator*() const noexcept
{
    return BSP_tree::as_value(_node->data[_index]);
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
typename BSP_tree<TKey, TValue, Compare, Order>::bsptree_iterator::pointer BSP_tree<TKey, TValue, Compare, Order>::bsptree_iterator::operator->() const noexcept
{
    return &(BSP_tree::as_value(_node->data[_index]));
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
bool BSP_tree<TKey, TValue, Compare, Order>::iterators_are_equal(const LeafNode* node, size_type index, const LeafNode* other_node, size_type other_index)
{
    return index == other_index && node == other_node;
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
template <typename NodePtr>
void BSP_tree<TKey, TValue, Compare, Order>::increment_iterator(NodePtr& node, size_type& index)
{
    if (node == nullptr)
        return;
    if (index + 1 < node->data.size())
    {
        ++index;
    }
    else
    {
        node = node->next;
        index = 0;
    }
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
typename BSP_tree<TKey, TValue, Compare, Order>::bsptree_iterator& BSP_tree<TKey, TValue, Compare, Order>::bsptree_iterator::operator++()
{
    BSP_tree::increment_iterator(_node, _index);
    return *this;
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
typename BSP_tree<TKey, TValue, Compare, Order>::bsptree_iterator BSP_tree<TKey, TValue, Compare, Order>::bsptree_iterator::operator++(int)
{
    auto tmp = *this;
    ++(*this);
    return tmp;
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
bool BSP_tree<TKey, TValue, Compare, Order>::bsptree_iterator::operator==(const self& other) const noexcept
{
    return BSP_tree::iterators_are_equal(_node, _index, other._node, other._index);
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
bool BSP_tree<TKey, TValue, Compare, Order>::bsptree_iterator::operator!=(const self& other) const noexcept
{
    return !(*this == other);
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
typename BSP_tree<TKey, TValue, Compare, Order>::size_type BSP_tree<TKey, TValue, Compare, Order>::bsptree_iterator::current_node_keys_count() const noexcept
{
    return _node == nullptr ? 0 : _node->data.size();
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
typename BSP_tree<TKey, TValue, Compare, Order>::size_type BSP_tree<TKey, TValue, Compare, Order>::bsptree_iterator::index() const noexcept
{
    return _index;
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
BSP_tree<TKey, TValue, Compare, Order>::bsptree_const_iterator::bsptree_const_iterator(const bsptree_iterator& it) noexcept
    : _node(it._node), _index(it._index)
{
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
typename BSP_tree<TKey, TValue, Compare, Order>::bsptree_const_iterator::reference BSP_tree<TKey, TValue, Compare, Order>::bsptree_const_iterator::operator*() const noexcept
{
    return BSP_tree::as_value(_node->data[_index]);
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
typename BSP_tree<TKey, TValue, Compare, Order>::bsptree_const_iterator::pointer BSP_tree<TKey, TValue, Compare, Order>::bsptree_const_iterator::operator->() const noexcept
{
    return &(BSP_tree::as_value(_node->data[_index]));
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
typename BSP_tree<TKey, TValue, Compare, Order>::bsptree_const_iterator& BSP_tree<TKey, TValue, Compare, Order>::bsptree_const_iterator::operator++()
{
    BSP_tree::increment_iterator(_node, _index);
    return *this;
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
typename BSP_tree<TKey, TValue, Compare, Order>::bsptree_const_iterator BSP_tree<TKey, TValue, Compare, Order>::bsptree_const_iterator::operator++(int)
{
    auto tmp = *this;
    ++(*this);
    return tmp;
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
bool BSP_tree<TKey, TValue, Compare, Order>::bsptree_const_iterator::operator==(const self& other) const noexcept
{
    return BSP_tree::iterators_are_equal(_node, _index, other._node, other._index);
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
bool BSP_tree<TKey, TValue, Compare, Order>::bsptree_const_iterator::operator!=(const self& other) const noexcept
{
    return !(*this == other);
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
typename BSP_tree<TKey, TValue, Compare, Order>::size_type BSP_tree<TKey, TValue, Compare, Order>::bsptree_const_iterator::current_node_keys_count() const noexcept
{
    return _node == nullptr ? 0 : _node->data.size();
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
typename BSP_tree<TKey, TValue, Compare, Order>::size_type BSP_tree<TKey, TValue, Compare, Order>::bsptree_const_iterator::index() const noexcept
{
    return _index;
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
TValue& BSP_tree<TKey, TValue, Compare, Order>::at(const TKey& key)
{
    auto it = find(key);
    if (it == end())
        throw std::out_of_range("BSP_tree::at");
    return it->second;
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
const TValue& BSP_tree<TKey, TValue, Compare, Order>::at(const TKey& key) const
{
    auto it = find(key);
    if (it == end())
        throw std::out_of_range("BSP_tree::at");
    return it->second;
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
TValue& BSP_tree<TKey, TValue, Compare, Order>::operator[](const TKey& key)
{
    return emplace(key, TValue{}).first->second;
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
TValue& BSP_tree<TKey, TValue, Compare, Order>::operator[](TKey&& key)
{
    return emplace(std::move(key), TValue{}).first->second;
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
typename BSP_tree<TKey, TValue, Compare, Order>::LeafNode* BSP_tree<TKey, TValue, Compare, Order>::get_leftmost_leaf()
{
    if (_root == nullptr)
        return nullptr;
    NodeBase* child = _root;
    while (true)
    {
        if (child->is_leaf)
        {
            return static_cast<LeafNode*>(child);
        }
        child = static_cast<InternalNode*>(child)->children[0];
    }
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
typename BSP_tree<TKey, TValue, Compare, Order>::bsptree_iterator BSP_tree<TKey, TValue, Compare, Order>::begin()
{
    if (_root == nullptr)
        return bsptree_iterator();
    return bsptree_iterator(get_leftmost_leaf(), 0);
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
typename BSP_tree<TKey, TValue, Compare, Order>::bsptree_iterator BSP_tree<TKey, TValue, Compare, Order>::end()
{
    return bsptree_iterator(nullptr, 0);
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
typename BSP_tree<TKey, TValue, Compare, Order>::bsptree_const_iterator BSP_tree<TKey, TValue, Compare, Order>::begin() const
{
    return cbegin();
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
typename BSP_tree<TKey, TValue, Compare, Order>::bsptree_const_iterator BSP_tree<TKey, TValue, Compare, Order>::end() const
{
    return cend();
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
typename BSP_tree<TKey, TValue, Compare, Order>::bsptree_const_iterator BSP_tree<TKey, TValue, Compare, Order>::cbegin() const
{
    return bsptree_const_iterator(const_cast<BSP_tree*>(this)->begin());
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
typename BSP_tree<TKey, TValue, Compare, Order>::bsptree_const_iterator BSP_tree<TKey, TValue, Compare, Order>::cend() const
{
    return bsptree_const_iterator(const_cast<BSP_tree*>(this)->end());
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
typename BSP_tree<TKey, TValue, Compare, Order>::size_type BSP_tree<TKey, TValue, Compare, Order>::size() const noexcept
{
    return _size;
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
bool BSP_tree<TKey, TValue, Compare, Order>::empty() const noexcept
{
    return _size == 0;
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
typename BSP_tree<TKey, TValue, Compare, Order>::size_type BSP_tree<TKey, TValue, Compare, Order>::find_key_index(InternalNode* node, const TKey& key)
{
    size_type left = 0;
    size_type right = node->keys.size();
    while (left < right)
    {
        size_type mid = (left + right) / 2;
        if (keys_equal(node->keys[mid], key))
            return mid;
        else if (compare_keys(node->keys[mid], key))
            left = mid + 1;
        else
            right = mid;
    }
    return left;
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
typename BSP_tree<TKey, TValue, Compare, Order>::size_type BSP_tree<TKey, TValue, Compare, Order>::find_key_index(LeafNode* node, const TKey& key)
{
    size_type left = 0;
    size_type right = node->data.size();
    while (left < right)
    {
        size_type mid = (left + right) / 2;
        if (keys_equal(node->data[mid].first, key))
            return mid;
        else if (compare_keys(node->data[mid].first, key))
            left = mid + 1;
        else
            right = mid;
    }
    return left;
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
typename BSP_tree<TKey, TValue, Compare, Order>::size_type BSP_tree<TKey, TValue, Compare, Order>::upper_bound_key_index(InternalNode* node, const TKey& key)
{
    size_type idx = find_key_index(node, key);
    if (idx < node->keys.size() && keys_equal(node->keys[idx], key))
        return idx + 1;
    return idx;
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
typename BSP_tree<TKey, TValue, Compare, Order>::size_type BSP_tree<TKey, TValue, Compare, Order>::upper_bound_key_index(LeafNode* node, const TKey& key)
{
    size_type idx = find_key_index(node, key);
    if (idx < node->data.size() && keys_equal(node->data[idx].first, key))
        return idx + 1;
    return idx;
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
typename BSP_tree<TKey, TValue, Compare, Order>::bsptree_iterator BSP_tree<TKey, TValue, Compare, Order>::find(const TKey& key)
{
    if (_root == nullptr)
        return end();
    NodeBase* cur = _root;
    while (true)
    {
        if (cur->is_leaf)
        {
            LeafNode* term = static_cast<LeafNode*>(cur);
            size_type i = find_key_index(term, key);
            if (i < term->data.size() && keys_equal(term->data[i].first, key))
            {
                return bsptree_iterator(term, i);
            }
            return end();
        }
        InternalNode* mid = static_cast<InternalNode*>(cur);
        cur = mid->children[upper_bound_key_index(mid, key)];
    }
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
typename BSP_tree<TKey, TValue, Compare, Order>::bsptree_const_iterator BSP_tree<TKey, TValue, Compare, Order>::find(const TKey& key) const
{
    return bsptree_const_iterator(const_cast<BSP_tree*>(this)->find(key));
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
typename BSP_tree<TKey, TValue, Compare, Order>::bsptree_iterator BSP_tree<TKey, TValue, Compare, Order>::lower_bound(const TKey& key)
{
    if (_root == nullptr)
        return end();
    NodeBase* cur = _root;
    while (true)
    {
        if (cur->is_leaf)
        {
            LeafNode* term = static_cast<LeafNode*>(cur);
            size_type idx = find_key_index(term, key);
            if (idx == term->data.size())
            {
                if (term->next == nullptr) return end();
                return bsptree_iterator(term->next, 0);
            }
            return bsptree_iterator(term, idx);
        }
        InternalNode* mid = static_cast<InternalNode*>(cur);
        cur = mid->children[upper_bound_key_index(mid, key)];
    }
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
typename BSP_tree<TKey, TValue, Compare, Order>::bsptree_const_iterator BSP_tree<TKey, TValue, Compare, Order>::lower_bound(const TKey& key) const
{
    return bsptree_const_iterator(const_cast<BSP_tree*>(this)->lower_bound(key));
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
typename BSP_tree<TKey, TValue, Compare, Order>::bsptree_iterator BSP_tree<TKey, TValue, Compare, Order>::upper_bound(const TKey& key)
{
    if (_root == nullptr)
        return end();
    NodeBase* cur = _root;
    while (true)
    {
        if (cur->is_leaf)
        {
            LeafNode* term = static_cast<LeafNode*>(cur);
            size_type idx = upper_bound_key_index(term, key);
            if (idx == term->data.size())
            {
                if (term->next == nullptr) return end();
                return bsptree_iterator(term->next, 0);
            }
            return bsptree_iterator(term, idx);
        }
        InternalNode* mid = static_cast<InternalNode*>(cur);
        cur = mid->children[upper_bound_key_index(mid, key)];
    }
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
typename BSP_tree<TKey, TValue, Compare, Order>::bsptree_const_iterator BSP_tree<TKey, TValue, Compare, Order>::upper_bound(const TKey& key) const
{
    return bsptree_const_iterator(const_cast<BSP_tree*>(this)->upper_bound(key));
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
bool BSP_tree<TKey, TValue, Compare, Order>::contains(const TKey& key) const
{
    return find(key) != end();
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
typename BSP_tree<TKey, TValue, Compare, Order>::InternalNode* BSP_tree<TKey, TValue, Compare, Order>::make_node_internal()
{
    return get_node_allocator_internal().template new_object<InternalNode>();
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
typename BSP_tree<TKey, TValue, Compare, Order>::LeafNode* BSP_tree<TKey, TValue, Compare, Order>::make_node_leaf()
{
    return get_node_allocator_leaf().template new_object<LeafNode>();
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
void BSP_tree<TKey, TValue, Compare, Order>::delete_node_internal(InternalNode* node)
{
    if (node != nullptr)
        get_node_allocator_internal().template delete_object<InternalNode>(node);
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
void BSP_tree<TKey, TValue, Compare, Order>::delete_node_leaf(LeafNode* node)
{
    if (node != nullptr)
        get_node_allocator_leaf().template delete_object<LeafNode>(node);
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
void BSP_tree<TKey, TValue, Compare, Order>::destroy_tree(NodeBase* node)
{
    if (node == nullptr)
        return;
    if (node->is_leaf)
    {
        delete_node_leaf(static_cast<LeafNode*>(node));
    }
    else
    {
        InternalNode* mid = static_cast<InternalNode*>(node);
        for (auto* child : mid->children)
        {
            destroy_tree(child);
        }
        delete_node_internal(mid);
    }
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
void BSP_tree<TKey, TValue, Compare, Order>::clear() noexcept
{
    destroy_tree(_root);
    _root = nullptr;
    _size = 0;
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
std::pair<typename BSP_tree<TKey, TValue, Compare, Order>::bsptree_iterator, bool> BSP_tree<TKey, TValue, Compare, Order>::insert(const tree_data_type& data)
{
    return emplace(data);
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
std::pair<typename BSP_tree<TKey, TValue, Compare, Order>::bsptree_iterator, bool> BSP_tree<TKey, TValue, Compare, Order>::insert(tree_data_type&& data)
{
    return emplace(std::move(data));
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
typename BSP_tree<TKey, TValue, Compare, Order>::size_type BSP_tree<TKey, TValue, Compare, Order>::get_keys_size(NodeBase* node)
{
    if (node == nullptr) return 0;
    if (node->is_leaf)
        return static_cast<LeafNode*>(node)->data.size();
    return static_cast<InternalNode*>(node)->keys.size();
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
void BSP_tree<TKey, TValue, Compare, Order>::borrow_from_left(InternalNode* parent, size_type child_index)
{
    if (parent->children[child_index]->is_leaf)
    {
        LeafNode* child_term = static_cast<LeafNode*>(parent->children[child_index]);
        LeafNode* left_term = static_cast<LeafNode*>(parent->children[child_index - 1]);
        child_term->data.insert(child_term->data.begin(), std::move(left_term->data.back()));
        parent->keys[child_index - 1] = child_term->data.front().first;
        left_term->data.pop_back();
    }
    else
    {
        InternalNode* child_mid = static_cast<InternalNode*>(parent->children[child_index]);
        InternalNode* left_mid = static_cast<InternalNode*>(parent->children[child_index - 1]);
        child_mid->keys.insert(child_mid->keys.begin(), std::move(parent->keys[child_index - 1]));
        child_mid->children.insert(child_mid->children.begin(), left_mid->children.back());
        left_mid->children.pop_back();
        parent->keys[child_index - 1] = std::move(left_mid->keys.back());
        left_mid->keys.pop_back();
    }
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
void BSP_tree<TKey, TValue, Compare, Order>::borrow_from_right(InternalNode* parent, size_type child_index)
{
    if (parent->children[child_index]->is_leaf)
    {
        LeafNode* child_term = static_cast<LeafNode*>(parent->children[child_index]);
        LeafNode* right_term = static_cast<LeafNode*>(parent->children[child_index + 1]);
        child_term->data.push_back(std::move(right_term->data.front()));
        right_term->data.erase(right_term->data.begin());
        parent->keys[child_index] = right_term->data.front().first;
    }
    else
    {
        InternalNode* child_mid = static_cast<InternalNode*>(parent->children[child_index]);
        InternalNode* right_mid = static_cast<InternalNode*>(parent->children[child_index + 1]);
        child_mid->keys.push_back(std::move(parent->keys[child_index]));
        child_mid->children.push_back(right_mid->children.front());
        right_mid->children.erase(right_mid->children.begin());
        parent->keys[child_index] = std::move(right_mid->keys.front());
        right_mid->keys.erase(right_mid->keys.begin());
    }
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
bool BSP_tree<TKey, TValue, Compare, Order>::try_lend_key_to_neighbour(InternalNode* parent, size_type child_index)
{
    if (child_index > 0 && get_keys_size(parent->children[child_index - 1]) < maximum_keys_in_node)
    {
        borrow_from_right(parent, child_index - 1);
        return true;
    }
    if (child_index < parent->children.size() - 1 && get_keys_size(parent->children[child_index + 1]) < maximum_keys_in_node)
    {
        borrow_from_left(parent, child_index + 1);
        return true;
    }
    return false;
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
void BSP_tree<TKey, TValue, Compare, Order>::split_child(InternalNode* parent, size_type child_index)
{
    NodeBase* child = parent->children[child_index];
    if (child->is_leaf)
    {
        LeafNode* old_leaf = static_cast<LeafNode*>(child);
        LeafNode* new_leaf = make_node_leaf();
        size_type mid = old_leaf->data.size() / 2;
        for (size_type i = mid; i < old_leaf->data.size(); ++i)
            new_leaf->data.push_back(std::move(old_leaf->data[i]));
        old_leaf->data.erase(old_leaf->data.begin() + mid, old_leaf->data.end());
        new_leaf->next = old_leaf->next;
        old_leaf->next = new_leaf;
        parent->children.insert(parent->children.begin() + child_index + 1, new_leaf);
        parent->keys.insert(parent->keys.begin() + child_index, new_leaf->data.front().first);
    }
    else
    {
        InternalNode* old_internal = static_cast<InternalNode*>(child);
        InternalNode* new_internal = make_node_internal();
        size_type mid = old_internal->keys.size() / 2;
        TKey median = std::move(old_internal->keys[mid]);
        for (size_type i = mid + 1; i < old_internal->keys.size(); ++i)
            new_internal->keys.push_back(std::move(old_internal->keys[i]));
        old_internal->keys.erase(old_internal->keys.begin() + mid, old_internal->keys.end());
        for (size_type i = mid + 1; i < old_internal->children.size(); ++i)
            new_internal->children.push_back(old_internal->children[i]);
        old_internal->children.erase(old_internal->children.begin() + mid + 1, old_internal->children.end());
        parent->children.insert(parent->children.begin() + child_index + 1, new_internal);
        parent->keys.insert(parent->keys.begin() + child_index, std::move(median));
    }
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
void BSP_tree<TKey, TValue, Compare, Order>::split_root()
{
    size_type med_idx = minimum_keys_in_node + 1;
    InternalNode* new_root = make_node_internal();
    if (_root->is_leaf)
    {
        LeafNode* left_term = static_cast<LeafNode*>(_root);
        LeafNode* right_term = make_node_leaf();
        right_term->next = left_term->next;
        left_term->next = right_term;
        TKey median = left_term->data[med_idx].first;
        for (size_type i = med_idx; i < left_term->data.size(); ++i)
        {
            right_term->data.push_back(std::move(left_term->data[i]));
        }
        left_term->data.erase(left_term->data.begin() + med_idx, left_term->data.end());
        new_root->children.push_back(left_term);
        new_root->children.push_back(right_term);
        new_root->keys.push_back(std::move(median));
    }
    else
    {
        InternalNode* left = static_cast<InternalNode*>(_root);
        InternalNode* right = make_node_internal();
        TKey median = std::move(left->keys[med_idx]);
        for (size_type i = med_idx + 1; i < left->keys.size(); ++i)
        {
            right->keys.push_back(std::move(left->keys[i]));
        }
        left->keys.erase(left->keys.begin() + med_idx, left->keys.end());
        for (size_type i = med_idx + 1; i < left->children.size(); ++i)
        {
            right->children.push_back(left->children[i]);
        }
        left->children.erase(left->children.begin() + (med_idx + 1), left->children.end());
        new_root->children.push_back(left);
        new_root->children.push_back(right);
        new_root->keys.push_back(std::move(median));
    }
    _root = new_root;
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
void BSP_tree<TKey, TValue, Compare, Order>::insert_bottom_up(NodeBase* node, tree_data_type data)
{
    if (node->is_leaf)
    {
        LeafNode* term = static_cast<LeafNode*>(node);
        size_type idx = find_key_index(term, data.first);
        term->data.insert(term->data.begin() + idx, std::move(data));
        return;
    }
    InternalNode* mid = static_cast<InternalNode*>(node);
    size_type i = upper_bound_key_index(mid, data.first);
    insert_bottom_up(mid->children[i], std::move(data));
    if (get_keys_size(mid->children[i]) > maximum_keys_in_node)
    {
        if (!try_lend_key_to_neighbour(mid, i))
            split_child(mid, i);
    }
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
template <typename ...Args>
std::pair<typename BSP_tree<TKey, TValue, Compare, Order>::bsptree_iterator, bool> BSP_tree<TKey, TValue, Compare, Order>::emplace(Args&&... args)
{
    tree_data_type data(std::forward<Args>(args)...);
    auto existing = find(data.first);
    if (existing != end())
    {
        return { existing, false };
    }
    TKey key = data.first;
    if (_root == nullptr)
    {
        LeafNode* term = make_node_leaf();
        term->data.push_back(std::move(data));
        term->next = nullptr;
        _size++;
        _root = term;
        return { begin(), true };
    }
    insert_bottom_up(_root, std::move(data));
    if (get_keys_size(_root) > maximum_keys_in_root)
    {
        split_root();
    }
    _size++;
    return { find(key), true };
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
typename BSP_tree<TKey, TValue, Compare, Order>::bsptree_iterator BSP_tree<TKey, TValue, Compare, Order>::insert_or_assign(const tree_data_type& data)
{
    auto it = find(data.first);
    if (it != end())
    {
        it->second = data.second;
        return it;
    }
    return insert(data).first;
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
typename BSP_tree<TKey, TValue, Compare, Order>::bsptree_iterator BSP_tree<TKey, TValue, Compare, Order>::insert_or_assign(tree_data_type&& data)
{
    auto it = find(data.first);
    if (it != end())
    {
        it->second = std::move(data.second);
        return it;
    }
    return insert(std::move(data)).first;
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
template <typename ...Args>
typename BSP_tree<TKey, TValue, Compare, Order>::bsptree_iterator BSP_tree<TKey, TValue, Compare, Order>::emplace_or_assign(Args&&... args)
{
    tree_data_type data(std::forward<Args>(args)...);
    return insert_or_assign(std::move(data));
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
typename BSP_tree<TKey, TValue, Compare, Order>::bsptree_iterator BSP_tree<TKey, TValue, Compare, Order>::erase(bsptree_iterator pos)
{
    if (pos == end())
        return end();
    return erase(pos->first);
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
typename BSP_tree<TKey, TValue, Compare, Order>::bsptree_iterator BSP_tree<TKey, TValue, Compare, Order>::erase(bsptree_const_iterator pos)
{
    if (pos == cend())
        return end();
    return erase(pos->first);
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
typename BSP_tree<TKey, TValue, Compare, Order>::bsptree_iterator BSP_tree<TKey, TValue, Compare, Order>::erase(bsptree_iterator beg, bsptree_iterator en)
{
    while (beg != en)
    {
        beg = erase(beg);
    }
    return beg;
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
typename BSP_tree<TKey, TValue, Compare, Order>::bsptree_iterator BSP_tree<TKey, TValue, Compare, Order>::erase(bsptree_const_iterator beg, bsptree_const_iterator en)
{
    auto first = (beg == cend()) ? end() : find(beg->first);
    auto last = (en == cend()) ? end() : find(en->first);
    return erase(first, last);
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
void BSP_tree<TKey, TValue, Compare, Order>::merge_3_children_internal(InternalNode* parent, size_type mid_child_index)
{
    InternalNode* left = static_cast<InternalNode*>(parent->children[mid_child_index - 1]);
    InternalNode* mid = static_cast<InternalNode*>(parent->children[mid_child_index]);
    InternalNode* right = static_cast<InternalNode*>(parent->children[mid_child_index + 1]);
    size_type med_idx = (left->keys.size() + mid->keys.size() + right->keys.size() + 2) / 2 - left->keys.size() - 1;
    left->keys.push_back(std::move(parent->keys[mid_child_index - 1]));
    for (size_type i = 0; i < med_idx; ++i)
    {
        left->keys.push_back(std::move(mid->keys[i]));
    }
    for (size_type i = 0; i <= med_idx; ++i)
    {
        left->children.push_back(mid->children[i]);
    }
    parent->keys[mid_child_index - 1] = std::move(mid->keys[med_idx]);
    mid->keys.erase(mid->keys.begin(), mid->keys.begin() + (med_idx + 1));
    mid->children.erase(mid->children.begin(), mid->children.begin() + (med_idx + 1));
    mid->keys.push_back(parent->keys[mid_child_index]);
    for (size_type i = 0; i < right->keys.size(); ++i)
    {
        mid->keys.push_back(right->keys[i]);
    }
    for (size_type i = 0; i < right->children.size(); ++i)
    {
        mid->children.push_back(right->children[i]);
    }
    parent->keys.erase(parent->keys.begin() + mid_child_index);
    parent->children.erase(parent->children.begin() + (mid_child_index + 1));
    delete_node_internal(right);
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
void BSP_tree<TKey, TValue, Compare, Order>::merge_3_children_leaf(InternalNode* parent, size_type mid_child_index)
{
    LeafNode* left = static_cast<LeafNode*>(parent->children[mid_child_index - 1]);
    LeafNode* mid = static_cast<LeafNode*>(parent->children[mid_child_index]);
    LeafNode* right = static_cast<LeafNode*>(parent->children[mid_child_index + 1]);
    size_type med_idx = (left->data.size() + mid->data.size() + right->data.size()) / 2 - left->data.size();
    for (size_type i = 0; i < med_idx; ++i)
    {
        left->data.push_back(std::move(mid->data[i]));
    }
    parent->keys[mid_child_index - 1] = mid->data[med_idx].first;
    mid->data.erase(mid->data.begin(), mid->data.begin() + med_idx);
    for (size_type i = 0; i < right->data.size(); ++i)
    {
        mid->data.push_back(right->data[i]);
    }
    parent->keys.erase(parent->keys.begin() + mid_child_index);
    parent->children.erase(parent->children.begin() + (mid_child_index + 1));
    mid->next = right->next;
    delete_node_leaf(right);
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
void BSP_tree<TKey, TValue, Compare, Order>::merge_3_children(InternalNode* parent, size_type mid_child_index)
{
    if (parent->children[mid_child_index]->is_leaf)
    {
        merge_3_children_leaf(parent, mid_child_index);
    }
    else
    {
        merge_3_children_internal(parent, mid_child_index);
    }
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
void BSP_tree<TKey, TValue, Compare, Order>::merge_2_children(InternalNode* parent, size_type left_child_index)
{
    if (parent->children[left_child_index]->is_leaf)
    {
        LeafNode* left_term = static_cast<LeafNode*>(parent->children[left_child_index]);
        LeafNode* right = static_cast<LeafNode*>(parent->children[left_child_index + 1]);
        for (auto& key : right->data)
        {
            left_term->data.push_back(std::move(key));
        }
        parent->keys.erase(parent->keys.begin() + left_child_index);
        parent->children.erase(parent->children.begin() + (left_child_index + 1));
        left_term->next = right->next;
        delete_node_leaf(right);
    }
    else
    {
        InternalNode* left = static_cast<InternalNode*>(parent->children[left_child_index]);
        InternalNode* right = static_cast<InternalNode*>(parent->children[left_child_index + 1]);
        left->keys.push_back(parent->keys[left_child_index]);
        for (auto& key : right->keys)
        {
            left->keys.push_back(std::move(key));
        }
        for (auto* ptr : right->children)
        {
            left->children.push_back(ptr);
        }
        parent->keys.erase(parent->keys.begin() + left_child_index);
        parent->children.erase(parent->children.begin() + (left_child_index + 1));
        delete_node_internal(right);
    }
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
void BSP_tree<TKey, TValue, Compare, Order>::ensure_child_has_enough_keys(InternalNode* node, size_type& idx)
{
    if (get_keys_size(node->children[idx]) >= minimum_keys_in_node)
        return;
    if (idx > 0 && get_keys_size(node->children[idx - 1]) > minimum_keys_in_node)
    {
        borrow_from_left(node, idx);
    }
    else if (idx < node->children.size() - 1 && get_keys_size(node->children[idx + 1]) > minimum_keys_in_node)
    {
        borrow_from_right(node, idx);
    }
    else if (idx == 0 && node->children.size() >= 3 && get_keys_size(node->children[idx + 2]) > minimum_keys_in_node)
    {
        borrow_from_right(node, idx + 1);
        borrow_from_right(node, idx);
    }
    else if (idx == node->children.size() - 1 && node->children.size() >= 3 && get_keys_size(node->children[idx - 2]) > minimum_keys_in_node)
    {
        borrow_from_left(node, idx - 1);
        borrow_from_left(node, idx);
    }
    else
    {
        if (node->children.size() >= 3)
        {
            if (idx == 0)
                merge_3_children(node, idx + 1);
            else if (idx == node->children.size() - 1)
                merge_3_children(node, idx - 1);
            else
                merge_3_children(node, idx);
        }
        else
        {
            merge_2_children(node, 0);
        }
    }
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
bool BSP_tree<TKey, TValue, Compare, Order>::try_erase_from_node(NodeBase* node, const TKey& key)
{
    if (node->is_leaf)
    {
        LeafNode* node_term = static_cast<LeafNode*>(node);
        size_type idx = find_key_index(node_term, key);
        if (idx < node_term->data.size() && keys_equal(node_term->data[idx].first, key))
        {
            node_term->data.erase(node_term->data.begin() + idx);
            return true;
        }
        return false;
    }
    else
    {
        InternalNode* node_mid = static_cast<InternalNode*>(node);
        size_type idx = upper_bound_key_index(node_mid, key);
        bool res = try_erase_from_node(node_mid->children[idx], key);
        if (res)
            ensure_child_has_enough_keys(node_mid, idx);
        return res;
    }
}

template <typename TKey, typename TValue, typename Compare, std::size_t Order>
typename BSP_tree<TKey, TValue, Compare, Order>::bsptree_iterator BSP_tree<TKey, TValue, Compare, Order>::erase(const TKey& key)
{
    if (_root == nullptr)
        return end();
    if (!try_erase_from_node(_root, key))
        return end();
    --_size;
    if (_root->is_leaf)
    {
        LeafNode* root_term = static_cast<LeafNode*>(_root);
        if (root_term->data.empty())
        {
            _root = nullptr;
            delete_node_leaf(root_term);
        }
    }
    else
    {
        InternalNode* root_mid = static_cast<InternalNode*>(_root);
        if (root_mid->keys.empty())
        {
            if (root_mid->children.empty())
            {
                _root = nullptr;
            }
            else
            {
                _root = root_mid->children[0];
            }
            delete_node_internal(root_mid);
        }
    }
    return lower_bound(key);
}

#endif