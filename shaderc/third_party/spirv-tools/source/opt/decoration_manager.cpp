// Copyright (c) 2017 Pierre Moreau
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "decoration_manager.h"

#include <algorithm>
#include <set>
#include <stack>

#include "ir_context.h"

namespace spvtools {
namespace opt {
namespace analysis {

void DecorationManager::RemoveDecorationsFrom(
    uint32_t id, std::function<bool(const ir::Instruction&)> pred) {
  const auto ids_iter = id_to_decoration_insts_.find(id);
  if (ids_iter == id_to_decoration_insts_.end()) return;

  TargetData& decorations_info = ids_iter->second;
  auto context = module_->context();
  std::vector<ir::Instruction*> insts_to_kill;
  const bool is_group = !decorations_info.decorate_insts.empty();

  // Schedule all direct decorations for removal if instructed as such by
  // |pred|.
  for (ir::Instruction* inst : decorations_info.direct_decorations)
    if (pred(*inst)) insts_to_kill.push_back(inst);

  // For all groups being directly applied to |id|, remove |id| (and the
  // literal if |inst| is an OpGroupMemberDecorate) from the instruction
  // applying the group.
  std::unordered_set<const ir::Instruction*> indirect_decorations_to_remove;
  for (ir::Instruction* inst : decorations_info.indirect_decorations) {
    assert(inst->opcode() == SpvOpGroupDecorate ||
           inst->opcode() == SpvOpGroupMemberDecorate);

    std::vector<ir::Instruction*> group_decorations_to_keep;
    const uint32_t group_id = inst->GetSingleWordInOperand(0u);
    const auto group_iter = id_to_decoration_insts_.find(group_id);
    assert(group_iter != id_to_decoration_insts_.end() &&
           "Unknown decoration group");
    const auto& group_decorations = group_iter->second.direct_decorations;
    for (ir::Instruction* decoration : group_decorations) {
      if (!pred(*decoration)) group_decorations_to_keep.push_back(decoration);
    }

    // If all decorations should be kept, move to the next group
    if (group_decorations_to_keep.size() == group_decorations.size()) continue;

    // Otherwise, remove |id| from the targets of |group_id|
    const uint32_t stride = inst->opcode() == SpvOpGroupDecorate ? 1u : 2u;
    bool was_modified = false;
    for (uint32_t i = 1u; i < inst->NumInOperands();) {
      if (inst->GetSingleWordInOperand(i) != id) {
        i += stride;
        continue;
      }

      const uint32_t last_operand_index = inst->NumInOperands() - stride;
      if (i < last_operand_index)
        inst->GetInOperand(i) = inst->GetInOperand(last_operand_index);
      // Remove the associated literal, if it exists.
      if (stride == 2u) {
        if (i < last_operand_index)
          inst->GetInOperand(i + 1u) =
              inst->GetInOperand(last_operand_index + 1u);
        inst->RemoveInOperand(last_operand_index + 1u);
      }
      inst->RemoveInOperand(last_operand_index);
      was_modified = true;
    }

    // If the instruction has no targets left, remove the instruction
    // altogether.
    if (inst->NumInOperands() == 1u) {
      indirect_decorations_to_remove.emplace(inst);
      insts_to_kill.push_back(inst);
    } else if (was_modified) {
      context->ForgetUses(inst);
      indirect_decorations_to_remove.emplace(inst);
      context->AnalyzeUses(inst);
    }

    // If only some of the decorations should be kept, clone them and apply
    // them directly to |id|.
    if (!group_decorations_to_keep.empty()) {
      for (ir::Instruction* decoration : group_decorations_to_keep) {
        // simply clone decoration and change |group_id| to |id|
        std::unique_ptr<ir::Instruction> new_inst(
            decoration->Clone(module_->context()));
        new_inst->SetInOperand(0, {id});
        module_->AddAnnotationInst(std::move(new_inst));
        auto decoration_iter = --module_->annotation_end();
        context->AnalyzeUses(&*decoration_iter);
      }
    }
  }

  auto& indirect_decorations = decorations_info.indirect_decorations;
  indirect_decorations.erase(
      std::remove_if(
          indirect_decorations.begin(), indirect_decorations.end(),
          [&indirect_decorations_to_remove](const ir::Instruction* inst) {
            return indirect_decorations_to_remove.count(inst);
          }),
      indirect_decorations.end());

  for (ir::Instruction* inst : insts_to_kill) context->KillInst(inst);
  insts_to_kill.clear();

  // Schedule all instructions applying the group for removal if this group no
  // longer applies decorations, either directly or indirectly.
  if (is_group && decorations_info.direct_decorations.empty() &&
      decorations_info.indirect_decorations.empty()) {
    for (ir::Instruction* inst : decorations_info.decorate_insts)
      insts_to_kill.push_back(inst);
  }
  for (ir::Instruction* inst : insts_to_kill) context->KillInst(inst);

  if (decorations_info.direct_decorations.empty() &&
      decorations_info.indirect_decorations.empty() &&
      decorations_info.decorate_insts.empty()) {
    id_to_decoration_insts_.erase(ids_iter);

    // Remove the OpDecorationGroup defining this group.
    if (is_group) context->KillInst(context->get_def_use_mgr()->GetDef(id));
  }
}

std::vector<ir::Instruction*> DecorationManager::GetDecorationsFor(
    uint32_t id, bool include_linkage) {
  return InternalGetDecorationsFor<ir::Instruction*>(id, include_linkage);
}

std::vector<const ir::Instruction*> DecorationManager::GetDecorationsFor(
    uint32_t id, bool include_linkage) const {
  return const_cast<DecorationManager*>(this)
      ->InternalGetDecorationsFor<const ir::Instruction*>(id, include_linkage);
}

bool DecorationManager::HaveTheSameDecorations(uint32_t id1,
                                               uint32_t id2) const {
  using InstructionList = std::vector<const ir::Instruction*>;
  using DecorationSet = std::set<std::u32string>;

  const InstructionList decorations_for1 = GetDecorationsFor(id1, false);
  const InstructionList decorations_for2 = GetDecorationsFor(id2, false);

  // This function splits the decoration instructions into different sets,
  // based on their opcode; only OpDecorate, OpDecorateId,
  // OpDecorateStringGOOGLE, and OpMemberDecorate are considered, the other
  // opcodes are ignored.
  const auto fillDecorationSets =
      [](const InstructionList& decoration_list, DecorationSet* decorate_set,
         DecorationSet* decorate_id_set, DecorationSet* decorate_string_set,
         DecorationSet* member_decorate_set) {
        for (const ir::Instruction* inst : decoration_list) {
          std::u32string decoration_payload;
          // Ignore the opcode and the target as we do not want them to be
          // compared.
          for (uint32_t i = 1u; i < inst->NumInOperands(); ++i) {
            for (uint32_t word : inst->GetInOperand(i).words) {
              decoration_payload.push_back(word);
            }
          }

          switch (inst->opcode()) {
            case SpvOpDecorate:
              decorate_set->emplace(std::move(decoration_payload));
              break;
            case SpvOpMemberDecorate:
              member_decorate_set->emplace(std::move(decoration_payload));
              break;
            case SpvOpDecorateId:
              decorate_id_set->emplace(std::move(decoration_payload));
              break;
            case SpvOpDecorateStringGOOGLE:
              decorate_string_set->emplace(std::move(decoration_payload));
              break;
            default:
              break;
          }
        }
      };

  DecorationSet decorate_set_for1;
  DecorationSet decorate_id_set_for1;
  DecorationSet decorate_string_set_for1;
  DecorationSet member_decorate_set_for1;
  fillDecorationSets(decorations_for1, &decorate_set_for1,
                     &decorate_id_set_for1, &decorate_string_set_for1,
                     &member_decorate_set_for1);

  DecorationSet decorate_set_for2;
  DecorationSet decorate_id_set_for2;
  DecorationSet decorate_string_set_for2;
  DecorationSet member_decorate_set_for2;
  fillDecorationSets(decorations_for2, &decorate_set_for2,
                     &decorate_id_set_for2, &decorate_string_set_for2,
                     &member_decorate_set_for2);

  const bool result = decorate_set_for1 == decorate_set_for2 &&
                      decorate_id_set_for1 == decorate_id_set_for2 &&
                      member_decorate_set_for1 == member_decorate_set_for2 &&
                      // Compare string sets last in case the strings are long.
                      decorate_string_set_for1 == decorate_string_set_for2;
  return result;
}

// TODO(pierremoreau): If OpDecorateId is referencing an OpConstant, one could
//                     check that the constants are the same rather than just
//                     looking at the constant ID.
bool DecorationManager::AreDecorationsTheSame(const ir::Instruction* inst1,
                                              const ir::Instruction* inst2,
                                              bool ignore_target) const {
  switch (inst1->opcode()) {
    case SpvOpDecorate:
    case SpvOpMemberDecorate:
    case SpvOpDecorateId:
    case SpvOpDecorateStringGOOGLE:
      break;
    default:
      return false;
  }

  if (inst1->opcode() != inst2->opcode() ||
      inst1->NumInOperands() != inst2->NumInOperands())
    return false;

  for (uint32_t i = ignore_target ? 1u : 0u; i < inst1->NumInOperands(); ++i)
    if (inst1->GetInOperand(i) != inst2->GetInOperand(i)) return false;

  return true;
}

void DecorationManager::AnalyzeDecorations() {
  if (!module_) return;

  // For each group and instruction, collect all their decoration instructions.
  for (ir::Instruction& inst : module_->annotations()) {
    AddDecoration(&inst);
  }
}
void DecorationManager::AddDecoration(ir::Instruction* inst) {
  switch (inst->opcode()) {
    case SpvOpDecorate:
    case SpvOpDecorateId:
    case SpvOpDecorateStringGOOGLE:
    case SpvOpMemberDecorate: {
      const auto target_id = inst->GetSingleWordInOperand(0u);
      id_to_decoration_insts_[target_id].direct_decorations.push_back(inst);
      break;
    }
    case SpvOpGroupDecorate:
    case SpvOpGroupMemberDecorate: {
      const uint32_t start = inst->opcode() == SpvOpGroupDecorate ? 1u : 2u;
      const uint32_t stride = start;
      for (uint32_t i = start; i < inst->NumInOperands(); i += stride) {
        const auto target_id = inst->GetSingleWordInOperand(i);
        TargetData& target_data = id_to_decoration_insts_[target_id];
        target_data.indirect_decorations.push_back(inst);
      }
      const auto target_id = inst->GetSingleWordInOperand(0u);
      id_to_decoration_insts_[target_id].decorate_insts.push_back(inst);
      break;
    }
    default:
      break;
  }
}

template <typename T>
std::vector<T> DecorationManager::InternalGetDecorationsFor(
    uint32_t id, bool include_linkage) {
  std::vector<T> decorations;

  const auto ids_iter = id_to_decoration_insts_.find(id);
  // |id| has no decorations
  if (ids_iter == id_to_decoration_insts_.end()) return decorations;

  const TargetData& target_data = ids_iter->second;

  const auto process_direct_decorations =
      [include_linkage,
       &decorations](const std::vector<ir::Instruction*>& direct_decorations) {
        for (ir::Instruction* inst : direct_decorations) {
          const bool is_linkage = inst->opcode() == SpvOpDecorate &&
                                  inst->GetSingleWordInOperand(1u) ==
                                      SpvDecorationLinkageAttributes;
          if (include_linkage || !is_linkage) decorations.push_back(inst);
        }
      };

  // Process |id|'s decorations.
  process_direct_decorations(ids_iter->second.direct_decorations);

  // Process the decorations of all groups applied to |id|.
  for (const ir::Instruction* inst : target_data.indirect_decorations) {
    const uint32_t group_id = inst->GetSingleWordInOperand(0u);
    const auto group_iter = id_to_decoration_insts_.find(group_id);
    assert(group_iter != id_to_decoration_insts_.end() && "Unknown group ID");
    process_direct_decorations(group_iter->second.direct_decorations);
  }

  return decorations;
}

bool DecorationManager::WhileEachDecoration(
    uint32_t id, uint32_t decoration,
    std::function<bool(const ir::Instruction&)> f) {
  for (const ir::Instruction* inst : GetDecorationsFor(id, true)) {
    switch (inst->opcode()) {
      case SpvOpMemberDecorate:
        if (inst->GetSingleWordInOperand(2) == decoration) {
          if (!f(*inst)) return false;
        }
        break;
      case SpvOpDecorate:
      case SpvOpDecorateId:
      case SpvOpDecorateStringGOOGLE:
        if (inst->GetSingleWordInOperand(1) == decoration) {
          if (!f(*inst)) return false;
        }
        break;
      default:
        assert(false && "Unexpected decoration instruction");
    }
  }
  return true;
}

void DecorationManager::ForEachDecoration(
    uint32_t id, uint32_t decoration,
    std::function<void(const ir::Instruction&)> f) {
  WhileEachDecoration(id, decoration, [&f](const ir::Instruction& inst) {
    f(inst);
    return true;
  });
}

void DecorationManager::CloneDecorations(uint32_t from, uint32_t to) {
  const auto decoration_list = id_to_decoration_insts_.find(from);
  if (decoration_list == id_to_decoration_insts_.end()) return;
  auto context = module_->context();
  for (ir::Instruction* inst : decoration_list->second.direct_decorations) {
    // simply clone decoration and change |target-id| to |to|
    std::unique_ptr<ir::Instruction> new_inst(inst->Clone(module_->context()));
    new_inst->SetInOperand(0, {to});
    module_->AddAnnotationInst(std::move(new_inst));
    auto decoration_iter = --module_->annotation_end();
    context->AnalyzeUses(&*decoration_iter);
  }
  // We need to copy the list of instructions as ForgetUses and AnalyzeUses are
  // going to modify it.
  std::vector<ir::Instruction*> indirect_decorations =
      decoration_list->second.indirect_decorations;
  for (ir::Instruction* inst : indirect_decorations) {
    switch (inst->opcode()) {
      case SpvOpGroupDecorate:
        context->ForgetUses(inst);
        // add |to| to list of decorated id's
        inst->AddOperand(
            ir::Operand(spv_operand_type_t::SPV_OPERAND_TYPE_ID, {to}));
        context->AnalyzeUses(inst);
        break;
      case SpvOpGroupMemberDecorate: {
        context->ForgetUses(inst);
        // for each (id == from), add (to, literal) as operands
        const uint32_t num_operands = inst->NumOperands();
        for (uint32_t i = 1; i < num_operands; i += 2) {
          ir::Operand op = inst->GetOperand(i);
          if (op.words[0] == from) {  // add new pair of operands: (to, literal)
            inst->AddOperand(
                ir::Operand(spv_operand_type_t::SPV_OPERAND_TYPE_ID, {to}));
            op = inst->GetOperand(i + 1);
            inst->AddOperand(std::move(op));
          }
        }
        context->AnalyzeUses(inst);
        break;
      }
      default:
        assert(false && "Unexpected decoration instruction");
    }
  }
}

void DecorationManager::RemoveDecoration(ir::Instruction* inst) {
  const auto remove_from_container = [inst](std::vector<ir::Instruction*>& v) {
    v.erase(std::remove(v.begin(), v.end(), inst), v.end());
  };

  switch (inst->opcode()) {
    case SpvOpDecorate:
    case SpvOpDecorateId:
    case SpvOpDecorateStringGOOGLE:
    case SpvOpMemberDecorate: {
      const auto target_id = inst->GetSingleWordInOperand(0u);
      auto const iter = id_to_decoration_insts_.find(target_id);
      if (iter == id_to_decoration_insts_.end()) return;
      remove_from_container(iter->second.direct_decorations);
    } break;
    case SpvOpGroupDecorate:
    case SpvOpGroupMemberDecorate: {
      const uint32_t stride = inst->opcode() == SpvOpGroupDecorate ? 1u : 2u;
      for (uint32_t i = 1u; i < inst->NumInOperands(); i += stride) {
        const auto target_id = inst->GetSingleWordInOperand(i);
        auto const iter = id_to_decoration_insts_.find(target_id);
        if (iter == id_to_decoration_insts_.end()) continue;
        remove_from_container(iter->second.indirect_decorations);
      }
      const auto group_id = inst->GetSingleWordInOperand(0u);
      auto const iter = id_to_decoration_insts_.find(group_id);
      if (iter == id_to_decoration_insts_.end()) return;
      remove_from_container(iter->second.decorate_insts);
    } break;
    default:
      break;
  }
}

}  // namespace analysis
}  // namespace opt
}  // namespace spvtools
