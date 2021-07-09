#include <torch/csrc/jit/passes/concat_opt.h>

#include <algorithm>
#include <unordered_set>
#include <vector>

#include <torch/csrc/jit/ir/alias_analysis.h>
#include <torch/csrc/jit/ir/ir.h>
#include <torch/csrc/jit/ir/named_value.h>
#include <torch/csrc/jit/jit_log.h>
#include <torch/csrc/jit/passes/constant_pooling.h>
#include <torch/csrc/jit/passes/dead_code_elimination.h>
#include <torch/csrc/jit/passes/remove_mutation.h>

namespace torch {
namespace jit {

namespace {

void removeCatNodeFromGraph(Node* n) {
  TORCH_INTERNAL_ASSERT(n->kind() == aten::cat);
  auto inp_list = n->input(0);
  GRAPH_UPDATE("Deleting\n", *n);
  n->destroy();
  if (!inp_list->hasUses()) {
    GRAPH_UPDATE("Deleting\n", *inp_list->node());
    inp_list->node()->destroy();
  }
}

bool equal(at::ArrayRef<Value*> list1, at::ArrayRef<Value*> list2) {
  return list1.size() == list2.size() &&
      std::equal(list1.begin(), list1.end(), list2.begin());
}

class ConcatCommonInputsEliminator {
 public:
  explicit ConcatCommonInputsEliminator(std::shared_ptr<Graph> graph)
      : graph_(std::move(graph)) {}

  bool run() {
    handleBlock(graph_->block());
    return postprocess();
  }

 private:
  void handleBlock(Block* block) {
    for (auto node : block->nodes()) {
      if (node->kind() == prim::Concat) {
        handleCat(node);
      }
      for (Block* block : node->blocks()) {
        handleBlock(block);
      }
    }
  }

  void handleCat(Node* node) {
    GRAPH_DEBUG("Considering cat node for CSE opt: ", node);

    auto curr_all_inputs = node->inputs();
    auto curr_tensor_inputs =
        curr_all_inputs.slice(0, curr_all_inputs.size() - 1);
    auto curr_dim = curr_all_inputs.back();

    // Save the input list and the current cat node, so that this can be
    // used for subsequent cat nodes, unless there are writes to this cat
    // node. When there are writes to this cat node, its output does not
    // represent this concatenated list beyond the writes. Currently, we do
    // not perform such fine-grained analysis. So, if there are any writes to
    // the output, we do not use this cat node for optimization here.
    if (!getOrCreateAliasDb()->hasWriters(node->output())) {
      concated_outputs_.insert(node);
    }

    if (curr_tensor_inputs.size() <= 2) {
      // The case when concat has 2 input tensors could only be optimized if
      // there is another concat of the exact same 2 input tensors. That case
      // is expected to be handled by the CSE pass.
      return;
    }

    // Now, we check if the first N-1 elements in %inputs appeared in any of
    // the previous cat ops.
    //
    // Example:
    //    %11 = prim::Concat(%0, %1, <dim>)
    //    ...
    //    %13 = prim::Concat(%0, %1, %2, <dim>) // first 2 inputs same as %11
    //    ...
    //        = %13 ... // Use %13
    //
    // After CSE opt:
    //    %11 = prim::Concat(%0, %1, <dim>)
    //    ...
    //    %14 = prim::Concat(%11, %2, <dim>) // Replace first 2 inputs with %11
    //    ...
    //        = %14 ... // Replace use of %13 with %14

    auto curr_tensor_inputs_prefix =
        curr_tensor_inputs.slice(0, curr_tensor_inputs.size() - 1);
    for (const auto& prev : concated_outputs_) {
      auto prev_all_inputs = prev->inputs();
      auto prev_tensor_inputs =
          prev_all_inputs.slice(0, prev_all_inputs.size() - 1);
      auto prev_dim = prev_all_inputs.back();
      if (equal(curr_tensor_inputs_prefix, prev_tensor_inputs) &&
          curr_dim == prev_dim) {
        if (!node->isDominatedBy(prev)) {
          // We can't use the previous concatenated output if it does not
          // dominate the current concat node.
          continue;
        }

        std::vector<Value*> new_inputs = {
            prev->output(), curr_tensor_inputs.back(), curr_dim};
        auto new_concat = node->owningGraph()->create(prim::Concat, new_inputs);
        new_concat->output()->setType(node->output()->type());
        concats_to_replace_[node] = new_concat;
        return;
      }
    }

    // Now, we check if the last N-1 elements in %inputs appeared in any of
    // the previous cat ops.
    //
    // Example:
    //    %10 = prim::ListConstruct(%1, %2)
    //    %11 = aten::cat(%10, ...)
    //    ...
    //    %12 = prim::ListConstruct(%0, %1, %2)  // last 2 inputs same as %11
    //    %13 = aten::cat(%12, ...)
    //    ...
    //        = %13 ... // Use %13
    //
    // After CSE opt:
    //    %10 = prim::ListConstruct(%0, %1)
    //    %11 = aten::cat(%10, ...)
    //    ...
    //    %12 = prim::ListConstruct(%0, %11) // Replace last 2 inputs with %11
    //    %13 = aten::cat(%12, ...)
    //    ...
    //        = %13 ... // Use %13
    auto curr_tensor_inputs_suffix =
        curr_tensor_inputs.slice(1, curr_tensor_inputs.size() - 1);
    for (const auto& prev : concated_outputs_) {
      auto prev_all_inputs = prev->inputs();
      auto prev_tensor_inputs =
          prev_all_inputs.slice(0, prev_all_inputs.size() - 1);
      auto prev_dim = prev_all_inputs.back();
      if (equal(curr_tensor_inputs_suffix, prev_tensor_inputs) &&
          curr_dim == prev_dim) {
        if (!node->isDominatedBy(prev)) {
          // We can't use the previous concatenated list if it does not
          // dominate the current list.
          continue;
        }

        std::vector<Value*> new_inputs = {
            curr_tensor_inputs.front(), prev->output(), curr_dim};
        auto new_concat = node->owningGraph()->create(prim::Concat, new_inputs);
        new_concat->output()->setType(node->output()->type());
        concats_to_replace_[node] = new_concat;
        return;
      }
    }

    // Do we need to handle other cases where N-2 or lesser elements from
    // %inputs appear in any of the previous cat ops?
    // TODO.
  }

