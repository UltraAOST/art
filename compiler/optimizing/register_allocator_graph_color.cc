/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "register_allocator_graph_color.h"

#include "code_generator.h"
#include "register_allocation_resolver.h"
#include "ssa_liveness_analysis.h"
#include "thread-inl.h"

namespace art {

// Highest number of registers that we support for any platform. This can be used for std::bitset,
// for example, which needs to know its size at compile time.
static constexpr size_t kMaxNumRegs = 32;

// The maximum number of graph coloring attempts before triggering a DCHECK.
// This is meant to catch changes to the graph coloring algorithm that undermine its forward
// progress guarantees. Forward progress for the algorithm means splitting live intervals on
// every graph coloring attempt so that eventually the interference graph will be sparse enough
// to color. The main threat to forward progress is trying to split short intervals which cannot be
// split further; this could cause infinite looping because the interference graph would never
// change. This is avoided by prioritizing short intervals before long ones, so that long
// intervals are split when coloring fails.
static constexpr size_t kMaxGraphColoringAttemptsDebug = 100;

// We always want to avoid spilling inside loops.
static constexpr size_t kLoopSpillWeightMultiplier = 10;

// If we avoid moves in single jump blocks, we can avoid jumps to jumps.
static constexpr size_t kSingleJumpBlockWeightMultiplier = 2;

// We avoid moves in blocks that dominate the exit block, since these blocks will
// be executed on every path through the method.
static constexpr size_t kDominatesExitBlockWeightMultiplier = 2;

enum class CoalesceKind {
  kAdjacentSibling,       // Prevents moves at interval split points.
  kFixedOutputSibling,    // Prevents moves from a fixed output location.
  kFixedInput,            // Prevents moves into a fixed input location.
  kNonlinearControlFlow,  // Prevents moves between blocks.
  kPhi,                   // Prevents phi resolution moves.
  kFirstInput,            // Prevents a single input move.
  kAnyInput,              // May lead to better instruction selection / smaller encodings.
};

std::ostream& operator<<(std::ostream& os, const CoalesceKind& kind) {
  return os << static_cast<typename std::underlying_type<CoalesceKind>::type>(kind);
}

static size_t LoopDepthAt(HBasicBlock* block) {
  HLoopInformation* loop_info = block->GetLoopInformation();
  size_t depth = 0;
  while (loop_info != nullptr) {
    ++depth;
    loop_info = loop_info->GetPreHeader()->GetLoopInformation();
  }
  return depth;
}

// Return the runtime cost of inserting a move instruction at the specified location.
static size_t CostForMoveAt(size_t position, const SsaLivenessAnalysis& liveness) {
  HBasicBlock* block = liveness.GetBlockFromPosition(position / 2);
  DCHECK(block != nullptr);
  size_t cost = 1;
  if (block->IsSingleJump()) {
    cost *= kSingleJumpBlockWeightMultiplier;
  }
  if (block->Dominates(block->GetGraph()->GetExitBlock())) {
    cost *= kDominatesExitBlockWeightMultiplier;
  }
  for (size_t loop_depth = LoopDepthAt(block); loop_depth > 0; --loop_depth) {
    cost *= kLoopSpillWeightMultiplier;
  }
  return cost;
}

// In general, we estimate coalesce priority by whether it will definitely avoid a move,
// and by how likely it is to create an interference graph that's harder to color.
static size_t ComputeCoalescePriority(CoalesceKind kind,
                                      size_t position,
                                      const SsaLivenessAnalysis& liveness) {
  if (kind == CoalesceKind::kAnyInput) {
    // This type of coalescing can affect instruction selection, but not moves, so we
    // give it the lowest priority.
    return 0;
  } else {
    return CostForMoveAt(position, liveness);
  }
}

enum class CoalesceStage {
  kWorklist,  // Currently in the iterative coalescing worklist.
  kActive,    // Not in a worklist, but could be considered again during iterative coalescing.
  kInactive,  // No longer considered until last-chance coalescing.
  kDefunct,   // Either the two nodes interfere, or have already been coalesced.
};

std::ostream& operator<<(std::ostream& os, const CoalesceStage& stage) {
  return os << static_cast<typename std::underlying_type<CoalesceStage>::type>(stage);
}

// Represents a coalesce opportunity between two nodes.
struct CoalesceOpportunity : public ArenaObject<kArenaAllocRegisterAllocator> {
  CoalesceOpportunity(InterferenceNode* a,
                      InterferenceNode* b,
                      CoalesceKind kind,
                      size_t position,
                      const SsaLivenessAnalysis& liveness)
        : node_a(a),
          node_b(b),
          stage(CoalesceStage::kWorklist),
          priority(ComputeCoalescePriority(kind, position, liveness)) {}

  InterferenceNode* const node_a;
  InterferenceNode* const node_b;

  // The current stage of this coalesce opportunity, indicating whether it is in a worklist,
  // and whether it should still be considered.
  CoalesceStage stage;

  // The priority of this coalesce opportunity, based on heuristics.
  const size_t priority;
};

enum class NodeStage {
  kInitial,           // Uninitialized.
  kPrecolored,        // Marks fixed nodes.
  kSafepoint,         // Marks safepoint nodes.
  kPrunable,          // Marks uncolored nodes in the interference graph.
  kSimplifyWorklist,  // Marks non-move-related nodes with degree less than the number of registers.
  kFreezeWorklist,    // Marks move-related nodes with degree less than the number of registers.
  kSpillWorklist,     // Marks nodes with degree greater or equal to the number of registers.
  kPruned             // Marks nodes already pruned from the interference graph.
};

std::ostream& operator<<(std::ostream& os, const NodeStage& stage) {
  return os << static_cast<typename std::underlying_type<NodeStage>::type>(stage);
}

// Returns the estimated cost of spilling a particular live interval.
static float ComputeSpillWeight(LiveInterval* interval, const SsaLivenessAnalysis& liveness) {
  if (interval->HasRegister()) {
    // Intervals with a fixed register cannot be spilled.
    return std::numeric_limits<float>::min();
  }

  size_t length = interval->GetLength();
  if (length == 1) {
    // Tiny intervals should have maximum priority, since they cannot be split any further.
    return std::numeric_limits<float>::max();
  }

  size_t use_weight = 0;
  if (interval->GetDefinedBy() != nullptr && interval->DefinitionRequiresRegister()) {
    // Cost for spilling at a register definition point.
    use_weight += CostForMoveAt(interval->GetStart() + 1, liveness);
  }

  UsePosition* use = interval->GetFirstUse();
  while (use != nullptr && use->GetPosition() <= interval->GetStart()) {
    // Skip uses before the start of this live interval.
    use = use->GetNext();
  }

  while (use != nullptr && use->GetPosition() <= interval->GetEnd()) {
    if (use->GetUser() != nullptr && use->RequiresRegister()) {
      // Cost for spilling at a register use point.
      use_weight += CostForMoveAt(use->GetUser()->GetLifetimePosition() - 1, liveness);
    }
    use = use->GetNext();
  }

  // We divide by the length of the interval because we want to prioritize
  // short intervals; we do not benefit much if we split them further.
  return static_cast<float>(use_weight) / static_cast<float>(length);
}

// Interference nodes make up the interference graph, which is the primary data structure in
// graph coloring register allocation. Each node represents a single live interval, and contains
// a set of adjacent nodes corresponding to intervals overlapping with its own. To save memory,
// pre-colored nodes never contain outgoing edges (only incoming ones).
//
// As nodes are pruned from the interference graph, incoming edges of the pruned node are removed,
// but outgoing edges remain in order to later color the node based on the colors of its neighbors.
//
// Note that a pair interval is represented by a single node in the interference graph, which
// essentially requires two colors. One consequence of this is that the degree of a node is not
// necessarily equal to the number of adjacent nodes--instead, the degree reflects the maximum
// number of colors with which a node could interfere. We model this by giving edges different
// weights (1 or 2) to control how much it increases the degree of adjacent nodes.
// For example, the edge between two single nodes will have weight 1. On the other hand,
// the edge between a single node and a pair node will have weight 2. This is because the pair
// node could block up to two colors for the single node, and because the single node could
// block an entire two-register aligned slot for the pair node.
// The degree is defined this way because we use it to decide whether a node is guaranteed a color,
// and thus whether it is safe to prune it from the interference graph early on.
class InterferenceNode : public ArenaObject<kArenaAllocRegisterAllocator> {
 public:
  InterferenceNode(ArenaAllocator* allocator,
                   LiveInterval* interval,
                   size_t id,
                   const SsaLivenessAnalysis& liveness)
        : stage(NodeStage::kInitial),
          interval_(interval),
          adjacent_nodes_(allocator->Adapter(kArenaAllocRegisterAllocator)),
          coalesce_opportunities_(allocator->Adapter(kArenaAllocRegisterAllocator)),
          out_degree_(interval->HasRegister() ? std::numeric_limits<size_t>::max() : 0),
          id_(id),
          alias_(this),
          spill_weight_(ComputeSpillWeight(interval, liveness)),
          requires_color_(interval->RequiresRegister()) {
    DCHECK(!interval->IsHighInterval()) << "Pair nodes should be represented by the low interval";
  }

  void AddInterference(InterferenceNode* other, bool guaranteed_not_interfering_yet) {
    DCHECK(!IsPrecolored()) << "To save memory, fixed nodes should not have outgoing interferences";
    DCHECK_NE(this, other) << "Should not create self loops in the interference graph";
    DCHECK_EQ(this, alias_) << "Should not add interferences to a node that aliases another";
    DCHECK_NE(stage, NodeStage::kPruned);
    DCHECK_NE(other->stage, NodeStage::kPruned);
    if (guaranteed_not_interfering_yet) {
      DCHECK(std::find(adjacent_nodes_.begin(), adjacent_nodes_.end(), other)
             == adjacent_nodes_.end());
      adjacent_nodes_.push_back(other);
      out_degree_ += EdgeWeightWith(other);
    } else {
      auto it = std::find(adjacent_nodes_.begin(), adjacent_nodes_.end(), other);
      if (it == adjacent_nodes_.end()) {
        adjacent_nodes_.push_back(other);
        out_degree_ += EdgeWeightWith(other);
      }
    }
  }

