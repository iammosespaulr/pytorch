#include "fbgemm_pack_op.h"

#include "caffe2/core/tensor_int8.h"

#include "caffe2_dnnlowp_utils.h"

C10_DECLARE_int32(dnnlowp_nbits_in_non_outlier);

namespace caffe2 {

using namespace std;
using dnnlowp::TensorQuantizationParams;

// Helper functions

template <typename T>
void QuantizeWeight(
    const Blob& blob,
    int kernel_dim,
    int M,
    vector<TensorQuantizationParams>& qparams,
    vector<typename make_signed<T>::type>& W_quantized,
    dnnlowp::QuantizationFactory* qfactory) {
  using T_signed = typename make_signed<T>::type;

  const auto& filter = blob.IsType<int8::Int8TensorCPU>()
      ? blob.Get<int8::Int8TensorCPU>().t
      : blob.Get<TensorCPU>();

  W_quantized.resize(filter.numel());

  int signed_min = -(1 << (qfactory->GetWeightPrecision() - 1));
  if (blob.IsType<int8::Int8TensorCPU>()) {
    qparams[0].scale = blob.Get<int8::Int8TensorCPU>().scale;
    qparams[0].zero_point =
        blob.Get<int8::Int8TensorCPU>().zero_point + signed_min;

    const T* W_data = filter.data<T>();
    for (auto i = 0; i < filter.numel(); ++i) {
      W_quantized[i] = W_data[i] + signed_min;
    }
  } else {
    for (int g = 0; g < qparams.size(); ++g) {
      size_t offset = g * (M / qparams.size()) * kernel_dim;
      qparams[g] = qfactory->ChooseQuantizationParams(
          filter.data<float>() + offset,
          (M / qparams.size()) * kernel_dim,
          true /*weight*/);

      // qparams[g] is computed for unsigned type.
      // Adjust for the fact that weight will actually use signed.
      qparams[g].zero_point += signed_min;

      fbgemm::Quantize<T_signed>(
          filter.data<float>() + offset,
          W_quantized.data() + offset,
          (M / qparams.size()) * kernel_dim,
          qparams[g]);
    }
  }
}

template void QuantizeWeight<uint8_t>(
    const Blob& blob,
    int kernel_dim,
    int M,
    vector<TensorQuantizationParams>& qparams,
    vector<int8_t>& W_quantized,
    dnnlowp::QuantizationFactory* qfactory);

template void QuantizeWeight<uint16_t>(
    const Blob& blob,
    int kernel_dim,
    int M,
    vector<TensorQuantizationParams>& qparams,
    vector<int16_t>& W_quantized,
    dnnlowp::QuantizationFactory* qfactory);

// TODO reuse col_offsets_with_zero_pt_s8acc32_ref in fbgemm
// RefImplementations.cc . We can't do this now because W_quantized is
// not transposed here.
template <typename T>
void ComputeColumnOffsets(
    int num_rows,
    int num_cols,
    const T* W,
    const vector<TensorQuantizationParams>& qparams,
    vector<int32_t>& col_offsets) {
  col_offsets.resize(num_cols);
  int num_quant_groups = qparams.size();
  for (int g = 0; g < num_quant_groups; ++g) {
    int j_begin = g * (num_cols / num_quant_groups);
    int j_end = j_begin + (num_cols / num_quant_groups);
    for (int j = j_begin; j < j_end; ++j) {
      int32_t sum = 0;
      for (int k = 0; k < num_rows; ++k) {
        sum += W[j * num_rows + k];
      }
      col_offsets[j] = sum - qparams[g].zero_point * num_rows;
    }
  }
}

template void ComputeColumnOffsets<int8_t>(
    int num_rows,
    int num_cols,
    const int8_t* W,
    const vector<TensorQuantizationParams>& qparams,
    vector<int32_t>& col_offsets);

template void ComputeColumnOffsets<int16_t>(
    int num_rows,
    int num_cols,
    const int16_t* W,
    const vector<TensorQuantizationParams>& qparams,
    vector<int32_t>& col_offsets);

fbgemm::CompressedSparseColumn* ExtractOutlierMatrix(
    int groups,
    int kernel_dim,
    int M,
    int nbits_in_non_outlier,
    vector<int8_t>& W_quantized) {
  int outlier_cnt = 0;
  for (int group_id = 0; group_id < groups; ++group_id) {
    for (int i = 0; i < (M / groups) * kernel_dim; ++i) {
      int8_t w = W_quantized[group_id * (M / groups) * kernel_dim + i];
      bool is_outlier = nbits_in_non_outlier == 0 ||
          w < -(1 << (nbits_in_non_outlier - 1)) ||
          w >= (1 << (nbits_in_non_outlier - 1));
      if (is_outlier) {
        ++outlier_cnt;
      }
    }
  }

  fbgemm::CompressedSparseColumn* Wq_outlier =
      new fbgemm::CompressedSparseColumn(kernel_dim, M);
  Wq_outlier->RowIdx().resize(outlier_cnt);
  Wq_outlier->Values().resize(outlier_cnt);

  outlier_cnt = 0;
  for (int group_id = 0; group_id < groups; ++group_id) {
    for (int j = 0; j < M / groups; ++j) {
      Wq_outlier->ColPtr()[group_id * (M / groups) + j] = outlier_cnt;

      for (int k = 0; k < kernel_dim; ++k) {
        int8_t w = W_quantized[(group_id * (M / groups) + j) * kernel_dim + k];
        bool is_outlier = nbits_in_non_outlier == 0 ||
            w < -(1 << (nbits_in_non_outlier - 1)) ||
            w >= (1 << (nbits_in_non_outlier - 1));
        if (is_outlier) {
          CAFFE_ENFORCE_LE(k, numeric_limits<int16_t>::max());
          Wq_outlier->RowIdx()[outlier_cnt] = k;
          Wq_outlier->Values()[outlier_cnt] = w;
          ++outlier_cnt;

          W_quantized[(group_id * (M / groups) + j) * kernel_dim + k] = 0;
        }
      }
    }
  } // for each group
  Wq_outlier->ColPtr()[M] = outlier_cnt;

  return Wq_outlier;
}

// FIXME: code duplication with ConvDNNLowPOp::QuantizeBias_
static void QuantizeConvBias(
    const Blob& blob,
    int M,
    const TensorQuantizationParams& in_qparams,
    const vector<TensorQuantizationParams>& filter_qparams,
    vector<int32_t>& b_quantized) {
  const auto& bias = blob.IsType<int8::Int8TensorCPU>()
      ? blob.Get<int8::Int8TensorCPU>().t
      : blob.Get<TensorCPU>();
  if (blob.IsType<int8::Int8TensorCPU>()) {
    TensorQuantizationParams bias_qparams;
    bias_qparams.scale = blob.Get<int8::Int8TensorCPU>().scale;
    bias_qparams.zero_point = blob.Get<int8::Int8TensorCPU>().zero_point;
    CAFFE_ENFORCE_LE(
        std::abs(
            bias_qparams.scale - in_qparams.scale * filter_qparams[0].scale),
        1e-4);
    CAFFE_ENFORCE_EQ(bias_qparams.zero_point, 0);
    b_quantized.resize(bias.numel());
    b_quantized.assign(
        bias.data<int32_t>(), bias.data<int32_t>() + bias.numel());
  } else {
    const float* bdata = bias.data<float>();
    b_quantized.resize(bias.numel());
    for (int g = 0; g < filter_qparams.size(); ++g) {
      int i_begin = g * (M / filter_qparams.size());
      int i_end = i_begin + (M / filter_qparams.size());
      for (int i = i_begin; i < i_end; ++i) {
        b_quantized[i] = fbgemm::Quantize<int32_t>(
            bdata[i],
            0,
            in_qparams.scale * filter_qparams[g].scale,
            32,
            true /* signed */);
      }
    }
  }
}

// FullyConnectedDNNLowPPackWeightOp

FullyConnectedDNNLowPPackWeightOp::FullyConnectedDNNLowPPackWeightOp(
    const OperatorDef& operator_def,
    Workspace* ws)
    : DNNLowPOp<uint8_t, FCFp32Op>(operator_def, ws),
      axis_w_(this->GetSingleArgument<int32_t>("axis_w", 1)) {
  if (this->debug_def().engine() == "DNNLOWP_ACC16") {
    nbits_in_non_outlier_ = this->GetSingleArgument<int>(
        "nbits_in_non_outlier", FLAGS_dnnlowp_nbits_in_non_outlier);
  }
}

bool FullyConnectedDNNLowPPackWeightOp::RunOnDevice() {
  const auto& filter = InputTensorCPU_(0);
  const auto canonical_axis_w = filter.canonical_axis_index(axis_w_);
  const auto K = filter.size_from_dim(canonical_axis_w);
  const auto N = filter.size_to_dim(canonical_axis_w);

  auto* Y = this->Output<Int8FCDNNLowPPackedWeightBlob>(0);

  // Create tensor with the same shape but this new tensor shouldn't actually
  // allocate memory for the tensor.
  // This is just a convenient way to pass tensor shape information
  Y->original_tensor.ResizeLike(filter);

  Y->qparams.resize((this->debug_def().engine() == "DNNLOWP_ROWWISE") ? N : 1);

  vector<int8_t> W_quantized;
  QuantizeWeight<uint8_t>(
      InputBlob(0), K, N, Y->qparams, W_quantized, qfactory_.get());

  if (this->InputIsType<int8::Int8TensorCPU>(0) &&
      this->debug_def().engine() == "DNNLOWP_ROWWISE") {
    static int log_occurences = 0;
    if (log_occurences < 32) {
      ++log_occurences;
      LOG(WARNING) << "Cannot do row-wise quantization for "
                      "pre-quantized weight "
                   << this->debug_def().input(0);
    }
  }

  // Pre-compute column offsets
  // This should happen before ExtractOutlierMatrix because W_quantized is
  // changed in ExtractOutlierMatrix.
  Y->column_offsets.reset(new vector<int32_t>());
  ComputeColumnOffsets(
      K, N, W_quantized.data(), Y->qparams, *Y->column_offsets);

  if (this->debug_def().engine() == "DNNLOWP_ACC16") {
    if (nbits_in_non_outlier_ < 8) {
      Y->W_outlier.reset(
          ExtractOutlierMatrix(1, K, N, nbits_in_non_outlier_, W_quantized));
      int outlier_cnt = Y->W_outlier->ColPtr()[N];

      LOG(INFO) << "Proportion of outlier for Conv layer with weight blob "
                << this->debug_def().input(0) << " is "
                << static_cast<float>(outlier_cnt) / W_quantized.size();
      LOG(INFO) << "nbits_in_non_outlier " << nbits_in_non_outlier_;
    }

    Y->nbits_in_non_outlier = nbits_in_non_outlier_;
    Y->W_acc16.reset(new fbgemm::PackBMatrix<int8_t, int16_t>(
        fbgemm::matrix_op_t::Transpose,
        K,
        N,
        W_quantized.data(),
        K,
        nullptr, // pmat
        1)); // group
  } else {
    Y->W.reset(new fbgemm::PackBMatrix<int8_t>(
        fbgemm::matrix_op_t::Transpose,
        K,
        N,
        W_quantized.data(),
        K,
        nullptr, // pmat
        1)); // group
  }

  // Quantize bias
  if (InputSize() >= 2) {
    TensorQuantizationParams in_qparams;
    CAFFE_ENFORCE(HasSingleArgumentOfType<float>("in_scale"));
    in_qparams.scale = GetSingleArgument<float>("in_scale", 0);
    Y->bias.reset(new vector<int32_t>());
    QuantizeConvBias(InputBlob(1), N, in_qparams, Y->qparams, *Y->bias);
  } else {
    Y->bias = nullptr;
  }

  return true;
}

// ConvDNNLowPPackWeightOp

ConvDNNLowPPackWeightOp::ConvDNNLowPPackWeightOp(
    const OperatorDef& operator_def,
    Workspace* ws)
    : ConvPoolDNNLowPOpBase<uint8_t, ConvFp32Op>(operator_def, ws),
      quantize_groupwise_(
          this->GetSingleArgument<bool>("quantize_groupwise", false)) {
  if (this->debug_def().engine() == "DNNLOWP_ACC16") {
    nbits_in_non_outlier_ = this->GetSingleArgument<int>(
        "nbits_in_non_outlier", FLAGS_dnnlowp_nbits_in_non_outlier);
  }
}

bool ConvDNNLowPPackWeightOp::TakeDepthWise3x3FastPath_() {
  const auto& filter = this->InputTensorCPU_(FILTER);
  // The number of output channels
  int M = filter.dim32(0);
  // The number of input channels per group
  int C_per_group = filter.dim32(filter.dim() - 1);
  return this->debug_def().engine() != "DNNLOWP_ACC16" && group_ == M &&
      C_per_group == 1 && group_ % 8 == 0 && this->kernel_.size() == 2 &&
      kernel_h() == 3 && kernel_w() == 3 && stride_h() == stride_w() &&
      (stride_h() == 1 || stride_h() == 2) && dilation_h() == 1 &&
      dilation_w() == 1 && pad_t() == 1 && pad_b() == 1 && pad_l() == 1 &&
      pad_r() == 1 && GetCpuId().avx2() && !quantize_groupwise_;
}

bool ConvDNNLowPPackWeightOp::TakeDepthWise3x3x3FastPath_() {
  const auto& filter = this->InputTensorCPU_(FILTER);
  // The number of output channels
  int M = filter.dim32(0);
  // The number of input channels per group
  int C_per_group = filter.dim32(filter.dim() - 1);
  bool ret = this->debug_def().engine() != "DNNLOWP_ACC16" && group_ == M &&
      C_per_group == 1 && group_ % 8 == 0 && this->kernel_.size() == 3 &&
      this->kernel_[0] == 3 && this->kernel_[1] == 3 && this->kernel_[2] == 3 &&
      this->stride_[0] == this->stride_[1] &&
      this->stride_[0] == this->stride_[2] &&
      (this->stride_[0] == 1 || this->stride_[0] == 2) &&
      this->dilation_[0] == 1 && this->dilation_[1] == 1 &&
      this->dilation_[2] == 1 &&
      accumulate(
          this->pads_.begin(), this->pads_.end(), 1, multiplies<int>()) == 1 &&
      GetCpuId().avx2() && !quantize_groupwise_;
  return ret;
}

bool ConvDNNLowPPackWeightOp::RunOnDevice() {
  const auto& filter = InputTensorCPU_(FILTER);

  auto* Y = this->Output<Int8ConvDNNLowPPackedWeightBlob>(0);
  // Create tensor with the same shape but this new tensor shouldn't actually
  // allocate memory for the tensor.
  // This is just a convenient way to pass tensor shape information
  Y->original_tensor.ResizeLike(filter);

  // Assume KRSC layout
  // The number of output channels
  int M = filter.dim32(0);
  // The number of input channels per group
  int C_per_group = filter.dim32(filter.dim() - 1);

  int kernel_dims_size = 1;
  for (int i = 0; i < filter.dim() - 2; ++i) {
    kernel_dims_size *= filter.dim32(i + 1);
  }
  int kernel_dim = C_per_group * kernel_dims_size;

  vector<int8_t> W_quantized;
  Y->qparams.resize(quantize_groupwise_ ? group_ : 1);
  QuantizeWeight<uint8_t>(
      InputBlob(FILTER),
      kernel_dim,
      M,
      Y->qparams,
      W_quantized,
      qfactory_.get());

  if (this->InputIsType<int8::Int8TensorCPU>(FILTER) && quantize_groupwise_) {
    static int log_occurences = 0;
    if (log_occurences < 32) {
      ++log_occurences;
      LOG(WARNING) << "Cannot do group-wise quantization for "
                      "pre-quantized weight "
                   << this->debug_def().input(0);
    }
  }

  // Pre-compute column offsets
  // This should happen before ExtractOutlierMatrix because W_quantized is
  // changed in ExtractOutlierMatrix.
  Y->column_offsets.reset(new vector<int32_t>());
  ComputeColumnOffsets(
      kernel_dim, M, W_quantized.data(), Y->qparams, *Y->column_offsets);

  if (this->debug_def().engine() == "DNNLOWP_ACC16") {
    if (nbits_in_non_outlier_ < 8) {
      Y->W_outlier.reset(ExtractOutlierMatrix(
          group_, kernel_dim, M, nbits_in_non_outlier_, W_quantized));
      int outlier_cnt = Y->W_outlier->ColPtr()[M];

      LOG(INFO) << "Proportion of outlier for Conv layer with weight blob "
                << this->debug_def().input(0) << " is "
                << static_cast<float>(outlier_cnt) / W_quantized.size();
      LOG(INFO) << "nbits_in_non_outlier " << nbits_in_non_outlier_;
    }

    Y->nbits_in_non_outlier = nbits_in_non_outlier_;
    Y->W_acc16.reset(new fbgemm::PackBMatrix<int8_t, int16_t>(
        fbgemm::matrix_op_t::Transpose,
        group_ * kernel_dim,
        M / group_,
        W_quantized.data(),
        kernel_dim,
        nullptr, // pmat
        group_));
  } else if (TakeDepthWise3x3FastPath_()) {
    Y->W_depthwise_3x3.reset(
        new fbgemm::Packed3x3ConvMatrix(group_, W_quantized.data()));
  } else if (TakeDepthWise3x3x3FastPath_()) {
    Y->W_depthwise_3x3x3.reset(
        new fbgemm::Packed3x3x3ConvMatrix(group_, W_quantized.data()));
  } else {
    Y->W.reset(new fbgemm::PackBMatrix<int8_t>(
        fbgemm::matrix_op_t::Transpose,
        group_ * kernel_dim,
        M / group_,
        W_quantized.data(),
        kernel_dim,
        nullptr, // pmat
        group_));
  }

  if (InputSize() >= 2) {
    TensorQuantizationParams in_qparams;
    CAFFE_ENFORCE(HasSingleArgumentOfType<float>("in_scale"));
    in_qparams.scale = GetSingleArgument<float>("in_scale", 0);
    Y->bias.reset(new vector<int32_t>());
    QuantizeConvBias(InputBlob(BIAS), M, in_qparams, Y->qparams, *Y->bias);
  } else {
    Y->bias = nullptr;
  }

  return true;
}

// Explicitly register TypeMeta
CAFFE_KNOWN_TYPE(Int8FCDNNLowPPackedWeightBlob);
CAFFE_KNOWN_TYPE(Int8ConvDNNLowPPackedWeightBlob);

REGISTER_CPU_OPERATOR_WITH_ENGINE(
    Int8FCPackWeight,
    DNNLOWP,
    FullyConnectedDNNLowPPackWeightOp);

REGISTER_CPU_OPERATOR_WITH_ENGINE(
    Int8FCPackWeight,
    DNNLOWP_ACC16,
    FullyConnectedDNNLowPPackWeightOp);

REGISTER_CPU_OPERATOR_WITH_ENGINE(
    Int8FCPackWeight,
    DNNLOWP_ROWWISE,
    FullyConnectedDNNLowPPackWeightOp);

OPERATOR_SCHEMA(Int8FCPackWeight)
    .NumInputs(1, 2)
    .NumOutputs(1)
    .SetDoc(R"DOC(Prepack weight for Int8FC)DOC")
    .Input(0, "W", "Weight tensor in KRSC layout")
    .Input(1, "b", "Bias tensor")
    .Output(0, "W_q", "Weight/bias tensor in a packed format");

REGISTER_CPU_OPERATOR_WITH_ENGINE(
    Int8ConvPackWeight,
    DNNLOWP,
    ConvDNNLowPPackWeightOp);

REGISTER_CPU_OPERATOR_WITH_ENGINE(
    Int8ConvPackWeight,
    DNNLOWP_ACC16,
    ConvDNNLowPPackWeightOp);

OPERATOR_SCHEMA(Int8ConvPackWeight)
    .NumInputs(1, 2)
    .NumOutputs(1)
    .SetDoc(R"DOC(Prepack weight for Int8Conv)DOC")
    .Input(0, "W", "Weight tensor in KRSC layout")
    .Input(1, "b", "Bias tensor")
    .Output(0, "W_q", "Weight/bias tensor in a packed format");

} // namespace caffe2