  bool postprocess() {
    // Replace the list nodes that have been marked.
    bool changed = false;
    for (auto it : concats_to_replace_) {
      auto curr_node = it.first;
      auto new_node = it.second;
      GRAPH_UPDATE("Inserting\n", *new_node, "before\n", *curr_node);
      new_node->insertBefore(curr_node);
      GRAPH_UPDATE("Replacing uses of\n", *curr_node, "with\n", *new_node);
      curr_node->output()->replaceAllUsesWith(new_node->output());
      GRAPH_UPDATE("Deleting\n", *curr_node);
      curr_node->destroy();
      changed = true;
    }
    return changed;
  }

  AliasDb* getOrCreateAliasDb() {
    if (!aliasDb_) {
      aliasDb_ = std::make_unique<AliasDb>(graph_);
    }
    return aliasDb_.get();
  }

  std::shared_ptr<Graph> graph_;
  std::unique_ptr<AliasDb> aliasDb_ = nullptr;

  std::unordered_set<Node*> concated_outputs_;
  std::unordered_map<Node*, Node*> concats_to_replace_;
};

} // namespace

bool EliminateConcatCommonInputs(const std::shared_ptr<Graph>& graph) {
  GRAPH_DUMP("Before eliminating Concat common inputs", graph);
  bool changed = ConcatCommonInputsEliminator(graph).run();
  if (changed) {
    GRAPH_DUMP("After eliminating Concat common inputs", graph);
  }
  return changed;
}

namespace {

class ConcatExpander {
 public:
  explicit ConcatExpander(std::shared_ptr<Graph> graph)
      : graph_(std::move(graph)) {}

  void run() {
    handleBlock(graph_->block());
    cleanupExpandedCatOps();
    GRAPH_DUMP("Before reusing copy buffers: ", graph_);
    reuseBuffersInCopies();
  }

 private:
  void handleBlock(Block* block) {
    for (auto node : block->nodes()) {
      if (node->kind() == aten::cat) {
        expandCat(node);
      }
      for (Block* block : node->blocks()) {
        handleBlock(block);
      }
    }
  }

