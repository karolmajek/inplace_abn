#include <tuple>

#include <torch/extension.h>
#include <c10/util/Optional.h>

#include "inplace_abn.h"
#include "checks.h"
#include "utils.h"

/***********************************************************************************************************************
 * Exposed methods
 **********************************************************************************************************************/

std::tuple<at::Tensor, at::Tensor, at::Tensor> statistics(const at::Tensor& x) {
  AT_CHECK(x.ndimension() >= 2, "x should have at least 2 dimensions");

  if (x.is_cuda()) {
    return statistics_cuda(x);
  } else {
    return statistics_cpu(x);
  }
}

std::tuple<at::Tensor, at::Tensor, at::Tensor> reduce_statistics(
    const at::Tensor& all_mean, const at::Tensor& all_var, const at::Tensor& all_count) {
  // Inputs shouldn't be half
  CHECK_NOT_HALF(all_mean);
  CHECK_NOT_HALF(all_var);

  // reduce_statistics is only used on GPU
  CHECK_CUDA(all_mean);
  CHECK_CUDA(all_var);
  CHECK_CUDA(all_count);

  // Check types and dimensions
  CHECK_SAME_TYPE(all_mean, all_var);
  AT_CHECK(all_count.scalar_type() == at::ScalarType::Long, "all_count should have type int64");
  AT_CHECK(all_mean.ndimension() == 2, "all_mean should have size N x C");
  AT_CHECK(all_var.ndimension() == 2, "all_var should have size N x C");
  AT_CHECK(all_count.ndimension() == 2 && all_count.size(1) == 1, "all_count should have size N x 1");
  AT_CHECK(all_mean.size(0) == all_var.size(0) && all_mean.size(0) == all_count.size(0),
      "Inputs should have the same size in dimension 0");
  AT_CHECK(all_mean.size(1) == all_var.size(1), "all_mean and all_var should have the same size in dimension 1");

  return reduce_statistics_cuda(all_mean, all_var, all_count);
}

void forward(at::Tensor& x, const at::Tensor& mean, const at::Tensor& var,
             const c10::optional<at::Tensor>& weight, const c10::optional<at::Tensor>& bias,
             float eps, Activation activation, float activation_param) {
  // Check dimensions and types
  AT_CHECK(x.ndimension() >= 2, "x should have at least 2 dimensions");
  AT_CHECK(is_compatible_stat(x, mean), "mean is not compatible with x (wrong size or scalar type)");
  AT_CHECK(is_compatible_stat(x, var), "var is not compatible with x (wrong size or scalar type)");
  if (weight.has_value())
    AT_CHECK(is_compatible_weight(x, weight.value()), "weight is not compatible with x (wrong size or scalar type)");
  if (bias.has_value())
    AT_CHECK(is_compatible_weight(x, bias.value()), "bias is not compatible with x (wrong size or scalar type)");
  if (weight.has_value() && bias.has_value())
    CHECK_SAME_TYPE(weight.value(), bias.value());

  AT_CHECK((weight.has_value() && bias.has_value()) || (!weight.has_value() && !bias.has_value()),
      "weight and bias must be equally present or not present");

  if (x.is_cuda()) {
    forward_cuda(x, mean, var, weight, bias, eps, activation, activation_param);
  } else {
    forward_cpu(x, mean, var, weight, bias, eps, activation, activation_param);
  }
}

std::tuple<at::Tensor, at::Tensor> backward_reduce(
    at::Tensor& y_act, at::Tensor& dy_act, const c10::optional<at::Tensor>& weight,
    const c10::optional<at::Tensor>& bias, float eps, Activation activation, float activation_param) {
  // Check dimensions and types
  AT_CHECK(y_act.ndimension() >= 2, "y_act should have at least 2 dimensions");
  AT_CHECK(have_same_dims(y_act, dy_act), "y_act and dy_act should have the same size");
  CHECK_SAME_TYPE(y_act, dy_act);
  if (weight.has_value())
    AT_CHECK(is_compatible_weight(y_act, weight.value()),
        "weight is not compatible with y_act (wrong size or scalar type)");
  if (bias.has_value())
    AT_CHECK(is_compatible_weight(y_act, bias.value()),
        "bias is not compatible with y_act (wrong size or scalar type)");
  if (weight.has_value() && bias.has_value())
    CHECK_SAME_TYPE(weight.value(), bias.value());

  AT_CHECK((weight.has_value() && bias.has_value()) || (!weight.has_value() && !bias.has_value()),
      "weight and bias must be equally present or not present");

  if (y_act.is_cuda()) {
    return backward_reduce_cuda(y_act, dy_act, weight, bias, eps, activation, activation_param);
  } else {
    return backward_reduce_cpu(y_act, dy_act, weight, bias, eps, activation, activation_param);
  }
}

