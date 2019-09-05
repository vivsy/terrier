#include <utility>
#include <vector>

#include "catalog/catalog_accessor.h"
#include "catalog/index_schema.h"
#include "common/managed_pointer.h"
#include "optimizer/child_property_deriver.h"
#include "optimizer/group_expression.h"
#include "optimizer/index_util.h"
#include "optimizer/memo.h"
#include "optimizer/properties.h"
#include "optimizer/property_set.h"
#include "parser/expression_util.h"

namespace terrier::optimizer {

std::vector<std::pair<PropertySet *, std::vector<PropertySet *>>> ChildPropertyDeriver::GetProperties(
    GroupExpression *gexpr, PropertySet *requirements, Memo *memo, catalog::CatalogAccessor *accessor) {
  requirements_ = requirements;
  output_.clear();
  memo_ = memo;
  gexpr_ = gexpr;
  accessor_ = accessor;
  gexpr->Op().Accept(this);
  return move(output_);
}

void ChildPropertyDeriver::Visit(UNUSED_ATTRIBUTE const SeqScan *op) {
  // Seq Scan does not provide any property
  output_.emplace_back(new PropertySet(), std::vector<PropertySet *>{});
}

void ChildPropertyDeriver::Visit(const IndexScan *op) {
  // Use GetIndexes() to get all indexes on table_alias
  auto tbl_id = accessor_->GetTableOid(op->GetNamespaceOID(), op->GetTableAlias());
  std::vector<catalog::index_oid_t> tbl_indexes = accessor_->GetIndexes(tbl_id);

  for (auto prop : requirements_->Properties()) {
    if (prop->Type() == PropertyType::SORT) {
      auto sort_prop = prop->As<PropertySort>();
      if (!IndexUtil::CheckSortProperty(sort_prop)) {
        continue;
      }

      // Iterate through all the table indexes and check whether any
      // of the indexes can be used to satisfy the sort property.
      for (auto &index : tbl_indexes) {
        if (IndexUtil::SatisfiesSortWithIndex(sort_prop, tbl_id, index, accessor_)) {
          auto prop = requirements_->Copy();
          output_.emplace_back(prop, std::vector<PropertySet *>{});
          break;
        }
      }
    }
  }

  if (output_.empty()) {
    // No index can be used, so output provides no properties
    output_.emplace_back(new PropertySet(), std::vector<PropertySet *>{});
  }
}

void ChildPropertyDeriver::Visit(UNUSED_ATTRIBUTE const ExternalFileScan *op) {
  // External file scans (like sequential scans) do not provide properties
  output_.emplace_back(new PropertySet(), std::vector<PropertySet *>{});
}

void ChildPropertyDeriver::Visit(const QueryDerivedScan *op) {
  auto output = requirements_->Copy();
  auto input = requirements_->Copy();
  output_.emplace_back(output, std::vector<PropertySet *>{input});
}

/**
 * Note:
 * Fulfill the entire projection property in the aggregation. Should
 * enumerate different combination of the aggregation functions and other
 * projection.
 */
void ChildPropertyDeriver::Visit(UNUSED_ATTRIBUTE const HashGroupBy *op) {
  output_.emplace_back(new PropertySet(), std::vector<PropertySet *>{new PropertySet()});
}

void ChildPropertyDeriver::Visit(UNUSED_ATTRIBUTE const SortGroupBy *op) {
  // Child must provide sort for Groupby columns
  std::vector<planner::OrderByOrderingType> sort_ascending(op->GetColumns().size(), planner::OrderByOrderingType::ASC);

  auto sort_prop = new PropertySort(op->GetColumns(), std::move(sort_ascending));
  auto prop_set = new PropertySet(std::vector<Property *>{sort_prop});
  output_.emplace_back(prop_set, std::vector<PropertySet *>{prop_set->Copy()});
}

void ChildPropertyDeriver::Visit(UNUSED_ATTRIBUTE const Aggregate *op) {
  output_.emplace_back(new PropertySet(), std::vector<PropertySet *>{new PropertySet()});
}

void ChildPropertyDeriver::Visit(const Limit *op) {
  // Limit fulfill the internal sort property
  std::vector<PropertySet *> child_input_properties{new PropertySet()};
  auto provided_prop = new PropertySet();
  if (!op->GetSortExpressions().empty()) {
    const std::vector<common::ManagedPointer<const parser::AbstractExpression>> &exprs = op->GetSortExpressions();
    const std::vector<planner::OrderByOrderingType> &sorts{op->GetSortAscending()};
    provided_prop->AddProperty(new PropertySort(exprs, sorts));
  }

  output_.emplace_back(provided_prop, std::move(child_input_properties));
}

void ChildPropertyDeriver::Visit(UNUSED_ATTRIBUTE const Distinct *op) {
  // Let child fulfil all the required properties
  std::vector<PropertySet *> child_input_properties{requirements_->Copy()};
  output_.emplace_back(requirements_->Copy(), std::move(child_input_properties));
}

void ChildPropertyDeriver::Visit(UNUSED_ATTRIBUTE const OrderBy *op) {}
void ChildPropertyDeriver::Visit(UNUSED_ATTRIBUTE const InnerNLJoin *op) { DeriveForJoin(); }
void ChildPropertyDeriver::Visit(UNUSED_ATTRIBUTE const LeftNLJoin *op) {}
void ChildPropertyDeriver::Visit(UNUSED_ATTRIBUTE const RightNLJoin *op) {}
void ChildPropertyDeriver::Visit(UNUSED_ATTRIBUTE const OuterNLJoin *op) {}
void ChildPropertyDeriver::Visit(UNUSED_ATTRIBUTE const InnerHashJoin *op) { DeriveForJoin(); }

void ChildPropertyDeriver::Visit(UNUSED_ATTRIBUTE const LeftHashJoin *op) {}
void ChildPropertyDeriver::Visit(UNUSED_ATTRIBUTE const RightHashJoin *op) {}
void ChildPropertyDeriver::Visit(UNUSED_ATTRIBUTE const OuterHashJoin *op) {}

void ChildPropertyDeriver::Visit(UNUSED_ATTRIBUTE const Insert *op) {
  std::vector<PropertySet *> child_input_properties;
  output_.emplace_back(requirements_->Copy(), std::move(child_input_properties));
}

void ChildPropertyDeriver::Visit(UNUSED_ATTRIBUTE const InsertSelect *op) {
  // Let child fulfil all the required properties
  std::vector<PropertySet *> child_input_properties{requirements_->Copy()};
  output_.emplace_back(requirements_->Copy(), std::move(child_input_properties));
}

void ChildPropertyDeriver::Visit(UNUSED_ATTRIBUTE const Update *op) {
  // Let child fulfil all the required properties
  std::vector<PropertySet *> child_input_properties{requirements_->Copy()};
  output_.emplace_back(requirements_->Copy(), std::move(child_input_properties));
}

void ChildPropertyDeriver::Visit(UNUSED_ATTRIBUTE const Delete *op) {
  // Let child fulfil all the required properties
  std::vector<PropertySet *> child_input_properties{requirements_->Copy()};
  output_.emplace_back(requirements_->Copy(), std::move(child_input_properties));
}

void ChildPropertyDeriver::Visit(UNUSED_ATTRIBUTE const TableFreeScan *op) {
  // Provide nothing
  output_.emplace_back(new PropertySet(), std::vector<PropertySet *>{});
}

void ChildPropertyDeriver::Visit(UNUSED_ATTRIBUTE const ExportExternalFile *op) {
  // Let child fulfil all the required properties
  std::vector<PropertySet *> child_input_properties{requirements_->Copy()};
  output_.emplace_back(requirements_->Copy(), std::move(child_input_properties));
}

void ChildPropertyDeriver::DeriveForJoin() {
  output_.emplace_back(new PropertySet(), std::vector<PropertySet *>{new PropertySet(), new PropertySet()});

  // If there is sort property and all the sort columns are from the probe
  // table (currently right table), we can push down the sort property
  for (auto prop : requirements_->Properties()) {
    if (prop->Type() == PropertyType::SORT) {
      bool can_pass_down = true;

      auto sort_prop = prop->As<PropertySort>();
      size_t sort_col_size = sort_prop->GetSortColumnSize();
      Group *probe_group = memo_->GetGroupByID(gexpr_->GetChildGroupId(1));
      for (size_t idx = 0; idx < sort_col_size; ++idx) {
        ExprSet tuples;
        parser::ExpressionUtil::GetTupleValueExprs(&tuples, sort_prop->GetSortColumn(idx).Get());
        for (auto &expr : tuples) {
          auto tv_expr = dynamic_cast<const parser::ColumnValueExpression *>(expr);
          TERRIER_ASSERT(tv_expr, "Expected ColumnValueExpression");

          // If a column is not in the prob table, we cannot fulfill the sort
          // property in the requirement
          if (probe_group->GetTableAliases().count(tv_expr->GetTableName()) == 0U) {
            can_pass_down = false;
            break;
          }
        }

        if (!can_pass_down) {
          break;
        }
      }

      if (can_pass_down) {
        std::vector<PropertySet *> children{new PropertySet(), requirements_->Copy()};
        output_.emplace_back(requirements_->Copy(), std::move(children));
      }
    }
  }
}

}  // namespace terrier::optimizer