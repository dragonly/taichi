#include "taichi/ir/ir.h"
#include "taichi/ir/statements.h"
#include "taichi/ir/transforms.h"
#include "taichi/ir/analysis.h"
#include "taichi/ir/visitors.h"
#include "taichi/program/program.h"

#include <set>
#include <unordered_map>
#include <utility>

TLANG_NAMESPACE_BEGIN

namespace irpass {
namespace {

class SquashPtrOffset : public IRVisitor {
 public:
  SquashPtrOffset() {
    allow_undefined_visitor = true;
    invoke_default_visitor = true;
  }
  void visit(Stmt *stmt) override {
    top_level_ptr = stmt;
  }
  void visit(PtrOffsetStmt *stmt) override {
    stmt->origin->accept(this);
  }
  static Stmt *run(Stmt *root) {
    SquashPtrOffset v;
    root->accept(&v);
    return v.top_level_ptr;
  }

 private:
  Stmt *top_level_ptr = nullptr;
};

// Offloaded local variables to its offset in the global tmps memory.
using StmtToOffsetMap = std::unordered_map<const Stmt *, std::size_t>;

struct OffloadedRanges {
  using Map = std::unordered_map<const OffloadedStmt *, Stmt *>;
  Map begin_stmts;
  Map end_stmts;
};

// Break kernel into multiple parts and emit struct for listgens
// For GPU backends this pass also determines the grid dim and block dims
class Offloader {
 public:
  static OffloadedRanges run(IRNode *root, const CompileConfig &config) {
    OffloadedRanges offloaded_ranges;

    auto root_block = dynamic_cast<Block *>(root);
    auto root_statements = std::move(root_block->statements);
    root_block->statements.clear();
    const auto arch = config.arch;
    auto pending_serial_statements =
        Stmt::make_typed<OffloadedStmt>(OffloadedStmt::TaskType::serial, arch);
    pending_serial_statements->grid_dim = 1;
    pending_serial_statements->block_dim = 1;

    auto assemble_serial_statements = [&]() {
      if (!pending_serial_statements->body->statements.empty()) {
        root_block->insert(std::move(pending_serial_statements));
        pending_serial_statements = Stmt::make_typed<OffloadedStmt>(
            OffloadedStmt::TaskType::serial, arch);
        pending_serial_statements->grid_dim = 1;
        pending_serial_statements->block_dim = 1;
      }
    };

    for (int i = 0; i < (int)root_statements.size(); i++) {
      auto &stmt = root_statements[i];
      // Note that stmt->parent is root_block, which doesn't contain stmt now.
      if (auto s = stmt->cast<RangeForStmt>(); s && !s->strictly_serialized) {
        assemble_serial_statements();
        auto offloaded = Stmt::make_typed<OffloadedStmt>(
            OffloadedStmt::TaskType::range_for, arch);
        // offloaded->body is an empty block now.
        offloaded->grid_dim = config.saturating_grid_dim;
        if (s->block_dim == 0) {
          offloaded->block_dim = Program::default_block_dim(config);
        } else {
          offloaded->block_dim = s->block_dim;
        }
        if (auto val = s->begin->cast<ConstStmt>()) {
          offloaded->const_begin = true;
          offloaded->begin_value = val->val[0].val_int32();
        } else {
          offloaded_ranges.begin_stmts.insert(
              std::make_pair(offloaded.get(), s->begin));
        }
        if (auto val = s->end->cast<ConstStmt>()) {
          offloaded->const_end = true;
          offloaded->end_value = val->val[0].val_int32();
        } else {
          offloaded_ranges.end_stmts.insert(
              std::make_pair(offloaded.get(), s->end));
        }
        offloaded->num_cpu_threads =
            std::min(s->num_cpu_threads, config.cpu_max_num_threads);
        replace_all_usages_with(s, s, offloaded.get());
        for (int j = 0; j < (int)s->body->statements.size(); j++) {
          offloaded->body->insert(std::move(s->body->statements[j]));
        }
        root_block->insert(std::move(offloaded));
      } else if (auto st = stmt->cast<StructForStmt>()) {
        assemble_serial_statements();
        emit_struct_for(st, root_block, config, st->mem_access_opt);
      } else {
        pending_serial_statements->body->insert(std::move(stmt));
      }
    }
    assemble_serial_statements();
    return offloaded_ranges;
  }