  // Expand cat node into multiple copy nodes.
  //
  // Example:
  //     %2 = aten::clamp(%0, ...)
  //     %3 = aten::clamp(%1, ...)
  //     %10 = prim::ListConstruct(%2, %3)
  //     %11 = aten::cat(%10, ...)
  //     ...
  //         = %11 ... // Use %11
  //
  // After expanding cat:
  //     %2 = aten::clamp(%0, ...)
  //     %3 = aten::clamp(%1, ...)
  //     %20 = aten::empty(...)          // cat output buffer
  //     %21 = aten::slice(%20, ...)     // slice for %2
  //     %22 = aten::copy_(%21, %2)      // copy %2
  //     %23 = aten::slice(%20, ...)     // slice for %3
  //     %24 = aten::copy_(%23, %3)      // copy %3
  //     ...
  //         = %20 ... // Use %20 in place of %11
  void expandCat(Node* node) {
    GRAPH_DEBUG("Considering cat node for expansion: ", node);
    // Do not optimize cat nodes whose inputs are mutated in the graph.
    // TODO: Improve this by checking if it is mutated in the graph region
    // where this optimization is applied.
    if (getOrCreateAliasDb()->hasWriters(node->input(0))) {
      return;
    }
    if (node->input(0)->node()->kind() != prim::ListConstruct) {
      // Unknown form of input to `cat` op.
      return;
    }
    if (!allShapesAreKnown(node)) {
      // Can't expand when shapes are not known for the `cat` op.
      return;
    }
    for (auto cat_inp : node->input(0)->node()->inputs()) {
      if (!shapeIsKnown(cat_inp)) {
        // Can't expand when shapes of the inputs to `cat` are not known.
        return;
      }
    }
    // TODO: Handle non-contiguous Tensors.
    // For example, how to handle the cases where the inputs are all channels
    // last?

    auto maybe_cat_dim = constant_as<int64_t>(node->input(1));
    if (!maybe_cat_dim) {
      // Can't expand when cat dimension is not a constant.
      return;
    }
    auto cat_dim_value = maybe_cat_dim.value();
    auto cat_dim = node->input(1);

    // Set the insertion point to the curent `cat` node.
    WithInsertPoint guard(node);
    auto none = graph_->insertConstant(IValue());
    auto one = graph_->insertConstant(1);

    // Insert the constants needed for the `cat` output buffer size.
    auto tensortype = node->output()->type()->expect<TensorType>();
    TORCH_INTERNAL_ASSERT(tensortype);
    auto tensortype_sizes = tensortype->sizes();
    std::vector<Value*> cat_out_size;
    for (size_t i = 0; i < tensortype_sizes.size(); ++i) {
      cat_out_size.push_back(graph_->insertConstant(tensortype_sizes[i]));
    }

    // Create a list of int for `cat` output buffer size.
    auto cat_out_size_list = graph_->createList(IntType::get(), cat_out_size);
    cat_out_size_list->insertBefore(node);

    // Create an empty buffer to be used as `cat` output buffer.
    // TODO: Handle tensors with different dtype, layout, device, memory
    // format, etc.
    auto cat_out_empty = graph_->create(
        aten::empty,
        {cat_out_size_list->output(), none, none, none, none, none});
    cat_out_empty->insertBefore(node);

    // For every input to this `cat` node:
    //   * Create a slice of `cat` output buffer.
    auto cat_out_value = cat_out_empty->output();
    auto cat_inp_list = node->input(0)->node();
    int start_idx = 0;
    auto start = graph_->insertConstant(start_idx);
    for (auto cat_inp : cat_inp_list->inputs()) {
      // Create a slice of the cat output buffer that correspond to
      // this input size and position in the output.
      auto cat_inp_tensor_type =
          dynamic_cast<TensorType*>(cat_inp->type().get());
      TORCH_INTERNAL_ASSERT(cat_inp_tensor_type);
      TORCH_INTERNAL_ASSERT(cat_inp_tensor_type->dim());
      auto cat_inp_tensortype_sizes = cat_inp_tensor_type->sizes();
      // NOLINTNEXTLINE(bugprone-narrowing-conversions,cppcoreguidelines-narrowing-conversions)
      int end_idx = start_idx + *cat_inp_tensortype_sizes[cat_dim_value];
      auto end = graph_->insertConstant(end_idx);

      auto slice = graph_->create(
          aten::slice, {cat_out_value, cat_dim, start, end, one});
      GRAPH_UPDATE("Inserting\n", *slice, "before\n", *node);
      slice->insertBefore(node);
      slices_added_.push_back(slice);

      // Insert a copy from this input to the output slice.
      auto copy = graph_->create(aten::copy_, {slice->output(), cat_inp});
      GRAPH_UPDATE("Inserting\n", *copy, "before\n", *node);
      copy->insertBefore(node);
      copies_added_.push_back(copy);

      start_idx = end_idx;
      start = end;
    }

    // Replace the uses of `cat` node with the cat output buffer.
    replace_uses_with_[node->output()] = cat_out_value;
    nodes_to_remove_.insert(node);
  }

