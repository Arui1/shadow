#ifndef SHADOW_OPERATORS_NORMALIZE_OP_HPP
#define SHADOW_OPERATORS_NORMALIZE_OP_HPP

#include "core/operator.hpp"

namespace Shadow {

class NormalizeOp : public Operator {
 public:
  NormalizeOp(const shadow::OpParam &op_param, Workspace *ws)
      : Operator(op_param, ws) {
    across_spatial_ = get_single_argument<bool>("across_spatial", true);
    channel_shared_ = get_single_argument<bool>("channel_shared", true);
  }

  void Forward() override;

 private:
  bool across_spatial_, channel_shared_;
};

}  // namespace Shadow

#endif  // SHADOW_OPERATORS_NORMALIZE_OP_HPP