 private:
  static void emit_struct_for(StructForStmt *for_stmt,
                              Block *root_block,
                              const CompileConfig &config,
                              const MemoryAccessOptions &mem_access_opt) {
    auto leaf = for_stmt->snode;
    // make a list of nodes, from the leaf block (instead of 'place') to root
    std::vector<SNode *> path;
    // leaf is the place (scalar)
    // leaf->parent is the leaf block
    // so listgen should be invoked from the root to leaf->parent
    for (auto p = leaf; p; p = p->parent) {
      path.push_back(p);
    }
    std::reverse(path.begin(), path.end());

    // If |demotable| is true, this will later be demoting into a range-for
    // task, so we don't need to generate clear/listgen tasks.
    const bool demotable =
        (leaf->is_path_all_dense && config.demote_dense_struct_fors);
    const auto arch = config.arch;
    if (!demotable) {
      for (int i = 1; i < path.size(); i++) {
        auto snode_child = path[i];
        if ((snode_child->type == SNodeType::bit_array ||
             snode_child->type == SNodeType::bit_struct) &&
            i == path.size() - 1) {
          continue;
        }
        auto offloaded_clear_list = Stmt::make_typed<OffloadedStmt>(
            OffloadedStmt::TaskType::serial, arch);
        offloaded_clear_list->body->insert(
            Stmt::make<ClearListStmt>(snode_child));
        offloaded_clear_list->grid_dim = 1;
        offloaded_clear_list->block_dim = 1;
        // Intentionally do not set offloaded_clear_list->snode, so that there
        // is nothing special about this task, which could otherwise cause
        // problems when fused with other serial tasks.
        root_block->insert(std::move(offloaded_clear_list));
        auto offloaded_listgen = Stmt::make_typed<OffloadedStmt>(
            OffloadedStmt::TaskType::listgen, arch);
        offloaded_listgen->snode = snode_child;
        offloaded_listgen->grid_dim = config.saturating_grid_dim;
        offloaded_listgen->block_dim =
            std::min(snode_child->max_num_elements(),
                     (int64)std::min(Program::default_block_dim(config),
                                     config.max_block_dim));
        root_block->insert(std::move(offloaded_listgen));
      }
    }

    auto offloaded_struct_for = Stmt::make_typed<OffloadedStmt>(
        OffloadedStmt::TaskType::struct_for, arch);

    offloaded_struct_for->index_offsets = for_stmt->index_offsets;

    offloaded_struct_for->grid_dim = config.saturating_grid_dim;

    const auto snode_num_elements = for_stmt->snode->max_num_elements();
    if (for_stmt->block_dim == 0) {
      // adaptive
      offloaded_struct_for->block_dim =
          std::min(snode_num_elements, (int64)config.default_gpu_block_dim);
    } else {
      if (for_stmt->block_dim > snode_num_elements) {
        TI_WARN(
            "Specified block dim {} is bigger than SNode element size {}. "
            "Clipping.",
            for_stmt->block_dim, snode_num_elements);
        offloaded_struct_for->block_dim = snode_num_elements;
      } else {
        offloaded_struct_for->block_dim = for_stmt->block_dim;
      }
    }

    replace_all_usages_with(for_stmt, for_stmt, offloaded_struct_for.get());

    for (int i = 0; i < (int)for_stmt->body->statements.size(); i++) {
      offloaded_struct_for->body->insert(
          std::move(for_stmt->body->statements[i]));
    }

    offloaded_struct_for->snode = for_stmt->snode;
    offloaded_struct_for->num_cpu_threads =
        std::min(for_stmt->num_cpu_threads, config.cpu_max_num_threads);
    offloaded_struct_for->mem_access_opt = mem_access_opt;

    root_block->insert(std::move(offloaded_struct_for));
  }
};

// Build a mapping from all statements to its containing OffloadedStmt
class StmtToOffloaded : public BasicStmtVisitor {
 private:
  StmtToOffloaded() {
    allow_undefined_visitor = true;
    invoke_default_visitor = true;
    current_offloaded = nullptr;
  }