  bool shapeIsKnown(Value* v) {
    if (v->type()->cast<TensorType>()) {
      if (!v->isCompleteTensor()) {
        return false;
      }
      if (*v->type()->castRaw<TensorType>()->dim() == 0) {
        return false;
      }
    }
    return true;
  }
  bool allShapesAreKnown(Node* node) {
    // TODO: Relax the checks to support dynamic shapes
    for (Value* input : node->inputs()) {
      if (!shapeIsKnown(input)) {
        return false;
      }
    }
    for (Value* output : node->outputs()) {
      if (!shapeIsKnown(output)) {
        return false;
      }
    }
    return true;
  }

  void cleanupExpandedCatOps() {
    for (auto it : replace_uses_with_) {
      GRAPH_UPDATE(
          "Replacing uses of\n",
          *it.first->node(),
          "with\n",
          *it.second->node());
      it.first->replaceAllUsesWith(it.second);
    }
    for (auto n : nodes_to_remove_) {
      removeCatNodeFromGraph(n);
    }
  }

  void moveBefore(Node* node, Node* before) {
    // In order to move a node before another node, we need to move
    // all the nodes it depends on as well.
    for (auto inp : node->inputs()) {
      moveBefore(inp->node(), before);
    }
    node->moveBefore(before);
  }

  // Reuse buffers in copies wherever possible.
  //
  // For example, consider the following sequence of ops:
  //     %10 = prim::ListConstruct(%0, %1)
  //     %11 = aten::cat(%10, ...)
  //     ...
  //     %12 = prim::ListConstruct(%11, %2)  // Uses the result of above cat
  //     %13 = aten::cat(%12, ...)
  //
  // Once these cat ops are expanded into copies, we will have two buffers; one
  // for %11 and another for %13. This can be optimized by using only one
  // buffer. We can only have the buffer that represents %13 and use a view
  // (slice) of that one as the buffer for %11.
  //
  // If any of the copies added earlier has `aten::empty` as its source,
  // those cases can be replaced with a single buffer.
  //
  // Example:
  //     %20 = aten::empty(...)          // cat.1 output buffer
  //     %21 = aten::slice(%20, ...)
  //     %22 = aten::copy_(%21, %2)
  //     %23 = aten::slice(%20, ...)
  //     %24 = aten::copy_(%23, %3)
  //     ...
  //     %30 = aten::empty(...)          // cat.2 output buffer
  //     %31 = aten::slice(%30, ...)
  //     %32 = aten::copy_(%31, %20)     // src of copy is aten::empty
  //                                     // so, we reuse this buffer above
  //     %33 = aten::slice(%30, ...)
  //     %34 = aten::copy_(%33, %4)
  //
  // After reusing copy buffers:
  //     %30 = aten::empty(...)          // cat.2 output buffer
  //     %31 = aten::slice(%30, ...)     // move %31 and inputs before %20
  //     %21 = aten::slice(%31, ...)     // use %31 in place of %20
  //     %22 = aten::copy_(%21, %2)
  //     %23 = aten::slice(%31, ...)     // use %31 in place of %20
  //     %24 = aten::copy_(%23, %3)
  //     ...
  //     ...                             // copy to %31 is now removed
  //     %33 = aten::slice(%30, ...)
  //     %34 = aten::copy_(%33, %4)
  void reuseBuffersInCopies() {
    for (auto copy : copies_added_) {
      auto src = copy->input(1);
      auto dst = copy->input(0);
      if (src->node()->kind() != aten::empty) {
        continue;
      }

      // Move the destination node before the source.
      GRAPH_UPDATE("Moving\n", *dst->node(), "before\n", *src->node());
      moveBefore(dst->node(), src->node());

      GRAPH_UPDATE("Replacing\n", *src->node(), "with\n", *dst->node());
      src->replaceAllUsesWith(dst);

      GRAPH_UPDATE("Deleting\n", *src->node());
      src->node()->destroy();

      GRAPH_UPDATE("Deleting\n", *copy);
      copy->destroy();
    }
  }

