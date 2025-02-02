/* Copyright (c) 2020 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include "graph/planner/match/MatchPlanner.h"

#include "graph/context/ast/CypherAstContext.h"
#include "graph/planner/match/MatchClausePlanner.h"
#include "graph/planner/match/ReturnClausePlanner.h"
#include "graph/planner/match/SegmentsConnector.h"
#include "graph/planner/match/UnwindClausePlanner.h"
#include "graph/planner/match/WhereClausePlanner.h"
#include "graph/planner/match/WithClausePlanner.h"
#include "graph/planner/plan/Algo.h"
#include "graph/planner/plan/Logic.h"
#include "graph/planner/plan/Query.h"
#include "graph/util/ExpressionUtils.h"

namespace nebula {
namespace graph {
bool MatchPlanner::match(AstContext* astCtx) {
  return astCtx->sentence->kind() == Sentence::Kind::kMatch;
}

StatusOr<SubPlan> MatchPlanner::transform(AstContext* astCtx) {
  if (astCtx->sentence->kind() != Sentence::Kind::kMatch) {
    return Status::Error("Only MATCH is accepted for match planner.");
  }
  auto* cypherCtx = static_cast<CypherContext*>(astCtx);
  SubPlan queryPlan;
  for (auto& queryPart : cypherCtx->queryParts) {
    NG_RETURN_IF_ERROR(genQueryPartPlan(cypherCtx->qctx, queryPlan, queryPart));
  }

  return queryPlan;
}

StatusOr<SubPlan> MatchPlanner::genPlan(CypherClauseContextBase* clauseCtx) {
  switch (clauseCtx->kind) {
    case CypherClauseKind::kMatch: {
      return std::make_unique<MatchClausePlanner>()->transform(clauseCtx);
    }
    case CypherClauseKind::kUnwind: {
      return std::make_unique<UnwindClausePlanner>()->transform(clauseCtx);
    }
    case CypherClauseKind::kWith: {
      return std::make_unique<WithClausePlanner>()->transform(clauseCtx);
    }
    case CypherClauseKind::kReturn: {
      return std::make_unique<ReturnClausePlanner>()->transform(clauseCtx);
    }
    default: {
      return Status::Error("Unsupported clause.");
    }
  }
  return Status::OK();
}

// Connect current match plan to previous queryPlan
Status MatchPlanner::connectMatchPlan(SubPlan& queryPlan, MatchClauseContext* matchCtx) {
  // Generate current match plan
  auto matchPlanStatus = genPlan(matchCtx);
  NG_RETURN_IF_ERROR(matchPlanStatus);
  auto matchPlan = std::move(matchPlanStatus).value();

  if (queryPlan.root == nullptr) {
    queryPlan = matchPlan;
    return Status::OK();
  }
  std::unordered_set<std::string> intersectedAliases;
  for (auto& alias : matchCtx->aliasesGenerated) {
    auto it = matchCtx->aliasesAvailable.find(alias.first);
    if (it != matchCtx->aliasesAvailable.end()) {
      // Joined type should be same
      if (it->second != alias.second) {
        return Status::SemanticError(fmt::format("{} binding to different type: {} vs {}",
                                                 alias.first,
                                                 AliasTypeName[static_cast<int>(alias.second)],
                                                 AliasTypeName[static_cast<int>(it->second)]));
      }
      // Joined On EdgeList is not supported
      if (alias.second == AliasType::kEdgeList) {
        return Status::SemanticError(alias.first +
                                     " defined with type EdgeList, which cannot be joined on");
      }
      intersectedAliases.insert(alias.first);
    }
  }
  if (!intersectedAliases.empty()) {
    if (matchPlan.tail->kind() == PlanNode::Kind::kArgument) {
      // The input of the argument operator is always the output of the plan on the other side of
      // the join
      matchPlan.tail->setInputVar(queryPlan.root->outputVar());
    }
    if (matchCtx->isOptional) {
      // connect LeftJoin match filter
      auto& whereCtx = matchCtx->where;
      if (whereCtx.get() != nullptr && whereCtx->filter != nullptr) {
        auto exprs = ExpressionUtils::collectAll(
            whereCtx->filter, {Expression::Kind::kVarProperty, Expression::Kind::kLabel});

        // Check if all aliases in where clause are generated by the current match statement pattern
        std::vector<std::string> aliases;
        for (const auto* expr : exprs) {
          if (expr->kind() == Expression::Kind::kVarProperty) {
            aliases.emplace_back(static_cast<const PropertyExpression*>(expr)->prop());
          } else {
            DCHECK_EQ(expr->kind(), Expression::Kind::kLabel);
            aliases.emplace_back(static_cast<const LabelExpression*>(expr)->name());
          }
        }
        auto aliasesGenerated = matchCtx->aliasesGenerated;
        if (!std::all_of(aliases.begin(), aliases.end(), [&aliasesGenerated](std::string& alias) {
              return aliasesGenerated.find(alias) != aliasesGenerated.end();
            })) {
          return Status::SemanticError(
              "The where clause of optional match statement that reference variables defined by "
              "other statements is not supported yet.");
        }
        whereCtx->inputColNames = matchPlan.root->colNames();
        auto wherePlanStatus =
            std::make_unique<WhereClausePlanner>()->transform(matchCtx->where.get());
        NG_RETURN_IF_ERROR(wherePlanStatus);
        auto wherePlan = std::move(wherePlanStatus).value();
        matchPlan = SegmentsConnector::addInput(wherePlan, matchPlan, true);
      }
      queryPlan =
          SegmentsConnector::leftJoin(matchCtx->qctx, queryPlan, matchPlan, intersectedAliases);
    } else {
      queryPlan =
          SegmentsConnector::innerJoin(matchCtx->qctx, queryPlan, matchPlan, intersectedAliases);
    }
  } else {
    queryPlan.root = BiCartesianProduct::make(matchCtx->qctx, queryPlan.root, matchPlan.root);
  }

  return Status::OK();
}

Status MatchPlanner::genQueryPartPlan(QueryContext* qctx,
                                      SubPlan& queryPlan,
                                      const QueryPart& queryPart) {
  // generate plan for matchs
  for (auto& match : queryPart.matchs) {
    NG_RETURN_IF_ERROR(connectMatchPlan(queryPlan, match.get()));
    // connect match filter
    if (match->where != nullptr && !match->isOptional) {
      match->where->inputColNames = queryPlan.root->colNames();
      auto wherePlanStatus = std::make_unique<WhereClausePlanner>()->transform(match->where.get());
      NG_RETURN_IF_ERROR(wherePlanStatus);
      auto wherePlan = std::move(wherePlanStatus).value();
      queryPlan = SegmentsConnector::addInput(wherePlan, queryPlan, true);
    }
  }

  if (queryPlan.root != nullptr) {
    queryPart.boundary->inputColNames = queryPlan.root->colNames();
  }

  // generate plan for boundary
  auto boundaryPlanStatus = genPlan(queryPart.boundary.get());
  NG_RETURN_IF_ERROR(boundaryPlanStatus);
  auto boundaryPlan = std::move(boundaryPlanStatus).value();
  if (queryPlan.root == nullptr) {
    queryPlan = boundaryPlan;
  } else {
    queryPlan = SegmentsConnector::addInput(boundaryPlan, queryPlan, false);
  }

  // TBD: need generate var for all queryPlan.tail?
  if (queryPlan.tail->isSingleInput()) {
    queryPlan.tail->setInputVar(qctx->vctx()->anonVarGen()->getVar());
    if (!tailConnected_) {
      auto start = StartNode::make(qctx);
      queryPlan.tail->setDep(0, start);
      tailConnected_ = true;
      queryPlan.tail = start;
    }
  }
  VLOG(1) << queryPlan;

  return Status::OK();
}
}  // namespace graph
}  // namespace nebula