 public:
  void visit(OffloadedStmt *stmt) override {
    current_offloaded = stmt;
    stmt_to_offloaded[stmt] = current_offloaded;
    if (stmt->body)
      stmt->body->accept(this);
    current_offloaded = nullptr;
  }

  void visit(Stmt *stmt) override {
    if (current_offloaded != nullptr) {
      // inside a offloaded stmt, record its belonging offloaded_stmt
      stmt_to_offloaded[stmt] = current_offloaded;
    }
  }

  void preprocess_container_stmt(Stmt *stmt) override {
    if (current_offloaded != nullptr) {
      // inside a offloaded stmt, record its belonging offloaded_stmt
      stmt_to_offloaded[stmt] = current_offloaded;
    }
  }

 public:
  static std::unordered_map<Stmt *, Stmt *> run(IRNode *ir) {
    StmtToOffloaded pass;
    ir->accept(&pass);
    return pass.stmt_to_offloaded;
  }

 private:
  using BasicStmtVisitor::visit;

  // Local variables to its containing offloaded statement
  std::unordered_map<Stmt *, Stmt *> stmt_to_offloaded;

  Stmt *current_offloaded;
};

/*
After offloading, some local variables/instructions are accessed across
offloaded blocks. This pass promote these local values into global variables.

Steps:
  1. IdentifyValuesUsedInOtherOffloads
  2. PromoteIntermediateToGlobalTmp
  3. FixCrossOffloadReferences
*/

// Traverse offloaded blocks to identify out-of-offload local LD/ST and
// statement references
class IdentifyValuesUsedInOtherOffloads : public BasicStmtVisitor {
  using BasicStmtVisitor::visit;

 private:
  IdentifyValuesUsedInOtherOffloads(
      const CompileConfig &config,
      const std::unordered_map<Stmt *, Stmt *> &stmt_to_offloaded,
      OffloadedRanges *offloaded_ranges)
      : config(config),
        stmt_to_offloaded(stmt_to_offloaded),
        offloaded_ranges_(offloaded_ranges) {
    allow_undefined_visitor = true;
    invoke_default_visitor = true;
    current_offloaded = nullptr;
    global_offset = 0;
  }

  std::size_t allocate_global(DataType type) {
    TI_ASSERT(type->vector_width() == 1 || type->is<TensorType>());
    auto ret = global_offset;
    if (type->is<TensorType>()) {
      auto tensor_type = type->cast<TensorType>();
      global_offset += tensor_type->get_num_elements() *
                       data_type_size(tensor_type->get_element_type());
    } else {
      std::size_t type_size = data_type_size(type);
      // align global_offset to a multiple of type_size
      global_offset = ((global_offset + type_size - 1) / type_size) * type_size;
      ret = global_offset;
      global_offset += type_size;
    }
    TI_ASSERT(global_offset < taichi_global_tmp_buffer_size);
    return ret;
  }

 public:
  void visit(OffloadedStmt *stmt) override {
    current_offloaded = stmt;
    if (auto begin = offloaded_ranges_->begin_stmts.find(stmt);
        begin != offloaded_ranges_->begin_stmts.end()) {
      test_and_allocate(begin->second);
    }
    if (auto end = offloaded_ranges_->end_stmts.find(stmt);
        end != offloaded_ranges_->end_stmts.end()) {
      test_and_allocate(end->second);
    }
    if (stmt->body)
      stmt->body->accept(this);
    current_offloaded = nullptr;
  }

  void visit(AllocaStmt *stmt) override {
    TI_ASSERT(current_offloaded);
  }

