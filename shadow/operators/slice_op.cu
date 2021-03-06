#include "slice_op.hpp"

namespace Shadow {

namespace Vision {

template <typename T>
__global__ void KernelSlice(const T *in_data, int count, int num_slices,
                            int slice_size, int bottom_slice_axis,
                            int top_slice_axis, int offset_slice_axis,
                            T *out_data) {
  CUDA_KERNEL_LOOP(globalid, count) {
    int total_slice_size = slice_size * top_slice_axis;
    int slice_num = globalid / total_slice_size;
    int slice_index = globalid % total_slice_size;
    int bottom_index =
        slice_index +
        (slice_num * bottom_slice_axis + offset_slice_axis) * slice_size;
    out_data[globalid] = in_data[bottom_index];
  }
}

template <typename T>
void Slice(const T *in_data, int count, int num_slices, int slice_size,
           int bottom_slice_axis, int top_slice_axis, int offset_slice_axis,
           T *out_data, Context *context) {
  KernelSlice<T><<<GetBlocks(count), NumThreads, 0,
                   cudaStream_t(context->cuda_stream())>>>(
      in_data, count, num_slices, slice_size, bottom_slice_axis, top_slice_axis,
      offset_slice_axis, out_data);
  CUDA_CHECK(cudaPeekAtLastError());
}

template void Slice(const float *, int, int, int, int, int, int, float *,
                    Context *);

}  // namespace Vision

}  // namespace Shadow