  AliasDb* getOrCreateAliasDb() {
    if (!aliasDb_) {
      aliasDb_ = std::make_unique<AliasDb>(graph_);
    }
    return aliasDb_.get();
  }

  std::shared_ptr<Graph> graph_;
  std::unique_ptr<AliasDb> aliasDb_ = nullptr;

  std::unordered_set<Node*> nodes_to_remove_;
  std::unordered_map<Value*, Value*> replace_uses_with_;
  std::vector<Node*> copies_added_;
  std::vector<Node*> slices_added_;
};

} // namespace

void ExpandConcatAndEliminateRedundancy(const std::shared_ptr<Graph>& graph) {
  ConcatExpander(graph).run();
  GRAPH_DUMP("After expanding Concat and eliminating redundancy", graph);
}

namespace {

class VariadicCatUpdater {
 public:
  explicit VariadicCatUpdater(std::shared_ptr<Graph> graph)
      : graph_(std::move(graph)) {}

  bool run() {
    collectCatNodes(graph_->block());
    bool changed = false;
    for (auto c : cat_nodes_) {
      changed = changed || replaceWithVariadicCat(c);
    }
    return changed;
  }

 private:
  void collectCatNodes(Block* block) {
    for (auto node : block->nodes()) {
      if (node->kind() == aten::cat) {
        cat_nodes_.push_back(node);
      }
      for (Block* b : node->blocks()) {
        collectCatNodes(b);
      }
    }
  }

  bool replaceWithVariadicCat(Node* cat) {
    if (cat->input(0)->node()->kind() != prim::ListConstruct) {
      return false;
    }
    auto list = cat->input(0)->node();
    // We do not transform cat ops whose list input can not be moved to the
    // position before cat. This in turn implies that there is some mutation
    // of the input list before cat.
    if (!getOrCreateAliasDb()->couldMoveBeforeTopologically(list, cat)) {
      return false;
    }
    std::vector<Value*> inputs = list->inputs().vec();
    inputs.push_back(cat->input(1));
    auto var_cat = cat->owningGraph()->create(prim::Concat, inputs);
    GRAPH_UPDATE("Adding\n", *var_cat);
    var_cat->insertBefore(cat);
    GRAPH_UPDATE("Replacing\n", *cat, "with\n", *var_cat);
    cat->output()->replaceAllUsesWith(var_cat->output());
    GRAPH_UPDATE("Deleting\n", *cat);
    cat->destroy();
    if (!list->hasUses()) {
      GRAPH_UPDATE("Deleting\n", *list);
      list->destroy();
    }
    return true;
  }

  AliasDb* getOrCreateAliasDb() {
    if (!aliasDb_) {
      aliasDb_ = std::make_unique<AliasDb>(graph_);
    }
    return aliasDb_.get();
  }

  std::shared_ptr<Graph> graph_;
  std::unique_ptr<AliasDb> aliasDb_ = nullptr;

  std::vector<Node*> cat_nodes_;
};

} // namespace

bool UseVariadicCat(const std::shared_ptr<Graph>& graph) {
  GRAPH_DUMP("Before VariadicCat", graph);
  bool changed = VariadicCatUpdater(graph).run();
  if (changed) {
    GRAPH_DUMP("After VariadicCat", graph);
  }
  return changed;
}

bool RemoveListMutationAndUseVariadicCat(const std::shared_ptr<Graph>& graph) {
  bool changed_in_last_iter = true;
  bool changed = false;
  while (changed_in_last_iter) {
    changed_in_last_iter = RemoveListMutation(graph);
    changed_in_last_iter = changed_in_last_iter || UseVariadicCat(graph);
    changed = changed || changed_in_last_iter;
  }
  return changed;
}

} // namespace jit
} // namespace torch