  void test_and_allocate(Stmt *stmt) {
    if (stmt == nullptr)
      return;
    if (stmt_to_offloaded[stmt] == current_offloaded)
      return;
    // Directly insert copies of ConstStmts later
    if (stmt->is<ConstStmt>())
      return;
    auto top_level_ptr = SquashPtrOffset::run(stmt);
    // We don't support storing a pointer for now.
    if (top_level_ptr->is<GlobalPtrStmt>())
      return;
    // Not yet allocated
    if (local_to_global.find(top_level_ptr) == local_to_global.end()) {
      local_to_global[top_level_ptr] = allocate_global(top_level_ptr->ret_type);
    }
  }

  void visit(Stmt *stmt) override {
    int n_op = stmt->num_operands();
    for (int i = 0; i < n_op; i++) {
      auto op = stmt->operand(i);
      test_and_allocate(op);
    }
  }

  static StmtToOffsetMap run(
      IRNode *root,
      const CompileConfig &config,
      const std::unordered_map<Stmt *, Stmt *> &stmt_to_offloaded,
      OffloadedRanges *offloaded_ranges) {
    IdentifyValuesUsedInOtherOffloads pass(config, stmt_to_offloaded,
                                           offloaded_ranges);
    root->accept(&pass);
    return pass.local_to_global;
  }

 private:
  CompileConfig config;
  std::unordered_map<Stmt *, Stmt *> stmt_to_offloaded;
  OffloadedRanges *const offloaded_ranges_;
  // Local variables to global temporary offsets (in bytes)
  StmtToOffsetMap local_to_global;
  Stmt *current_offloaded;
  std::size_t global_offset;
};

// Store intermediate values to globals so that statements in later offloaded
// statement can load
class PromoteIntermediateToGlobalTmp : public BasicStmtVisitor {
  using BasicStmtVisitor::visit;

 private:
  explicit PromoteIntermediateToGlobalTmp(
      const StmtToOffsetMap &local_to_global_offset)
      : local_to_global_offset(local_to_global_offset) {
    allow_undefined_visitor = true;
    invoke_default_visitor = true;
  }

 public:
  void visit(Stmt *stmt) override {
    if (!stmt->is<AllocaStmt>() &&
        local_to_global_offset.find(stmt) != local_to_global_offset.end() &&
        stored_to_global.find(stmt) == stored_to_global.end()) {
      stored_to_global.insert(stmt);
      auto offset = local_to_global_offset[stmt];
      auto ptr = stmt->insert_after_me(
          Stmt::make<GlobalTemporaryStmt>(offset, stmt->ret_type));
      ptr->insert_after_me(Stmt::make<GlobalStoreStmt>(ptr, stmt));
      throw IRModified();
    }
  }

  static void run(IRNode *root, const StmtToOffsetMap &local_to_global_offset) {
    PromoteIntermediateToGlobalTmp pass(local_to_global_offset);
    while (true) {
      try {
        root->accept(&pass);
      } catch (IRModified) {
        continue;
      }
      break;
    }
  }

 private:
  StmtToOffsetMap local_to_global_offset;
  std::set<Stmt *> stored_to_global;
};

class FixCrossOffloadReferences : public BasicStmtVisitor {
  using BasicStmtVisitor::visit;

 private:
  FixCrossOffloadReferences(
      const CompileConfig &config,
      const StmtToOffsetMap &local_to_global_offset,
      const std::unordered_map<Stmt *, Stmt *> &stmt_to_offloaded,
      OffloadedRanges *offloaded_ranges)
      : config(config),
        local_to_global_offset(local_to_global_offset),
        stmt_to_offloaded(stmt_to_offloaded),
        offloaded_ranges_(offloaded_ranges) {
    allow_undefined_visitor = true;
    invoke_default_visitor = true;
  }

