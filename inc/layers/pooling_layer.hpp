#ifndef SHADOW_POOLING_LAYER_HPP
#define SHADOW_POOLING_LAYER_HPP

#include "layer.hpp"

#include <string>

enum PoolType { kMax, kAve };

class PoolingLayer : public Layer {
public:
  explicit PoolingLayer(LayerType type);
  ~PoolingLayer();

  void MakePoolingLayer(SizeParams params, int ksize, int stride,
                        std::string pool_type);
  void ForwardLayer();

#ifdef USE_CUDA
  void CUDAForwardLayer();
#endif

#ifdef USE_CL
  void CLForwardLayer();
#endif

  void ReleaseLayer();

  int ksize_;
  int stride_;
  PoolType pool_type_;
};

#endif // SHADOW_POOLING_LAYER_HPP