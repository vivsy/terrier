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
#include "optimizer/rules/rewrite_rules.h"
#include "optimizer/util.h"
#include "parser/expression_util.h"
#include "type/transient_value_factory.h"

namespace terrier::optimizer {

///////////////////////////////////////////////////////////////////////////////
/// PushFilterThroughJoin
///////////////////////////////////////////////////////////////////////////////
RewritePushImplicitFilterThroughJoin::RewritePushImplicitFilterThroughJoin() {
  type_ = RuleType::PUSH_FILTER_THROUGH_JOIN;

  // Make join for pattern matching
  match_pattern_ = new Pattern(OpType::LOGICALJOIN);
  match_pattern_->AddChild(new Pattern(OpType::LEAF));
  match_pattern_->AddChild(new Pattern(OpType::LEAF));
}

bool RewritePushImplicitFilterThroughJoin::Check(common::ManagedPointer<OperatorNode> plan,
                                                 OptimizationContext *context) const {
  (void)plan;
  (void)context;
  return true;
}

void RewritePushImplicitFilterThroughJoin::Transform(common::ManagedPointer<OperatorNode> input,
                                                     std::vector<std::unique_ptr<OperatorNode>> *transformed,
                                                     UNUSED_ATTRIBUTE OptimizationContext *context) const {
  OPTIMIZER_LOG_TRACE("RewritePushImplicitFilterThroughJoin::Transform");

  auto &memo = context->GetOptimizerContext()->GetMemo();
  auto join_op_expr = input;
  auto join_children = join_op_expr->GetChildren();
  auto left_group_id = join_children[0]->GetOp().As<LeafOperator>()->GetOriginGroup();
  auto right_group_id = join_children[1]->GetOp().As<LeafOperator>()->GetOriginGroup();

  const auto &left_group_aliases_set = memo.GetGroupByID(left_group_id)->GetTableAliases();
  const auto &right_group_aliases_set = memo.GetGroupByID(right_group_id)->GetTableAliases();
  TERRIER_ASSERT(input->GetOp().As<LogicalJoin>()->GetJoinType() == LogicalJoinType::INNER, "join type should be Inner");
  auto &predicates = input->GetOp().As<LogicalJoin>()->GetJoinPredicates();

  std::vector<AnnotatedExpression> left_predicates;
  std::vector<AnnotatedExpression> right_predicates;
  std::vector<AnnotatedExpression> join_predicates;

  // Loop over all predicates, check each of them if they can be pushed down to
  // either the left child or the right child to be evaluated
  // All predicates in this loop follow conjunction relationship because we
  // already extract these predicates from the original.
  // E.g. An expression (test.a = test1.b and test.a = 5) would become
  // {test.a = test1.b, test.a = 5}
  for (auto &predicate : predicates) {
    if (OptimizerUtil::IsSubset(left_group_aliases_set, predicate.GetTableAliasSet())) {
      left_predicates.emplace_back(predicate);
    } else if (OptimizerUtil::IsSubset(right_group_aliases_set, predicate.GetTableAliasSet())) {
      right_predicates.emplace_back(predicate);
    } else {
      join_predicates.emplace_back(predicate);
    }
  }

  std::unique_ptr<OperatorNode> left_branch;
  std::unique_ptr<OperatorNode> right_branch;
  bool pushed_down = false;

  // Construct left filter if any
  if (!left_predicates.empty()) {
    pushed_down = true;
    std::vector<std::unique_ptr<OperatorNode>> c;
    auto left_child = join_op_expr->GetChildren()[0]->Copy();
    c.emplace_back(std::move(left_child));
    left_branch = std::make_unique<OperatorNode>(LogicalFilter::Make(std::move(left_predicates)), std::move(c));
  } else {
    left_branch = join_op_expr->GetChildren()[0]->Copy();
  }

  // Construct right filter if any
  if (!right_predicates.empty()) {
    pushed_down = true;
    std::vector<std::unique_ptr<OperatorNode>> c;
    auto right_child = join_op_expr->GetChildren()[1]->Copy();
    c.emplace_back(std::move(right_child));
    right_branch = std::make_unique<OperatorNode>(LogicalFilter::Make(std::move(right_predicates)), std::move(c));
  } else {
    right_branch = join_op_expr->GetChildren()[1]->Copy();
  }

  // Only construct the output if either filter has been pushed down
  if (pushed_down) {
    std::vector<std::unique_ptr<OperatorNode>> c;
    c.emplace_back(std::move(left_branch));
    c.emplace_back(std::move(right_branch));
    auto output = std::make_unique<OperatorNode>(LogicalJoin::Make(LogicalJoinType::INNER, std::move(join_predicates)), std::move(c));
    transformed->emplace_back(std::move(output));
  }
}

///////////////////////////////////////////////////////////////////////////////
/// RewritePushExplicitFilterThroughJoin
///////////////////////////////////////////////////////////////////////////////
RewritePushExplicitFilterThroughJoin::RewritePushExplicitFilterThroughJoin() {
  type_ = RuleType::PUSH_FILTER_THROUGH_JOIN;

  // Make join for pattern matching
  auto *join_pattern = new Pattern(OpType::LOGICALJOIN);
  join_pattern->AddChild(new Pattern(OpType::LEAF));
  join_pattern->AddChild(new Pattern(OpType::LEAF));

  match_pattern_ = new Pattern(OpType::LOGICALFILTER);
  match_pattern_->AddChild(join_pattern);
}

bool RewritePushExplicitFilterThroughJoin::Check(common::ManagedPointer<OperatorNode> plan,
                                                 OptimizationContext *context) const {
  (void)plan;
  (void)context;
  return true;
}

void RewritePushExplicitFilterThroughJoin::Transform(common::ManagedPointer<OperatorNode> input,
                                                     std::vector<std::unique_ptr<OperatorNode>> *transformed,
                                                     UNUSED_ATTRIBUTE OptimizationContext *context) const {
  OPTIMIZER_LOG_TRACE("RewritePushExplicitFilterThroughJoin::Transform");

  auto &memo = context->GetOptimizerContext()->GetMemo();
  auto join_op_expr = input->GetChildren()[0];
  auto join_children = join_op_expr->GetChildren();
  auto left_group_id = join_children[0]->GetOp().As<LeafOperator>()->GetOriginGroup();
  auto right_group_id = join_children[1]->GetOp().As<LeafOperator>()->GetOriginGroup();

  const auto &left_group_aliases_set = memo.GetGroupByID(left_group_id)->GetTableAliases();
  const auto &right_group_aliases_set = memo.GetGroupByID(right_group_id)->GetTableAliases();
  TERRIER_ASSERT(join_op_expr->GetOp().As<LogicalJoin>()->GetJoinType() == LogicalJoinType::INNER, "join type should be Inner");
  auto &input_join_predicates = join_op_expr->GetOp().As<LogicalJoin>()->GetJoinPredicates();
  auto &filter_predicates = input->GetOp().As<LogicalFilter>()->GetPredicates();

  std::vector<AnnotatedExpression> left_predicates;
  std::vector<AnnotatedExpression> right_predicates;
  std::vector<AnnotatedExpression> join_predicates;

  // Loop over all predicates, check each of them if they can be pushed down to
  // either the left child or the right child to be evaluated
  // All predicates in this loop follow conjunction relationship because we
  // already extract these predicates from the original.
  // E.g. An expression (test.a = test1.b and test.a = 5) would become
  // {test.a = test1.b, test.a = 5}
  for (auto &predicate : input_join_predicates) {
    if (OptimizerUtil::IsSubset(left_group_aliases_set, predicate.GetTableAliasSet())) {
      left_predicates.emplace_back(predicate);
    } else if (OptimizerUtil::IsSubset(right_group_aliases_set, predicate.GetTableAliasSet())) {
      right_predicates.emplace_back(predicate);
    } else {
      join_predicates.emplace_back(predicate);
    }
  }
  for (auto &predicate : filter_predicates) {
    if (OptimizerUtil::IsSubset(left_group_aliases_set, predicate.GetTableAliasSet())) {
      left_predicates.emplace_back(predicate);
    } else if (OptimizerUtil::IsSubset(right_group_aliases_set, predicate.GetTableAliasSet())) {
      right_predicates.emplace_back(predicate);
    } else {
      join_predicates.emplace_back(predicate);
    }
  }

  std::unique_ptr<OperatorNode> left_branch;
  std::unique_ptr<OperatorNode> right_branch;

  // Construct left filter if any
  if (!left_predicates.empty()) {
    std::vector<std::unique_ptr<OperatorNode>> c;
    auto left_child = join_op_expr->GetChildren()[0]->Copy();
    c.emplace_back(std::move(left_child));
    left_branch = std::make_unique<OperatorNode>(LogicalFilter::Make(std::move(left_predicates)), std::move(c));
  } else {
    left_branch = join_op_expr->GetChildren()[0]->Copy();
  }

  // Construct right filter if any
  if (!right_predicates.empty()) {
    std::vector<std::unique_ptr<OperatorNode>> c;
    auto right_child = join_op_expr->GetChildren()[1]->Copy();
    c.emplace_back(std::move(right_child));
    right_branch = std::make_unique<OperatorNode>(LogicalFilter::Make(std::move(right_predicates)), std::move(c));
  } else {
    right_branch = join_op_expr->GetChildren()[1]->Copy();
  }

  std::vector<std::unique_ptr<OperatorNode>> c;
  c.emplace_back(std::move(left_branch));
  c.emplace_back(std::move(right_branch));
  auto output = std::make_unique<OperatorNode>(LogicalJoin::Make(LogicalJoinType::INNER, std::move(join_predicates)), std::move(c));
  transformed->emplace_back(std::move(output));
}

///////////////////////////////////////////////////////////////////////////////
/// RewritePushFilterThroughAggregation
///////////////////////////////////////////////////////////////////////////////
RewritePushFilterThroughAggregation::RewritePushFilterThroughAggregation() {
  type_ = RuleType::PUSH_FILTER_THROUGH_AGGREGATION;

  auto child = new Pattern(OpType::LOGICALAGGREGATEANDGROUPBY);
  child->AddChild(new Pattern(OpType::LEAF));

  // Initialize a pattern for optimizer to match
  match_pattern_ = new Pattern(OpType::LOGICALFILTER);

  // Add node - we match (filter)->(aggregation)->(leaf)
  match_pattern_->AddChild(child);
}

bool RewritePushFilterThroughAggregation::Check(common::ManagedPointer<OperatorNode> plan,
                                                OptimizationContext *context) const {
  (void)plan;
  (void)context;
  return true;
}

void RewritePushFilterThroughAggregation::Transform(common::ManagedPointer<OperatorNode> input,
                                                    std::vector<std::unique_ptr<OperatorNode>> *transformed,
                                                    UNUSED_ATTRIBUTE OptimizationContext *context) const {
  OPTIMIZER_LOG_TRACE("RewritePushFilterThroughAggregation::Transform");
  auto aggregation_op = input->GetChildren()[0]->GetOp().As<LogicalAggregateAndGroupBy>();

  auto &predicates = input->GetOp().As<LogicalFilter>()->GetPredicates();
  std::vector<AnnotatedExpression> embedded_predicates;
  std::vector<AnnotatedExpression> pushdown_predicates;

  for (auto &predicate : predicates) {
    std::vector<common::ManagedPointer<parser::AbstractExpression>> aggr_exprs;
    parser::ExpressionUtil::GetAggregateExprs(&aggr_exprs, predicate.GetExpr());

    // No aggr_expr in the predicate -- pushdown to evaluate
    if (aggr_exprs.empty()) {
      pushdown_predicates.emplace_back(predicate);
    } else {
      embedded_predicates.emplace_back(predicate);
    }
  }

  // Add original having predicates
  for (auto &predicate : aggregation_op->GetHaving()) {
    embedded_predicates.emplace_back(predicate);
  }

  // Original leaf
  auto leaf = input->GetChildren()[0]->GetChildren()[0]->Copy();
  std::unique_ptr<OperatorNode> pushdown;

  // Construct filter if needed
  if (!pushdown_predicates.empty()) {
    std::vector<std::unique_ptr<OperatorNode>> c;
    c.emplace_back(std::move(leaf));
    pushdown = std::make_unique<OperatorNode>(LogicalFilter::Make(std::move(pushdown_predicates)), std::move(c));
  }

  std::vector<common::ManagedPointer<parser::AbstractExpression>> cols = aggregation_op->GetColumns();

  std::vector<std::unique_ptr<OperatorNode>> c;
  if (pushdown != nullptr)
    c.emplace_back(std::move(pushdown));
  else
    c.emplace_back(std::move(leaf));

  auto output = std::make_unique<OperatorNode>(
      LogicalAggregateAndGroupBy::Make(std::move(cols), std::move(embedded_predicates)), std::move(c));
  transformed->emplace_back(std::move(output));
}

///////////////////////////////////////////////////////////////////////////////
/// RewriteCombineConsecutiveFilter
///////////////////////////////////////////////////////////////////////////////
RewriteCombineConsecutiveFilter::RewriteCombineConsecutiveFilter() {
  type_ = RuleType::COMBINE_CONSECUTIVE_FILTER;

  match_pattern_ = new Pattern(OpType::LOGICALFILTER);
  auto child = new Pattern(OpType::LOGICALFILTER);
  child->AddChild(new Pattern(OpType::LEAF));

  match_pattern_->AddChild(child);
}

bool RewriteCombineConsecutiveFilter::Check(common::ManagedPointer<OperatorNode> plan,
                                            OptimizationContext *context) const {
  (void)context;
  (void)plan;
  return true;
}

void RewriteCombineConsecutiveFilter::Transform(common::ManagedPointer<OperatorNode> input,
                                                std::vector<std::unique_ptr<OperatorNode>> *transformed,
                                                UNUSED_ATTRIBUTE OptimizationContext *context) const {
  auto child_filter = input->GetChildren()[0];
  std::vector<AnnotatedExpression> root_predicates = input->GetOp().As<LogicalFilter>()->GetPredicates();
  std::vector<AnnotatedExpression> child_predicates = child_filter->GetOp().As<LogicalFilter>()->GetPredicates();
  root_predicates.insert(root_predicates.end(), child_predicates.begin(), child_predicates.end());

  std::vector<std::unique_ptr<OperatorNode>> c;
  auto child = child_filter->GetChildren()[0]->Copy();
  c.emplace_back(std::move(child));
  auto output = std::make_unique<OperatorNode>(LogicalFilter::Make(std::move(root_predicates)), std::move(c));
  transformed->emplace_back(std::move(output));
}

///////////////////////////////////////////////////////////////////////////////
/// RewriteEmbedFilterIntoGet
///////////////////////////////////////////////////////////////////////////////
RewriteEmbedFilterIntoGet::RewriteEmbedFilterIntoGet() {
  type_ = RuleType::EMBED_FILTER_INTO_GET;

  match_pattern_ = new Pattern(OpType::LOGICALFILTER);
  auto child = new Pattern(OpType::LOGICALGET);

  match_pattern_->AddChild(child);
}

bool RewriteEmbedFilterIntoGet::Check(common::ManagedPointer<OperatorNode> plan, OptimizationContext *context) const {
  (void)context;
  (void)plan;
  return true;
}

void RewriteEmbedFilterIntoGet::Transform(common::ManagedPointer<OperatorNode> input,
                                          std::vector<std::unique_ptr<OperatorNode>> *transformed,
                                          UNUSED_ATTRIBUTE OptimizationContext *context) const {
  auto get = input->GetChildren()[0]->GetOp().As<LogicalGet>();
  std::string tbl_alias = std::string(get->GetTableAlias());
  std::vector<AnnotatedExpression> predicates = input->GetOp().As<LogicalFilter>()->GetPredicates();
  std::vector<std::unique_ptr<OperatorNode>> c;
  auto output =
      std::make_unique<OperatorNode>(LogicalGet::Make(get->GetDatabaseOid(), get->GetNamespaceOid(), get->GetTableOid(),
                                                      predicates, tbl_alias, get->GetIsForUpdate()),
                                     std::move(c));
  transformed->emplace_back(std::move(output));
}

///////////////////////////////////////////////////////////////////////////////
/// RewritePullFilterThroughMarkJoin
///////////////////////////////////////////////////////////////////////////////
RewritePullFilterThroughMarkJoin::RewritePullFilterThroughMarkJoin() {
  type_ = RuleType::PULL_FILTER_THROUGH_MARK_JOIN;

  match_pattern_ = new Pattern(OpType::LOGICALJOIN);
  match_pattern_->AddChild(new Pattern(OpType::LEAF));
  auto filter = new Pattern(OpType::LOGICALFILTER);
  filter->AddChild(new Pattern(OpType::LEAF));
  match_pattern_->AddChild(filter);
}

RulePromise RewritePullFilterThroughMarkJoin::Promise(GroupExpression *group_expr) const {
  return RulePromise::UNNEST_PROMISE_HIGH;
}

bool RewritePullFilterThroughMarkJoin::Check(common::ManagedPointer<OperatorNode> plan,
                                             OptimizationContext *context) const {
  (void)context;
  (void)plan;

  auto children = plan->GetChildren();
  TERRIER_ASSERT(children.size() == 2, "MarkJoin should have two children");

  UNUSED_ATTRIBUTE auto r_grandchildren = children[0]->GetChildren();
  TERRIER_ASSERT(r_grandchildren.size() == 1, "Filter should have only 1 child");
  return true;
}

void RewritePullFilterThroughMarkJoin::Transform(common::ManagedPointer<OperatorNode> input,
                                                 std::vector<std::unique_ptr<OperatorNode>> *transformed,
                                                 UNUSED_ATTRIBUTE OptimizationContext *context) const {
  OPTIMIZER_LOG_TRACE("RewritePullFilterThroughMarkJoin::Transform");
  UNUSED_ATTRIBUTE auto mark_join = input->GetOp().As<LogicalJoin>();
  TERRIER_ASSERT(mark_join->GetJoinType() == LogicalJoinType::MARK, "Join type should be Mark");
  TERRIER_ASSERT(mark_join->GetJoinPredicates().empty(), "MarkJoin should have zero children");

  auto join_children = input->GetChildren();
  auto filter_children = join_children[1]->GetChildren();

  std::vector<std::unique_ptr<OperatorNode>> c;
  c.emplace_back(join_children[0]->Copy());
  c.emplace_back(filter_children[0]->Copy());
  auto join = std::make_unique<OperatorNode>(Operator(input->GetOp()), std::move(c));

  std::vector<std::unique_ptr<OperatorNode>> cc;
  cc.emplace_back(std::move(join));
  auto output = std::make_unique<OperatorNode>(Operator(join_children[1]->GetOp()), std::move(cc));
  transformed->emplace_back(std::move(output));
}

///////////////////////////////////////////////////////////////////////////////
/// RewritePullFilterThroughAggregation
///////////////////////////////////////////////////////////////////////////////
RewritePullFilterThroughAggregation::RewritePullFilterThroughAggregation() {
  type_ = RuleType::PULL_FILTER_THROUGH_AGGREGATION;

  auto filter = new Pattern(OpType::LOGICALFILTER);
  filter->AddChild(new Pattern(OpType::LEAF));
  match_pattern_ = new Pattern(OpType::LOGICALAGGREGATEANDGROUPBY);
  match_pattern_->AddChild(filter);
}

RulePromise RewritePullFilterThroughAggregation::Promise(GroupExpression *group_expr) const {
  return RulePromise::UNNEST_PROMISE_HIGH;
}

bool RewritePullFilterThroughAggregation::Check(common::ManagedPointer<OperatorNode> plan,
                                                OptimizationContext *context) const {
  (void)context;
  (void)plan;

  auto children = plan->GetChildren();
  TERRIER_ASSERT(children.size() == 1, "AggregateAndGroupBy should have 1 child");

  UNUSED_ATTRIBUTE auto r_grandchildren = children[1]->GetChildren();
  TERRIER_ASSERT(r_grandchildren.size() == 1, "Filter should have 1 child");
  return true;
}

void RewritePullFilterThroughAggregation::Transform(common::ManagedPointer<OperatorNode> input,
                                                    std::vector<std::unique_ptr<OperatorNode>> *transformed,
                                                    UNUSED_ATTRIBUTE OptimizationContext *context) const {
  OPTIMIZER_LOG_TRACE("RewritePullFilterThroughAggregation::Transform");
  auto &memo = context->GetOptimizerContext()->GetMemo();
  auto filter_expr = input->GetChildren()[0];
  auto child_group_id = filter_expr->GetChildren()[0]->GetOp().As<LeafOperator>()->GetOriginGroup();
  const auto &child_group_aliases_set = memo.GetGroupByID(child_group_id)->GetTableAliases();
  auto &predicates = filter_expr->GetOp().As<LogicalFilter>()->GetPredicates();

  std::vector<AnnotatedExpression> correlated_predicates;
  std::vector<AnnotatedExpression> normal_predicates;
  std::vector<common::ManagedPointer<parser::AbstractExpression>> new_groupby_cols;
  for (auto &predicate : predicates) {
    if (OptimizerUtil::IsSubset(child_group_aliases_set, predicate.GetTableAliasSet())) {
      normal_predicates.emplace_back(predicate);
    } else {
      // Correlated predicate, already in the form of
      // (outer_relation.a = (expr))
      correlated_predicates.emplace_back(predicate);
      auto root_expr = predicate.GetExpr();
      if (root_expr->GetChild(0)->GetDepth() < root_expr->GetDepth()) {
        new_groupby_cols.emplace_back(root_expr->GetChild(1).Get());
      } else {
        new_groupby_cols.emplace_back(root_expr->GetChild(0).Get());
      }
    }
  }

  if (correlated_predicates.empty()) {
    // No need to pull
    return;
  }

  auto aggregation = input->GetOp().As<LogicalAggregateAndGroupBy>();
  for (auto &col : aggregation->GetColumns()) {
    new_groupby_cols.emplace_back(col);
  }

  auto aggr_child = filter_expr->GetChildren()[0]->Copy();
  if (!normal_predicates.empty()) {
    std::vector<std::unique_ptr<OperatorNode>> c;
    c.emplace_back(std::move(aggr_child));
    aggr_child = std::make_unique<OperatorNode>(LogicalFilter::Make(std::move(normal_predicates)), std::move(c));
  }

  std::vector<std::unique_ptr<OperatorNode>> c;
  c.emplace_back(std::move(aggr_child));

  std::vector<AnnotatedExpression> new_having = aggregation->GetHaving();
  auto new_aggr = std::make_unique<OperatorNode>(
      LogicalAggregateAndGroupBy::Make(std::move(new_groupby_cols), std::move(new_having)), std::move(c));

  std::vector<std::unique_ptr<OperatorNode>> ca;
  ca.emplace_back(std::move(new_aggr));
  auto output = std::make_unique<OperatorNode>(LogicalFilter::Make(std::move(correlated_predicates)), std::move(ca));
  transformed->emplace_back(std::move(output));
}

}  // namespace terrier::optimizer