  void visit(OffloadedStmt *stmt) override {
    if (stmt->body)
      stmt->body->accept(this);
    if (stmt->task_type == OffloadedStmt::TaskType::range_for) {
      if (!stmt->const_begin) {
        TI_ASSERT(offloaded_ranges_->begin_stmts.find(stmt) !=
                  offloaded_ranges_->begin_stmts.end())
        TI_ASSERT_INFO(local_to_global_offset.find(
                           offloaded_ranges_->begin_stmts.find(stmt)->second) !=
                           local_to_global_offset.end(),
                       "Begin fails.")
        stmt->begin_offset =
            local_to_global_offset[offloaded_ranges_->begin_stmts.find(stmt)
                                       ->second];
      }
      if (!stmt->const_end) {
        TI_ASSERT(offloaded_ranges_->end_stmts.find(stmt) !=
                  offloaded_ranges_->end_stmts.end())
        TI_ASSERT_INFO(local_to_global_offset.find(
                           offloaded_ranges_->end_stmts.find(stmt)->second) !=
                           local_to_global_offset.end(),
                       "End fails.")
        stmt->end_offset =
            local_to_global_offset[offloaded_ranges_->end_stmts.find(stmt)
                                       ->second];
      }
    }
  }

  // Replace alloca with global var initialization (set to 0)
  void visit(AllocaStmt *stmt) override {
    if (local_to_global_offset.find(stmt) == local_to_global_offset.end())
      return;
    VecStatement replacement;
    auto ret_type = stmt->ret_type;
    local_to_global_vector_type[stmt] = ret_type;
    auto ptr = replacement.push_back<GlobalTemporaryStmt>(
        local_to_global_offset[stmt], ret_type);
    auto offloaded = stmt_to_offloaded[stmt];
    stmt_to_offloaded[ptr] = offloaded;
    if (auto tensor_type = stmt->ret_type->cast<TensorType>()) {
      LaneAttribute<TypedConstant> zero(std::vector<TypedConstant>(
          1, TypedConstant(tensor_type->get_element_type())));
      auto const_zero_stmt = replacement.push_back<ConstStmt>(zero);
      stmt_to_offloaded[const_zero_stmt] = offloaded;
      for (int i = 0; i < tensor_type->get_num_elements(); ++i) {
        LaneAttribute<TypedConstant> offset(std::vector<TypedConstant>(
            1, TypedConstant(i *
                             data_type_size(tensor_type->get_element_type()))));
        auto const_offset_stmt = replacement.push_back<ConstStmt>(offset);
        auto ptr_offset_stmt =
            replacement.push_back<PtrOffsetStmt>(ptr, const_offset_stmt);
        auto global_store_stmt = replacement.push_back<GlobalStoreStmt>(
            ptr_offset_stmt, const_zero_stmt);
        stmt_to_offloaded[const_offset_stmt] = offloaded;
        stmt_to_offloaded[ptr_offset_stmt] = offloaded;
        stmt_to_offloaded[global_store_stmt] = offloaded;
      }
    } else {
      LaneAttribute<TypedConstant> zeros(std::vector<TypedConstant>(
          stmt->width(), TypedConstant(stmt->ret_type)));
      auto const_zeros = replacement.push_back<ConstStmt>(zeros);
      auto global_store_stmt =
          replacement.push_back<GlobalStoreStmt>(ptr, const_zeros);
      stmt_to_offloaded[global_store_stmt] = offloaded;
    }

    stmt->parent->replace_with(stmt, std::move(replacement), false);
    // To deal with the same offloaded visit_operand()
    stmt_to_offloaded[stmt] = nullptr;
    throw IRModified();
  }

  // Replace local LD/ST with global LD/ST
  void visit(LocalLoadStmt *stmt) override {
    generic_visit(stmt);
    TI_ASSERT(stmt->width() == 1)
    auto ptr = stmt->src[0].var;
    auto top_level_ptr = SquashPtrOffset::run(ptr);
    if (top_level_ptr->is<GlobalTemporaryStmt>()) {
      VecStatement replacement;
      auto global_load = replacement.push_back<GlobalLoadStmt>(ptr);
      stmt_to_offloaded[global_load] = stmt_to_offloaded[stmt];
      stmt->parent->replace_with(stmt, std::move(replacement));
      throw IRModified();
    }
  }

