#include "vision.hpp"
#include "common.hpp"
#include "kernel.hpp"
#include "util/util.hpp"

#include <cmath>

namespace Shadow {

namespace Vision {

#if !defined(USE_CUDA) & !defined(USE_CL)
template <typename T>
void DataTransform(const T *in_data, const VecInt &in_shape, float scale,
                   int num_mean, const T *mean_value, T *out_data) {
  int in_c = in_shape[1], spatial_dim = in_shape[2] * in_shape[3];
  int count = in_shape[0] * in_c * spatial_dim;
  if (num_mean == 1) {
    for (int i = 0; i < count; ++i) {
      out_data[i] = (in_data[i] - mean_value[0]) * scale;
    }
  } else if (num_mean == in_c) {
    for (int i = 0; i < count; ++i) {
      int c_out = (i / spatial_dim) % in_c;
      out_data[i] = (in_data[i] - mean_value[c_out]) * scale;
    }
  } else if (num_mean == in_c * spatial_dim) {
    for (int i = 0; i < count; ++i) {
      int c_out = (i / spatial_dim) % in_c;
      int s_out = i % spatial_dim;
      out_data[i] =
          (in_data[i] - mean_value[c_out * spatial_dim + s_out]) * scale;
    }
  }
}

// check for 0 <= a < b
inline bool check_border(int a, int b) {
  return static_cast<unsigned>(a) < static_cast<unsigned>(b);
}

template <typename T>
void Im2Col(const T *in_data, const VecInt &in_shape, int offset,
            int kernel_size, int stride, int pad, int dilation, int zero_point,
            const VecInt &out_shape, T *out_data) {
  in_data += offset;
  int in_c = in_shape[1], in_h = in_shape[2], in_w = in_shape[3];
  int out_h = out_shape[2], out_w = out_shape[3];
  int spatial_dim = in_h * in_w;
  for (int k_c = 0; k_c < in_c; ++k_c, in_data += spatial_dim) {
    for (int k_s = 0; k_s < kernel_size * kernel_size; ++k_s) {
      int k_h = k_s / kernel_size;
      int k_w = k_s % kernel_size;
      int im_row = -pad + k_h * dilation;
      for (int h = 0; h < out_h; ++h, im_row += stride) {
        if (check_border(im_row, in_h)) {
          int im_col = -pad + k_w * dilation;
          for (int w = 0; w < out_w; ++w, im_col += stride) {
            if (check_border(im_col, in_w)) {
              *(out_data++) = in_data[im_row * in_w + im_col];
            } else {
              *(out_data++) = static_cast<T>(zero_point);
            }
          }
        } else {
          for (int w = 0; w < out_w; ++w) {
            *(out_data++) = static_cast<T>(zero_point);
          }
        }
      }
    }
  }
}

template <typename T>
void Pooling(const T *in_data, const VecInt &in_shape, int kernel_size,
             int stride, int pad, int mode, const VecInt &out_shape,
             T *out_data) {
  int batch = in_shape[0];
  int in_c = in_shape[1], in_h = in_shape[2], in_w = in_shape[3];
  int out_h = out_shape[2], out_w = out_shape[3];
  for (int b = 0; b < batch; ++b) {
    for (int c = 0; c < in_c; ++c) {
      for (int h = 0; h < out_h; ++h) {
        for (int w = 0; w < out_w; ++w) {
          int kistart = h * stride - pad, kjstart = w * stride - pad;
          int kiend = std::min(kistart + kernel_size, in_h + pad);
          int kjend = std::min(kjstart + kernel_size, in_w + pad);
          int pool_size = (kiend - kistart) * (kjend - kjstart);
          kistart = std::max(kistart, 0), kjstart = std::max(kjstart, 0);
          kiend = std::min(kiend, in_h), kjend = std::min(kjend, in_w);
          T max = std::numeric_limits<T>::lowest();
          auto sum = T(0);
          for (int ki = kistart; ki < kiend; ++ki) {
            for (int kj = kjstart; kj < kjend; ++kj) {
              int index = kj + in_w * (ki + in_h * (c + in_c * b));
              T value = in_data[index];
              max = (value > max) ? value : max;
              sum += value;
            }
          }
          int out_index = w + out_w * (h + out_h * (c + in_c * b));
          out_data[out_index] = (mode == 0) ? max : sum / pool_size;
        }
      }
    }
  }
}

template <typename T>
void Concat(const T *in_data, int count, int num_concats, int concat_size,
            int top_concat_axis, int bottom_concat_axis, int offset_concat_axis,
            T *out_data) {
  for (int n = 0; n < num_concats; ++n) {
    memcpy(out_data + (n * top_concat_axis + offset_concat_axis) * concat_size,
           in_data + n * bottom_concat_axis * concat_size,
           bottom_concat_axis * concat_size * sizeof(T));
  }
}

template <typename T, typename Dtype>
void Permute(const T *in_data, int count, int num_axes,
             const Dtype *permute_order, const Dtype *old_steps,
             const Dtype *new_steps, T *out_data) {
  for (int i = 0; i < count; ++i) {
    int old_idx = 0;
    int idx = i;
    for (int j = 0; j < num_axes; ++j) {
      int order = permute_order[j];
      old_idx += (idx / new_steps[j]) * old_steps[order];
      idx %= new_steps[j];
    }
    out_data[i] = in_data[old_idx];
  }
}

template <typename T>
void Scale(const T *in_data, int count, const T *scale_data, const T *bias_data,
           int scale_dim, int inner_dim, T *out_data) {
  for (int i = 0; i < count; ++i) {
    int index = (i / inner_dim) % scale_dim;
    out_data[i] = in_data[i] * scale_data[index] + bias_data[index];
  }
}

template <typename T>
void Bias(const T *in_data, int count, const T *bias_data, int bias_dim,
          int inner_dim, T *out_data) {
  for (int i = 0; i < count; ++i) {
    int index = (i / inner_dim) % bias_dim;
    out_data[i] = in_data[i] + bias_data[index];
  }
}

template <typename T>
void Reorg(const T *in_data, const VecInt &in_shape, int stride, T *out_data) {
  int batch = in_shape[0], in_c = in_shape[1];
  int in_h = in_shape[2], in_w = in_shape[3];
  int out_c = in_c * stride * stride;
  int out_h = in_h / stride, out_w = in_w / stride;
  for (int b = 0; b < batch; ++b) {
    for (int c = 0; c < out_c; ++c) {
      for (int h = 0; h < out_h; ++h) {
        for (int w = 0; w < out_w; ++w) {
          int c_in = c % in_c;
          int area = c / in_c;
          int h_in = h * stride + area / stride;
          int w_in = w * stride + area % stride;
          int in_index = ((b * in_c + c_in) * in_h + h_in) * in_w + w_in;
          int out_index = ((b * out_c + c) * out_h + h) * out_w + w;
          out_data[out_index] = in_data[in_index];
        }
      }
    }
  }
}

template <typename T>
void LRN(const T *in_data, const VecInt &in_shape, int size, float alpha,
         float beta, float k, T *scale_data, T *out_data) {
  int batch = in_shape[0], in_c = in_shape[1];
  int in_h = in_shape[2], in_w = in_shape[3];
  int step = in_h * in_w, count = batch * in_c * step;
  int pre_pad = (size - 1) / 2, post_pad = size - pre_pad - 1;
  float alpha_over_size = alpha / size;
  for (int b = 0; b < batch; ++b) {
    for (int h = 0; h < in_h; ++h) {
      for (int w = 0; w < in_w; ++w) {
        int offset = (b * in_c * in_h + h) * in_w + w, head = 0;
        const T *in_off = in_data + offset;
        T *scale_off = scale_data + offset;
        auto accum_scale = T(0);
        while (head < post_pad && head < in_c) {
          accum_scale += in_off[head * step] * in_off[head * step];
          head++;
        }
        while (head < in_c) {
          accum_scale += in_off[head * step] * in_off[head * step];
          if (head - size >= 0) {
            accum_scale -=
                in_off[(head - size) * step] * in_off[(head - size) * step];
          }
          scale_off[(head - post_pad) * step] =
              k + accum_scale * alpha_over_size;
          head++;
        }
        while (head < in_c + post_pad) {
          if (head - size >= 0) {
            accum_scale -=
                in_off[(head - size) * step] * in_off[(head - size) * step];
          }
          scale_off[(head - post_pad) * step] =
              k + accum_scale * alpha_over_size;
          head++;
        }
      }
    }
  }
  for (int i = 0; i < count; ++i) {
    out_data[i] = in_data[i] * std::pow(scale_data[i], -beta);
  }
}

template <typename T>
void ROIPooling(const T *in_data, const VecInt &in_shape, const T *roi_data,
                int num_rois, int pooled_h, int pooled_w, float spatial_scale,
                T *out_data) {
  int batch = in_shape[0];
  int in_c = in_shape[1], in_h = in_shape[2], in_w = in_shape[3];
  int in_num = in_c * in_h * in_w;
  for (int n = 0; n < num_rois; ++n) {
    int roi_offset = 5 * n;
    int roi_batch_id = roi_data[roi_offset];
    int roi_start_w = Util::round(roi_data[roi_offset + 1] * spatial_scale);
    int roi_start_h = Util::round(roi_data[roi_offset + 2] * spatial_scale);
    int roi_end_w = Util::round(roi_data[roi_offset + 3] * spatial_scale);
    int roi_end_h = Util::round(roi_data[roi_offset + 4] * spatial_scale);
    assert(roi_batch_id >= 0);
    assert(roi_batch_id < batch);
    int roi_height = std::max(roi_end_h - roi_start_h + 1, 1);
    int roi_width = std::max(roi_end_w - roi_start_w + 1, 1);
    float bin_size_h = roi_height / static_cast<float>(pooled_h);
    float bin_size_w = roi_width / static_cast<float>(pooled_w);
    const T *batch_data = in_data + roi_batch_id * in_num;
    for (int c = 0; c < in_c; ++c) {
      for (int ph = 0; ph < pooled_h; ++ph) {
        for (int pw = 0; pw < pooled_w; ++pw) {
          auto hstart = static_cast<int>(std::floor(ph * bin_size_h));
          auto wstart = static_cast<int>(std::floor(pw * bin_size_w));
          auto hend = static_cast<int>(std::ceil((ph + 1) * bin_size_h));
          auto wend = static_cast<int>(std::ceil((pw + 1) * bin_size_w));
          hstart = std::min(std::max(hstart + roi_start_h, 0), in_h);
          hend = std::min(std::max(hend + roi_start_h, 0), in_h);
          wstart = std::min(std::max(wstart + roi_start_w, 0), in_w);
          wend = std::min(std::max(wend + roi_start_w, 0), in_w);
          bool is_empty = (hend <= hstart) || (wend <= wstart);
          T max =
              is_empty ? T(0) : batch_data[(c * in_h + hstart) * in_w + wstart];
          for (int h = hstart; h < hend; ++h) {
            for (int w = wstart; w < wend; ++w) {
              max = std::max(max, batch_data[(c * in_h + h) * in_w + w]);
            }
          }
          int pool_index = ((n * in_c + c) * pooled_h + ph) * pooled_w + pw;
          out_data[pool_index] = max;
        }
      }
    }
  }
}

template <typename T>
void Proposal(const T *anchor_data, const T *score_data, const T *delta_data,
              const T *info_data, const VecInt &in_shape, int num_anchors,
              int feat_stride, int min_size, T *proposal_data) {
  int in_h = in_shape[2], in_w = in_shape[3], spatial_dim = in_h * in_w;
  int num_proposals = spatial_dim * num_anchors;
  T im_h = info_data[0], im_w = info_data[1], im_scale = info_data[2];
  T min_box_size = min_size * im_scale;
  for (int n = 0; n < num_anchors; ++n) {
    const auto *anchor_ptr = anchor_data + n * 4;
    const auto *score_ptr = score_data + num_proposals + n * spatial_dim;
    const auto *dx_ptr = delta_data + (n * 4 + 0) * spatial_dim;
    const auto *dy_ptr = delta_data + (n * 4 + 1) * spatial_dim;
    const auto *dw_ptr = delta_data + (n * 4 + 2) * spatial_dim;
    const auto *dh_ptr = delta_data + (n * 4 + 3) * spatial_dim;
    T anchor_w = anchor_ptr[2] - anchor_ptr[0] + 1;
    T anchor_h = anchor_ptr[3] - anchor_ptr[1] + 1;
    for (int h = 0; h < in_h; ++h) {
      for (int w = 0; w < in_w; ++w) {
        int spatial_offset = h * in_w + w;
        T anchor_x = anchor_ptr[0] + w * feat_stride;
        T anchor_y = anchor_ptr[1] + h * feat_stride;
        T anchor_cx = anchor_x + anchor_w * T(0.5);
        T anchor_cy = anchor_y + anchor_h * T(0.5);
        T dx = dx_ptr[spatial_offset], dy = dy_ptr[spatial_offset];
        T dw = dw_ptr[spatial_offset], dh = dh_ptr[spatial_offset];
        T pb_cx = anchor_cx + anchor_w * dx;
        T pb_cy = anchor_cy + anchor_h * dy;
        T pb_w = anchor_w * std::exp(dw), pb_h = anchor_h * std::exp(dh);
        T pb_xmin = pb_cx - pb_w * T(0.5);
        T pb_ymin = pb_cy - pb_h * T(0.5);
        T pb_xmax = pb_cx + pb_w * T(0.5);
        T pb_ymax = pb_cy + pb_h * T(0.5);
        auto *prop_ptr = proposal_data + (spatial_offset * num_anchors + n) * 6;
        prop_ptr[0] = std::min(std::max(pb_xmin, T(0)), im_w - 1);
        prop_ptr[1] = std::min(std::max(pb_ymin, T(0)), im_h - 1);
        prop_ptr[2] = std::min(std::max(pb_xmax, T(0)), im_w - 1);
        prop_ptr[3] = std::min(std::max(pb_ymax, T(0)), im_h - 1);
        prop_ptr[4] = score_ptr[spatial_offset];
        pb_w = prop_ptr[2] - prop_ptr[0] + 1;
        pb_h = prop_ptr[3] - prop_ptr[1] + 1;
        prop_ptr[5] = (pb_w >= min_box_size) && (pb_h >= min_box_size);
      }
    }
  }
}

template <typename T>
inline T Activate(T x, int type, float slope) {
  switch (type) {
    case 1:
      return x * (x > 0);
    case 2:
      return x > 0 ? x : T(slope * x);
    case 3:
      return 1 / (1 + std::exp(-x));
    case 4:
      return std::log(1 + std::exp(x));
    case 5: {
      T exp_2x = std::exp(2 * x);
      return (exp_2x - 1) / (exp_2x + 1);
    }
    default:
      return x;
  }
}

template <typename T>
void Activate(T *data, int count, int type, float slope) {
// PRelu: 0, Relu: 1, Leaky: 2, Sigmoid: 3, SoftPlus: 4, Tanh: 5
#if defined(USE_Eigen)
  auto data_eigen = MapVector<T>(data, count);
  switch (type) {
    case 1:
      data_eigen = data_eigen.cwiseMax(T(0));
      break;
    case 2:
      data_eigen = data_eigen.unaryExpr(
          [slope](T x) { return x > 0 ? x : T(slope * x); });
      break;
    case 3:
      data_eigen =
          data_eigen.unaryExpr([](T x) { return 1 / (1 + std::exp(-x)); });
      break;
    case 4:
      data_eigen =
          data_eigen.unaryExpr([](T x) { return std::log(1 + std::exp(x)); });
      break;
    case 5:
      data_eigen = data_eigen.unaryExpr([](T x) {
        T exp_2x = std::exp(2 * x);
        return (exp_2x - 1) / (exp_2x + 1);
      });
      break;
    default:
      return;
  }
#else
  for (int i = 0; i < count; ++i) {
    data[i] = Activate(data[i], type, slope);
  }
#endif
}

template <typename T>
void PRelu(T *data, const VecInt &in_shape, bool channel_shared,
           const T *slope_data) {
  int channels = in_shape[1], dim = 1;
  for (int i = 2; i < in_shape.size(); ++i) dim *= in_shape[i];
  int count = in_shape[0] * channels * dim;
  int div_factor = channel_shared ? channels : 1;
  for (int i = 0; i < count; ++i) {
    int c = (i / dim) % channels / div_factor;
    data[i] = data[i] > 0 ? data[i] : data[i] * slope_data[c];
  }
}

// Explicit instantiation
template void DataTransform(const float *in_data, const VecInt &in_shape,
                            float scale, int num_mean, const float *mean_value,
                            float *out_data);
template void Im2Col(const float *in_data, const VecInt &in_shape, int offset,
                     int kernel_size, int stride, int pad, int dilation,
                     int zero_point, const VecInt &out_shape, float *out_data);
template void Im2Col(const unsigned char *in_data, const VecInt &in_shape,
                     int offset, int kernel_size, int stride, int pad,
                     int dilation, int zero_point, const VecInt &out_shape,
                     unsigned char *out_data);
template void Pooling(const float *in_data, const VecInt &in_shape,
                      int kernel_size, int stride, int pad, int mode,
                      const VecInt &out_shape, float *out_data);
template void Concat(const float *in_data, int count, int num_concats,
                     int concat_size, int top_concat_axis,
                     int bottom_concat_axis, int offset_concat_axis,
                     float *out_data);
template void Permute(const float *in_data, int count, int num_axes,
                      const int *permute_order, const int *old_steps,
                      const int *new_steps, float *out_data);
template void Scale(const float *in_data, int count, const float *scale_data,
                    const float *bias_data, int scale_dim, int inner_dim,
                    float *out_data);
template void Bias(const float *in_data, int count, const float *bias_data,
                   int bias_dim, int inner_dim, float *out_data);
template void Reorg(const float *in_data, const VecInt &in_shape, int stride,
                    float *out_data);
template void LRN(const float *in_data, const VecInt &in_shape, int size,
                  float alpha, float beta, float k, float *scale_data,
                  float *out_data);
template void ROIPooling(const float *in_data, const VecInt &in_shape,
                         const float *roi_data, int num_rois, int pooled_h,
                         int pooled_w, float spatial_scale, float *out_data);
template void Proposal(const float *anchor_data, const float *score_data,
                       const float *delta_data, const float *info_data,
                       const VecInt &in_shape, int num_anchors, int feat_stride,
                       int min_size, float *proposal_data);
template void Activate(float *data, int count, int type, float slope);
template void PRelu(float *data, const VecInt &in_shape, bool channel_shared,
                    const float *slope_data);

#elif defined(USE_CL)
template <typename T>
void DataTransform(const T *in_data, const VecInt &in_shape, float scale,
                   int num_mean, const T *mean_value, T *out_data) {
  int in_c = in_shape[1], spatial_dim = in_shape[2] * in_shape[3];
  int count = in_shape[0] * in_c * spatial_dim;

  size_t global = count;
  auto *kernel = Kernel::cl_kernels_["DataTransform"];
  kernel->SetArguments(*in_data, count, in_c, spatial_dim, scale, num_mean,
                       *mean_value, *out_data);
  kernel->Launch(*Kernel::queue_, {global}, Kernel::event_);
  Kernel::queue_->Finish();
}

template <typename T>
void Im2Col(const T *in_data, const VecInt &in_shape, int offset,
            int kernel_size, int stride, int pad, int dilation, int zero_point,
            const VecInt &out_shape, T *out_data) {
  int in_c = in_shape[1], in_h = in_shape[2], in_w = in_shape[3];
  int out_h = out_shape[2], out_w = out_shape[3];
  int count = in_c * out_h * out_w;

  size_t global = count;
  auto *kernel = Kernel::cl_kernels_["Im2Col"];
  kernel->SetArguments(*in_data, offset, count, in_c, in_h, in_w, kernel_size,
                       stride, pad, dilation, zero_point, out_h, out_w,
                       *out_data);
  kernel->Launch(*Kernel::queue_, {global}, Kernel::event_);
  Kernel::queue_->Finish();
}

template <typename T>
void Pooling(const T *in_data, const VecInt &in_shape, int kernel_size,
             int stride, int pad, int mode, const VecInt &out_shape,
             T *out_data) {
  int batch = in_shape[0];
  int in_c = in_shape[1], in_h = in_shape[2], in_w = in_shape[3];
  int out_h = out_shape[2], out_w = out_shape[3];
  int count = batch * in_c * out_h * out_w;

  size_t global = count;
  auto *kernel = Kernel::cl_kernels_["Pooling"];
  kernel->SetArguments(*in_data, count, in_c, in_h, in_w, kernel_size, stride,
                       pad, mode, out_h, out_w, *out_data);
  kernel->Launch(*Kernel::queue_, {global}, Kernel::event_);
  Kernel::queue_->Finish();
}

template <typename T>
void Concat(const T *in_data, int count, int num_concats, int concat_size,
            int top_concat_axis, int bottom_concat_axis, int offset_concat_axis,
            T *out_data) {
  size_t global = count;
  auto *kernel = Kernel::cl_kernels_["Concat"];
  kernel->SetArguments(*in_data, count, num_concats, concat_size,
                       top_concat_axis, bottom_concat_axis, offset_concat_axis,
                       *out_data);
  kernel->Launch(*Kernel::queue_, {global}, Kernel::event_);
  Kernel::queue_->Finish();
}

template <typename T, typename Dtype>
void Permute(const T *in_data, int count, int num_axes,
             const Dtype *permute_order, const Dtype *old_steps,
             const Dtype *new_steps, T *out_data) {
  size_t global = count;
  auto *kernel = Kernel::cl_kernels_["Permute"];
  kernel->SetArguments(*in_data, count, num_axes, *permute_order, *old_steps,
                       *new_steps, *out_data);
  kernel->Launch(*Kernel::queue_, {global}, Kernel::event_);
  Kernel::queue_->Finish();
}

template <typename T>
void Scale(const T *in_data, int count, const T *scale_data, const T *bias_data,
           int scale_dim, int inner_dim, T *out_data) {
  size_t global = count;
  auto *kernel = Kernel::cl_kernels_["Scale"];
  kernel->SetArguments(*in_data, count, *scale_data, *bias_data, scale_dim,
                       inner_dim, *out_data);
  kernel->Launch(*Kernel::queue_, {global}, Kernel::event_);
  Kernel::queue_->Finish();
}

template <typename T>
void Bias(const T *in_data, int count, const T *bias_data, int bias_dim,
          int inner_dim, T *out_data) {
  size_t global = count;
  auto *kernel = Kernel::cl_kernels_["Bias"];
  kernel->SetArguments(*in_data, count, *bias_data, bias_dim, inner_dim,
                       *out_data);
  kernel->Launch(*Kernel::queue_, {global}, Kernel::event_);
  Kernel::queue_->Finish();
}

template <typename T>
void Reorg(const T *in_data, const VecInt &in_shape, int stride, T *out_data) {
  int batch = in_shape[0];
  int in_c = in_shape[1], in_h = in_shape[2], in_w = in_shape[3];
  int out_c = in_c * stride * stride;
  int out_h = in_h / stride, out_w = in_w / stride;
  int count = batch * out_c * out_h * out_w;

  size_t global = count;
  auto *kernel = Kernel::cl_kernels_["Reorg"];
  kernel->SetArguments(*in_data, count, in_c, in_h, in_w, out_c, out_h, out_w,
                       stride, *out_data);
  kernel->Launch(*Kernel::queue_, {global}, Kernel::event_);
  Kernel::queue_->Finish();
}

template <typename T>
void LRN(const T *in_data, const VecInt &in_shape, int size, float alpha,
         float beta, float k, T *scale_data, T *out_data) {
  int batch = in_shape[0], in_c = in_shape[1];
  int in_h = in_shape[2], in_w = in_shape[3];
  float alpha_over_size = alpha / size, negative_beta = -beta;
  int count = batch * in_h * in_w;

  size_t global = count;
  auto *kernel = Kernel::cl_kernels_["LRNFillScale"];
  kernel->SetArguments(*in_data, count, in_c, in_h, in_w, size, alpha_over_size,
                       k, *scale_data);
  kernel->Launch(*Kernel::queue_, {global}, Kernel::event_);
  Kernel::queue_->Finish();

  count *= in_c;
  global = count;
  kernel = Kernel::cl_kernels_["LRN"];
  kernel->SetArguments(*in_data, count, *scale_data, negative_beta, *out_data);
  kernel->Launch(*Kernel::queue_, {global}, Kernel::event_);
  Kernel::queue_->Finish();
}

template <typename T>
void ROIPooling(const T *in_data, const VecInt &in_shape, const T *roi_data,
                int num_rois, int pooled_h, int pooled_w, float spatial_scale,
                T *out_data) {
  int in_c = in_shape[1], in_h = in_shape[2], in_w = in_shape[3];
  int count = num_rois * in_c * pooled_h * pooled_w;

  size_t global = count;
  auto *kernel = Kernel::cl_kernels_["POIPooling"];
  kernel->SetArguments(*in_data, count, *roi_data, in_c, in_h, in_w, pooled_h,
                       pooled_w, spatial_scale, *out_data);
  kernel->Launch(*Kernel::queue_, {global}, Kernel::event_);
  Kernel::queue_->Finish();
}

template <typename T>
void Proposal(const T *anchor_data, const T *score_data, const T *delta_data,
              const T *info_data, const VecInt &in_shape, int num_anchors,
              int feat_stride, int min_size, T *proposal_data) {
  int in_h = in_shape[2], in_w = in_shape[3];
  int count = in_h * in_w * num_anchors;

  size_t global = count;
  auto *kernel = Kernel::cl_kernels_["Proposal"];
  kernel->SetArguments(count, *anchor_data, *score_data, *delta_data,
                       *info_data, in_h, in_w, num_anchors, feat_stride,
                       min_size, *proposal_data);
  kernel->Launch(*Kernel::queue_, {global}, Kernel::event_);
  Kernel::queue_->Finish();
}

template <typename T>
void Activate(T *data, int count, int type, float slope) {
  size_t global = count;
  auto *kernel = Kernel::cl_kernels_["Activate"];
  kernel->SetArguments(*data, count, type, slope);
  kernel->Launch(*Kernel::queue_, {global}, Kernel::event_);
  Kernel::queue_->Finish();
}

template <typename T>
void PRelu(T *data, const VecInt &in_shape, bool channel_shared,
           const T *slope_data) {
  int channels = in_shape[1], dim = 1;
  for (int i = 2; i < in_shape.size(); ++i) dim *= in_shape[i];
  int count = in_shape[0] * channels * dim;
  int div_factor = channel_shared ? channels : 1;

  size_t global = count;
  auto *kernel = Kernel::cl_kernels_["PRelu"];
  kernel->SetArguments(*data, count, channels, dim, div_factor, *slope_data);
  kernel->Launch(*Kernel::queue_, {global}, Kernel::event_);
  Kernel::queue_->Finish();
}

// Explicit instantiation
template void DataTransform(const BufferF *in_data, const VecInt &in_shape,
                            float scale, int num_mean,
                            const BufferF *mean_value, BufferF *out_data);
template void Im2Col(const BufferF *in_data, const VecInt &in_shape, int offset,
                     int kernel_size, int stride, int pad, int dilation,
                     int zero_point, const VecInt &out_shape,
                     BufferF *out_data);
template void Pooling(const BufferF *in_data, const VecInt &in_shape,
                      int kernel_size, int stride, int pad, int mode,
                      const VecInt &out_shape, BufferF *out_data);
template void Concat(const BufferF *in_data, int count, int num_concats,
                     int concat_size, int top_concat_axis,
                     int bottom_concat_axis, int offset_concat_axis,
                     BufferF *out_data);
template void Permute(const BufferF *in_data, int count, int num_axes,
                      const BufferI *permute_order, const BufferI *old_steps,
                      const BufferI *new_steps, BufferF *out_data);
template void Scale(const BufferF *in_data, int count,
                    const BufferF *scale_data, const BufferF *bias_data,
                    int scale_dim, int inner_dim, BufferF *out_data);
template void Bias(const BufferF *in_data, int count, const BufferF *bias_data,
                   int bias_dim, int inner_dim, BufferF *out_data);
template void Reorg(const BufferF *in_data, const VecInt &in_shape, int stride,
                    BufferF *out_data);
template void LRN(const BufferF *in_data, const VecInt &in_shape, int size,
                  float alpha, float beta, float k, BufferF *scale_data,
                  BufferF *out_data);
template void ROIPooling(const BufferF *in_data, const VecInt &in_shape,
                         const BufferF *roi_data, int num_rois, int pooled_h,
                         int pooled_w, float spatial_scale, BufferF *out_data);
template void Proposal(const BufferF *anchor_data, const BufferF *score_data,
                       const BufferF *delta_data, const BufferF *info_data,
                       const VecInt &in_shape, int num_anchors, int feat_stride,
                       int min_size, BufferF *proposal_data);
template void Activate(BufferF *data, int count, int type, float slope);
template void PRelu(BufferF *data, const VecInt &in_shape, bool channel_shared,
                    const BufferF *slope_data);
#endif

}  // namespace Vision

}  // namespace Shadow
