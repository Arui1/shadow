#include "reduce_op.hpp"

namespace Shadow {

void ReduceOp::Forward() {
  const auto bottom = bottoms(0);
  auto top = tops(0);

  CHECK_NE(bottom, top);

  int num_axes = bottom->num_axes();

  if (bottom_shape_ != bottom->shape()) {
    bottom_shape_ = bottom->shape();

    auto axes = axes_;
    if (axes_.empty()) {
      for (int n = 0; n < num_axes; ++n) {
        axes.push_back(n);
      }
    }

    top_shape_ = bottom->shape();
    for (auto axis : axes) {
      top_shape_[bottom->canonical_index(axis)] = 1;
    }
    top->reshape(top_shape_);

    VecInt shape_acc(1, 1);
    for (int n = num_axes - 1; n > 0; --n) {
      shape_acc.insert(shape_acc.begin(), bottom->shape(n) * shape_acc[0]);
    }
    list_value_ = {0};
    for (int n = static_cast<int>(axes.size()) - 1; n >= 0; --n) {
      int axis = axes[n], num_list = static_cast<int>(list_value_.size());
      for (int k = 1; k < bottom->shape(axis); ++k) {
        for (int j = 0; j < num_list; ++j) {
          list_value_.push_back(list_value_[j] + k * shape_acc[axis]);
        }
      }
    }
    offset_value_.clear();
    for (int i = 0; i < top->count(); ++i) {
      int offset = 0, cc = i;
      for (int n = num_axes - 1; n >= 0; --n) {
        offset += (cc % top_shape_[n]) * shape_acc[n];
        cc /= top_shape_[n];
      }
      offset_value_.push_back(offset);
    }
  } else {
    top->reshape(top_shape_);
  }

#if defined(USE_CUDNN)
  cudnn::setReduceDesc<float>(&reduce_desc_, operation_);
  if (num_axes > 4) {
    cudnn::setTensorNdDesc<float>(&bottom_desc_, num_axes,
                                  bottom_shape_.data());
    cudnn::setTensorNdDesc<float>(&top_desc_, num_axes, top_shape_.data());
  } else {
    auto bottom_shape = bottom->shape(), top_shape = top->shape();
    for (int n = num_axes; n < 4; ++n) {
      bottom_shape.push_back(1);
      top_shape.push_back(1);
    }
    cudnn::setTensor4dDesc<float>(&bottom_desc_, bottom_shape[0],
                                  bottom_shape[1], bottom_shape[2],
                                  bottom_shape[3]);
    cudnn::setTensor4dDesc<float>(&top_desc_, top_shape[0], top_shape[1],
                                  top_shape[2], top_shape[3]);
  }

  size_t workspace_size = 0;

  CUDNN_CHECK(cudnnGetReductionWorkspaceSize(
      cudnnHandle_t(ws_->Ctx()->cudnn_handle()), reduce_desc_, bottom_desc_,
      top_desc_, &workspace_size));

  std::shared_ptr<Blob> workspace = nullptr;
  const void *workspace_ptr = nullptr;
  if (workspace_size > 0) {
    ws_->GrowTempBuffer(workspace_size);
    workspace =
        ws_->CreateTempBlob({static_cast<int>(workspace_size)}, DataType::kU8);
    workspace_ptr = workspace->data<unsigned char>();
  }

  CUDNN_CHECK(cudnnReduceTensor(
      cudnnHandle_t(ws_->Ctx()->cudnn_handle()), reduce_desc_, nullptr, 0,
      const_cast<void *>(workspace_ptr), workspace_size,
      cudnn::dataType<float>::one, bottom_desc_, bottom->data<float>(),
      cudnn::dataType<float>::zero, top_desc_, top->mutable_data<float>()));

#else
  int num_list = static_cast<int>(list_value_.size());
  int num_offset = static_cast<int>(offset_value_.size());

  ws_->GrowTempBuffer((num_list + num_offset) * sizeof(int));

  auto list = ws_->CreateTempBlob({num_list}, DataType::kI32);
  auto offset = ws_->CreateTempBlob({num_offset}, DataType::kI32);

  list->set_data<int>(list_value_.data(), num_list);
  offset->set_data<int>(offset_value_.data(), num_offset);

  Vision::Reduce(bottom->data<float>(), list->data<int>(), offset->data<int>(),
                 num_list, operation_, top->count(), top->mutable_data<float>(),
                 ws_->Ctx());
#endif

  if (!keep_dims_) {
    VecInt shape;
    for (int n = 0; n < num_axes; ++n) {
      bool need_squeeze = axes_.empty();
      for (auto axis : axes_) {
        if (n == axis) {
          need_squeeze = true;
          break;
        }
      }
      int dim = top->shape(n);
      if (need_squeeze) {
        CHECK_EQ(dim, 1);
      } else {
        shape.push_back(dim);
      }
    }
    top->set_shape(shape);
  }
}

REGISTER_OPERATOR(Reduce, ReduceOp);

namespace Vision {

#if !defined(USE_CUDA)
template <typename T>
inline T Reduce(const T *data, const int *list, int num_list, int offset,
                int operation) {
  switch (operation) {
    case ReduceOp::kProd: {
      T val = 1;
      for (int i = 0; i < num_list; ++i) {
        val *= data[list[i] + offset];
      }
      return val;
    }
    case ReduceOp::kSum: {
      T val = 0;
      for (int i = 0; i < num_list; ++i) {
        val += data[list[i] + offset];
      }
      return val;
    }
    case ReduceOp::kMax: {
      T val = std::numeric_limits<T>::lowest();
      for (int i = 0; i < num_list; ++i) {
        val = std::max(val, data[list[i] + offset]);
      }
      return val;
    }
    case ReduceOp::kMin: {
      T val = std::numeric_limits<T>::max();
      for (int i = 0; i < num_list; ++i) {
        val = std::min(val, data[list[i] + offset]);
      }
      return val;
    }
    case ReduceOp::kAvg: {
      T val = 0;
      for (int i = 0; i < num_list; ++i) {
        val += data[list[i] + offset];
      }
      return val / num_list;
    }
    default:
      return 0;
  }
}

template <typename T>
void Reduce(const T *in_data, const int *list_data, const int *offset_data,
            int num_list, int operation, int count, T *out_data,
            Context *context) {
  for (int i = 0; i < count; ++i) {
    out_data[i] =
        Reduce(in_data, list_data, num_list, offset_data[i], operation);
  }
}

template void Reduce(const float *, const int *, const int *, int, int, int,
                     float *, Context *);
#endif

}  // namespace Vision

}  // namespace Shadow