  void visit(LocalStoreStmt *stmt) override {
    generic_visit(stmt);
    auto ptr = stmt->dest;
    auto top_level_ptr = SquashPtrOffset::run(ptr);
    if (top_level_ptr->is<GlobalTemporaryStmt>()) {
      VecStatement replacement;
      auto global_store =
          replacement.push_back<GlobalStoreStmt>(ptr, stmt->val);
      stmt_to_offloaded[global_store] = stmt_to_offloaded[stmt];
      stmt->parent->replace_with(stmt, std::move(replacement));
      throw IRModified();
    }
  }

  bool visit_operand(Stmt *stmt, int index) {
    // return true if modified
    TI_ASSERT(index >= 0 && index < stmt->num_operands());
    auto op = stmt->operand(index);
    if (op == nullptr)
      return false;
    if (stmt_to_offloaded[stmt] == stmt_to_offloaded[op])  // same OffloadedStmt
      return false;

    auto offloaded = stmt_to_offloaded[stmt];

    if (op->is<GlobalPtrStmt>()) {
      auto copy = op->clone();
      copy->as<GlobalPtrStmt>()->activate = false;
      stmt_to_offloaded[copy.get()] = offloaded;
      stmt->set_operand(index, copy.get());
      stmt->insert_before_me(std::move(copy));
      return true;
    }

    if (local_to_global_offset.find(op) == local_to_global_offset.end()) {
      TI_ASSERT_INFO(op->is<ConstStmt>() || op->is<PtrOffsetStmt>() ||
                         op->is<GlobalTemporaryStmt>(),
                     "{} is not allowed here.", op->type());
      // For cases like ConstStmt
      auto copy = op->clone();
      stmt_to_offloaded[copy.get()] = offloaded;
      stmt->set_operand(index, copy.get());
      stmt->insert_before_me(std::move(copy));
    } else {
      auto global_temporary = Stmt::make<GlobalTemporaryStmt>(
          local_to_global_offset[op], op->ret_type);
      stmt_to_offloaded[global_temporary.get()] = offloaded;
      stmt->set_operand(index, global_temporary.get());
      if (op->is<AllocaStmt>() || op->ret_type.is_pointer()) {
        // For cases like Alloca both TensorType and Scalar which will be
        // followed by LocalLoad. Avoid repeated loads here.
        stmt->insert_before_me(std::move(global_temporary));
      } else {
        // For other cases like ArgLoadStmt UnaryOpStmt which needs to load.
        auto load = Stmt::make<GlobalLoadStmt>(global_temporary.get());
        stmt_to_offloaded[load.get()] = offloaded;
        stmt->set_operand(index, load.get());
        stmt->insert_before_me(std::move(global_temporary));
        stmt->insert_before_me(std::move(load));
      }
    }
    return true;
  }

  void generic_visit(Stmt *stmt) {
    int n_op = stmt->num_operands();
    bool modified = false;
    for (int i = 0; i < n_op; i++) {
      if (visit_operand(stmt, i))
        modified = true;
    }
    if (modified)
      throw IRModified();
  }

  void visit(Stmt *stmt) override {
    TI_ASSERT(stmt->width() == 1)
    generic_visit(stmt);
  }

  void preprocess_container_stmt(Stmt *stmt) override {
    generic_visit(stmt);
  }

 public:
  static void run(IRNode *root,
                  const CompileConfig &config,
                  const StmtToOffsetMap &local_to_global_offset,
                  const std::unordered_map<Stmt *, Stmt *> &stmt_to_offloaded,
                  OffloadedRanges *offloaded_ranges) {
    FixCrossOffloadReferences pass(config, local_to_global_offset,
                                   stmt_to_offloaded, offloaded_ranges);
    while (true) {
      try {
        root->accept(&pass);
      } catch (IRModified) {
        continue;
      }
      break;
    }
  }