  void RemoveInterference(InterferenceNode* other) {
    DCHECK_EQ(this, alias_) << "Should not remove interferences from a coalesced node";
    DCHECK_EQ(other->stage, NodeStage::kPruned) << "Should only remove interferences when pruning";
    auto it = std::find(adjacent_nodes_.begin(), adjacent_nodes_.end(), other);
    if (it != adjacent_nodes_.end()) {
      adjacent_nodes_.erase(it);
      out_degree_ -= EdgeWeightWith(other);
    }
  }

  bool ContainsInterference(InterferenceNode* other) const {
    DCHECK(!IsPrecolored()) << "Should not query fixed nodes for interferences";
    DCHECK_EQ(this, alias_) << "Should not query a coalesced node for interferences";
    auto it = std::find(adjacent_nodes_.begin(), adjacent_nodes_.end(), other);
    return it != adjacent_nodes_.end();
  }

  LiveInterval* GetInterval() const {
    return interval_;
  }

  const ArenaVector<InterferenceNode*>& GetAdjacentNodes() const {
    return adjacent_nodes_;
  }

  size_t GetOutDegree() const {
    // Pre-colored nodes have infinite degree.
    DCHECK(!IsPrecolored() || out_degree_ == std::numeric_limits<size_t>::max());
    return out_degree_;
  }

  size_t GetId() const {
    return id_;
  }

  void AddCoalesceOpportunity(CoalesceOpportunity* opportunity) {
    coalesce_opportunities_.push_back(opportunity);
  }

  void ClearCoalesceOpportunities() {
    coalesce_opportunities_.clear();
  }

  bool IsMoveRelated() const {
    for (CoalesceOpportunity* opportunity : coalesce_opportunities_) {
      if (opportunity->stage == CoalesceStage::kWorklist ||
          opportunity->stage == CoalesceStage::kActive) {
        return true;
      }
    }
    return false;
  }

  // Return whether this node already has a color.
  // Used to find fixed nodes in the interference graph before coloring.
  bool IsPrecolored() const {
    return interval_->HasRegister();
  }

  bool IsPair() const {
    return interval_->HasHighInterval();
  }

  void SetAlias(InterferenceNode* rep) {
    DCHECK_NE(rep->stage, NodeStage::kPruned);
    DCHECK_EQ(this, alias_) << "Should only set a node's alias once";
    alias_ = rep;
  }

  InterferenceNode* GetAlias() {
    if (alias_ != this) {
      // Recurse in order to flatten tree of alias pointers.
      alias_ = alias_->GetAlias();
    }
    return alias_;
  }

  const ArenaVector<CoalesceOpportunity*>& GetCoalesceOpportunities() const {
    return coalesce_opportunities_;
  }

  float GetSpillWeight() const {
    return spill_weight_;
  }

  bool RequiresColor() const {
    return requires_color_;
  }

  // We give extra weight to edges adjacent to pair nodes. See the general comment on the
  // interference graph above.
  size_t EdgeWeightWith(const InterferenceNode* other) const {
    return (IsPair() || other->IsPair()) ? 2 : 1;
  }

  // The current stage of this node, indicating which worklist it belongs to.
  NodeStage stage;

 private:
  // The live interval that this node represents.
  LiveInterval* const interval_;

  // All nodes interfering with this one.
  // We use an unsorted vector as a set, since a tree or hash set is too heavy for the
  // set sizes that we encounter. Using a vector leads to much better performance.
  ArenaVector<InterferenceNode*> adjacent_nodes_;

  // Interference nodes that this node should be coalesced with to reduce moves.
  ArenaVector<CoalesceOpportunity*> coalesce_opportunities_;

  // The maximum number of colors with which this node could interfere. This could be more than
  // the number of adjacent nodes if this is a pair node, or if some adjacent nodes are pair nodes.
  // We use "out" degree because incoming edges come from nodes already pruned from the graph,
  // and do not affect the coloring of this node.
  // Pre-colored nodes are treated as having infinite degree.
  size_t out_degree_;

  // A unique identifier for this node, used to maintain determinism when storing
  // interference nodes in sets.
  const size_t id_;

  // The node representing this node in the interference graph.
  // Initially set to `this`, and only changed if this node is coalesced into another.
  InterferenceNode* alias_;

  // The cost of splitting and spilling this interval to the stack.
  // Nodes with a higher spill weight should be prioritized when assigning registers.
  // This is essentially based on use density and location; short intervals with many uses inside
  // deeply nested loops have a high spill weight.
  const float spill_weight_;

  const bool requires_color_;

