// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include <gsl/gsl>
#include <array>
#include <memory>
#include <unordered_map>
#include <vector>

#include "core/framework/node_unit.h"
#include "core/providers/qnn/builder/qnn_node_group.h"

namespace onnxruntime {
namespace qnn {

class QnnModelWrapper;

/// <summary>
/// Represents a fusion of a Reshape->Gemm->Reshape sequence to a single Gemm node.
/// </summary>
class ReshapeGemmFusion : public IQnnNodeGroup {
 public:
  ReshapeGemmFusion(const NodeUnit& input_reshape_node_unit, const NodeUnit& gemm_node_unit,
                    const NodeUnit& output_reshape_node_unit);
  ORT_DISALLOW_COPY_AND_ASSIGNMENT(ReshapeGemmFusion);

  Status IsSupported(QnnModelWrapper& qmw, const logging::Logger& logger) const override;
  Status AddToModelBuilder(QnnModelWrapper& qmw, const logging::Logger& logger) const override;
  gsl::span<const NodeUnit* const> GetNodeUnits() const override;
  const NodeUnit* GetTargetNodeUnit() const override;
  std::string_view Type() const override { return "ReshapeGemmFusion"; }

  static std::unique_ptr<IQnnNodeGroup> TryFusion(
      QnnModelWrapper& qnn_model_wrapper, const NodeUnit& gemm_node_unit,
      const std::unordered_map<const Node*, const NodeUnit*>& node_to_node_unit,
      const std::unordered_map<const NodeUnit*, const IQnnNodeGroup*>& node_unit_to_qnn_node_group,
      const logging::Logger& logger);

 private:
  std::array<const NodeUnit*, 3> node_units_;
};

}  // namespace qnn
}  // namespace onnxruntime