 private:
  const CompileConfig &config;
  StmtToOffsetMap local_to_global_offset;
  std::unordered_map<Stmt *, Stmt *> stmt_to_offloaded;
  OffloadedRanges *const offloaded_ranges_;
  std::unordered_map<Stmt *, DataType> local_to_global_vector_type;
};

void insert_gc(IRNode *root, const CompileConfig &config) {
  auto *b = dynamic_cast<Block *>(root);
  TI_ASSERT(b);
  std::vector<std::pair<int, std::vector<SNode *>>> gc_statements;
  for (int i = 0; i < (int)b->statements.size(); i++) {
    auto snodes =
        irpass::analysis::gather_deactivations(b->statements[i].get());
    gc_statements.emplace_back(
        std::make_pair(i, std::vector<SNode *>(snodes.begin(), snodes.end())));
  }

  for (int i = (int)b->statements.size() - 1; i >= 0; i--) {
    auto snodes = gc_statements[i].second;
    for (auto *snode : snodes) {
      if (is_gc_able(snode->type)) {
        auto gc_task = Stmt::make_typed<OffloadedStmt>(
            OffloadedStmt::TaskType::gc, config.arch);
        gc_task->snode = snode;
        b->insert(std::move(gc_task), i + 1);
      }
    }
  }
}

class AssociateContinueScope : public BasicStmtVisitor {
 public:
  using BasicStmtVisitor::visit;
  using Parent = BasicStmtVisitor;

  void visit(WhileStmt *stmt) override {
    auto *old_loop = cur_internal_loop_;
    cur_internal_loop_ = stmt;
    Parent::visit(stmt);
    cur_internal_loop_ = old_loop;
  }

  void visit(RangeForStmt *stmt) override {
    auto *old_loop = cur_internal_loop_;
    cur_internal_loop_ = stmt;
    Parent::visit(stmt);
    cur_internal_loop_ = old_loop;
  }

  void visit(StructForStmt *stmt) override {
    TI_ERROR("struct_for cannot be nested inside a kernel, stmt={}",
             stmt->name());
  }

  void visit(OffloadedStmt *stmt) override {
    TI_ASSERT(cur_offloaded_stmt_ == nullptr);
    TI_ASSERT(cur_internal_loop_ == nullptr);
    cur_offloaded_stmt_ = stmt;
    Parent::visit(stmt);
    cur_offloaded_stmt_ = nullptr;
  }

  void visit(ContinueStmt *stmt) override {
    if (stmt->scope == nullptr) {
      if (cur_internal_loop_ != nullptr) {
        stmt->scope = cur_internal_loop_;
      } else {
        stmt->scope = cur_offloaded_stmt_;
      }
      modified_ = true;
    }
    TI_ASSERT(stmt->scope != nullptr);
  }

  static void run(IRNode *root) {
    while (true) {
      AssociateContinueScope pass;
      root->accept(&pass);
      if (!pass.modified_) {
        break;
      }
    }
  }

 private:
  explicit AssociateContinueScope()
      : modified_(false),
        cur_offloaded_stmt_(nullptr),
        cur_internal_loop_(nullptr) {
  }

  bool modified_;
  OffloadedStmt *cur_offloaded_stmt_;
  Stmt *cur_internal_loop_;
};

}  // namespace

void offload(IRNode *root, const CompileConfig &config) {
  TI_AUTO_PROF;
  auto offloaded_ranges = Offloader::run(root, config);
  type_check(root, config);
  {
    auto stmt_to_offloaded = StmtToOffloaded::run(root);
    const auto local_to_global_offset = IdentifyValuesUsedInOtherOffloads::run(
        root, config, stmt_to_offloaded, &offloaded_ranges);
    PromoteIntermediateToGlobalTmp::run(root, local_to_global_offset);
    stmt_to_offloaded = StmtToOffloaded::run(root);
    FixCrossOffloadReferences::run(root, config, local_to_global_offset,
                                   stmt_to_offloaded, &offloaded_ranges);
  }
  insert_gc(root, config);
  // TODO(k-ye): Move this into its own pass. However, we need to wait for all
  // backends to integrate with https://github.com/taichi-dev/taichi/pull/700
  AssociateContinueScope::run(root);
  type_check(root, config);
  re_id(root);
}

}  // namespace irpass

TLANG_NAMESPACE_END
