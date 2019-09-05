// TODO(wz2): Verify OutputSchema after sync-up meeting
#include <memory>
#include <string>

#include "optimizer/optimizer.h"
#include "optimizer/properties.h"
#include "optimizer/property_set.h"
#include "parser/expression/constant_value_expression.h"
#include "parser/expression/derived_value_expression.h"
#include "parser/postgresparser.h"
#include "planner/plannodes/abstract_plan_node.h"
#include "planner/plannodes/limit_plan_node.h"
#include "planner/plannodes/order_by_plan_node.h"
#include "planner/plannodes/projection_plan_node.h"
#include "planner/plannodes/seq_scan_plan_node.h"
#include "util/test_harness.h"
#include "util/tpcc/tpcc_plan_test.h"

namespace terrier::optimizer {

class TpccPlanSeqScanTests : public TpccPlanTest {};

// NOLINTNEXTLINE
TEST_F(TpccPlanSeqScanTests, SimpleSeqScanSelect) {
  auto check = [](TpccPlanTest *test, parser::SelectStatement *sel_stmt, catalog::table_oid_t tbl_oid,
                  std::shared_ptr<planner::AbstractPlanNode> plan) {
    EXPECT_EQ(plan->GetChildrenSize(), 0);
    EXPECT_EQ(plan->GetPlanNodeType(), planner::PlanNodeType::SEQSCAN);

    auto seq = std::dynamic_pointer_cast<planner::SeqScanPlanNode>(plan);
    EXPECT_EQ(seq->GetScanPredicate(), nullptr);
    EXPECT_EQ(seq->IsForUpdate(), false);
    EXPECT_EQ(seq->IsParallel(), test->settings_manager_->GetBool(settings::Param::parallel_execution));
    EXPECT_EQ(seq->GetDatabaseOid(), test->db_);
    EXPECT_EQ(seq->GetNamespaceOid(), test->accessor_->GetDefaultNamespace());

    auto &schema = test->accessor_->GetSchema(tbl_oid);
    EXPECT_EQ(seq->GetTableOid(), test->tbl_new_order_);

    EXPECT_EQ(seq->GetColumnIds().size(), 1);
    EXPECT_EQ(seq->GetColumnIds()[0], schema.GetColumn("no_o_id").Oid());
  };

  std::string query = "SELECT NO_O_ID FROM NEW_ORDER";
  OptimizeQuery(query, "new_order", tbl_new_order_, check);
}

// NOLINTNEXTLINE
TEST_F(TpccPlanSeqScanTests, SimpleSeqScanSelectWithPredicate) {
  auto check = [](TpccPlanTest *test, parser::SelectStatement *sel_stmt, catalog::table_oid_t tbl_oid,
                  std::shared_ptr<planner::AbstractPlanNode> plan) {
    auto &schema = test->accessor_->GetSchema(test->tbl_order_);
    auto offset = 0;
    for (size_t idx = 0; idx < schema.GetColumns().size(); idx++) {
      unsigned idx_u = static_cast<unsigned>(idx);
      if (schema.GetColumn(idx_u).Name() == "o_carrier_id") {
        offset = static_cast<unsigned>(idx_u);
      }
    }

    EXPECT_EQ(plan->GetChildrenSize(), 0);
    EXPECT_EQ(plan->GetPlanNodeType(), planner::PlanNodeType::SEQSCAN);

    auto seq = std::dynamic_pointer_cast<planner::SeqScanPlanNode>(plan);
    EXPECT_EQ(seq->IsForUpdate(), false);
    EXPECT_EQ(seq->IsParallel(), test->settings_manager_->GetBool(settings::Param::parallel_execution));
    EXPECT_EQ(seq->GetDatabaseOid(), test->db_);
    EXPECT_EQ(seq->GetNamespaceOid(), test->accessor_->GetDefaultNamespace());
    EXPECT_EQ(seq->GetTableOid(), test->tbl_order_);
    EXPECT_EQ(seq->GetColumnIds().size(), 1);
    EXPECT_EQ(seq->GetColumnIds()[0], schema.GetColumn("o_id").Oid());

    // Check Scan Predicate
    auto scan_pred = seq->GetScanPredicate();
    EXPECT_TRUE(scan_pred != nullptr);
    EXPECT_EQ(scan_pred->GetExpressionType(), parser::ExpressionType::COMPARE_EQUAL);
    EXPECT_EQ(scan_pred->GetChildrenSize(), 2);
    EXPECT_EQ(scan_pred->GetChild(0)->GetExpressionType(), parser::ExpressionType::VALUE_TUPLE);
    EXPECT_EQ(scan_pred->GetChild(1)->GetExpressionType(), parser::ExpressionType::VALUE_CONSTANT);
    auto *dve = reinterpret_cast<const parser::DerivedValueExpression *>(scan_pred->GetChild(0).Get());
    auto *cve = reinterpret_cast<const parser::ConstantValueExpression *>(scan_pred->GetChild(1).Get());
    EXPECT_EQ(dve->GetTupleIdx(), 0);
    EXPECT_EQ(dve->GetValueIdx(), offset);  // ValueIdx() should be offset into underlying tuple
    EXPECT_EQ(type::TransientValuePeeker::PeekInteger(cve->GetValue()), 5);
  };

  std::string query = "SELECT o_id from \"ORDER\" where o_carrier_id = 5";
  OptimizeQuery(query, "order", tbl_order_, check);
}

// NOLINTNEXTLINE
TEST_F(TpccPlanSeqScanTests, SimpleSeqScanSelectWithPredicateOrderBy) {
  auto check = [](TpccPlanTest *test, parser::SelectStatement *sel_stmt, catalog::table_oid_t tbl_oid,
                  std::shared_ptr<planner::AbstractPlanNode> plan) {
    // Find column offset that matches with o_carrier_id
    auto &schema = test->accessor_->GetSchema(test->tbl_order_);
    unsigned carrier_offset = 0;
    for (size_t idx = 0; idx < schema.GetColumns().size(); idx++) {
      if (schema.GetColumns()[idx].Name() == "o_carrier_id") carrier_offset = static_cast<unsigned>(idx);
    }

    EXPECT_EQ(plan->GetChildrenSize(), 1);
    EXPECT_EQ(plan->GetPlanNodeType(), planner::PlanNodeType::PROJECTION);

    // Order By
    auto planc = plan->GetChild(0);
    EXPECT_EQ(planc->GetChildrenSize(), 1);
    EXPECT_EQ(planc->GetPlanNodeType(), planner::PlanNodeType::ORDERBY);
    auto orderby = reinterpret_cast<const planner::OrderByPlanNode *>(planc);
    EXPECT_EQ(orderby->HasLimit(), false);
    EXPECT_EQ(orderby->GetSortKeys().size(), 1);
    EXPECT_EQ(orderby->GetSortKeys()[0].second, planner::OrderByOrderingType::DESC);
    auto sortkey = reinterpret_cast<const parser::DerivedValueExpression *>(orderby->GetSortKeys()[0].first);
    EXPECT_TRUE(sortkey != nullptr);
    EXPECT_EQ(sortkey->GetExpressionType(), parser::ExpressionType::VALUE_TUPLE);
    EXPECT_EQ(sortkey->GetTupleIdx(), 0);
    EXPECT_EQ(sortkey->GetValueIdx(), 0);

    // Seq Scan
    auto plans = planc->GetChild(0);
    EXPECT_EQ(plans->GetChildrenSize(), 0);
    EXPECT_EQ(plans->GetPlanNodeType(), planner::PlanNodeType::SEQSCAN);

    // Check SeqScan
    auto seq = reinterpret_cast<const planner::SeqScanPlanNode *>(plans);
    EXPECT_EQ(seq->IsForUpdate(), false);
    EXPECT_EQ(seq->IsParallel(), test->settings_manager_->GetBool(settings::Param::parallel_execution));
    EXPECT_EQ(seq->GetDatabaseOid(), test->db_);
    EXPECT_EQ(seq->GetNamespaceOid(), test->accessor_->GetDefaultNamespace());
    EXPECT_EQ(seq->GetTableOid(), test->tbl_order_);
    EXPECT_EQ(seq->GetColumnIds().size(), 2);
    EXPECT_EQ(seq->GetColumnIds()[0], schema.GetColumn("o_ol_cnt").Oid());
    EXPECT_EQ(seq->GetColumnIds()[1], schema.GetColumn("o_id").Oid());

    // Check scan predicate
    auto scan_pred = seq->GetScanPredicate();
    EXPECT_TRUE(scan_pred != nullptr);
    EXPECT_EQ(scan_pred->GetExpressionType(), parser::ExpressionType::COMPARE_EQUAL);
    EXPECT_EQ(scan_pred->GetChildrenSize(), 2);
    EXPECT_EQ(scan_pred->GetChild(0)->GetExpressionType(), parser::ExpressionType::VALUE_TUPLE);
    EXPECT_EQ(scan_pred->GetChild(1)->GetExpressionType(), parser::ExpressionType::VALUE_CONSTANT);
    auto *dve = reinterpret_cast<const parser::DerivedValueExpression *>(scan_pred->GetChild(0).Get());
    auto *cve = reinterpret_cast<const parser::ConstantValueExpression *>(scan_pred->GetChild(1).Get());
    EXPECT_EQ(dve->GetTupleIdx(), 0);
    EXPECT_EQ(dve->GetValueIdx(), carrier_offset);  // ValueIdx() should be offset into underlying tuple (all cols)
    EXPECT_EQ(type::TransientValuePeeker::PeekInteger(cve->GetValue()), 5);
  };

  std::string query = "SELECT o_id from \"ORDER\" where o_carrier_id = 5 ORDER BY o_ol_cnt DESC";
  OptimizeQuery(query, "order", tbl_order_, check);
}

// NOLINTNEXTLINE
TEST_F(TpccPlanSeqScanTests, SimpleSeqScanSelectWithPredicateLimit) {
  auto check = [](TpccPlanTest *test, parser::SelectStatement *sel_stmt, catalog::table_oid_t tbl_oid,
                  std::shared_ptr<planner::AbstractPlanNode> plan) {
    // Find column offset that matches with o_carrier_id
    auto &schema = test->accessor_->GetSchema(test->tbl_order_);
    unsigned carrier_offset = 0;
    for (size_t idx = 0; idx < schema.GetColumns().size(); idx++) {
      if (schema.GetColumns()[idx].Name() == "o_carrier_id") carrier_offset = static_cast<unsigned>(idx);
    }

    EXPECT_EQ(plan->GetChildrenSize(), 1);
    EXPECT_EQ(plan->GetPlanNodeType(), planner::PlanNodeType::LIMIT);
    auto limit_plan = std::dynamic_pointer_cast<planner::LimitPlanNode>(plan);
    EXPECT_EQ(limit_plan->GetLimit(), 1);
    EXPECT_EQ(limit_plan->GetOffset(), 2);

    // Check SeqScan
    auto plans = plan->GetChild(0);
    EXPECT_EQ(plans->GetChildrenSize(), 0);
    EXPECT_EQ(plans->GetPlanNodeType(), planner::PlanNodeType::SEQSCAN);
    auto seq = reinterpret_cast<const planner::SeqScanPlanNode *>(plans);
    EXPECT_EQ(seq->IsForUpdate(), false);
    EXPECT_EQ(seq->IsParallel(), test->settings_manager_->GetBool(settings::Param::parallel_execution));
    EXPECT_EQ(seq->GetDatabaseOid(), test->db_);
    EXPECT_EQ(seq->GetNamespaceOid(), test->accessor_->GetDefaultNamespace());
    EXPECT_EQ(seq->GetTableOid(), test->tbl_order_);
    EXPECT_EQ(seq->GetColumnIds().size(), 1);
    EXPECT_EQ(seq->GetColumnIds()[0], schema.GetColumn("o_id").Oid());

    // Check scan predicate
    auto scan_pred = seq->GetScanPredicate();
    EXPECT_TRUE(scan_pred != nullptr);
    EXPECT_EQ(scan_pred->GetExpressionType(), parser::ExpressionType::COMPARE_EQUAL);
    EXPECT_EQ(scan_pred->GetChildrenSize(), 2);
    EXPECT_EQ(scan_pred->GetChild(0)->GetExpressionType(), parser::ExpressionType::VALUE_TUPLE);
    EXPECT_EQ(scan_pred->GetChild(1)->GetExpressionType(), parser::ExpressionType::VALUE_CONSTANT);
    auto *dve = reinterpret_cast<const parser::DerivedValueExpression *>(scan_pred->GetChild(0).Get());
    auto *cve = reinterpret_cast<const parser::ConstantValueExpression *>(scan_pred->GetChild(1).Get());
    EXPECT_EQ(dve->GetTupleIdx(), 0);
    EXPECT_EQ(dve->GetValueIdx(), carrier_offset);  // ValueIdx() should be offset into underlying tuple (all cols)
    EXPECT_EQ(type::TransientValuePeeker::PeekInteger(cve->GetValue()), 5);
  };

  std::string query = "SELECT o_id from \"ORDER\" where o_carrier_id = 5 LIMIT 1 OFFSET 2";
  OptimizeQuery(query, "order", tbl_order_, check);
}

// NOLINTNEXTLINE
TEST_F(TpccPlanSeqScanTests, SimpleSeqScanSelectWithPredicateOrderByLimit) {
  auto check = [](TpccPlanTest *test, parser::SelectStatement *sel_stmt, catalog::table_oid_t tbl_oid,
                  std::shared_ptr<planner::AbstractPlanNode> plan) {
    // Find column offset that matches with o_carrier_id
    auto &schema = test->accessor_->GetSchema(test->tbl_order_);
    unsigned carrier_offset = 0;
    for (size_t idx = 0; idx < schema.GetColumns().size(); idx++) {
      if (schema.GetColumns()[idx].Name() == "o_carrier_id") carrier_offset = static_cast<unsigned>(idx);
    }

    EXPECT_EQ(plan->GetChildrenSize(), 1);
    EXPECT_EQ(plan->GetPlanNodeType(), planner::PlanNodeType::PROJECTION);

    // Limit
    auto planl = plan->GetChild(0);
    EXPECT_EQ(planl->GetChildrenSize(), 1);
    EXPECT_EQ(planl->GetPlanNodeType(), planner::PlanNodeType::LIMIT);
    auto limit_plan = reinterpret_cast<const planner::LimitPlanNode *>(planl);
    EXPECT_EQ(limit_plan->GetLimit(), sel_stmt->GetSelectLimit()->GetLimit());
    EXPECT_EQ(limit_plan->GetOffset(), sel_stmt->GetSelectLimit()->GetOffset());

    // Order By
    auto planc = planl->GetChild(0);
    EXPECT_EQ(planc->GetChildrenSize(), 1);
    EXPECT_EQ(planc->GetPlanNodeType(), planner::PlanNodeType::ORDERBY);
    auto orderby = reinterpret_cast<const planner::OrderByPlanNode *>(planc);
    EXPECT_EQ(orderby->HasLimit(), true);
    EXPECT_EQ(orderby->GetLimit(), sel_stmt->GetSelectLimit()->GetLimit());
    EXPECT_EQ(orderby->GetOffset(), sel_stmt->GetSelectLimit()->GetOffset());
    EXPECT_EQ(orderby->GetSortKeys().size(), 1);
    EXPECT_EQ(orderby->GetSortKeys()[0].second, planner::OrderByOrderingType::DESC);
    auto sortkey = reinterpret_cast<const parser::DerivedValueExpression *>(orderby->GetSortKeys()[0].first);
    EXPECT_TRUE(sortkey != nullptr);
    EXPECT_EQ(sortkey->GetExpressionType(), parser::ExpressionType::VALUE_TUPLE);
    EXPECT_EQ(sortkey->GetTupleIdx(), 0);
    EXPECT_EQ(sortkey->GetValueIdx(), 0);

    // Seq Scan
    auto plans = planc->GetChild(0);
    EXPECT_EQ(plans->GetChildrenSize(), 0);
    EXPECT_EQ(plans->GetPlanNodeType(), planner::PlanNodeType::SEQSCAN);
    auto seq = reinterpret_cast<const planner::SeqScanPlanNode *>(plans);
    EXPECT_EQ(seq->IsForUpdate(), false);
    EXPECT_EQ(seq->IsParallel(), test->settings_manager_->GetBool(settings::Param::parallel_execution));
    EXPECT_EQ(seq->GetDatabaseOid(), test->db_);
    EXPECT_EQ(seq->GetNamespaceOid(), test->accessor_->GetDefaultNamespace());
    EXPECT_EQ(seq->GetTableOid(), test->tbl_order_);
    EXPECT_EQ(seq->GetColumnIds().size(), 2);
    EXPECT_EQ(seq->GetColumnIds()[0], schema.GetColumn("o_ol_cnt").Oid());
    EXPECT_EQ(seq->GetColumnIds()[1], schema.GetColumn("o_id").Oid());

    // Check scan predicate
    auto scan_pred = seq->GetScanPredicate();
    EXPECT_TRUE(scan_pred != nullptr);
    EXPECT_EQ(scan_pred->GetExpressionType(), parser::ExpressionType::COMPARE_EQUAL);
    EXPECT_EQ(scan_pred->GetChildrenSize(), 2);
    EXPECT_EQ(scan_pred->GetChild(0)->GetExpressionType(), parser::ExpressionType::VALUE_TUPLE);
    EXPECT_EQ(scan_pred->GetChild(1)->GetExpressionType(), parser::ExpressionType::VALUE_CONSTANT);
    auto *dve = reinterpret_cast<const parser::DerivedValueExpression *>(scan_pred->GetChild(0).Get());
    auto *cve = reinterpret_cast<const parser::ConstantValueExpression *>(scan_pred->GetChild(1).Get());
    EXPECT_EQ(dve->GetTupleIdx(), 0);
    EXPECT_EQ(dve->GetValueIdx(), carrier_offset);  // ValueIdx() should be offset into underlying tuple (all cols)
    EXPECT_EQ(type::TransientValuePeeker::PeekInteger(cve->GetValue()), 5);
  };

  std::string query = "SELECT o_id from \"ORDER\" where o_carrier_id = 5 ORDER BY o_ol_cnt DESC LIMIT 1 OFFSET 2";
  OptimizeQuery(query, "order", tbl_order_, check);
}

}  // namespace terrier::optimizer