  DISALLOW_COPY_AND_ASSIGN(InterferenceNode);
};

static bool IsCoreInterval(LiveInterval* interval) {
  return !Primitive::IsFloatingPointType(interval->GetType());
}

static size_t ComputeReservedArtMethodSlots(const CodeGenerator& codegen) {
  return static_cast<size_t>(InstructionSetPointerSize(codegen.GetInstructionSet())) / kVRegSize;
}

RegisterAllocatorGraphColor::RegisterAllocatorGraphColor(ArenaAllocator* allocator,
                                                         CodeGenerator* codegen,
                                                         const SsaLivenessAnalysis& liveness,
                                                         bool iterative_move_coalescing)
      : RegisterAllocator(allocator, codegen, liveness),
        iterative_move_coalescing_(iterative_move_coalescing),
        core_intervals_(allocator->Adapter(kArenaAllocRegisterAllocator)),
        fp_intervals_(allocator->Adapter(kArenaAllocRegisterAllocator)),
        temp_intervals_(allocator->Adapter(kArenaAllocRegisterAllocator)),
        safepoints_(allocator->Adapter(kArenaAllocRegisterAllocator)),
        physical_core_nodes_(allocator->Adapter(kArenaAllocRegisterAllocator)),
        physical_fp_nodes_(allocator->Adapter(kArenaAllocRegisterAllocator)),
        int_spill_slot_counter_(0),
        double_spill_slot_counter_(0),
        float_spill_slot_counter_(0),
        long_spill_slot_counter_(0),
        catch_phi_spill_slot_counter_(0),
        reserved_art_method_slots_(ComputeReservedArtMethodSlots(*codegen)),
        reserved_out_slots_(codegen->GetGraph()->GetMaximumNumberOfOutVRegs()),
        number_of_globally_blocked_core_regs_(0),
        number_of_globally_blocked_fp_regs_(0),
        max_safepoint_live_core_regs_(0),
        max_safepoint_live_fp_regs_(0),
        coloring_attempt_allocator_(nullptr),
        node_id_counter_(0),
        interval_node_map_(allocator->Adapter(kArenaAllocRegisterAllocator)),
        prunable_nodes_(allocator->Adapter(kArenaAllocRegisterAllocator)),
        pruned_nodes_(allocator->Adapter(kArenaAllocRegisterAllocator)),
        simplify_worklist_(allocator->Adapter(kArenaAllocRegisterAllocator)),
        freeze_worklist_(allocator->Adapter(kArenaAllocRegisterAllocator)),
        spill_worklist_(HasGreaterNodePriority, allocator->Adapter(kArenaAllocRegisterAllocator)),
        coalesce_worklist_(CmpCoalesceOpportunity,
                           allocator->Adapter(kArenaAllocRegisterAllocator)) {
  // Before we ask for blocked registers, set them up in the code generator.
  codegen->SetupBlockedRegisters();

  // Initialize physical core register live intervals and blocked registers.
  // This includes globally blocked registers, such as the stack pointer.
  physical_core_nodes_.resize(codegen_->GetNumberOfCoreRegisters(), nullptr);
  for (size_t i = 0; i < codegen_->GetNumberOfCoreRegisters(); ++i) {
    LiveInterval* interval = LiveInterval::MakeFixedInterval(allocator_, i, Primitive::kPrimInt);
    physical_core_nodes_[i] =
        new (allocator_) InterferenceNode(allocator_, interval, node_id_counter_++, liveness);
    physical_core_nodes_[i]->stage = NodeStage::kPrecolored;
    core_intervals_.push_back(interval);
    if (codegen_->IsBlockedCoreRegister(i)) {
      ++number_of_globally_blocked_core_regs_;
      interval->AddRange(0, liveness.GetMaxLifetimePosition());
    }
  }
  // Initialize physical floating point register live intervals and blocked registers.
  physical_fp_nodes_.resize(codegen_->GetNumberOfFloatingPointRegisters(), nullptr);
  for (size_t i = 0; i < codegen_->GetNumberOfFloatingPointRegisters(); ++i) {
    LiveInterval* interval = LiveInterval::MakeFixedInterval(allocator_, i, Primitive::kPrimFloat);
    physical_fp_nodes_[i] =
        new (allocator_) InterferenceNode(allocator_, interval, node_id_counter_++, liveness);
    physical_fp_nodes_[i]->stage = NodeStage::kPrecolored;
    fp_intervals_.push_back(interval);
    if (codegen_->IsBlockedFloatingPointRegister(i)) {
      ++number_of_globally_blocked_fp_regs_;
      interval->AddRange(0, liveness.GetMaxLifetimePosition());
    }
  }
}

void RegisterAllocatorGraphColor::AllocateRegisters() {
  // (1) Collect and prepare live intervals.
  ProcessInstructions();

  for (bool processing_core_regs : {true, false}) {
    ArenaVector<LiveInterval*>& intervals = processing_core_regs
        ? core_intervals_
        : fp_intervals_;
    size_t num_registers = processing_core_regs
        ? codegen_->GetNumberOfCoreRegisters()
        : codegen_->GetNumberOfFloatingPointRegisters();

    size_t attempt = 0;
    while (true) {
      ++attempt;
      DCHECK(attempt <= kMaxGraphColoringAttemptsDebug)
          << "Exceeded debug max graph coloring register allocation attempts. "
          << "This could indicate that the register allocator is not making forward progress, "
          << "which could be caused by prioritizing the wrong live intervals. (Short intervals "
          << "should be prioritized over long ones, because they cannot be split further.)";

      // Reset the allocator and fixed nodes for the next coloring attempt.
      ArenaAllocator coloring_attempt_allocator(allocator_->GetArenaPool());
      coloring_attempt_allocator_ = &coloring_attempt_allocator;
      for (InterferenceNode* node : physical_core_nodes_) {
        node->ClearCoalesceOpportunities();
      }
      for (InterferenceNode* node : physical_fp_nodes_) {
        node->ClearCoalesceOpportunities();
      }

      // Clear data structures.
      // TODO: One alternative idea is to create a separate struct that contains all of these
      //       data structures, and is passed around to each graph coloring function.
      interval_node_map_ = ArenaHashMap<LiveInterval*, InterferenceNode*>(
          coloring_attempt_allocator_->Adapter(kArenaAllocRegisterAllocator));
      prunable_nodes_ = ArenaVector<InterferenceNode*>(
          coloring_attempt_allocator_->Adapter(kArenaAllocRegisterAllocator));
      pruned_nodes_ = ArenaStdStack<InterferenceNode*>(
          coloring_attempt_allocator_->Adapter(kArenaAllocRegisterAllocator));
      simplify_worklist_ = ArenaDeque<InterferenceNode*>(
          coloring_attempt_allocator_->Adapter(kArenaAllocRegisterAllocator));
      freeze_worklist_ = ArenaDeque<InterferenceNode*>(
          coloring_attempt_allocator_->Adapter(kArenaAllocRegisterAllocator));
      spill_worklist_ = ArenaPriorityQueue<InterferenceNode*, decltype(&HasGreaterNodePriority)>(
          HasGreaterNodePriority, coloring_attempt_allocator_->Adapter(kArenaAllocRegisterAllocator));
      coalesce_worklist_ =
          ArenaPriorityQueue<CoalesceOpportunity*, decltype(&CmpCoalesceOpportunity)>(
              CmpCoalesceOpportunity,
              coloring_attempt_allocator_->Adapter(kArenaAllocRegisterAllocator));

      // (2) Build the interference graph. Also gather safepoints and build the interval node map.
      ArenaVector<InterferenceNode*> safepoints(
          coloring_attempt_allocator_->Adapter(kArenaAllocRegisterAllocator));
      ArenaVector<InterferenceNode*>& physical_nodes = processing_core_regs
          ? physical_core_nodes_
          : physical_fp_nodes_;
      BuildInterferenceGraph(intervals, physical_nodes, &safepoints);

      // (3) Add coalesce opportunities.
      //     If we have tried coloring the graph a suspiciously high number of times, give
      //     up on move coalescing, just in case the coalescing heuristics are not conservative.
      //     (This situation will be caught if DCHECKs are turned on.)
      if (iterative_move_coalescing_ && attempt <= kMaxGraphColoringAttemptsDebug) {
        FindCoalesceOpportunities();
      }

      // (4) Prune all uncolored nodes from interference graph.
      PruneInterferenceGraph(num_registers);

      // (5) Color pruned nodes based on interferences.
      bool successful = ColorInterferenceGraph(num_registers,
                                               processing_core_regs);

      if (successful) {
        // Compute the maximum number of live registers across safepoints.
        // Notice that we do not count globally blocked registers, such as the stack pointer.
        if (safepoints.size() > 0) {
          size_t max_safepoint_live_regs = ComputeMaxSafepointLiveRegisters(safepoints);
          if (processing_core_regs) {
            max_safepoint_live_core_regs_ =
                max_safepoint_live_regs - number_of_globally_blocked_core_regs_;
          } else {
            max_safepoint_live_fp_regs_=
                max_safepoint_live_regs - number_of_globally_blocked_fp_regs_;
          }
        }

        // Tell the code generator which registers were allocated.
        // We only look at prunable_nodes because we already told the code generator about
        // fixed intervals while processing instructions. We also ignore the fixed intervals
        // placed at the top of catch blocks.
        for (InterferenceNode* node : prunable_nodes_) {
          LiveInterval* interval = node->GetInterval();
          if (interval->HasRegister()) {
            Location low_reg = processing_core_regs
                ? Location::RegisterLocation(interval->GetRegister())
                : Location::FpuRegisterLocation(interval->GetRegister());
            codegen_->AddAllocatedRegister(low_reg);
            if (interval->HasHighInterval()) {
              LiveInterval* high = interval->GetHighInterval();
              DCHECK(high->HasRegister());
              Location high_reg = processing_core_regs
                  ? Location::RegisterLocation(high->GetRegister())
                  : Location::FpuRegisterLocation(high->GetRegister());
              codegen_->AddAllocatedRegister(high_reg);
            }
          } else {
            DCHECK(!interval->HasHighInterval() || !interval->GetHighInterval()->HasRegister());
          }
        }

        break;
      }
    }  // while unsuccessful
  }  // for processing_core_instructions

  // (6) Resolve locations and deconstruct SSA form.
  RegisterAllocationResolver(allocator_, codegen_, liveness_)
      .Resolve(max_safepoint_live_core_regs_,
               max_safepoint_live_fp_regs_,
               reserved_art_method_slots_ + reserved_out_slots_,
               int_spill_slot_counter_,
               long_spill_slot_counter_,
               float_spill_slot_counter_,
               double_spill_slot_counter_,
               catch_phi_spill_slot_counter_,
               temp_intervals_);

  if (kIsDebugBuild) {
    Validate(/*log_fatal_on_failure*/ true);
  }
}

bool RegisterAllocatorGraphColor::Validate(bool log_fatal_on_failure) {
  for (bool processing_core_regs : {true, false}) {
    ArenaVector<LiveInterval*> intervals(
        allocator_->Adapter(kArenaAllocRegisterAllocatorValidate));
    for (size_t i = 0; i < liveness_.GetNumberOfSsaValues(); ++i) {
      HInstruction* instruction = liveness_.GetInstructionFromSsaIndex(i);
      LiveInterval* interval = instruction->GetLiveInterval();
      if (interval != nullptr && IsCoreInterval(interval) == processing_core_regs) {
        intervals.push_back(instruction->GetLiveInterval());
      }
    }

    ArenaVector<InterferenceNode*>& physical_nodes = processing_core_regs
        ? physical_core_nodes_
        : physical_fp_nodes_;
    for (InterferenceNode* fixed : physical_nodes) {
      LiveInterval* interval = fixed->GetInterval();
      if (interval->GetFirstRange() != nullptr) {
        // Ideally we would check fixed ranges as well, but currently there are times when
        // two fixed intervals for the same register will overlap. For example, a fixed input
        // and a fixed output may sometimes share the same register, in which there will be two
        // fixed intervals for the same place.
      }
    }

    for (LiveInterval* temp : temp_intervals_) {
      if (IsCoreInterval(temp) == processing_core_regs) {
        intervals.push_back(temp);
      }
    }

    size_t spill_slots = int_spill_slot_counter_
                       + long_spill_slot_counter_
                       + float_spill_slot_counter_
                       + double_spill_slot_counter_
                       + catch_phi_spill_slot_counter_;
    bool ok = ValidateIntervals(intervals,
                                spill_slots,
                                reserved_art_method_slots_ + reserved_out_slots_,
                                *codegen_,
                                allocator_,
                                processing_core_regs,
                                log_fatal_on_failure);
    if (!ok) {
      return false;
    }
  }  // for processing_core_regs

  return true;
}

void RegisterAllocatorGraphColor::ProcessInstructions() {
  for (HLinearPostOrderIterator it(*codegen_->GetGraph()); !it.Done(); it.Advance()) {
    HBasicBlock* block = it.Current();

    // Note that we currently depend on this ordering, since some helper
    // code is designed for linear scan register allocation.
    for (HBackwardInstructionIterator instr_it(block->GetInstructions());
          !instr_it.Done();
          instr_it.Advance()) {
      ProcessInstruction(instr_it.Current());
    }

    for (HInstructionIterator phi_it(block->GetPhis()); !phi_it.Done(); phi_it.Advance()) {
      ProcessInstruction(phi_it.Current());
    }

    if (block->IsCatchBlock()
        || (block->IsLoopHeader() && block->GetLoopInformation()->IsIrreducible())) {
      // By blocking all registers at the top of each catch block or irreducible loop, we force
      // intervals belonging to the live-in set of the catch/header block to be spilled.
      // TODO(ngeoffray): Phis in this block could be allocated in register.
      size_t position = block->GetLifetimeStart();
      BlockRegisters(position, position + 1);
    }
  }
}

void RegisterAllocatorGraphColor::ProcessInstruction(HInstruction* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  if (locations == nullptr) {
    return;
  }
  if (locations->NeedsSafepoint() && codegen_->IsLeafMethod()) {
    // We do this here because we do not want the suspend check to artificially
    // create live registers.
    DCHECK(instruction->IsSuspendCheckEntry());
    DCHECK_EQ(locations->GetTempCount(), 0u);
    instruction->GetBlock()->RemoveInstruction(instruction);
    return;
  }

  CheckForTempLiveIntervals(instruction);
  CheckForSafepoint(instruction);
  if (instruction->GetLocations()->WillCall()) {
    // If a call will happen, create fixed intervals for caller-save registers.
    // TODO: Note that it may be beneficial to later split intervals at this point,
    //       so that we allow last-minute moves from a caller-save register
    //       to a callee-save register.
    BlockRegisters(instruction->GetLifetimePosition(),
                   instruction->GetLifetimePosition() + 1,
                   /*caller_save_only*/ true);
  }
  CheckForFixedInputs(instruction);

  LiveInterval* interval = instruction->GetLiveInterval();
  if (interval == nullptr) {
    // Instructions lacking a valid output location do not have a live interval.
    DCHECK(!locations->Out().IsValid());
    return;
  }

  // Low intervals act as representatives for their corresponding high interval.
  DCHECK(!interval->IsHighInterval());
  if (codegen_->NeedsTwoRegisters(interval->GetType())) {
    interval->AddHighInterval();
  }
  AddSafepointsFor(instruction);
  CheckForFixedOutput(instruction);
  AllocateSpillSlotForCatchPhi(instruction);

  ArenaVector<LiveInterval*>& intervals = IsCoreInterval(interval)
      ? core_intervals_
      : fp_intervals_;
  if (interval->HasSpillSlot() || instruction->IsConstant()) {
    // Note that if an interval already has a spill slot, then its value currently resides
    // in the stack (e.g., parameters). Thus we do not have to allocate a register until its first
    // register use. This is also true for constants, which can be materialized at any point.
    size_t first_register_use = interval->FirstRegisterUse();
    if (first_register_use != kNoLifetime) {
      LiveInterval* split = SplitBetween(interval, interval->GetStart(), first_register_use - 1);
      intervals.push_back(split);
    } else {
      // We won't allocate a register for this value.
    }
  } else {
    intervals.push_back(interval);
  }
}

void RegisterAllocatorGraphColor::CheckForFixedInputs(HInstruction* instruction) {
  // We simply block physical registers where necessary.
  // TODO: Ideally we would coalesce the physical register with the register
  //       allocated to the input value, but this can be tricky if, e.g., there
  //       could be multiple physical register uses of the same value at the
  //       same instruction. Furthermore, there's currently no distinction between
  //       fixed inputs to a call (which will be clobbered) and other fixed inputs (which
  //       may not be clobbered).
  LocationSummary* locations = instruction->GetLocations();
  size_t position = instruction->GetLifetimePosition();
  for (size_t i = 0; i < locations->GetInputCount(); ++i) {
    Location input = locations->InAt(i);
    if (input.IsRegister() || input.IsFpuRegister()) {
      BlockRegister(input, position, position + 1);
      codegen_->AddAllocatedRegister(input);
    } else if (input.IsPair()) {
      BlockRegister(input.ToLow(), position, position + 1);
      BlockRegister(input.ToHigh(), position, position + 1);
      codegen_->AddAllocatedRegister(input.ToLow());
      codegen_->AddAllocatedRegister(input.ToHigh());
    }
  }
}

void RegisterAllocatorGraphColor::CheckForFixedOutput(HInstruction* instruction) {
  // If an instruction has a fixed output location, we give the live interval a register and then
  // proactively split it just after the definition point to avoid creating too many interferences
  // with a fixed node.
  LiveInterval* interval = instruction->GetLiveInterval();
  Location out = interval->GetDefinedBy()->GetLocations()->Out();
  size_t position = instruction->GetLifetimePosition();
  DCHECK_GE(interval->GetEnd() - position, 2u);

  if (out.IsUnallocated() && out.GetPolicy() == Location::kSameAsFirstInput) {
    out = instruction->GetLocations()->InAt(0);
  }

  if (out.IsRegister() || out.IsFpuRegister()) {
    interval->SetRegister(out.reg());
    codegen_->AddAllocatedRegister(out);
    Split(interval, position + 1);
  } else if (out.IsPair()) {
    interval->SetRegister(out.low());
    interval->GetHighInterval()->SetRegister(out.high());
    codegen_->AddAllocatedRegister(out.ToLow());
    codegen_->AddAllocatedRegister(out.ToHigh());
    Split(interval, position + 1);
  } else if (out.IsStackSlot() || out.IsDoubleStackSlot()) {
    interval->SetSpillSlot(out.GetStackIndex());
  } else {
    DCHECK(out.IsUnallocated() || out.IsConstant());
  }
}

void RegisterAllocatorGraphColor::AddSafepointsFor(HInstruction* instruction) {
  LiveInterval* interval = instruction->GetLiveInterval();
  for (size_t safepoint_index = safepoints_.size(); safepoint_index > 0; --safepoint_index) {
    HInstruction* safepoint = safepoints_[safepoint_index - 1u];
    size_t safepoint_position = safepoint->GetLifetimePosition();

    // Test that safepoints_ are ordered in the optimal way.
    DCHECK(safepoint_index == safepoints_.size() ||
           safepoints_[safepoint_index]->GetLifetimePosition() < safepoint_position);

    if (safepoint_position == interval->GetStart()) {
      // The safepoint is for this instruction, so the location of the instruction
      // does not need to be saved.
      DCHECK_EQ(safepoint_index, safepoints_.size());
      DCHECK_EQ(safepoint, instruction);
      continue;
    } else if (interval->IsDeadAt(safepoint_position)) {
      break;
    } else if (!interval->Covers(safepoint_position)) {
      // Hole in the interval.
      continue;
    }
    interval->AddSafepoint(safepoint);
  }
}

void RegisterAllocatorGraphColor::CheckForTempLiveIntervals(HInstruction* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  size_t position = instruction->GetLifetimePosition();
  for (size_t i = 0; i < locations->GetTempCount(); ++i) {
    Location temp = locations->GetTemp(i);
    if (temp.IsRegister() || temp.IsFpuRegister()) {
      BlockRegister(temp, position, position + 1);
      codegen_->AddAllocatedRegister(temp);
    } else {
      DCHECK(temp.IsUnallocated());
      switch (temp.GetPolicy()) {
        case Location::kRequiresRegister: {
          LiveInterval* interval =
              LiveInterval::MakeTempInterval(allocator_, Primitive::kPrimInt);
          interval->AddTempUse(instruction, i);
          core_intervals_.push_back(interval);
          temp_intervals_.push_back(interval);
          break;
        }

        case Location::kRequiresFpuRegister: {
          LiveInterval* interval =
              LiveInterval::MakeTempInterval(allocator_, Primitive::kPrimDouble);
          interval->AddTempUse(instruction, i);
          fp_intervals_.push_back(interval);
          temp_intervals_.push_back(interval);
          if (codegen_->NeedsTwoRegisters(Primitive::kPrimDouble)) {
            interval->AddHighInterval(/*is_temp*/ true);
            temp_intervals_.push_back(interval->GetHighInterval());
          }
          break;
        }

        default:
          LOG(FATAL) << "Unexpected policy for temporary location "
                     << temp.GetPolicy();
      }
    }
  }
}

void RegisterAllocatorGraphColor::CheckForSafepoint(HInstruction* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  size_t position = instruction->GetLifetimePosition();

  if (locations->NeedsSafepoint()) {
    safepoints_.push_back(instruction);
    if (locations->OnlyCallsOnSlowPath()) {
      // We add a synthesized range at this position to record the live registers
      // at this position. Ideally, we could just update the safepoints when locations
      // are updated, but we currently need to know the full stack size before updating
      // locations (because of parameters and the fact that we don't have a frame pointer).
      // And knowing the full stack size requires to know the maximum number of live
      // registers at calls in slow paths.
      // By adding the following interval in the algorithm, we can compute this
      // maximum before updating locations.
      LiveInterval* interval = LiveInterval::MakeSlowPathInterval(allocator_, instruction);
      interval->AddRange(position, position + 1);
      core_intervals_.push_back(interval);
      fp_intervals_.push_back(interval);
    }
  }
}

LiveInterval* RegisterAllocatorGraphColor::TrySplit(LiveInterval* interval, size_t position) {
  if (interval->GetStart() < position && position < interval->GetEnd()) {
    return Split(interval, position);
  } else {
    return interval;
  }
}

void RegisterAllocatorGraphColor::SplitAtRegisterUses(LiveInterval* interval) {
  DCHECK(!interval->IsHighInterval());

  // Split just after a register definition.
  if (interval->IsParent() && interval->DefinitionRequiresRegister()) {
    interval = TrySplit(interval, interval->GetStart() + 1);
  }

  UsePosition* use = interval->GetFirstUse();
  while (use != nullptr && use->GetPosition() < interval->GetStart()) {
    use = use->GetNext();
  }

  // Split around register uses.
  size_t end = interval->GetEnd();
  while (use != nullptr && use->GetPosition() <= end) {
    if (use->RequiresRegister()) {
      size_t position = use->GetPosition();
      interval = TrySplit(interval, position - 1);
      if (liveness_.GetInstructionFromPosition(position / 2)->IsControlFlow()) {
        // If we are at the very end of a basic block, we cannot split right
        // at the use. Split just after instead.
        interval = TrySplit(interval, position + 1);
      } else {
        interval = TrySplit(interval, position);
      }
    }
    use = use->GetNext();
  }
}

void RegisterAllocatorGraphColor::AllocateSpillSlotForCatchPhi(HInstruction* instruction) {
  if (instruction->IsPhi() && instruction->AsPhi()->IsCatchPhi()) {
    HPhi* phi = instruction->AsPhi();
    LiveInterval* interval = phi->GetLiveInterval();

    HInstruction* previous_phi = phi->GetPrevious();
    DCHECK(previous_phi == nullptr ||
           previous_phi->AsPhi()->GetRegNumber() <= phi->GetRegNumber())
        << "Phis expected to be sorted by vreg number, "
        << "so that equivalent phis are adjacent.";

    if (phi->IsVRegEquivalentOf(previous_phi)) {
      // Assign the same spill slot.
      DCHECK(previous_phi->GetLiveInterval()->HasSpillSlot());
      interval->SetSpillSlot(previous_phi->GetLiveInterval()->GetSpillSlot());
    } else {
      interval->SetSpillSlot(catch_phi_spill_slot_counter_);
      catch_phi_spill_slot_counter_ += interval->NeedsTwoSpillSlots() ? 2 : 1;
    }
  }
}

void RegisterAllocatorGraphColor::BlockRegister(Location location,
                                                size_t start,
                                                size_t end) {
  DCHECK(location.IsRegister() || location.IsFpuRegister());
  int reg = location.reg();
  LiveInterval* interval = location.IsRegister()
      ? physical_core_nodes_[reg]->GetInterval()
      : physical_fp_nodes_[reg]->GetInterval();
  DCHECK(interval->GetRegister() == reg);
  bool blocked_by_codegen = location.IsRegister()
      ? codegen_->IsBlockedCoreRegister(reg)
      : codegen_->IsBlockedFloatingPointRegister(reg);
  if (blocked_by_codegen) {
    // We've already blocked this register for the entire method. (And adding a
    // range inside another range violates the preconditions of AddRange).
  } else {
    interval->AddRange(start, end);
  }
}

void RegisterAllocatorGraphColor::BlockRegisters(size_t start, size_t end, bool caller_save_only) {
  for (size_t i = 0; i < codegen_->GetNumberOfCoreRegisters(); ++i) {
    if (!caller_save_only || !codegen_->IsCoreCalleeSaveRegister(i)) {
      BlockRegister(Location::RegisterLocation(i), start, end);
    }
  }
  for (size_t i = 0; i < codegen_->GetNumberOfFloatingPointRegisters(); ++i) {
    if (!caller_save_only || !codegen_->IsFloatingPointCalleeSaveRegister(i)) {
      BlockRegister(Location::FpuRegisterLocation(i), start, end);
    }
  }
}

void RegisterAllocatorGraphColor::AddPotentialInterference(InterferenceNode* from,
                                                           InterferenceNode* to,
                                                           bool guaranteed_not_interfering_yet,
                                                           bool both_directions) {
  if (from->IsPrecolored()) {
    // We save space by ignoring outgoing edges from fixed nodes.
  } else if (to->GetInterval()->IsSlowPathSafepoint()) {
    // Safepoint intervals are only there to count max live registers,
    // so no need to give them incoming interference edges.
    // This is also necessary for correctness, because we don't want nodes
    // to remove themselves from safepoint adjacency sets when they're pruned.
  } else if (to->IsPrecolored()) {
    // It is important that only a single node represents a given fixed register in the
    // interference graph. We retrieve that node here.
    const ArenaVector<InterferenceNode*>& physical_nodes =
        to->GetInterval()->IsFloatingPoint() ? physical_fp_nodes_ : physical_core_nodes_;
    InterferenceNode* physical_node = physical_nodes[to->GetInterval()->GetRegister()];
    from->AddInterference(physical_node, /*guaranteed_not_interfering_yet*/ false);
    DCHECK_EQ(to->GetInterval()->GetRegister(), physical_node->GetInterval()->GetRegister());
    DCHECK_EQ(to->GetAlias(), physical_node) << "Fixed nodes should alias the canonical fixed node";

    // If a node interferes with a fixed pair node, the weight of the edge may
    // be inaccurate after using the alias of the pair node, because the alias of the pair node
    // is a singular node.
    // We could make special pair fixed nodes, but that ends up being too conservative because
    // a node could then interfere with both {r1} and {r1,r2}, leading to a degree of
    // three rather than two.
    // Instead, we explicitly add an interference with the high node of the fixed pair node.
    // TODO: This is too conservative at time for pair nodes, but the fact that fixed pair intervals
    //       can be unaligned on x86 complicates things.
    if (to->IsPair()) {
      InterferenceNode* high_node =
          physical_nodes[to->GetInterval()->GetHighInterval()->GetRegister()];
      DCHECK_EQ(to->GetInterval()->GetHighInterval()->GetRegister(),
                high_node->GetInterval()->GetRegister());
      from->AddInterference(high_node, /*guaranteed_not_interfering_yet*/ false);
    }
  } else {
    // Standard interference between two uncolored nodes.
    from->AddInterference(to, guaranteed_not_interfering_yet);
  }

  if (both_directions) {
    AddPotentialInterference(to, from, guaranteed_not_interfering_yet, /*both_directions*/ false);
  }
}

// Returns true if `in_node` represents an input interval of `out_node`, and the output interval
// is allowed to have the same register as the input interval.
// TODO: Ideally we should just produce correct intervals in liveness analysis.
//       We would need to refactor the current live interval layout to do so, which is
//       no small task.
static bool CheckInputOutputCanOverlap(InterferenceNode* in_node, InterferenceNode* out_node) {
  LiveInterval* output_interval = out_node->GetInterval();
  HInstruction* defined_by = output_interval->GetDefinedBy();
  if (defined_by == nullptr) {
    // This must not be a definition point.
    return false;
  }

  LocationSummary* locations = defined_by->GetLocations();
  if (locations->OutputCanOverlapWithInputs()) {
    // This instruction does not allow the output to reuse a register from an input.
    return false;
  }

  LiveInterval* input_interval = in_node->GetInterval();
  LiveInterval* next_sibling = input_interval->GetNextSibling();
  size_t def_position = defined_by->GetLifetimePosition();
  size_t use_position = def_position + 1;
  if (next_sibling != nullptr && next_sibling->GetStart() == use_position) {
    // The next sibling starts at the use position, so reusing the input register in the output
    // would clobber the input before it's moved into the sibling interval location.
    return false;
  }

  if (!input_interval->IsDeadAt(use_position) && input_interval->CoversSlow(use_position)) {
    // The input interval is live after the use position.
    return false;
  }

  HInputsRef inputs = defined_by->GetInputs();
  for (size_t i = 0; i < inputs.size(); ++i) {
    if (inputs[i]->GetLiveInterval()->GetSiblingAt(def_position) == input_interval) {
      DCHECK(input_interval->SameRegisterKind(*output_interval));
      return true;
    }
  }

  // The input interval was not an input for this instruction.
  return false;
}

void RegisterAllocatorGraphColor::BuildInterferenceGraph(
    const ArenaVector<LiveInterval*>& intervals,
    const ArenaVector<InterferenceNode*>& physical_nodes,
    ArenaVector<InterferenceNode*>* safepoints) {
  DCHECK(interval_node_map_.Empty() && prunable_nodes_.empty());
  // Build the interference graph efficiently by ordering range endpoints
  // by position and doing a linear sweep to find interferences. (That is, we
  // jump from endpoint to endpoint, maintaining a set of intervals live at each
  // point. If two nodes are ever in the live set at the same time, then they
  // interfere with each other.)
  //
  // We order by both position and (secondarily) by whether the endpoint
  // begins or ends a range; we want to process range endings before range
  // beginnings at the same position because they should not conflict.
  //
  // For simplicity, we create a tuple for each endpoint, and then sort the tuples.
  // Tuple contents: (position, is_range_beginning, node).
  ArenaVector<std::tuple<size_t, bool, InterferenceNode*>> range_endpoints(
      coloring_attempt_allocator_->Adapter(kArenaAllocRegisterAllocator));

  // We reserve plenty of space to avoid excessive copying.
  range_endpoints.reserve(4 * prunable_nodes_.size());

  for (LiveInterval* parent : intervals) {
    for (LiveInterval* sibling = parent; sibling != nullptr; sibling = sibling->GetNextSibling()) {
      LiveRange* range = sibling->GetFirstRange();
      if (range != nullptr) {
        InterferenceNode* node = new (coloring_attempt_allocator_) InterferenceNode(
            coloring_attempt_allocator_, sibling, node_id_counter_++, liveness_);
        interval_node_map_.Insert(std::make_pair(sibling, node));

        if (sibling->HasRegister()) {
          // Fixed nodes should alias the canonical node for the corresponding register.
          node->stage = NodeStage::kPrecolored;
          InterferenceNode* physical_node = physical_nodes[sibling->GetRegister()];
          node->SetAlias(physical_node);
          DCHECK_EQ(node->GetInterval()->GetRegister(),
                    physical_node->GetInterval()->GetRegister());
        } else if (sibling->IsSlowPathSafepoint()) {
          // Safepoint intervals are synthesized to count max live registers.
          // They will be processed separately after coloring.
          node->stage = NodeStage::kSafepoint;
          safepoints->push_back(node);
        } else {
          node->stage = NodeStage::kPrunable;
          prunable_nodes_.push_back(node);
        }

        while (range != nullptr) {
          range_endpoints.push_back(std::make_tuple(range->GetStart(), true, node));
          range_endpoints.push_back(std::make_tuple(range->GetEnd(), false, node));
          range = range->GetNext();
        }
      }
    }
  }

  // Sort the endpoints.
  // We explicitly ignore the third entry of each tuple (the node pointer) in order
  // to maintain determinism.
  std::sort(range_endpoints.begin(), range_endpoints.end(),
            [] (const std::tuple<size_t, bool, InterferenceNode*>& lhs,
                const std::tuple<size_t, bool, InterferenceNode*>& rhs) {
    return std::tie(std::get<0>(lhs), std::get<1>(lhs))
         < std::tie(std::get<0>(rhs), std::get<1>(rhs));
  });

  // Nodes live at the current position in the linear sweep.
  ArenaVector<InterferenceNode*> live(
      coloring_attempt_allocator_->Adapter(kArenaAllocRegisterAllocator));

  // Linear sweep. When we encounter the beginning of a range, we add the corresponding node to the
  // live set. When we encounter the end of a range, we remove the corresponding node
  // from the live set. Nodes interfere if they are in the live set at the same time.
  for (auto it = range_endpoints.begin(); it != range_endpoints.end(); ++it) {
    bool is_range_beginning;
    InterferenceNode* node;
    size_t position;
    // Extract information from the tuple, including the node this tuple represents.
    std::tie(position, is_range_beginning, node) = *it;

    if (is_range_beginning) {
      bool guaranteed_not_interfering_yet = position == node->GetInterval()->GetStart();
      for (InterferenceNode* conflicting : live) {
        DCHECK_NE(node, conflicting);
        if (CheckInputOutputCanOverlap(conflicting, node)) {
          // We do not add an interference, because the instruction represented by `node` allows
          // its output to share a register with an input, represented here by `conflicting`.
        } else {
          AddPotentialInterference(node, conflicting, guaranteed_not_interfering_yet);
        }
      }
      DCHECK(std::find(live.begin(), live.end(), node) == live.end());
      live.push_back(node);
    } else {
      // End of range.
      auto live_it = std::find(live.begin(), live.end(), node);
      DCHECK(live_it != live.end());
      live.erase(live_it);
    }
  }
  DCHECK(live.empty());
}

void RegisterAllocatorGraphColor::CreateCoalesceOpportunity(InterferenceNode* a,
                                                            InterferenceNode* b,
                                                            CoalesceKind kind,
                                                            size_t position) {
  DCHECK_EQ(a->IsPair(), b->IsPair())
      << "Nodes of different memory widths should never be coalesced";
  CoalesceOpportunity* opportunity =
      new (coloring_attempt_allocator_) CoalesceOpportunity(a, b, kind, position, liveness_);
  a->AddCoalesceOpportunity(opportunity);
  b->AddCoalesceOpportunity(opportunity);
  coalesce_worklist_.push(opportunity);
}

// When looking for coalesce opportunities, we use the interval_node_map_ to find the node
// corresponding to an interval. Note that not all intervals are in this map, notably the parents
// of constants and stack arguments. (However, these interval should not be involved in coalesce
// opportunities anyway, because they're not going to be in registers.)
void RegisterAllocatorGraphColor::FindCoalesceOpportunities() {
  DCHECK(coalesce_worklist_.empty());

  for (InterferenceNode* node : prunable_nodes_) {
    LiveInterval* interval = node->GetInterval();

    // Coalesce siblings.
    LiveInterval* next_sibling = interval->GetNextSibling();
    if (next_sibling != nullptr && interval->GetEnd() == next_sibling->GetStart()) {
      auto it = interval_node_map_.Find(next_sibling);
      if (it != interval_node_map_.end()) {
        InterferenceNode* sibling_node = it->second;
        CreateCoalesceOpportunity(node,
                                  sibling_node,
                                  CoalesceKind::kAdjacentSibling,
                                  interval->GetEnd());
      }
    }

    // Coalesce fixed outputs with this interval if this interval is an adjacent sibling.
    LiveInterval* parent = interval->GetParent();
    if (parent->HasRegister()
        && parent->GetNextSibling() == interval
        && parent->GetEnd() == interval->GetStart()) {
      auto it = interval_node_map_.Find(parent);
      if (it != interval_node_map_.end()) {
        InterferenceNode* parent_node = it->second;
        CreateCoalesceOpportunity(node,
                                  parent_node,
                                  CoalesceKind::kFixedOutputSibling,
                                  parent->GetEnd());
      }
    }

    // Try to prevent moves across blocks.
    // Note that this does not lead to many succeeding coalesce attempts, so could be removed
    // if found to add to compile time.
    if (interval->IsSplit() && liveness_.IsAtBlockBoundary(interval->GetStart() / 2)) {
      // If the start of this interval is at a block boundary, we look at the
      // location of the interval in blocks preceding the block this interval
      // starts at. This can avoid a move between the two blocks.
      HBasicBlock* block = liveness_.GetBlockFromPosition(interval->GetStart() / 2);
      for (HBasicBlock* predecessor : block->GetPredecessors()) {
        size_t position = predecessor->GetLifetimeEnd() - 1;
        LiveInterval* existing = interval->GetParent()->GetSiblingAt(position);
        if (existing != nullptr) {
          auto it = interval_node_map_.Find(existing);
          if (it != interval_node_map_.end()) {
            InterferenceNode* existing_node = it->second;
            CreateCoalesceOpportunity(node,
                                      existing_node,
                                      CoalesceKind::kNonlinearControlFlow,
                                      position);
          }
        }
      }
    }

    // Coalesce phi inputs with the corresponding output.
    HInstruction* defined_by = interval->GetDefinedBy();
    if (defined_by != nullptr && defined_by->IsPhi()) {
      const ArenaVector<HBasicBlock*>& predecessors = defined_by->GetBlock()->GetPredecessors();
      HInputsRef inputs = defined_by->GetInputs();

      for (size_t i = 0, e = inputs.size(); i < e; ++i) {
        // We want the sibling at the end of the appropriate predecessor block.
        size_t position = predecessors[i]->GetLifetimeEnd() - 1;
        LiveInterval* input_interval = inputs[i]->GetLiveInterval()->GetSiblingAt(position);

        auto it = interval_node_map_.Find(input_interval);
        if (it != interval_node_map_.end()) {
          InterferenceNode* input_node = it->second;
          CreateCoalesceOpportunity(node, input_node, CoalesceKind::kPhi, position);
        }
      }
    }

    // Coalesce output with first input when policy is kSameAsFirstInput.
    if (defined_by != nullptr) {
      Location out = defined_by->GetLocations()->Out();
      if (out.IsUnallocated() && out.GetPolicy() == Location::kSameAsFirstInput) {
        LiveInterval* input_interval
            = defined_by->InputAt(0)->GetLiveInterval()->GetSiblingAt(interval->GetStart() - 1);
        // TODO: Could we consider lifetime holes here?
        if (input_interval->GetEnd() == interval->GetStart()) {
          auto it = interval_node_map_.Find(input_interval);
          if (it != interval_node_map_.end()) {
            InterferenceNode* input_node = it->second;
            CreateCoalesceOpportunity(node,
                                      input_node,
                                      CoalesceKind::kFirstInput,
                                      interval->GetStart());
          }
        }
      }
    }

    // An interval that starts an instruction (that is, it is not split), may
    // re-use the registers used by the inputs of that instruction, based on the
    // location summary.
    if (defined_by != nullptr) {
      DCHECK(!interval->IsSplit());
      LocationSummary* locations = defined_by->GetLocations();
      if (!locations->OutputCanOverlapWithInputs()) {
        HInputsRef inputs = defined_by->GetInputs();
        for (size_t i = 0; i < inputs.size(); ++i) {
          size_t def_point = defined_by->GetLifetimePosition();
          // TODO: Getting the sibling at the def_point might not be quite what we want
          //       for fixed inputs, since the use will be *at* the def_point rather than after.
          LiveInterval* input_interval = inputs[i]->GetLiveInterval()->GetSiblingAt(def_point);
          if (input_interval != nullptr &&
              input_interval->HasHighInterval() == interval->HasHighInterval()) {
            auto it = interval_node_map_.Find(input_interval);
            if (it != interval_node_map_.end()) {
              InterferenceNode* input_node = it->second;
              CreateCoalesceOpportunity(node,
                                        input_node,
                                        CoalesceKind::kAnyInput,
                                        interval->GetStart());
            }
          }
        }
      }
    }

    // Try to prevent moves into fixed input locations.
    UsePosition* use = interval->GetFirstUse();
    for (; use != nullptr && use->GetPosition() <= interval->GetStart(); use = use->GetNext()) {
      // Skip past uses before the start of this interval.
    }
    for (; use != nullptr && use->GetPosition() <= interval->GetEnd(); use = use->GetNext()) {
      HInstruction* user = use->GetUser();
      if (user == nullptr) {
        // User may be null for certain intervals, such as temp intervals.
        continue;
      }
      LocationSummary* locations = user->GetLocations();
      Location input = locations->InAt(use->GetInputIndex());
      if (input.IsRegister() || input.IsFpuRegister()) {
        // TODO: Could try to handle pair interval too, but coalescing with fixed pair nodes
        //       is currently not supported.
        InterferenceNode* fixed_node = input.IsRegister()
            ? physical_core_nodes_[input.reg()]
            : physical_fp_nodes_[input.reg()];
        CreateCoalesceOpportunity(node,
                                  fixed_node,
                                  CoalesceKind::kFixedInput,
                                  user->GetLifetimePosition());
      }
    }
  }  // for node in prunable_nodes
}

// The order in which we color nodes is important. To guarantee forward progress,
// we prioritize intervals that require registers, and after that we prioritize
// short intervals. That way, if we fail to color a node, it either won't require a
// register, or it will be a long interval that can be split in order to make the
// interference graph sparser.
// To improve code quality, we prioritize intervals used frequently in deeply nested loops.
// (This metric is secondary to the forward progress requirements above.)
// TODO: May also want to consider:
// - Constants (since they can be rematerialized)
// - Allocated spill slots
bool RegisterAllocatorGraphColor::HasGreaterNodePriority(const InterferenceNode* lhs,
                                                         const InterferenceNode* rhs) {
  // (1) Prioritize the node that requires a color.
  if (lhs->RequiresColor() != rhs->RequiresColor()) {
    return lhs->RequiresColor();
  }

  // (2) Prioritize the interval that has a higher spill weight.
  return lhs->GetSpillWeight() > rhs->GetSpillWeight();
}

bool RegisterAllocatorGraphColor::CmpCoalesceOpportunity(const CoalesceOpportunity* lhs,
                                                         const CoalesceOpportunity* rhs) {
  return lhs->priority < rhs->priority;
}

static bool IsLowDegreeNode(InterferenceNode* node, size_t num_regs) {
  return node->GetOutDegree() < num_regs;
}

static bool IsHighDegreeNode(InterferenceNode* node, size_t num_regs) {
  return !IsLowDegreeNode(node, num_regs);
}

void RegisterAllocatorGraphColor::PruneInterferenceGraph(size_t num_regs) {
  DCHECK(pruned_nodes_.empty()
      && simplify_worklist_.empty()
      && freeze_worklist_.empty()
      && spill_worklist_.empty());
  // When pruning the graph, we refer to nodes with degree less than num_regs as low degree nodes,
  // and all others as high degree nodes. The distinction is important: low degree nodes are
  // guaranteed a color, while high degree nodes are not.

  // Build worklists. Note that the coalesce worklist has already been
  // filled by FindCoalesceOpportunities().
  for (InterferenceNode* node : prunable_nodes_) {
    DCHECK(!node->IsPrecolored()) << "Fixed nodes should never be pruned";
    DCHECK(!node->GetInterval()->IsSlowPathSafepoint()) << "Safepoint nodes should never be pruned";
    if (IsLowDegreeNode(node, num_regs)) {
      if (node->GetCoalesceOpportunities().empty()) {
        // Simplify Worklist.
        node->stage = NodeStage::kSimplifyWorklist;
        simplify_worklist_.push_back(node);
      } else {
        // Freeze Worklist.
        node->stage = NodeStage::kFreezeWorklist;
        freeze_worklist_.push_back(node);
      }
    } else {
      // Spill worklist.
      node->stage = NodeStage::kSpillWorklist;
      spill_worklist_.push(node);
    }
  }

  // Prune graph.
  // Note that we do not remove a node from its current worklist if it moves to another, so it may
  // be in multiple worklists at once; the node's `phase` says which worklist it is really in.
  while (true) {
    if (!simplify_worklist_.empty()) {
      // Prune low-degree nodes.
      // TODO: pop_back() should work as well, but it didn't; we get a
      //       failed check while pruning. We should look into this.
      InterferenceNode* node = simplify_worklist_.front();
      simplify_worklist_.pop_front();
      DCHECK_EQ(node->stage, NodeStage::kSimplifyWorklist) << "Cannot move from simplify list";
      DCHECK_LT(node->GetOutDegree(), num_regs) << "Nodes in simplify list should be low degree";
      DCHECK(!node->IsMoveRelated()) << "Nodes in simplify list should not be move related";
      PruneNode(node, num_regs);
    } else if (!coalesce_worklist_.empty()) {
      // Coalesce.
      CoalesceOpportunity* opportunity = coalesce_worklist_.top();
      coalesce_worklist_.pop();
      if (opportunity->stage == CoalesceStage::kWorklist) {
        Coalesce(opportunity, num_regs);
      }
    } else if (!freeze_worklist_.empty()) {
      // Freeze moves and prune a low-degree move-related node.
      InterferenceNode* node = freeze_worklist_.front();
      freeze_worklist_.pop_front();
      if (node->stage == NodeStage::kFreezeWorklist) {
        DCHECK_LT(node->GetOutDegree(), num_regs) << "Nodes in freeze list should be low degree";
        DCHECK(node->IsMoveRelated()) << "Nodes in freeze list should be move related";
        FreezeMoves(node, num_regs);
        PruneNode(node, num_regs);
      }
    } else if (!spill_worklist_.empty()) {
      // We spill the lowest-priority node, because pruning a node earlier
      // gives it a higher chance of being spilled.
      InterferenceNode* node = spill_worklist_.top();
      spill_worklist_.pop();
      if (node->stage == NodeStage::kSpillWorklist) {
        DCHECK_GE(node->GetOutDegree(), num_regs) << "Nodes in spill list should be high degree";
        FreezeMoves(node, num_regs);
        PruneNode(node, num_regs);
      }
    } else {
      // Pruning complete.
      break;
    }
  }
  DCHECK_EQ(prunable_nodes_.size(), pruned_nodes_.size());
}

void RegisterAllocatorGraphColor::EnableCoalesceOpportunities(InterferenceNode* node) {
  for (CoalesceOpportunity* opportunity : node->GetCoalesceOpportunities()) {
    if (opportunity->stage == CoalesceStage::kActive) {
      opportunity->stage = CoalesceStage::kWorklist;
      coalesce_worklist_.push(opportunity);
    }
  }
}

void RegisterAllocatorGraphColor::PruneNode(InterferenceNode* node,
                                            size_t num_regs) {
  DCHECK_NE(node->stage, NodeStage::kPruned);
  DCHECK(!node->IsPrecolored());
  node->stage = NodeStage::kPruned;
  pruned_nodes_.push(node);

  for (InterferenceNode* adj : node->GetAdjacentNodes()) {
    DCHECK(!adj->GetInterval()->IsSlowPathSafepoint())
        << "Nodes should never interfere with synthesized safepoint nodes";
    DCHECK_NE(adj->stage, NodeStage::kPruned) << "Should be no interferences with pruned nodes";

    if (adj->IsPrecolored()) {
      // No effect on pre-colored nodes; they're never pruned.
    } else {
      // Remove the interference.
      bool was_high_degree = IsHighDegreeNode(adj, num_regs);
      DCHECK(adj->ContainsInterference(node))
          << "Missing reflexive interference from non-fixed node";
      adj->RemoveInterference(node);

      // Handle transitions from high degree to low degree.
      if (was_high_degree && IsLowDegreeNode(adj, num_regs)) {
        EnableCoalesceOpportunities(adj);
        for (InterferenceNode* adj_adj : adj->GetAdjacentNodes()) {
          EnableCoalesceOpportunities(adj_adj);
        }

        DCHECK_EQ(adj->stage, NodeStage::kSpillWorklist);
        if (adj->IsMoveRelated()) {
          adj->stage = NodeStage::kFreezeWorklist;
          freeze_worklist_.push_back(adj);
        } else {
          adj->stage = NodeStage::kSimplifyWorklist;
          simplify_worklist_.push_back(adj);
        }
      }
    }
  }
}

void RegisterAllocatorGraphColor::CheckTransitionFromFreezeWorklist(InterferenceNode* node,
                                                                    size_t num_regs) {
  if (IsLowDegreeNode(node, num_regs) && !node->IsMoveRelated()) {
    DCHECK_EQ(node->stage, NodeStage::kFreezeWorklist);
    node->stage = NodeStage::kSimplifyWorklist;
    simplify_worklist_.push_back(node);
  }
}

void RegisterAllocatorGraphColor::FreezeMoves(InterferenceNode* node,
                                              size_t num_regs) {
  for (CoalesceOpportunity* opportunity : node->GetCoalesceOpportunities()) {
    if (opportunity->stage == CoalesceStage::kDefunct) {
      // Constrained moves should remain constrained, since they will not be considered
      // during last-chance coalescing.
    } else {
      opportunity->stage = CoalesceStage::kInactive;
    }
    InterferenceNode* other = opportunity->node_a->GetAlias() == node
        ? opportunity->node_b->GetAlias()
        : opportunity->node_a->GetAlias();
    if (other != node && other->stage == NodeStage::kFreezeWorklist) {
      DCHECK(IsLowDegreeNode(node, num_regs));
      CheckTransitionFromFreezeWorklist(other, num_regs);
    }
  }
}

bool RegisterAllocatorGraphColor::PrecoloredHeuristic(InterferenceNode* from,
                                                      InterferenceNode* into,
                                                      size_t num_regs) {
  if (!into->IsPrecolored()) {
    // The uncolored heuristic will cover this case.
    return false;
  }
  if (from->IsPair() || into->IsPair()) {
    // TODO: Merging from a pair node is currently not supported, since fixed pair nodes
    //       are currently represented as two single fixed nodes in the graph, and `into` is
    //       only one of them. (We may lose the implicit connections to the second one in a merge.)
    return false;
  }

  // If all adjacent nodes of `from` are "ok", then we can conservatively merge with `into`.
  // Reasons an adjacent node `adj` can be "ok":
  // (1) If `adj` is low degree, interference with `into` will not affect its existing
  //     colorable guarantee. (Notice that coalescing cannot increase its degree.)
  // (2) If `adj` is pre-colored, it already interferes with `into`. See (3).
  // (3) If there's already an interference with `into`, coalescing will not add interferences.
  for (InterferenceNode* adj : from->GetAdjacentNodes()) {
    if (IsLowDegreeNode(adj, num_regs) || adj->IsPrecolored() || adj->ContainsInterference(into)) {
      // Ok.
    } else {
      return false;
    }
  }
  return true;
}

bool RegisterAllocatorGraphColor::UncoloredHeuristic(InterferenceNode* from,
                                                     InterferenceNode* into,
                                                     size_t num_regs) {
  if (into->IsPrecolored()) {
    // The pre-colored heuristic will handle this case.
    return false;
  }

  // Arbitrary cap to improve compile time. Tests show that this has negligible affect
  // on generated code.
  if (from->GetOutDegree() + into->GetOutDegree() > 2 * num_regs) {
    return false;
  }

  // It's safe to coalesce two nodes if the resulting node has fewer than `num_regs` neighbors
  // of high degree. (Low degree neighbors can be ignored, because they will eventually be
  // pruned from the interference graph in the simplify stage.)
  size_t high_degree_interferences = 0;
  for (InterferenceNode* adj : from->GetAdjacentNodes()) {
    if (IsHighDegreeNode(adj, num_regs)) {
      high_degree_interferences += from->EdgeWeightWith(adj);
    }
  }
  for (InterferenceNode* adj : into->GetAdjacentNodes()) {
    if (IsHighDegreeNode(adj, num_regs)) {
      if (from->ContainsInterference(adj)) {
        // We've already counted this adjacent node.
        // Furthermore, its degree will decrease if coalescing succeeds. Thus, it's possible that
        // we should not have counted it at all. (This extends the textbook Briggs coalescing test,
        // but remains conservative.)
        if (adj->GetOutDegree() - into->EdgeWeightWith(adj) < num_regs) {
          high_degree_interferences -= from->EdgeWeightWith(adj);
        }
      } else {
        high_degree_interferences += into->EdgeWeightWith(adj);
      }
    }
  }

  return high_degree_interferences < num_regs;
}

void RegisterAllocatorGraphColor::Combine(InterferenceNode* from,
                                          InterferenceNode* into,
                                          size_t num_regs) {
  from->SetAlias(into);

  // Add interferences.
  for (InterferenceNode* adj : from->GetAdjacentNodes()) {
    bool was_low_degree = IsLowDegreeNode(adj, num_regs);
    AddPotentialInterference(adj, into, /*guaranteed_not_interfering_yet*/ false);
    if (was_low_degree && IsHighDegreeNode(adj, num_regs)) {
      // This is a (temporary) transition to a high degree node. Its degree will decrease again
      // when we prune `from`, but it's best to be consistent about the current worklist.
      adj->stage = NodeStage::kSpillWorklist;
      spill_worklist_.push(adj);
    }
  }

  // Add coalesce opportunities.
  for (CoalesceOpportunity* opportunity : from->GetCoalesceOpportunities()) {
    if (opportunity->stage != CoalesceStage::kDefunct) {
      into->AddCoalesceOpportunity(opportunity);
    }
  }
  EnableCoalesceOpportunities(from);

  // Prune and update worklists.
  PruneNode(from, num_regs);
  if (IsLowDegreeNode(into, num_regs)) {
    // Coalesce(...) takes care of checking for a transition to the simplify worklist.
    DCHECK_EQ(into->stage, NodeStage::kFreezeWorklist);
  } else if (into->stage == NodeStage::kFreezeWorklist) {
    // This is a transition to a high degree node.
    into->stage = NodeStage::kSpillWorklist;
    spill_worklist_.push(into);
  } else {
    DCHECK(into->stage == NodeStage::kSpillWorklist || into->stage == NodeStage::kPrecolored);
  }
}

void RegisterAllocatorGraphColor::Coalesce(CoalesceOpportunity* opportunity,
                                           size_t num_regs) {
  InterferenceNode* from = opportunity->node_a->GetAlias();
  InterferenceNode* into = opportunity->node_b->GetAlias();
  DCHECK_NE(from->stage, NodeStage::kPruned);
  DCHECK_NE(into->stage, NodeStage::kPruned);

  if (from->IsPrecolored()) {
    // If we have one pre-colored node, make sure it's the `into` node.
    std::swap(from, into);
  }

  if (from == into) {
    // These nodes have already been coalesced.
    opportunity->stage = CoalesceStage::kDefunct;
    CheckTransitionFromFreezeWorklist(from, num_regs);
  } else if (from->IsPrecolored() || from->ContainsInterference(into)) {
    // These nodes interfere.
    opportunity->stage = CoalesceStage::kDefunct;
    CheckTransitionFromFreezeWorklist(from, num_regs);
    CheckTransitionFromFreezeWorklist(into, num_regs);
  } else if (PrecoloredHeuristic(from, into, num_regs)
          || UncoloredHeuristic(from, into, num_regs)) {
    // We can coalesce these nodes.
    opportunity->stage = CoalesceStage::kDefunct;
    Combine(from, into, num_regs);
    CheckTransitionFromFreezeWorklist(into, num_regs);
  } else {
    // We cannot coalesce, but we may be able to later.
    opportunity->stage = CoalesceStage::kActive;
  }
}

// Build a mask with a bit set for each register assigned to some
// interval in `intervals`.
template <typename Container>
static std::bitset<kMaxNumRegs> BuildConflictMask(Container& intervals) {
  std::bitset<kMaxNumRegs> conflict_mask;
  for (InterferenceNode* adjacent : intervals) {
    LiveInterval* conflicting = adjacent->GetInterval();
    if (conflicting->HasRegister()) {
      conflict_mask.set(conflicting->GetRegister());
      if (conflicting->HasHighInterval()) {
        DCHECK(conflicting->GetHighInterval()->HasRegister());
        conflict_mask.set(conflicting->GetHighInterval()->GetRegister());
      }
    } else {
      DCHECK(!conflicting->HasHighInterval()
          || !conflicting->GetHighInterval()->HasRegister());
    }
  }
  return conflict_mask;
}

bool RegisterAllocatorGraphColor::IsCallerSave(size_t reg, bool processing_core_regs) {
  return processing_core_regs
      ? !codegen_->IsCoreCalleeSaveRegister(reg)
      : !codegen_->IsCoreCalleeSaveRegister(reg);
}

static bool RegisterIsAligned(size_t reg) {
  return reg % 2 == 0;
}

static size_t FindFirstZeroInConflictMask(std::bitset<kMaxNumRegs> conflict_mask) {
  // We use CTZ (count trailing zeros) to quickly find the lowest 0 bit.
  // Note that CTZ is undefined if all bits are 0, so we special-case it.
  return conflict_mask.all() ? conflict_mask.size() : CTZ(~conflict_mask.to_ulong());
}

bool RegisterAllocatorGraphColor::ColorInterferenceGraph(size_t num_regs,
                                                         bool processing_core_regs) {
  DCHECK_LE(num_regs, kMaxNumRegs) << "kMaxNumRegs is too small";
  ArenaVector<LiveInterval*> colored_intervals(
      coloring_attempt_allocator_->Adapter(kArenaAllocRegisterAllocator));
  bool successful = true;

  while (!pruned_nodes_.empty()) {
    InterferenceNode* node = pruned_nodes_.top();
    pruned_nodes_.pop();
    LiveInterval* interval = node->GetInterval();
    size_t reg = 0;

    InterferenceNode* alias = node->GetAlias();
    if (alias != node) {
      // This node was coalesced with another.
      LiveInterval* alias_interval = alias->GetInterval();
      if (alias_interval->HasRegister()) {
        reg = alias_interval->GetRegister();
        DCHECK(!BuildConflictMask(node->GetAdjacentNodes())[reg])
            << "This node conflicts with the register it was coalesced with";
      } else {
        DCHECK(false) << node->GetOutDegree() << " " << alias->GetOutDegree() << " "
            << "Move coalescing was not conservative, causing a node to be coalesced "
            << "with another node that could not be colored";
        if (interval->RequiresRegister()) {
          successful = false;
        }
      }
    } else {
      // Search for free register(s).
      std::bitset<kMaxNumRegs> conflict_mask = BuildConflictMask(node->GetAdjacentNodes());
      if (interval->HasHighInterval()) {
        // Note that the graph coloring allocator assumes that pair intervals are aligned here,
        // excluding pre-colored pair intervals (which can currently be unaligned on x86). If we
        // change the alignment requirements here, we will have to update the algorithm (e.g.,
        // be more conservative about the weight of edges adjacent to pair nodes.)
        while (reg < num_regs - 1 && (conflict_mask[reg] || conflict_mask[reg + 1])) {
          reg += 2;
        }

        // Try to use a caller-save register first.
        for (size_t i = 0; i < num_regs - 1; i += 2) {
          bool low_caller_save  = IsCallerSave(i, processing_core_regs);
          bool high_caller_save = IsCallerSave(i + 1, processing_core_regs);
          if (!conflict_mask[i] && !conflict_mask[i + 1]) {
            if (low_caller_save && high_caller_save) {
              reg = i;
              break;
            } else if (low_caller_save || high_caller_save) {
              reg = i;
              // Keep looking to try to get both parts in caller-save registers.
            }
          }
        }
      } else {
        // Not a pair interval.
        reg = FindFirstZeroInConflictMask(conflict_mask);

        // Try to use caller-save registers first.
        for (size_t i = 0; i < num_regs; ++i) {
          if (!conflict_mask[i] && IsCallerSave(i, processing_core_regs)) {
            reg = i;
            break;
          }
        }
      }

      // Last-chance coalescing.
      for (CoalesceOpportunity* opportunity : node->GetCoalesceOpportunities()) {
        if (opportunity->stage == CoalesceStage::kDefunct) {
          continue;
        }
        LiveInterval* other_interval = opportunity->node_a->GetAlias() == node
            ? opportunity->node_b->GetAlias()->GetInterval()
            : opportunity->node_a->GetAlias()->GetInterval();
        if (other_interval->HasRegister()) {
          size_t coalesce_register = other_interval->GetRegister();
          if (interval->HasHighInterval()) {
            if (!conflict_mask[coalesce_register] &&
                !conflict_mask[coalesce_register + 1] &&
                RegisterIsAligned(coalesce_register)) {
              reg = coalesce_register;
              break;
            }
          } else if (!conflict_mask[coalesce_register]) {
            reg = coalesce_register;
            break;
          }
        }
      }
    }

    if (reg < (interval->HasHighInterval() ? num_regs - 1 : num_regs)) {
      // Assign register.
      DCHECK(!interval->HasRegister());
      interval->SetRegister(reg);
      colored_intervals.push_back(interval);
      if (interval->HasHighInterval()) {
        DCHECK(!interval->GetHighInterval()->HasRegister());
        interval->GetHighInterval()->SetRegister(reg + 1);
        colored_intervals.push_back(interval->GetHighInterval());
      }
    } else if (interval->RequiresRegister()) {
      // The interference graph is too dense to color. Make it sparser by
      // splitting this live interval.
      successful = false;
      SplitAtRegisterUses(interval);
      // We continue coloring, because there may be additional intervals that cannot
      // be colored, and that we should split.
    } else {
      // Spill.
      AllocateSpillSlotFor(interval);
    }
  }

  // If unsuccessful, reset all register assignments.
  if (!successful) {
    for (LiveInterval* interval : colored_intervals) {
      interval->ClearRegister();
    }
  }

  return successful;
}

size_t RegisterAllocatorGraphColor::ComputeMaxSafepointLiveRegisters(
    const ArenaVector<InterferenceNode*>& safepoints) {
  size_t max_safepoint_live_regs = 0;
  for (InterferenceNode* safepoint : safepoints) {
    DCHECK(safepoint->GetInterval()->IsSlowPathSafepoint());
    std::bitset<kMaxNumRegs> conflict_mask = BuildConflictMask(safepoint->GetAdjacentNodes());
    size_t live_regs = conflict_mask.count();
    max_safepoint_live_regs = std::max(max_safepoint_live_regs, live_regs);
  }
  return max_safepoint_live_regs;
}

void RegisterAllocatorGraphColor::AllocateSpillSlotFor(LiveInterval* interval) {
  LiveInterval* parent = interval->GetParent();
  HInstruction* defined_by = parent->GetDefinedBy();
  if (parent->HasSpillSlot()) {
    // We already have a spill slot for this value that we can reuse.
  } else if (defined_by->IsParameterValue()) {
    // Parameters already have a stack slot.
    parent->SetSpillSlot(codegen_->GetStackSlotOfParameter(defined_by->AsParameterValue()));
  } else if (defined_by->IsCurrentMethod()) {
    // The current method is always at spill slot 0.
    parent->SetSpillSlot(0);
  } else if (defined_by->IsConstant()) {
    // Constants don't need a spill slot.
  } else {
    // Allocate a spill slot based on type.
    size_t* spill_slot_counter;
    switch (interval->GetType()) {
      case Primitive::kPrimDouble:
        spill_slot_counter = &double_spill_slot_counter_;
        break;
      case Primitive::kPrimLong:
        spill_slot_counter = &long_spill_slot_counter_;
        break;
      case Primitive::kPrimFloat:
        spill_slot_counter = &float_spill_slot_counter_;
        break;
      case Primitive::kPrimNot:
      case Primitive::kPrimInt:
      case Primitive::kPrimChar:
      case Primitive::kPrimByte:
      case Primitive::kPrimBoolean:
      case Primitive::kPrimShort:
        spill_slot_counter = &int_spill_slot_counter_;
        break;
      case Primitive::kPrimVoid:
        LOG(FATAL) << "Unexpected type for interval " << interval->GetType();
        UNREACHABLE();
    }

    parent->SetSpillSlot(*spill_slot_counter);
    *spill_slot_counter += parent->NeedsTwoSpillSlots() ? 2 : 1;
    // TODO: Could color stack slots if we wanted to, even if
    //       it's just a trivial coloring. See the linear scan implementation,
    //       which simply reuses spill slots for values whose live intervals
    //       have already ended.
  }
}

}  // namespace art