at::Tensor backward(const at::Tensor& xhat, const at::Tensor& dy, const at::Tensor& var, const at::Tensor& count,
                    const at::Tensor& sum_dy, const at::Tensor& sum_xhat_dy, const c10::optional<at::Tensor>& weight,
                    float eps) {
  // Check dimensions and types
  AT_CHECK(xhat.ndimension() >= 2, "xhat should have at least 2 dimensions");
  AT_CHECK(have_same_dims(xhat, dy), "xhat and dy should have the same size");
  CHECK_SAME_TYPE(xhat, dy);
  AT_CHECK(is_compatible_stat(xhat, var), "var is not compatible with xhat (wrong size or scalar type)");
  AT_CHECK(count.ndimension() == 1 && count.size(0) == 1, "count should be a vector with a single element");
  AT_CHECK(count.scalar_type() == at::ScalarType::Long, "count should have type int64");
  AT_CHECK(is_compatible_stat(xhat, sum_dy), "sum_dy is not compatible with xhat (wrong size or scalar type)");
  AT_CHECK(is_compatible_stat(xhat, sum_xhat_dy), "sum_xhat_dy is not compatible with xhat (wrong size or scalar type)");
  if (weight.has_value())
    AT_CHECK(is_compatible_weight(xhat, weight.value()),
        "weight is not compatible with xhat (wrong size or scalar type)");

  if (xhat.is_cuda()) {
    return backward_cuda(xhat, dy, var, count, sum_dy, sum_xhat_dy, weight, eps);
  } else {
    return backward_cpu(xhat, dy, var, count, sum_dy, sum_xhat_dy, weight, eps);
  }
}

at::Tensor backward_test(const at::Tensor& dy_, const at::Tensor& var, const c10::optional<at::Tensor>& weight,
                         float eps) {
  // Check dimensions and types
  AT_CHECK(dy_.ndimension() >= 2, "dy should have at least 2 dimensions");
  AT_CHECK(is_compatible_stat(dy_, var), "var is not compatible with dy (wrong size or scalar type)");
  if (weight.has_value())
    AT_CHECK(is_compatible_weight(dy_, weight.value()),
        "weight is not compatible with dy (wrong size or scalar type)");

  // TODO: optimize implementation for GPU
  auto dy = normalize_shape(dy_);
  auto mult = weight.has_value()
      ? (weight.value().to(var.options()).abs() + eps) / (var + eps).sqrt()
      : 1 / (var + eps).sqrt();
  auto dx = normalize_shape(mult) * dy.to(var.options());
  return dx.to(dy_.options()).view(dy_.sizes());
}

/***********************************************************************************************************************
 * Python Bindings
 **********************************************************************************************************************/

PYBIND11_MODULE(TORCH_EXTENSION_NAME, m) {
  pybind11::enum_<Activation>(m, "Activation")
    .value("LeakyReLU", Activation::LeakyReLU)
    .value("ELU", Activation::ELU)
    .value("Identity", Activation::Identity);

  // Forward methods
  m.def("statistics", &statistics, "Compute iABN statistics, i.e. mean, biased variance and sample count");
  m.def("reduce_statistics", &reduce_statistics, "Reduce statistics from multiple GPUs");
  m.def("forward", &forward, "iABN forward pass. This is an in-place operation w.r.t. x");

  // Backward methods
  m.def("backward_reduce", &backward_reduce,
      "First step of the backward pass. This is an in-place operation w.r.t. y_act and dy_act, which are transformed "
      "into xhat and dy, respectively.");
  m.def("backward", &backward, "Second step of the backward pass");
  m.def("backward_test", &backward_test, "Second step of the backward pass, test mode");
}
