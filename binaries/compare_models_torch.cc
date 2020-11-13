/**
 * Copyright (c) 2016-present, Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <string>
#include <vector>

#include "ATen/ATen.h"
#include "caffe2/core/timer.h"
#include "caffe2/utils/string_utils.h"
#include "torch/csrc/autograd/grad_mode.h"
#include "torch/csrc/jit/serialization/import.h"
#include "torch/script.h"

#include "c10/mobile/CPUCachingAllocator.h"

#include <chrono>
using namespace std::chrono;

C10_DEFINE_string(refmodel, "", "The reference torch script model to compare against.");
C10_DEFINE_string(model, "", "The torch script model to compare to the reference model.");
C10_DEFINE_string(
    input_dims,
    "",
    "Alternate to input_files, if all inputs are simple "
    "float TensorCPUs, specify the dimension using comma "
    "separated numbers. If multiple input needed, use "
    "semicolon to separate the dimension of different "
    "tensors.");
C10_DEFINE_string(input_type, "", "Input type (uint8_t/float)");
C10_DEFINE_string(
    input_memory_format,
    "contiguous_format",
    "Input memory format (contiguous_format/channels_last)");
C10_DEFINE_bool(
  no_inputs,
  false,
  "Whether the model has any input. Will ignore other input arugments if true");
C10_DEFINE_bool(
  use_caching_allocator,
  false,
  "Whether to cache allocations between inference iterations");
C10_DEFINE_int(
    use_bundled_input,
    -1,
    "If set, benchmark will expect the model to have bundled inputs "
    "and will run on the input with this index. ");
C10_DEFINE_bool(
  print_output,
  false,
  "Whether to print output with all one input tensor.");
C10_DEFINE_int(iter, 10, "The number of iterations to run.");
C10_DEFINE_bool(
  report_pep,
  false,
  "Whether to print performance stats for AI-PEP.");

C10_DEFINE_int(pytext_len, 0, "Length of input sequence.");
C10_DEFINE_string(backend, "cpu", "what backend to use for model (vulkan, cpu, metal) (default=cpu)");
C10_DEFINE_string(refbackend, "cpu", "what backend to use for model (vulkan, cpu, metal) (default=cpu)");

bool checkRtol(const at::Tensor& diff, const std::vector<at::Tensor>& inputs) {
  float maxValue = 0.0f;

  for (const auto& tensor : inputs) {
    maxValue = fmax(tensor.abs().max().item<float>(), maxValue);
  }

  return diff.abs().max().item<float>() < (2e-6 * maxValue);
}

bool almostEqual(const at::Tensor& a, const at::Tensor& b) {
  return checkRtol(a - b, {a, b});
}

std::vector<std::string>
split(char separator, const std::string& string, bool ignore_empty = true) {
  std::vector<std::string> pieces;
  std::stringstream ss(string);
  std::string item;
  while (getline(ss, item, separator)) {
    if (!ignore_empty || !item.empty()) {
      pieces.push_back(std::move(item));
    }
  }
  return pieces;
}

std::vector<c10::IValue> create_inputs(std::string backend) {
  if (FLAGS_no_inputs) {
    return {};
  }

  if (FLAGS_use_bundled_input >= 0) {
    // Need to get these after the model is loaded.
    return {};
  }

  CAFFE_ENFORCE_GE(FLAGS_input_dims.size(), 0, "Input dims must be specified.");
  CAFFE_ENFORCE_GE(FLAGS_input_type.size(), 0, "Input type must be specified.");

  std::vector<std::string> input_dims_list = split(';', FLAGS_input_dims);
  std::vector<std::string> input_type_list = split(';', FLAGS_input_type);
  std::vector<std::string> input_memory_format_list =
      split(';', FLAGS_input_memory_format);

  CAFFE_ENFORCE_EQ(
      input_dims_list.size(),
      input_type_list.size(),
      "Input dims and type should have the same number of items.");
  CAFFE_ENFORCE_EQ(
      input_dims_list.size(),
      input_memory_format_list.size(),
      "Input dims and format should have the same number of items.");

  std::vector<c10::IValue> inputs;
  for (size_t i = 0; i < input_dims_list.size(); ++i) {
    auto input_dims_str = split(',', input_dims_list[i]);
    std::vector<int64_t> input_dims;
    for (const auto& s : input_dims_str) {
      input_dims.push_back(c10::stoi(s));
    }

    at::ScalarType input_type;
    if (input_type_list[i] == "float") {
      input_type = at::ScalarType::Float;
    } else if (input_type_list[i] == "uint8_t") {
      input_type = at::ScalarType::Byte;
    } else if (input_type_list[i] == "int64") {
      input_type = at::ScalarType::Long;
    } else {
      CAFFE_THROW("Unsupported input type: ", input_type_list[i]);
    }

    at::MemoryFormat input_memory_format;
    if (input_memory_format_list[i] == "channels_last") {
      if (input_dims.size() != 4u) {
        CAFFE_THROW(
            "channels_last memory format only available on 4D tensors!");
      }
      input_memory_format = at::MemoryFormat::ChannelsLast;
    } else if (input_memory_format_list[i] == "contiguous_format") {
      input_memory_format = at::MemoryFormat::Contiguous;
    } else {
      CAFFE_THROW(
          "Unsupported input memory format: ", input_memory_format_list[i]);
    }

    const auto input_tensor = torch::rand(
        input_dims,
        at::TensorOptions(input_type).memory_format(input_memory_format));
    if (backend == "vulkan") {
      inputs.push_back(input_tensor.vulkan());
    } else {
      inputs.push_back(input_tensor);
    }
  }

  if (FLAGS_pytext_len > 0) {
    auto stensor = FLAGS_pytext_len * at::ones({1}, torch::kI64);
    inputs.push_back(stensor);
  }

  return inputs;
}

int main(int argc, char** argv) {
  c10::SetUsageMessage(
    "Run accuracy comparison to a reference model for a pytorch model.\n"
    "Example usage:\n"
    "./cpuref_compare"
    " --refmodel=<model_file>"
    " --model=<model_file>"
    " --use_bundled_input=0"
    " --iter=20");
  if (!c10::ParseCommandLineFlags(&argc, &argv)) {
    std::cerr << "Failed to parse command line flags!" << std::endl;
    return 1;
  }

  std::vector<c10::IValue> refinputs = create_inputs(FLAGS_refbackend);
  std::vector<c10::IValue> inputs = create_inputs(FLAGS_backend);

  torch::autograd::AutoGradMode guard(false);
  torch::jit::GraphOptimizerEnabledGuard no_optimizer_guard(false);
  auto module = torch::jit::load(FLAGS_model);
  auto refmodule = torch::jit::load(FLAGS_refmodel);

  if (FLAGS_use_bundled_input >= 0) {
    auto get_method = module.find_method("get_all_bundled_inputs");
    if (!get_method) {
      std::cerr << "Model does not have bundled inputs.  Before saving," << std::endl
        << "use torch.utils.bundled_inputs.augment_model_with_bundled_inputs." << std::endl;
      return 1;
    }
    auto ref_get_method = refmodule.find_method("get_all_bundled_inputs");
    if (!ref_get_method) {
      std::cerr << "Reference Model does not have bundled inputs.  Before saving," << std::endl
        << "use torch.utils.bundled_inputs.augment_model_with_bundled_inputs." << std::endl;
      return 1;
    }

    auto all_inputs = (*get_method)({}).toList();
    auto ref_all_inputs = (*ref_get_method)({}).toList();
    if (FLAGS_use_bundled_input >= all_inputs.size() || FLAGS_use_bundled_input >= ref_all_inputs.size()) {
      // NOTE: This check is only to make the error message nicer.
      // The get call below does internal bounds checking.
      std::cerr << "Model has only " << all_inputs.size() << " bundled inputs." << std::endl;
      std::cerr << "Reference Model has only " << ref_all_inputs.size() << " bundled inputs." << std::endl;
      return 1;
    }
    inputs = all_inputs.get(FLAGS_use_bundled_input).toTuple()->elements();
  }

  module.eval();
  refmodule.eval();

  c10::CPUCachingAllocator caching_allocator;
  c10::optional<c10::WithCPUCachingAllocatorGuard> caching_allocator_guard;
  if (FLAGS_use_caching_allocator) {
    caching_allocator_guard.emplace(&caching_allocator);
  }
  std::cout << "Running modules." << std::endl;

  const auto refoutput = refmodule.forward(refinputs).toTensor().cpu();
  const auto output = module.forward(inputs).toTensor().cpu();

	bool check = almostEqual(refoutput, output);
	if (check) {
    std::cout << "Passed!" << std::endl;
  }
  else {
    std::cout << "Failed!" << std::endl;
  }

  return 0;
}
