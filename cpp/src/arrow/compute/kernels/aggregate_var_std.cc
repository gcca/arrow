// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <cmath>

#include "arrow/compute/api_aggregate.h"
#include "arrow/compute/kernels/aggregate_internal.h"
#include "arrow/compute/kernels/aggregate_var_std_internal.h"
#include "arrow/compute/kernels/common.h"
#include "arrow/util/bit_run_reader.h"
#include "arrow/util/int128_internal.h"

namespace arrow {
namespace compute {
namespace internal {

namespace {

using arrow::internal::int128_t;
using arrow::internal::VisitSetBitRunsVoid;

template <typename ArrowType>
struct VarStdState {
  using ArrayType = typename TypeTraits<ArrowType>::ArrayType;
  using CType = typename ArrowType::c_type;
  using ThisType = VarStdState<ArrowType>;

  // float/double/int64: calculate `m2` (sum((X-mean)^2)) with `two pass algorithm`
  // https://en.wikipedia.org/wiki/Algorithms_for_calculating_variance#Two-pass_algorithm
  template <typename T = ArrowType>
  enable_if_t<is_floating_type<T>::value || (sizeof(CType) > 4)> Consume(
      const ArrayType& array) {
    int64_t count = array.length() - array.null_count();
    if (count == 0) {
      return;
    }

    using SumType =
        typename std::conditional<is_floating_type<T>::value, double, int128_t>::type;
    SumType sum =
        arrow::compute::detail::SumArray<CType, SumType, SimdLevel::NONE>(*array.data());

    const double mean = static_cast<double>(sum) / count;
    const double m2 = arrow::compute::detail::SumArray<CType, double, SimdLevel::NONE>(
        *array.data(), [mean](CType value) {
          const double v = static_cast<double>(value);
          return (v - mean) * (v - mean);
        });

    this->count = count;
    this->mean = mean;
    this->m2 = m2;
  }

  // int32/16/8: textbook one pass algorithm with integer arithmetic
  template <typename T = ArrowType>
  enable_if_t<is_integer_type<T>::value && (sizeof(CType) <= 4)> Consume(
      const ArrayType& array) {
    // max number of elements that sum will not overflow int64 (2Gi int32 elements)
    // for uint32:    0 <= sum < 2^63 (int64 >= 0)
    // for int32: -2^62 <= sum < 2^62
    constexpr int64_t max_length = 1ULL << (63 - sizeof(CType) * 8);

    int64_t start_index = 0;
    int64_t valid_count = array.length() - array.null_count();

    while (valid_count > 0) {
      // process in chunks that overflow will never happen
      const auto slice = array.Slice(start_index, max_length);
      const int64_t count = slice->length() - slice->null_count();
      start_index += max_length;
      valid_count -= count;

      if (count > 0) {
        IntegerVarStd<ArrowType> var_std;
        const ArrayData& data = *slice->data();
        const CType* values = data.GetValues<CType>(1);
        VisitSetBitRunsVoid(data.buffers[0], data.offset, data.length,
                            [&](int64_t pos, int64_t len) {
                              for (int64_t i = 0; i < len; ++i) {
                                const auto value = values[pos + i];
                                var_std.ConsumeOne(value);
                              }
                            });

        // merge variance
        ThisType state;
        state.count = var_std.count;
        state.mean = var_std.mean();
        state.m2 = var_std.m2();
        this->MergeFrom(state);
      }
    }
  }

  // Scalar: textbook algorithm
  void Consume(const Scalar& scalar, const int64_t count) {
    this->m2 = 0;
    if (scalar.is_valid) {
      this->count = count;
      this->mean = static_cast<double>(UnboxScalar<ArrowType>::Unbox(scalar));
    } else {
      this->count = 0;
      this->mean = 0;
    }
  }

  // Combine `m2` from two chunks (m2 = n*s2)
  // https://www.emathzone.com/tutorials/basic-statistics/combined-variance.html
  void MergeFrom(const ThisType& state) {
    if (state.count == 0) {
      return;
    }
    if (this->count == 0) {
      this->count = state.count;
      this->mean = state.mean;
      this->m2 = state.m2;
      return;
    }
    MergeVarStd(this->count, this->mean, state.count, state.mean, state.m2, &this->count,
                &this->mean, &this->m2);
  }

  int64_t count = 0;
  double mean = 0;
  double m2 = 0;  // m2 = count*s2 = sum((X-mean)^2)
};

template <typename ArrowType>
struct VarStdImpl : public ScalarAggregator {
  using ThisType = VarStdImpl<ArrowType>;
  using ArrayType = typename TypeTraits<ArrowType>::ArrayType;

  explicit VarStdImpl(const std::shared_ptr<DataType>& out_type,
                      const VarianceOptions& options, VarOrStd return_type)
      : out_type(out_type), options(options), return_type(return_type) {}

  Status Consume(KernelContext*, const ExecBatch& batch) override {
    if (batch[0].is_array()) {
      ArrayType array(batch[0].array());
      this->state.Consume(array);
    } else {
      this->state.Consume(*batch[0].scalar(), batch.length);
    }
    return Status::OK();
  }

