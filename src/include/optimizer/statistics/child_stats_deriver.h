#pragma once

#include <vector>

#include "optimizer/group_expression.h"
#include "optimizer/operator_visitor.h"

namespace terrier::parser {
class AbstractExpression;
}  // namespace terrier::parser

namespace terrier::optimizer {

class Memo;

/**
 * Derive child stats that have not yet been calculated for
 * a logical group expression.
 */
class ChildStatsDeriver : public OperatorVisitor {
 public:
  /**
   * Derives child statistics for an input logical group expression
   * @param gexpr Logical Group Expression to derive for
   * @param required_cols Expressions that derived stats must include
   * @param memo Memo table
   * @returns Set indicating what columns require stats
   */
  std::vector<ExprSet> DeriveInputStats(GroupExpression *gexpr, ExprSet required_cols, Memo *memo);

  /**
   * Visit for a LogicalQueryDerivedGet
   * @param op Visiting LogicalQueryDerivedGet
   */
  void Visit(const LogicalQueryDerivedGet *op) override;

  /**
   * Visit for a LogicalJoin
   * @param op Visiting LogicalInnerJoin
   */
  void Visit(const LogicalJoin *op) override;

  /**
   * Visit for a LogicalAggregateAndGroupBy
   * @param op Visiting LogicalAggregateAndGroupBy
   */
  void Visit(const LogicalAggregateAndGroupBy *op) override;

 private:
  /**
   * Function to pass down all required_cols_ to output list
   */
  void PassDownRequiredCols();

  /**
   * Function for passing down a single column
   * @param col Column to passdown
   */
  void PassDownColumn(common::ManagedPointer<parser::AbstractExpression> col);

  /**
   * Set of required child stats columns
   */
  ExprSet required_cols_;

  /**
   * GroupExpression to derive for
   */
  GroupExpression *gexpr_;

  /**
   * Memo
   */
  Memo *memo_;

  std::vector<ExprSet> output_;
};

}  // namespace terrier::optimizer
