/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/lite/c/builtin_op_data.h"
#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/kernels/internal/common.h"
#include "tensorflow/lite/kernels/internal/quantization_util.h"
#include "tensorflow/lite/kernels/internal/tensor_ctypes.h"
#include "tensorflow/lite/kernels/internal/types.h"
#include "tensorflow/lite/kernels/kernel_util.h"
#include "tensorflow/lite/kernels/op_macros.h"
#include "tensorflow/lite/micro/micro_utils.h"

namespace tflite {
namespace ops {
namespace micro {
namespace activations {

constexpr int kInputTensor = 0;
constexpr int kOutputTensor = 0;

template <typename Q>
inline void ReluQuantized(const TfLiteTensor* input, TfLiteTensor* output,
                          const Q* input_data, Q* output_data) {
  ReluParams params;
  float act_min = 0.0;
  float act_max = std::numeric_limits<float>::infinity();
  double real_multiplier =
      static_cast<double>(input->params.scale / output->params.scale);

  const RuntimeShape input_shape = GetTensorShape(input);
  const RuntimeShape output_shape = GetTensorShape(output);

  QuantizeMultiplier(real_multiplier, &params.output_multiplier,
                     &params.output_shift);

  params.quantized_activation_min =
      std::max(static_cast<int32_t>(std::numeric_limits<Q>::min()),
               output->params.zero_point +
                   static_cast<int32>(roundf(act_min / output->params.scale)));
  params.quantized_activation_max =
      act_max == std::numeric_limits<float>::infinity()
          ? static_cast<int32_t>(std::numeric_limits<Q>::max())
          : std::min(
                static_cast<int32_t>(std::numeric_limits<Q>::max()),
                output->params.zero_point +
                    static_cast<int32>(roundf(act_max / output->params.scale)));
  params.input_offset = input->params.zero_point;
  params.output_offset = output->params.zero_point;

  const int flat_size = MatchingFlatSize(input_shape, output_shape);
  for (int i = 0; i < flat_size; ++i) {
    const int32 val = static_cast<int32_t>(input_data[i]);
    int32 clamped = params.output_offset +
                    MultiplyByQuantizedMultiplier(val - params.input_offset,
                                                  params.output_multiplier,
                                                  params.output_shift);
    clamped = std::max(params.quantized_activation_min, clamped);
    clamped = std::min(params.quantized_activation_max, clamped);
    output_data[i] = static_cast<Q>(clamped);
  }
}

inline void ReluFloat(const RuntimeShape& input_shape, const float* input_data,
                      const RuntimeShape& output_shape, float* output_data) {
  const int flat_size = MatchingFlatSize(input_shape, output_shape);
  for (int i = 0; i < flat_size; ++i) {
    const float val = input_data[i];
    const float lower = 0.0f;
    const float clamped = val < lower ? lower : val;
    output_data[i] = clamped;
  }
}

inline void Relu6Float(const RuntimeShape& input_shape, const float* input_data,
                       const RuntimeShape& output_shape, float* output_data) {
  const int flat_size = MatchingFlatSize(input_shape, output_shape);
  for (int i = 0; i < flat_size; ++i) {
    const float val = input_data[i];
    const float upper = 6.0f;
    const float lower = 0.0f;
    const float clamped = val > upper ? upper : val < lower ? lower : val;
    output_data[i] = clamped;
  }
}

template <typename Q>
inline void Relu6Quantized(Q lower, Q upper, const RuntimeShape& input_shape,
                           const Q* input_data,
                           const RuntimeShape& output_shape, Q* output_data) {
  const int flat_size = MatchingFlatSize(input_shape, output_shape);
  for (int i = 0; i < flat_size; ++i) {
    const Q val = input_data[i];
    const Q clamped = val > upper ? upper : val < lower ? lower : val;
    output_data[i] = clamped;
  }
}

TfLiteStatus ReluPrepare(TfLiteContext* context, TfLiteNode* node) {
  return kTfLiteOk;
}

TfLiteStatus ReluEval(TfLiteContext* context, TfLiteNode* node) {
  const TfLiteTensor* input = GetInput(context, node, kInputTensor);
  TfLiteTensor* output = GetOutput(context, node, kOutputTensor);

  switch (input->type) {
    case kTfLiteFloat32: {
      ReluFloat(GetTensorShape(input), GetTensorData<float>(input),
                GetTensorShape(output), GetTensorData<float>(output));

      return kTfLiteOk;
    }
    case kTfLiteInt8: {
      ReluQuantized<int8_t>(input, output, GetTensorData<int8_t>(input),
                            GetTensorData<int8_t>(output));
      return kTfLiteOk;
    }
    case kTfLiteUInt8: {
      ReluQuantized<uint8_t>(input, output, GetTensorData<uint8_t>(input),
                             GetTensorData<uint8_t>(output));
      return kTfLiteOk;
    }
    default: {
      TF_LITE_KERNEL_LOG(context, "Only float32 is supported currently, got %s",
                         TfLiteTypeGetName(input->type));
      return kTfLiteError;
    }
  }
}

TfLiteStatus Relu6Prepare(TfLiteContext* context, TfLiteNode* node) {
  return kTfLiteOk;
}

TfLiteStatus Relu6Eval(TfLiteContext* context, TfLiteNode* node) {
  const TfLiteTensor* input = GetInput(context, node, kInputTensor);
  TfLiteTensor* output = GetOutput(context, node, kOutputTensor);

  switch (input->type) {
    case kTfLiteFloat32: {
      Relu6Float(GetTensorShape(input), GetTensorData<float>(input),
                 GetTensorShape(output), GetTensorData<float>(output));

      return kTfLiteOk;
    }
    case kTfLiteInt8: {
      const int8_t six = FloatToAsymmetricQuantizedInt8(
          6.0f, input->params.scale, input->params.zero_point);
      const int8_t zero = input->params.zero_point;
      Relu6Quantized<int8_t>(
          zero, six, GetTensorShape(input), GetTensorData<int8_t>(input),
          GetTensorShape(output), GetTensorData<int8_t>(output));
      return kTfLiteOk;
    }
    case kTfLiteUInt8: {
      const uint8_t six = FloatToAsymmetricQuantizedUInt8(
          6.0f, input->params.scale, input->params.zero_point);
      const uint8_t zero = input->params.zero_point;
      Relu6Quantized<uint8_t>(
          zero, six, GetTensorShape(input), GetTensorData<uint8_t>(input),
          GetTensorShape(output), GetTensorData<uint8_t>(output));
      return kTfLiteOk;
    }
    default: {
      TF_LITE_KERNEL_LOG(context, "Only float32 is supported currently, got %s",
                         TfLiteTypeGetName(input->type));
      return kTfLiteError;
    }
  }
}

}  // namespace activations

TfLiteRegistration Register_RELU() {
  return {/*init=*/nullptr,
          /*free=*/nullptr,
          /*prepare=*/activations::ReluPrepare,
          /*invoke=*/activations::ReluEval,
          /*profiling_string=*/nullptr,
          /*builtin_code=*/0,
          /*custom_name=*/nullptr,
          /*version=*/0};
}

TfLiteRegistration Register_RELU6() {
  return {/*init=*/nullptr,
          /*free=*/nullptr,
          /*prepare=*/activations::Relu6Prepare,
          /*invoke=*/activations::Relu6Eval,
          /*profiling_string=*/nullptr,
          /*builtin_code=*/0,
          /*custom_name=*/nullptr,
          /*version=*/0};
}

}  // namespace micro
}  // namespace ops
}  // namespace tflite