  Status MergeFrom(KernelContext*, KernelState&& src) override {
    const auto& other = checked_cast<const ThisType&>(src);
    this->state.MergeFrom(other.state);
    return Status::OK();
  }

  Status Finalize(KernelContext*, Datum* out) override {
    if (this->state.count <= options.ddof) {
      out->value = std::make_shared<DoubleScalar>();
    } else {
      double var = this->state.m2 / (this->state.count - options.ddof);
      out->value =
          std::make_shared<DoubleScalar>(return_type == VarOrStd::Var ? var : sqrt(var));
    }
    return Status::OK();
  }

  std::shared_ptr<DataType> out_type;
  VarStdState<ArrowType> state;
  VarianceOptions options;
  VarOrStd return_type;
};

struct VarStdInitState {
  std::unique_ptr<KernelState> state;
  KernelContext* ctx;
  const DataType& in_type;
  const std::shared_ptr<DataType>& out_type;
  const VarianceOptions& options;
  VarOrStd return_type;

  VarStdInitState(KernelContext* ctx, const DataType& in_type,
                  const std::shared_ptr<DataType>& out_type,
                  const VarianceOptions& options, VarOrStd return_type)
      : ctx(ctx),
        in_type(in_type),
        out_type(out_type),
        options(options),
        return_type(return_type) {}

  Status Visit(const DataType&) {
    return Status::NotImplemented("No variance/stddev implemented");
  }

  Status Visit(const HalfFloatType&) {
    return Status::NotImplemented("No variance/stddev implemented");
  }

  template <typename Type>
  enable_if_t<is_number_type<Type>::value, Status> Visit(const Type&) {
    state.reset(new VarStdImpl<Type>(out_type, options, return_type));
    return Status::OK();
  }

  Result<std::unique_ptr<KernelState>> Create() {
    RETURN_NOT_OK(VisitTypeInline(in_type, this));
    return std::move(state);
  }
};

Result<std::unique_ptr<KernelState>> StddevInit(KernelContext* ctx,
                                                const KernelInitArgs& args) {
  VarStdInitState visitor(
      ctx, *args.inputs[0].type, args.kernel->signature->out_type().type(),
      static_cast<const VarianceOptions&>(*args.options), VarOrStd::Std);
  return visitor.Create();
}

Result<std::unique_ptr<KernelState>> VarianceInit(KernelContext* ctx,
                                                  const KernelInitArgs& args) {
  VarStdInitState visitor(
      ctx, *args.inputs[0].type, args.kernel->signature->out_type().type(),
      static_cast<const VarianceOptions&>(*args.options), VarOrStd::Var);
  return visitor.Create();
}

void AddVarStdKernels(KernelInit init,
                      const std::vector<std::shared_ptr<DataType>>& types,
                      ScalarAggregateFunction* func) {
  for (const auto& ty : types) {
    auto sig = KernelSignature::Make({InputType(ty)}, float64());
    AddAggKernel(std::move(sig), init, func);
  }
}

const FunctionDoc stddev_doc{
    "Calculate the standard deviation of a numeric array",
    ("The number of degrees of freedom can be controlled using VarianceOptions.\n"
     "By default (`ddof` = 0), the population standard deviation is calculated.\n"
     "Nulls are ignored.  If there are not enough non-null values in the array\n"
     "to satisfy `ddof`, null is returned."),
    {"array"},
    "VarianceOptions"};

const FunctionDoc variance_doc{
    "Calculate the variance of a numeric array",
    ("The number of degrees of freedom can be controlled using VarianceOptions.\n"
     "By default (`ddof` = 0), the population variance is calculated.\n"
     "Nulls are ignored.  If there are not enough non-null values in the array\n"
     "to satisfy `ddof`, null is returned."),
    {"array"},
    "VarianceOptions"};

std::shared_ptr<ScalarAggregateFunction> AddStddevAggKernels() {
  static auto default_std_options = VarianceOptions::Defaults();
  auto func = std::make_shared<ScalarAggregateFunction>(
      "stddev", Arity::Unary(), &stddev_doc, &default_std_options);
  AddVarStdKernels(StddevInit, NumericTypes(), func.get());
  return func;
}

std::shared_ptr<ScalarAggregateFunction> AddVarianceAggKernels() {
  static auto default_var_options = VarianceOptions::Defaults();
  auto func = std::make_shared<ScalarAggregateFunction>(
      "variance", Arity::Unary(), &variance_doc, &default_var_options);
  AddVarStdKernels(VarianceInit, NumericTypes(), func.get());
  return func;
}

}  // namespace

void RegisterScalarAggregateVariance(FunctionRegistry* registry) {
  DCHECK_OK(registry->AddFunction(AddVarianceAggKernels()));
  DCHECK_OK(registry->AddFunction(AddStddevAggKernels()));
}

}  // namespace internal
}  // namespace compute
}  // namespace arrow
