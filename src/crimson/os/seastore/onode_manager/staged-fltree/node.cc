// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "node.h"

#include <cassert>
#include <exception>
#include <iostream>

#include "common/likely.h"
#include "node_extent_manager.h"
#include "node_impl.h"
#include "stages/node_stage_layout.h"

namespace crimson::os::seastore::onode {

using node_ertr = Node::node_ertr;
template <class... ValuesT>
using node_future = Node::node_future<ValuesT...>;

/*
 * tree_cursor_t
 */

tree_cursor_t::tree_cursor_t(
    Ref<LeafNode> node, const search_position_t& pos, const onode_t* p_value)
      : leaf_node{node}, position{pos}, p_value{p_value} {
  assert((!pos.is_end() && p_value) || (pos.is_end() && !p_value));
  if (!pos.is_end()) {
    assert(p_value == leaf_node->get_p_value(position));
    leaf_node->do_track_cursor(*this);
  }
}

tree_cursor_t::~tree_cursor_t() {
  if (!position.is_end()) {
    leaf_node->do_untrack_cursor(*this);
  }
}

const onode_t* tree_cursor_t::get_p_value() const {
  assert(!is_end());
  if (!p_value) {
    // NOTE: the leaf node is always present when we hold its reference.
    p_value = leaf_node->get_p_value(position);
  }
  assert(p_value);
  return p_value;
}

void tree_cursor_t::update_track(
    Ref<LeafNode> node, const search_position_t& pos) {
  // already untracked
  assert(!pos.is_end());
  assert(!is_end());
  leaf_node = node;
  position = pos;
  // p_value must be already invalidated
  assert(!p_value);
  leaf_node->do_track_cursor(*this);
}

void tree_cursor_t::set_p_value(const onode_t* _p_value) {
  if (!p_value) {
    p_value = _p_value;
  } else {
    assert(p_value == _p_value);
  }
}

/*
 * Node
 */

Node::Node(NodeImplURef&& impl) : impl{std::move(impl)} {}

Node::~Node() {
  // XXX: tolerate failure between allocate() and as_child()
  if (is_root()) {
    super->do_untrack_root(*this);
  } else {
    _parent_info->ptr->do_untrack_child(*this);
  }
}

level_t Node::level() const {
  return impl->level();
}

node_future<Node::search_result_t> Node::lower_bound(
    context_t c, const key_hobj_t& key) {
  return seastar::do_with(
    MatchHistory(), [this, c, &key](auto& history) {
      return lower_bound_tracked(c, key, history);
    }
  );
}

node_future<std::pair<Ref<tree_cursor_t>, bool>> Node::insert(
    context_t c, const key_hobj_t& key, const onode_t& value) {
  return seastar::do_with(
    MatchHistory(), [this, c, &key, &value](auto& history) {
      return lower_bound_tracked(c, key, history
      ).safe_then([c, &key, &value, &history](auto result) {
        if (result.match == MatchKindBS::EQ) {
          return node_ertr::make_ready_future<std::pair<Ref<tree_cursor_t>, bool>>(
              std::make_pair(result.p_cursor, false));
        } else {
          auto leaf_node = result.p_cursor->get_leaf_node();
          return leaf_node->insert_value(
              c, key, value, result.p_cursor->get_position(), history
          ).safe_then([](auto p_cursor) {
            return node_ertr::make_ready_future<std::pair<Ref<tree_cursor_t>, bool>>(
                std::make_pair(p_cursor, true));
          });
        }
      });
    }
  );
}

std::ostream& Node::dump(std::ostream& os) const {
  return impl->dump(os);
}

std::ostream& Node::dump_brief(std::ostream& os) const {
  return impl->dump_brief(os);
}

void Node::test_make_destructable(
    context_t c, NodeExtentMutable& mut, Super::URef&& _super) {
  impl->test_set_tail(mut);
  make_root(c, std::move(_super));
}

node_future<> Node::mkfs(context_t c, RootNodeTracker& root_tracker) {
  return LeafNode::allocate_root(c, root_tracker
  ).safe_then([](auto ret) { /* FIXME: discard_result(); */ });
}

node_future<Ref<Node>> Node::load_root(context_t c, RootNodeTracker& root_tracker) {
  return c.nm.get_super(c.t, root_tracker
  ).safe_then([c, &root_tracker](auto&& _super) {
    auto root_addr = _super->get_root_laddr();
    assert(root_addr != L_ADDR_NULL);
    return Node::load(c, root_addr, true
    ).safe_then([c, _super = std::move(_super),
                 &root_tracker](auto root) mutable {
      assert(root->impl->field_type() == field_type_t::N0);
      root->as_root(std::move(_super));
      assert(root == root_tracker.get_root(c.t));
      return node_ertr::make_ready_future<Ref<Node>>(root);
    });
  });
}

void Node::make_root(context_t c, Super::URef&& _super) {
  _super->write_root_laddr(c, impl->laddr());
  as_root(std::move(_super));
}

void Node::as_root(Super::URef&& _super) {
  assert(!super && !_parent_info);
  assert(_super->get_root_laddr() == impl->laddr());
  assert(impl->is_level_tail());
  super = std::move(_super);
  super->do_track_root(*this);
}

node_future<> Node::upgrade_root(context_t c) {
  assert(is_root());
  assert(impl->is_level_tail());
  assert(impl->field_type() == field_type_t::N0);
  super->do_untrack_root(*this);
  return InternalNode::allocate_root(c, impl->level(), impl->laddr(), std::move(super)
  ).safe_then([this](auto new_root) {
    as_child(search_position_t::end(), new_root);
  });
}

template <bool VALIDATE>
void Node::as_child(const search_position_t& pos, Ref<InternalNode> parent_node) {
  assert(!super);
  _parent_info = parent_info_t{pos, parent_node};
  parent_info().ptr->do_track_child<VALIDATE>(*this);
}
template void Node::as_child<true>(const search_position_t&, Ref<InternalNode>);
template void Node::as_child<false>(const search_position_t&, Ref<InternalNode>);

node_future<> Node::insert_parent(context_t c, Ref<Node> right_node) {
  assert(!is_root());
  // TODO(cross-node string dedup)
  return parent_info().ptr->apply_child_split(
      c, parent_info().position, this, right_node);
}

node_future<Ref<Node>> Node::load(
    context_t c, laddr_t addr, bool expect_is_level_tail) {
  // NOTE:
  // *option1: all types of node have the same length;
  // option2: length is defined by node/field types;
  // option3: length is totally flexible;
  return c.nm.read_extent(c.t, addr, NODE_BLOCK_SIZE
  ).safe_then([expect_is_level_tail](auto extent) {
    const auto header = reinterpret_cast<const node_header_t*>(extent->get_read());
    auto node_type = header->get_node_type();
    auto field_type = header->get_field_type();
    if (!field_type.has_value()) {
      throw std::runtime_error("load failed: bad field type");
    }
    if (node_type == node_type_t::LEAF) {
      auto impl = LeafNodeImpl::load(extent, *field_type, expect_is_level_tail);
      return Ref<Node>(new LeafNode(impl.get(), std::move(impl)));
    } else if (node_type == node_type_t::INTERNAL) {
      auto impl = InternalNodeImpl::load(extent, *field_type, expect_is_level_tail);
      return Ref<Node>(new InternalNode(impl.get(), std::move(impl)));
    } else {
      assert(false && "impossible path");
    }
  });
}

/*
 * InternalNode
 */

InternalNode::InternalNode(InternalNodeImpl* impl, NodeImplURef&& impl_ref)
  : Node(std::move(impl_ref)), impl{impl} {}

node_future<> InternalNode::apply_child_split(
    context_t c, const search_position_t& pos,
    Ref<Node> left_child, Ref<Node> right_child) {
#ifndef NDEBUG
  if (pos.is_end()) {
    assert(impl->is_level_tail());
  }
#endif
  impl->prepare_mutate(c);

  // update pos => left_child to pos => right_child
  auto left_child_addr = left_child->impl->laddr();
  auto right_child_addr = right_child->impl->laddr();
  impl->replace_child_addr(pos, right_child_addr, left_child_addr);
  replace_track(pos, right_child, left_child);

  auto left_key = left_child->impl->get_largest_key_view();
  search_position_t insert_pos = pos;
  auto [insert_stage, insert_size] = impl->evaluate_insert(
      left_key, left_child_addr, insert_pos);
  auto free_size = impl->free_size();
  if (free_size >= insert_size) {
    // insert
    auto p_value = impl->insert(left_key, left_child_addr,
                                insert_pos, insert_stage, insert_size);
    assert(impl->free_size() == free_size - insert_size);
    assert(insert_pos <= pos);
    assert(*p_value == left_child_addr);
    track_insert(insert_pos, insert_stage, left_child, right_child);
    validate_tracked_children();
    return node_ertr::now();
  }
  // split and insert
  std::cout << "  try insert at: " << insert_pos
            << ", insert_stage=" << (int)insert_stage
            << ", insert_size=" << insert_size
            << ", values=0x" << std::hex << left_child_addr
            << ",0x" << right_child_addr << std::dec << std::endl;
  Ref<InternalNode> this_ref = this;
  return (is_root() ? upgrade_root(c) : node_ertr::now()
  ).safe_then([this, c] {
    return InternalNode::allocate(
        c, impl->field_type(), impl->is_level_tail(), impl->level());
  }).safe_then([this_ref, this, c, left_key, left_child, right_child,
                insert_pos, insert_stage, insert_size](auto fresh_right) mutable {
    auto right_node = fresh_right.node;
    auto left_child_addr = left_child->impl->laddr();
    auto [split_pos, is_insert_left, p_value] = impl->split_insert(
        fresh_right.mut, *right_node->impl, left_key, left_child_addr,
        insert_pos, insert_stage, insert_size);
    assert(*p_value == left_child_addr);
    track_split(split_pos, right_node);
    if (is_insert_left) {
      track_insert(insert_pos, insert_stage, left_child);
    } else {
      right_node->track_insert(insert_pos, insert_stage, left_child);
    }
    validate_tracked_children();
    right_node->validate_tracked_children();

    // propagate index to parent
    return insert_parent(c, right_node);
    // TODO (optimize)
    // try to acquire space from siblings before split... see btrfs
  });
}

node_future<Ref<InternalNode>> InternalNode::allocate_root(
    context_t c, level_t old_root_level,
    laddr_t old_root_addr, Super::URef&& super) {
  return InternalNode::allocate(c, field_type_t::N0, true, old_root_level + 1
  ).safe_then([c, old_root_addr,
               super = std::move(super)](auto fresh_node) mutable {
    auto root = fresh_node.node;
    const laddr_t* p_value = root->impl->get_p_value(search_position_t::end());
    fresh_node.mut.copy_in_absolute(
        const_cast<laddr_t*>(p_value), old_root_addr);
    root->make_root_from(c, std::move(super), old_root_addr);
    return root;
  });
}

node_future<Ref<tree_cursor_t>>
InternalNode::lookup_smallest(context_t c) {
  auto position = search_position_t::begin();
  laddr_t child_addr = *impl->get_p_value(position);
  return get_or_track_child(c, position, child_addr
  ).safe_then([c](auto child) {
    return child->lookup_smallest(c);
  });
}

node_future<Ref<tree_cursor_t>>
InternalNode::lookup_largest(context_t c) {
  // NOTE: unlike LeafNode::lookup_largest(), this only works for the tail
  // internal node to return the tail child address.
  auto position = search_position_t::end();
  laddr_t child_addr = *impl->get_p_value(position);
  return get_or_track_child(c, position, child_addr).safe_then([c](auto child) {
    return child->lookup_largest(c);
  });
}

node_future<Node::search_result_t>
InternalNode::lower_bound_tracked(
    context_t c, const key_hobj_t& key, MatchHistory& history) {
  auto result = impl->lower_bound(key, history);
  return get_or_track_child(c, result.position, *result.p_value
  ).safe_then([c, &key, &history](auto child) {
    // XXX(multi-type): pass result.mstat to child
    return child->lower_bound_tracked(c, key, history);
  });
}

node_future<> InternalNode::test_clone_root(
    context_t c_other, RootNodeTracker& tracker_other) const {
  assert(is_root());
  assert(impl->is_level_tail());
  assert(impl->field_type() == field_type_t::N0);
  Ref<const InternalNode> this_ref = this;
  return InternalNode::allocate(c_other, field_type_t::N0, true, impl->level()
  ).safe_then([this, c_other, &tracker_other](auto fresh_other) {
    impl->test_copy_to(fresh_other.mut);
    auto cloned_root = fresh_other.node;
    return c_other.nm.get_super(c_other.t, tracker_other
    ).safe_then([c_other, cloned_root](auto&& super_other) {
      cloned_root->make_root_new(c_other, std::move(super_other));
      return cloned_root;
    });
  }).safe_then([this_ref, this, c_other](auto cloned_root) {
    // clone tracked children
    // In some unit tests, the children are stubbed out that they
    // don't exist in NodeExtentManager, and are only tracked in memory.
    return crimson::do_for_each(
      tracked_child_nodes.begin(),
      tracked_child_nodes.end(),
      [this_ref, c_other, cloned_root](auto& kv) {
        assert(kv.first == kv.second->parent_info().position);
        return kv.second->test_clone_non_root(c_other, cloned_root);
      }
    );
  });
}

node_future<Ref<Node>> InternalNode::get_or_track_child(
    context_t c, const search_position_t& position, laddr_t child_addr) {
  bool level_tail = position.is_end();
  Ref<Node> child;
  auto found = tracked_child_nodes.find(position);
  Ref<InternalNode> this_ref = this;
  return (found == tracked_child_nodes.end()
    ? Node::load(c, child_addr, level_tail
      ).safe_then([this, position] (auto child) {
        child->as_child(position, this);
        return child;
      })
    : node_ertr::make_ready_future<Ref<Node>>(found->second)
  ).safe_then([this_ref, this, position, child_addr] (auto child) {
    assert(child_addr == child->impl->laddr());
    assert(position == child->parent_info().position);
    validate_child(*child);
    return child;
  });
}

void InternalNode::track_insert(
      const search_position_t& insert_pos, match_stage_t insert_stage,
      Ref<Node> insert_child, Ref<Node> nxt_child) {
  // update tracks
  auto pos_upper_bound = insert_pos;
  pos_upper_bound.index_by_stage(insert_stage) = INDEX_END;
  auto first = tracked_child_nodes.lower_bound(insert_pos);
  auto last = tracked_child_nodes.lower_bound(pos_upper_bound);
  std::vector<Node*> nodes;
  std::for_each(first, last, [&nodes](auto& kv) {
    nodes.push_back(kv.second);
  });
  tracked_child_nodes.erase(first, last);
  for (auto& node : nodes) {
    auto _pos = node->parent_info().position;
    assert(!_pos.is_end());
    ++_pos.index_by_stage(insert_stage);
    node->as_child(_pos, this);
  }
  // track insert
  insert_child->as_child(insert_pos, this);

#ifndef NDEBUG
  // validate left_child is before right_child
  if (nxt_child) {
    auto iter = tracked_child_nodes.find(insert_pos);
    ++iter;
    assert(iter->second == nxt_child);
  }
#endif
}

void InternalNode::replace_track(
    const search_position_t& position, Ref<Node> new_child, Ref<Node> old_child) {
  assert(tracked_child_nodes[position] == old_child);
  tracked_child_nodes.erase(position);
  new_child->as_child(position, this);
  assert(tracked_child_nodes[position] == new_child);
}

void InternalNode::track_split(
    const search_position_t& split_pos, Ref<InternalNode> right_node) {
  auto first = tracked_child_nodes.lower_bound(split_pos);
  auto iter = first;
  while (iter != tracked_child_nodes.end()) {
    search_position_t new_pos = iter->first;
    new_pos -= split_pos;
    iter->second->as_child<false>(new_pos, right_node);
    ++iter;
  }
  tracked_child_nodes.erase(first, tracked_child_nodes.end());
}

void InternalNode::validate_child(const Node& child) const {
#ifndef NDEBUG
  assert(impl->level() - 1 == child.impl->level());
  assert(this == child.parent_info().ptr);
  auto& child_pos = child.parent_info().position;
  assert(*impl->get_p_value(child_pos) == child.impl->laddr());
  if (child_pos.is_end()) {
    assert(impl->is_level_tail());
    assert(child.impl->is_level_tail());
  } else {
    assert(!child.impl->is_level_tail());
    assert(impl->get_key_view(child_pos) == child.impl->get_largest_key_view());
  }
  // XXX(multi-type)
  assert(impl->field_type() <= child.impl->field_type());
#endif
}

node_future<InternalNode::fresh_node_t> InternalNode::allocate(
    context_t c, field_type_t field_type, bool is_level_tail, level_t level) {
  return InternalNodeImpl::allocate(c, field_type, is_level_tail, level
  ).safe_then([](auto&& fresh_impl) {
    auto node = Ref<InternalNode>(new InternalNode(
          fresh_impl.impl.get(), std::move(fresh_impl.impl)));
    return fresh_node_t{node, fresh_impl.mut};
  });
}

/*
 * LeafNode
 */

LeafNode::LeafNode(LeafNodeImpl* impl, NodeImplURef&& impl_ref)
  : Node(std::move(impl_ref)), impl{impl} {}

const onode_t* LeafNode::get_p_value(const search_position_t& pos) const {
  return impl->get_p_value(pos);
}

node_future<Ref<tree_cursor_t>>
LeafNode::lookup_smallest(context_t) {
  search_position_t pos;
  const onode_t* p_value;
  if (unlikely(impl->is_empty())) {
    assert(is_root());
    pos = search_position_t::end();
    p_value = nullptr;
  } else {
    pos = search_position_t::begin();
    p_value = impl->get_p_value(pos);
  }
  return node_ertr::make_ready_future<Ref<tree_cursor_t>>(
      get_or_track_cursor(pos, p_value));
}

node_future<Ref<tree_cursor_t>>
LeafNode::lookup_largest(context_t) {
  search_position_t pos;
  const onode_t* p_value = nullptr;
  if (unlikely(impl->is_empty())) {
    assert(is_root());
    pos = search_position_t::end();
  } else {
    impl->get_largest_value(pos, p_value);
    assert(p_value != nullptr);
  }
  return node_ertr::make_ready_future<Ref<tree_cursor_t>>(
      get_or_track_cursor(pos, p_value));
}

node_future<Node::search_result_t>
LeafNode::lower_bound_tracked(
    context_t c, const key_hobj_t& key, MatchHistory& history) {
  auto result = impl->lower_bound(key, history);
  auto cursor_ref = get_or_track_cursor(result.position, result.p_value);
  return node_ertr::make_ready_future<search_result_t>(
      search_result_t{cursor_ref, result.match()});
}

node_future<> LeafNode::test_clone_root(
    context_t c_other, RootNodeTracker& tracker_other) const {
  assert(is_root());
  assert(impl->is_level_tail());
  assert(impl->field_type() == field_type_t::N0);
  Ref<const LeafNode> this_ref = this;
  return LeafNode::allocate(c_other, field_type_t::N0, true
  ).safe_then([this, c_other, &tracker_other](auto fresh_other) {
    impl->test_copy_to(fresh_other.mut);
    auto cloned_root = fresh_other.node;
    return c_other.nm.get_super(c_other.t, tracker_other
    ).safe_then([c_other, cloned_root](auto&& super_other) {
      cloned_root->make_root_new(c_other, std::move(super_other));
    });
  }).safe_then([this_ref]{});
}

node_future<Ref<tree_cursor_t>> LeafNode::insert_value(
    context_t c, const key_hobj_t& key, const onode_t& value,
    const search_position_t& pos, const MatchHistory& history) {
#ifndef NDEBUG
  if (pos.is_end()) {
    assert(impl->is_level_tail());
  }
#endif
  impl->prepare_mutate(c);

  search_position_t insert_pos = pos;
  auto [insert_stage, insert_size] = impl->evaluate_insert(
      key, value, history, insert_pos);
  auto free_size = impl->free_size();
  if (free_size >= insert_size) {
    // insert
    auto p_value = impl->insert(key, value, insert_pos, insert_stage, insert_size);
    assert(impl->free_size() == free_size - insert_size);
    assert(insert_pos <= pos);
    assert(p_value->size == value.size);
    auto ret = track_insert(insert_pos, insert_stage, p_value);
    validate_tracked_cursors();
    return node_ertr::make_ready_future<Ref<tree_cursor_t>>(ret);
  }
  // split and insert
  std::cout << "  try insert at: " << insert_pos
            << ", insert_stage=" << (int)insert_stage
            << ", insert_size=" << insert_size
            << std::endl;
  Ref<LeafNode> this_ref = this;
  return (is_root() ? upgrade_root(c) : node_ertr::now()
  ).safe_then([this, c] {
    return LeafNode::allocate(c, impl->field_type(), impl->is_level_tail());
  }).safe_then([this_ref, this, c, &key, &value, &history,
                insert_pos, insert_stage, insert_size](auto fresh_right) mutable {
    auto right_node = fresh_right.node;
    auto [split_pos, is_insert_left, p_value] = impl->split_insert(
        fresh_right.mut, *right_node->impl, key, value,
        insert_pos, insert_stage, insert_size);
    assert(p_value->size == value.size);
    track_split(split_pos, right_node);
    Ref<tree_cursor_t> ret;
    if (is_insert_left) {
      ret = track_insert(insert_pos, insert_stage, p_value);
    } else {
      ret = right_node->track_insert(insert_pos, insert_stage, p_value);
    }
    validate_tracked_cursors();
    right_node->validate_tracked_cursors();

    // propagate insert to parent
    return insert_parent(c, right_node).safe_then([ret] {
      return ret;
    });
    // TODO (optimize)
    // try to acquire space from siblings before split... see btrfs
  });
}

node_future<Ref<LeafNode>> LeafNode::allocate_root(
    context_t c, RootNodeTracker& root_tracker) {
  return LeafNode::allocate(c, field_type_t::N0, true
  ).safe_then([c, &root_tracker](auto fresh_node) {
    auto root = fresh_node.node;
    return c.nm.get_super(c.t, root_tracker
    ).safe_then([c, root](auto&& super) {
      root->make_root_new(c, std::move(super));
      return root;
    });
  });
}

Ref<tree_cursor_t> LeafNode::get_or_track_cursor(
    const search_position_t& position, const onode_t* p_value) {
  if (position.is_end()) {
    assert(impl->is_level_tail());
    assert(!p_value);
    // we need to return the leaf node to insert
    return new tree_cursor_t(this, position, p_value);
  }

  Ref<tree_cursor_t> p_cursor;
  auto found = tracked_cursors.find(position);
  if (found == tracked_cursors.end()) {
    p_cursor = new tree_cursor_t(this, position, p_value);
  } else {
    p_cursor = found->second;
    assert(p_cursor->get_leaf_node() == this);
    assert(p_cursor->get_position() == position);
    p_cursor->set_p_value(p_value);
  }
  return p_cursor;
}

Ref<tree_cursor_t> LeafNode::track_insert(
    const search_position_t& insert_pos, match_stage_t insert_stage,
    const onode_t* p_onode) {
  // invalidate cursor value
  // TODO: version based invalidation
  auto pos_invalidate_begin = insert_pos;
  pos_invalidate_begin.index_by_stage(STAGE_RIGHT) = 0;
  auto begin_invalidate = tracked_cursors.lower_bound(pos_invalidate_begin);
  std::for_each(begin_invalidate, tracked_cursors.end(), [](auto& kv) {
    kv.second->invalidate_p_value();
  });

  // update cursor position
  auto pos_upper_bound = insert_pos;
  pos_upper_bound.index_by_stage(insert_stage) = INDEX_END;
  auto first = tracked_cursors.lower_bound(insert_pos);
  auto last = tracked_cursors.lower_bound(pos_upper_bound);
  std::vector<tree_cursor_t*> p_cursors;
  std::for_each(first, last, [&p_cursors](auto& kv) {
    p_cursors.push_back(kv.second);
  });
  tracked_cursors.erase(first, last);
  for (auto& p_cursor : p_cursors) {
    search_position_t new_pos = p_cursor->get_position();
    ++new_pos.index_by_stage(insert_stage);
    p_cursor->update_track(this, new_pos);
  }

  // track insert
  return new tree_cursor_t(this, insert_pos, p_onode);
}

void LeafNode::track_split(
    const search_position_t& split_pos, Ref<LeafNode> right_node) {
  // invalidate cursor value
  // TODO: version based invalidation
  auto pos_invalidate_begin = split_pos;
  pos_invalidate_begin.index_by_stage(STAGE_RIGHT) = 0;
  auto begin_invalidate = tracked_cursors.lower_bound(pos_invalidate_begin);
  std::for_each(begin_invalidate, tracked_cursors.end(), [](auto& kv) {
    kv.second->invalidate_p_value();
  });

  // update cursor ownership and position
  auto first = tracked_cursors.lower_bound(split_pos);
  auto iter = first;
  while (iter != tracked_cursors.end()) {
    search_position_t new_pos = iter->first;
    new_pos -= split_pos;
    iter->second->update_track(right_node, new_pos);
    ++iter;
  }
  tracked_cursors.erase(first, tracked_cursors.end());
}

node_future<LeafNode::fresh_node_t> LeafNode::allocate(
    context_t c, field_type_t field_type, bool is_level_tail) {
  return LeafNodeImpl::allocate(c, field_type, is_level_tail
  ).safe_then([](auto&& fresh_impl) {
    auto node = Ref<LeafNode>(new LeafNode(
          fresh_impl.impl.get(), std::move(fresh_impl.impl)));
    return fresh_node_t{node, fresh_impl.mut};
  });
}

}
