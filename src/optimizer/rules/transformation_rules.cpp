#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "catalog/catalog_accessor.h"
#include "loggers/optimizer_logger.h"
#include "optimizer/group_expression.h"
#include "optimizer/index_util.h"
#include "optimizer/optimizer_context.h"
#include "optimizer/optimizer_defs.h"
#include "optimizer/physical_operators.h"
#include "optimizer/properties.h"
#include "optimizer/rules/transformation_rules.h"
#include "optimizer/util.h"
#include "parser/expression_util.h"
#include "type/transient_value_factory.h"

namespace terrier::optimizer {

///////////////////////////////////////////////////////////////////////////////
/// LogicalInnerJoinCommutativity
///////////////////////////////////////////////////////////////////////////////
LogicalInnerJoinCommutativity::LogicalInnerJoinCommutativity() {
  type_ = RuleType::INNER_JOIN_COMMUTE;

  auto left_child = new Pattern(OpType::LEAF);
  auto right_child = new Pattern(OpType::LEAF);
  match_pattern_ = new Pattern(OpType::LOGICALJOIN);
  match_pattern_->AddChild(left_child);
  match_pattern_->AddChild(right_child);
}

bool LogicalInnerJoinCommutativity::Check(common::ManagedPointer<OperatorNode> plan,
                                          OptimizationContext *context) const {
  (void)context;
  (void)plan;
  return true;
}

void LogicalInnerJoinCommutativity::Transform(common::ManagedPointer<OperatorNode> input,
                                              std::vector<std::unique_ptr<OperatorNode>> *transformed,
                                              UNUSED_ATTRIBUTE OptimizationContext *context) const {
  auto join_op = input->GetOp().As<LogicalJoin>();
  TERRIER_ASSERT(join_op->GetJoinType() == LogicalJoinType::INNER, "Join type should be Inner");
  auto join_predicates = std::vector<AnnotatedExpression>(join_op->GetJoinPredicates());

  const auto &children = input->GetChildren();
  TERRIER_ASSERT(children.size() == 2, "There should be two children");
  OPTIMIZER_LOG_TRACE("Reorder left child with op {0} and right child with op {1} for inner join",
                      children[0]->GetOp().GetName().c_str(), children[1]->GetOp().GetName().c_str());

  std::vector<std::unique_ptr<OperatorNode>> new_child;
  new_child.emplace_back(children[1]->Copy());
  new_child.emplace_back(children[0]->Copy());

  auto result_plan =
      std::make_unique<OperatorNode>(LogicalJoin::Make(LogicalJoinType::INNER, std::move(join_predicates)), std::move(new_child));
  transformed->emplace_back(std::move(result_plan));
}

///////////////////////////////////////////////////////////////////////////////
/// LogicalInnerJoinAssociativity
///////////////////////////////////////////////////////////////////////////////
LogicalInnerJoinAssociativity::LogicalInnerJoinAssociativity() {
  type_ = RuleType::INNER_JOIN_ASSOCIATE;

  // Create left nested join
  auto left_child = new Pattern(OpType::LOGICALJOIN);
  left_child->AddChild(new Pattern(OpType::LEAF));
  left_child->AddChild(new Pattern(OpType::LEAF));

  auto right_child = new Pattern(OpType::LEAF);

  match_pattern_ = new Pattern(OpType::LOGICALJOIN);
  match_pattern_->AddChild(left_child);
  match_pattern_->AddChild(right_child);
}

bool LogicalInnerJoinAssociativity::Check(common::ManagedPointer<OperatorNode> plan,
                                          OptimizationContext *context) const {
  (void)context;
  (void)plan;
  return true;
}

void LogicalInnerJoinAssociativity::Transform(common::ManagedPointer<OperatorNode> input,
                                              std::vector<std::unique_ptr<OperatorNode>> *transformed,
                                              OptimizationContext *context) const {
  // NOTE: Transforms (left JOIN middle) JOIN right -> left JOIN (middle JOIN
  // right) Variables are named accordingly to above transformation
  auto parent_join = input->GetOp().As<LogicalJoin>();
  TERRIER_ASSERT(parent_join->GetJoinType() == LogicalJoinType::INNER, "Join type should be Inner");
  const auto &children = input->GetChildren();
  TERRIER_ASSERT(children.size() == 2, "There should be 2 children");
  TERRIER_ASSERT(children[0]->GetOp().GetType() == OpType::LOGICALJOIN, "Left should be join");
  TERRIER_ASSERT(children[0]->GetOp().As<LogicalJoin>()->GetJoinType() == LogicalJoinType::INNER, "Left should be inner join");
  TERRIER_ASSERT(children[0]->GetChildren().size() == 2, "Left join should have 2 children");

  auto child_join = children[0]->GetOp().As<LogicalJoin>();
  auto left = children[0]->GetChildren()[0];
  auto middle = children[0]->GetChildren()[1];
  auto right = children[1];

  OPTIMIZER_LOG_DEBUG("Reordered join structured: ({0} JOIN {1}) JOIN {2}", left->GetOp().GetName().c_str(),
                      middle->GetOp().GetName().c_str(), right->GetOp().GetName().c_str());

  // Get Alias sets
  auto &memo = context->GetOptimizerContext()->GetMemo();
  auto middle_group_id = middle->GetOp().As<LeafOperator>()->GetOriginGroup();
  auto right_group_id = right->GetOp().As<LeafOperator>()->GetOriginGroup();

  const auto &middle_group_aliases_set = memo.GetGroupByID(middle_group_id)->GetTableAliases();
  const auto &right_group_aliases_set = memo.GetGroupByID(right_group_id)->GetTableAliases();

  // Union Predicates into single alias set for new child join
  std::unordered_set<std::string> right_join_aliases_set;
  right_join_aliases_set.insert(middle_group_aliases_set.begin(), middle_group_aliases_set.end());
  right_join_aliases_set.insert(right_group_aliases_set.begin(), right_group_aliases_set.end());

  // Redistribute predicates
  std::vector<AnnotatedExpression> predicates;
  auto parent_join_predicates = std::vector<AnnotatedExpression>(parent_join->GetJoinPredicates());
  auto child_join_predicates = std::vector<AnnotatedExpression>(child_join->GetJoinPredicates());
  predicates.insert(predicates.end(), parent_join_predicates.begin(), parent_join_predicates.end());
  predicates.insert(predicates.end(), child_join_predicates.begin(), child_join_predicates.end());

  std::vector<AnnotatedExpression> new_child_join_predicates;
  std::vector<AnnotatedExpression> new_parent_join_predicates;
  for (const auto &predicate : predicates) {
    if (OptimizerUtil::IsSubset(right_join_aliases_set, predicate.GetTableAliasSet())) {
      new_child_join_predicates.emplace_back(predicate);
    } else {
      new_parent_join_predicates.emplace_back(predicate);
    }
  }

  // Construct new child join operator
  std::vector<std::unique_ptr<OperatorNode>> child_children;
  child_children.emplace_back(middle->Copy());
  child_children.emplace_back(right->Copy());
  auto new_child_join = std::make_unique<OperatorNode>(LogicalJoin::Make(LogicalJoinType::INNER, std::move(new_child_join_predicates)),
                                                       std::move(child_children));

  // Construct new parent join operator
  std::vector<std::unique_ptr<OperatorNode>> parent_children;
  parent_children.emplace_back(left->Copy());
  parent_children.emplace_back(std::move(new_child_join));
  auto new_parent_join = std::make_unique<OperatorNode>(LogicalJoin::Make(LogicalJoinType::INNER, std::move(new_parent_join_predicates)),
                                                        std::move(parent_children));

  transformed->emplace_back(std::move(new_parent_join));
}

}  // namespace terrier::optimizer
