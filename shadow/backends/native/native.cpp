#include "native.hpp"

#include "util/io.hpp"

namespace Shadow {

void Native::LoadModel(const shadow::NetParam &net_param) {
  net_param_ = net_param;
  Initial();
}

void Native::LoadModel(const void *proto_data, int proto_size) {
  LoadProtoData(proto_data, proto_size, &net_param_);
  Initial();
}

void Native::LoadModel(const std::string &proto_bin) {
  LoadProtoBin(proto_bin, &net_param_);
  Initial();
}

void Native::LoadModel(const std::string &proto_str,
                       const std::vector<const void *> &weights) {
  LoadProtoStrOrText(proto_str, &net_param_);
  Initial();
  CopyWeights(weights);
}

void Native::LoadModel(const std::string &proto_str, const void *weights_data) {
  LoadProtoStrOrText(proto_str, &net_param_);
  Initial();
  CopyWeights(weights_data);
}

void Native::Forward(const std::map<std::string, void *> &data_map,
                     const std::map<std::string, std::vector<int>> &shape_map) {
  if (ops_.empty()) return;

  for (const auto &in_map : data_map) {
    const auto &blob_name = in_map.first;
    const auto *blob_data = in_map.second;
    CHECK_NOTNULL(blob_data) << blob_name << " has null data";

    const auto &blob_shape = shape_map.count(blob_name)
                                 ? shape_map.at(blob_name)
                                 : std::vector<int>();

    const auto &blob_type = ws_->GetBlobType(blob_name);

    if (blob_type == int_id) {
      SetData(blob_name, blob_shape, static_cast<const int *>(blob_data));
    } else if (blob_type == float_id) {
      SetData(blob_name, blob_shape, static_cast<const float *>(blob_data));
    } else if (blob_type == uchar_id) {
      SetData(blob_name, blob_shape,
              static_cast<const unsigned char *>(blob_data));
    } else {
      LOG(FATAL) << "Blob " << blob_name << " has unsupported type "
                 << blob_type;
    }
  }

  for (auto &op : ops_) {
    op->Forward();
    DLOG(INFO) << op->debug_log();
  }

  DLOG(INFO) << "Forward Network!";
}

void Native::SaveEngine(const std::string &save_path,
                        std::vector<char> *save_data) {}

void Native::LoadProtoData(const void *proto_data, int proto_size,
                           shadow::NetParam *net_param) {
#if defined(USE_Protobuf)
  CHECK(IO::ReadProtoFromArray(proto_data, proto_size, net_param))
      << "Error when loading proto array data";

#else
  LOG(FATAL)
      << "Unsupported load proto array model, recompiled with USE_Protobuf";
#endif
}

void Native::LoadProtoBin(const std::string &proto_bin,
                          shadow::NetParam *net_param) {
#if defined(USE_Protobuf)
  CHECK(IO::ReadProtoFromBinaryFile(proto_bin, net_param))
      << "Error when loading proto binary file: " << proto_bin;

#else
  LOG(FATAL)
      << "Unsupported load proto binary model, recompiled with USE_Protobuf";
#endif
}

void Native::LoadProtoStrOrText(const std::string &proto_str_or_text,
                                shadow::NetParam *net_param) {
  bool success;
  Path path(proto_str_or_text);
  if (path.is_file()) {
    success = IO::ReadProtoFromTextFile(proto_str_or_text, net_param);
  } else {
    success = IO::ReadProtoFromText(proto_str_or_text, net_param);
  }
  CHECK(!proto_str_or_text.empty() && success)
      << "Error when loading proto: " << proto_str_or_text;
}

void Native::Initial() {
  for (const auto &blob : net_param_.blob()) {
    std::vector<int> shape;
    int cc = 1;
    for (auto dim : blob.shape()) {
      cc *= dim;
      shape.push_back(dim);
    }
    const auto &blob_name = blob.name();
    const auto blob_type = blob.has_type() ? blob.type() : std::string("float");
    if (blob_type == "int") {
      auto *blob_ptr = ws_->CreateBlob<int>(shape, blob_name, true);
      CHECK_NOTNULL(blob_ptr) << "Failed to create int blob " << blob_name;
      int data_i_size = blob.data_i_size();
      if (data_i_size > 0) {
        CHECK_EQ(data_i_size, cc)
            << "Blob int data size and blob shape are mismatch";
        blob_ptr->set_data(blob.data_i().data(), data_i_size);
      }
    } else if (blob_type == "float") {
      auto *blob_ptr = ws_->CreateBlob<float>(shape, blob_name, true);
      CHECK_NOTNULL(blob_ptr) << "Failed to create float blob " << blob_name;
      int data_f_size = blob.data_f_size();
      if (data_f_size > 0) {
        CHECK_EQ(data_f_size, cc)
            << "Blob float data size and blob shape are mismatch";
        blob_ptr->set_data(blob.data_f().data(), data_f_size);
      }
    } else if (blob_type == "unsigned char") {
      auto *blob_ptr = ws_->CreateBlob<unsigned char>(shape, blob_name, true);
      CHECK_NOTNULL(blob_ptr)
          << "Failed to create unsigned char blob " << blob_name;
      int data_b_size = 0;
      if (blob.data_b_size() > 0) {
        CHECK_EQ(blob.data_b_size(), 1);
        data_b_size = static_cast<int>(blob.data_b(0).size());
      }
      if (data_b_size > 0) {
        CHECK_EQ(data_b_size, cc)
            << "Blob unsigned char data size and blob shape are mismatch";
        auto uc_data_ptr = static_cast<unsigned char *>(
            static_cast<void *>(const_cast<char *>(blob.data_b(0).data())));
        blob_ptr->set_data(uc_data_ptr, data_b_size);
      }
    } else {
      LOG(FATAL) << "Failed to create blob " << blob_name << ", asked for type "
                 << blob_type;
    }
  }

  ops_.clear();
  for (const auto &op_param : net_param_.op()) {
    std::shared_ptr<Operator> op(CreateOperator(op_param, ws_));
    ops_.push_back(op);
  }

  arg_helper_ = ArgumentHelper(net_param_);

  in_blob_.clear();
  for (const auto &op_param : net_param_.op()) {
    if (op_param.type() == "Input") {
      for (const auto &blob_name : op_param.top()) {
        in_blob_.push_back(blob_name);
      }
      break;
    }
  }

  CHECK(arg_helper_.HasArgument("out_blob"))
      << "Network must have out_blob argument";
  out_blob_ = arg_helper_.GetRepeatedArgument<std::string>("out_blob");

  DLOG(INFO) << "Initial Network!";
}

void Native::CopyWeights(const std::vector<const void *> &weights) {
  int weights_count = 0;
  for (const auto &blob : net_param_.blob()) {
    CHECK_LT(weights_count, weights.size());
    const auto *weight = weights[weights_count++];
    const auto &blob_name = blob.name();
    const auto &blob_type = ws_->GetBlobType(blob_name);
    if (blob_type == int_id) {
      const auto *weight_data = static_cast<const int *>(weight);
      auto *weight_blob = ws_->GetBlob<int>(blob_name);
      weight_blob->set_data(weight_data, weight_blob->count());
    } else if (blob_type == float_id) {
      const auto *weight_data = static_cast<const float *>(weight);
      auto *weight_blob = ws_->GetBlob<float>(blob_name);
      weight_blob->set_data(weight_data, weight_blob->count());
    } else if (blob_type == uchar_id) {
      const auto *weight_data = static_cast<const unsigned char *>(weight);
      auto *weight_blob = ws_->GetBlob<unsigned char>(blob_name);
      weight_blob->set_data(weight_data, weight_blob->count());
    } else {
      LOG(FATAL) << "Unknown blob type " << blob_type;
    }
  }
}

void Native::CopyWeights(const void *weights_data) {
  for (const auto &blob : net_param_.blob()) {
    const auto &blob_name = blob.name();
    const auto &blob_type = ws_->GetBlobType(blob_name);
    if (blob_type == int_id) {
      auto *weight_blob = ws_->GetBlob<int>(blob_name);
      const auto *blob_data_ptr = static_cast<const int *>(weights_data);
      int blob_count = weight_blob->count();
      weight_blob->set_data(blob_data_ptr, blob_count);
      weights_data = blob_data_ptr + blob_count;
    } else if (blob_type == float_id) {
      auto *weight_blob = ws_->GetBlob<float>(blob_name);
      const auto *blob_data_ptr = static_cast<const float *>(weights_data);
      int blob_count = weight_blob->count();
      weight_blob->set_data(blob_data_ptr, blob_count);
      weights_data = blob_data_ptr + blob_count;
    } else if (blob_type == uchar_id) {
      auto *weight_blob = ws_->GetBlob<unsigned char>(blob_name);
      const auto *blob_data_ptr =
          static_cast<const unsigned char *>(weights_data);
      int blob_count = weight_blob->count();
      weight_blob->set_data(blob_data_ptr, blob_count);
      weights_data = blob_data_ptr + blob_count;
    } else {
      LOG(FATAL) << "Unknown blob type " << blob_type;
    }
  }
}

template <typename T>
void Native::SetData(const std::string &blob_name,
                     const std::vector<int> &blob_shape, const T *blob_data) {
  auto *blob = ws_->GetBlob<T>(blob_name);
  CHECK_NOTNULL(blob) << "Can not find blob " << blob_name;
  if (!blob_shape.empty()) {
    blob->reshape(blob_shape);
  }
  if (device_input_) {
#if defined(USE_CUDA)
    Allocator::CopyBuffer<T>(blob->count(), blob_data, blob->mutable_data());
#else
    LOG(FATAL) << "device input is only supported when USE_CUDA is ON";
#endif
  } else {
    blob->set_data(blob_data, blob->count());
  }
}

REGISTER_BACKEND(Native, Native);

}  // namespace Shadow
