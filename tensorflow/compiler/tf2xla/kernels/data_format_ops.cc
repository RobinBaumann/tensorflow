/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

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

#include <array>
#include <string>

#include "absl/strings/str_cat.h"
#include "absl/types/span.h"
#include "tensorflow/compiler/tf2xla/xla_op_kernel.h"
#include "tensorflow/compiler/tf2xla/xla_op_registry.h"
#include "tensorflow/compiler/xla/client/lib/slicing.h"
#include "tensorflow/compiler/xla/client/xla_builder.h"
#include "tensorflow/compiler/xla/types.h"
#include "tensorflow/compiler/xla/xla_data.pb.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/tensor_shape.h"
#include "tensorflow/core/framework/types.pb.h"
#include "tensorflow/core/platform/errors.h"
#include "tensorflow/core/platform/macros.h"
#include "tensorflow/core/platform/types.h"
#include "tensorflow/core/util/tensor_format.h"

namespace tensorflow {
namespace {

class DataFormatDimMapOp : public XlaOpKernel {
 public:
  explicit DataFormatDimMapOp(OpKernelConstruction* context)
      : XlaOpKernel(context) {
    string src_format;
    OP_REQUIRES_OK(context, context->GetAttr("src_format", &src_format));
    string dst_format;
    OP_REQUIRES_OK(context, context->GetAttr("dst_format", &dst_format));
    OP_REQUIRES(context, src_format.size() == 4,
                errors::InvalidArgument(absl::StrCat(
                    "Source format must of length 4, received src_format = ",
                    src_format)));
    OP_REQUIRES(
        context, dst_format.size() == 4,
        errors::InvalidArgument(absl::StrCat(
            "Destination format must of length 4, received dst_format = ",
            dst_format)));
    for (int i = 0; i < src_format.size(); ++i) {
      for (int j = 0; j < dst_format.size(); ++j) {
        if (dst_format[j] == src_format[i]) {
          dst_idx_[i] = j;
          break;
        }
      }
      OP_REQUIRES(context, dst_idx_[i] != -1,
                  errors::InvalidArgument(absl::StrCat(
                      src_format, " is not a permutation of ", dst_format)));
    }
  }

  void Compile(XlaOpKernelContext* context) override {
    auto builder = context->builder();
    xla::XlaOp dst_indices =
        xla::ConstantR1(builder, absl::Span<const int32>(dst_idx_));
    xla::XlaOp four = xla::ConstantR0<int32>(builder, 4);
    xla::XlaOp src_indices =
        (xla::ConvertElementType(context->Input(0), xla::S32) + four) % four;
    xla::XlaOp output =
        xla::TorchIndexSelect(dst_indices, src_indices, /*dim=*/0);
    context->SetOutput(
        0, xla::ConvertElementType(output, context->input_xla_type(0)));
  }

 private:
  std::array<int32, 4> dst_idx_ = {{-1, -1, -1, -1}};

  TF_DISALLOW_COPY_AND_ASSIGN(DataFormatDimMapOp);
};

REGISTER_XLA_OP(
    Name("DataFormatDimMap").TypeConstraint("T", {DT_INT32, DT_INT64}),
    DataFormatDimMapOp);

class DataFormatVecPermuteOp : public XlaOpKernel {
 public:
  explicit DataFormatVecPermuteOp(OpKernelConstruction* ctx)
      : XlaOpKernel(ctx) {
    OP_REQUIRES_OK(ctx, ctx->GetAttr("src_format", &src_format_));
    OP_REQUIRES(
        ctx, src_format_.size() == 4,
        errors::InvalidArgument("Data format should have 4 characters"));
    TensorFormat data_format;
    OP_REQUIRES(ctx, FormatFromString(src_format_, &data_format),
                errors::InvalidArgument("Invalid data format"));
    OP_REQUIRES_OK(ctx, ctx->GetAttr("dst_format", &dst_format_));
    OP_REQUIRES(
        ctx, dst_format_.size() == 4,
        errors::InvalidArgument("Data format should have 4 characters"));
    OP_REQUIRES(ctx, FormatFromString(dst_format_, &data_format),
                errors::InvalidArgument("Invalid data format"));
  }
  void Compile(XlaOpKernelContext* ctx) override {
    auto builder = ctx->builder();
    const TensorShape input_tensor_shape = ctx->InputShape(0);
    int input_rank = input_tensor_shape.dims();
    OP_REQUIRES(ctx, input_rank == 1 || input_rank == 2,
                errors::InvalidArgument(
                    "Input must be a vector or matrix, but got shape ",
                    input_tensor_shape.DebugString()));
    OP_REQUIRES(
        ctx, input_tensor_shape.dim_size(0) == 4,
        errors::InvalidArgument(
            "First dimension of input must be of size 4, but got shape ",
            input_tensor_shape.DebugString()));
    if (input_rank == 2) {
      OP_REQUIRES(
          ctx, input_tensor_shape.dim_size(1) == 2,
          errors::InvalidArgument(
              "Second dimension of 2D input must be of size 2, but got shape ",
              input_tensor_shape.DebugString()));
    }
    int32 dst_indices[4];
    for (int i = 0; i < 4; ++i) {
      for (int j = 0; j < 4; ++j) {
        if (src_format_[i] == dst_format_[j]) {
          dst_indices[j] = i;
          break;
        }
      }
    }
    xla::XlaOp indices =
        xla::ConstantR1(builder, absl::Span<const int32>(dst_indices));
    xla::XlaOp output = xla::TorchIndexSelect(ctx->Input(0), indices, 0);
    ctx->SetOutput(0, output);
  }

 private:
  string src_format_;
  string dst_format_;

  TF_DISALLOW_COPY_AND_ASSIGN(DataFormatVecPermuteOp);
};

REGISTER_XLA_OP(
    Name("DataFormatVecPermute").TypeConstraint("T", {DT_INT32, DT_INT64}),
    DataFormatVecPermuteOp);
REGISTER_XLA_OP(Name("DataFormatVecPermute")
                    .Label("host")
                    .TypeConstraint("T", {DT_INT32, DT_INT64}),
                DataFormatVecPermuteOp);

}  // namespace
}  // namespace tensorflow
