/* Copyright 2019 Google LLC

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
#include <memory>

#include "absl/types/span.h"
#include "tensorflow_graphics/rendering/opengl/macros.h"
#include "tensorflow_graphics/rendering/opengl/rasterizer_with_context.h"
#include "tensorflow_graphics/rendering/opengl/thread_safe_resource_pool.h"
#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/shape_inference.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/tensor_shape.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/status.h"

REGISTER_OP("Rasterize")
    .Attr("output_resolution: shape")
    .Attr("red_clear: float = 0.0")
    .Attr("green_clear: float = 0.0")
    .Attr("blue_clear: float = 0.0")
    .Attr("depth_clear: float = 1.0")
    .Attr("vertex_shader: string")
    .Attr("fragment_shader: string")
    .Attr("geometry_shader: string")
    .Attr("variable_names: list(string)")
    .Attr("variable_kinds: list({'mat', 'buffer'})")
    .Attr("T: list({float})")
    .Input("num_points: int32")
    .Input("variable_values: T")
    .Output("rendered_image: float")
    .SetShapeFn([](::tensorflow::shape_inference::InferenceContext* c) {
      // Error handling.
      std::vector<std::string> variable_names;
      TF_RETURN_IF_ERROR(c->GetAttr("variable_names", &variable_names));

      std::vector<tensorflow::shape_inference::ShapeHandle> variable_values;
      TF_RETURN_IF_ERROR(c->input("variable_values", &variable_values));

      if (variable_names.size() != variable_values.size())
        return tensorflow::errors::InvalidArgument(
            "The number of elements in variable_names, and variable_values.");

      // Defines the shape of the output tensor.
      c->set_output(0, c->UnknownShape());

      return tensorflow::Status::OK();
    });

class RasterizeOp : public tensorflow::OpKernel {
 public:
  explicit RasterizeOp(tensorflow::OpKernelConstruction* context)
      : OpKernel(context) {
    std::string fragment_shader;
    std::string geometry_shader;
    std::string vertex_shader;
    float red_clear = 0.0;
    float green_clear = 0.0;
    float blue_clear = 0.0;
    float depth_clear = 1.0;

    OP_REQUIRES_OK(context, context->GetAttr("red_clear", &red_clear));
    OP_REQUIRES_OK(context, context->GetAttr("green_clear", &green_clear));
    OP_REQUIRES_OK(context, context->GetAttr("blue_clear", &blue_clear));
    OP_REQUIRES_OK(context, context->GetAttr("depth_clear", &depth_clear));
    OP_REQUIRES_OK(context, context->GetAttr("vertex_shader", &vertex_shader));
    OP_REQUIRES_OK(context,
                   context->GetAttr("fragment_shader", &fragment_shader));
    OP_REQUIRES_OK(context,
                   context->GetAttr("geometry_shader", &geometry_shader));
    OP_REQUIRES_OK(context,
                   context->GetAttr("variable_names", &variable_names_));
    OP_REQUIRES_OK(context,
                   context->GetAttr("variable_kinds", &variable_kinds_));
    OP_REQUIRES_OK(context,
                   context->GetAttr("output_resolution", &output_resolution_));
    OP_REQUIRES(context, variable_names_.size() == variable_kinds_.size(),
                tensorflow::errors::InvalidArgument(
                    "The variable names and kinds must have the same size"));

    auto rasterizer_creator =
        [vertex_shader, geometry_shader, fragment_shader, red_clear,
         green_clear, blue_clear, depth_clear,
         this](std::unique_ptr<RasterizerWithContext>* resource)
        -> tensorflow::Status {
      return RasterizerWithContext::Create(
          output_resolution_.dim_size(1), output_resolution_.dim_size(0),
          vertex_shader, geometry_shader, fragment_shader, resource, red_clear,
          green_clear, blue_clear, depth_clear);
    };
    rasterizer_pool_ =
        std::unique_ptr<ThreadSafeResourcePool<RasterizerWithContext>>(
            new ThreadSafeResourcePool<RasterizerWithContext>(
                rasterizer_creator));
  }

  void Compute(tensorflow::OpKernelContext* context) override {
    tensorflow::TensorShape batch_shape;
    OP_REQUIRES_OK(context, ValidateVariables(context, &batch_shape));

    // Allocate the output images.
    tensorflow::Tensor* output_image;
    tensorflow::TensorShape output_image_shape;

    output_image_shape.AppendShape(batch_shape);
    output_image_shape.AppendShape(output_resolution_);
    output_image_shape.AddDim(4);
    OP_REQUIRES_OK(context, context->allocate_output(0, output_image_shape,
                                                     &output_image));

    // Render.
    std::unique_ptr<RasterizerWithContext> rasterizer;
    float* image_data = output_image->flat<float>().data();
    const int64 image_size =
        output_resolution_.dim_size(0) * output_resolution_.dim_size(1) * 4;

    OP_REQUIRES_OK(context, rasterizer_pool_->AcquireResource(&rasterizer));
    for (int i = 0; i < batch_shape.num_elements(); ++i) {
      OP_REQUIRES_OK(context, SetVariables(context, rasterizer, i));
      OP_REQUIRES_OK(context, RenderImage(context, rasterizer, image_size,
                                          image_data + i * image_size));
    }
    OP_REQUIRES_OK(context, rasterizer_pool_->ReturnResource(rasterizer));
  }

 private:
  tensorflow::Status SetVariables(
      tensorflow::OpKernelContext* context,
      std::unique_ptr<RasterizerWithContext>& rasterizer, int outer_dim);
  tensorflow::Status RenderImage(
      tensorflow::OpKernelContext* context,
      std::unique_ptr<RasterizerWithContext>& rasterizer, int64 image_size,
      float* image_data);
  tensorflow::Status ValidateVariables(tensorflow::OpKernelContext* context,
                                       tensorflow::TensorShape* batch_shape);

  std::unique_ptr<ThreadSafeResourcePool<RasterizerWithContext>>
      rasterizer_pool_;
  std::vector<std::string> variable_names_;
  std::vector<std::string> variable_kinds_;
  tensorflow::TensorShape output_resolution_;
};

tensorflow::Status RasterizeOp::RenderImage(
    tensorflow::OpKernelContext* context,
    std::unique_ptr<RasterizerWithContext>& rasterizer, const int64 image_size,
    float* image_data) {
  int num_points = context->input(0).scalar<int>()();

  TF_RETURN_IF_ERROR(rasterizer->Render(
      num_points, absl::MakeSpan(image_data, image_data + image_size)));
  return tensorflow::Status::OK();
}

tensorflow::Status RasterizeOp::SetVariables(
    tensorflow::OpKernelContext* context,
    std::unique_ptr<RasterizerWithContext>& rasterizer, int outer_dim) {
  tensorflow::OpInputList variable_values;
  TF_RETURN_IF_ERROR(context->input_list("variable_values", &variable_values));

  for (int index = 0; index < variable_names_.size(); ++index) {
    const std::string name = variable_names_[index];
    const std::string kind = variable_kinds_[index];
    const tensorflow::Tensor& value = variable_values[index];
    const tensorflow::TensorShape value_shape = value.shape();
    const tensorflow::DataType value_dtype = value.dtype();

    if (kind == "mat" && value_dtype == tensorflow::DT_FLOAT) {
      const int num_rows = value_shape.dim_size(value_shape.dims() - 2);
      const int num_cols = value_shape.dim_size(value_shape.dims() - 1);
      const int num_elements = num_rows * num_cols;
      const auto value_pointer = value.flat<float>().data();

      TF_RETURN_IF_ERROR(rasterizer->SetUniformMatrix(
          name, num_cols, num_rows, true,
          absl::MakeConstSpan(value_pointer + num_elements * outer_dim,
                              value_pointer + num_elements * (outer_dim + 1))));
    } else if (kind == "buffer" && value_dtype == tensorflow::DT_FLOAT) {
      const int32 buffer_length = value_shape.dim_size(value_shape.dims() - 1);

      const auto value_pointer = value.flat<float>().data();
      TF_RETURN_IF_ERROR(rasterizer->SetShaderStorageBuffer(
          name, absl::MakeConstSpan(
                    value_pointer + buffer_length * outer_dim,
                    value_pointer + buffer_length * (outer_dim + 1))));
    } else {
      return tensorflow::errors::InvalidArgument(
          "Don't know how to handle variable with name='", name,
          "', kind=", kind, " shape=", value_shape.DebugString(),
          " and type=", value_dtype);
    }
  }
  return tensorflow::Status::OK();
}

tensorflow::Status RasterizeOp::ValidateVariables(
    tensorflow::OpKernelContext* context,
    tensorflow::TensorShape* batch_shape) {
  tensorflow::OpInputList variable_values;
  TF_RETURN_IF_ERROR(context->input_list("variable_values", &variable_values));

  if (variable_names_.size() != variable_values.size() ||
      variable_names_.size() != variable_kinds_.size()) {
    return tensorflow::errors::InvalidArgument(
        "The variable names, kinds, and values must have the same size.");
  }

  bool batch_initialized = false;
  batch_shape->Clear();

  for (int index = 0; index < variable_kinds_.size(); ++index) {
    const std::string name = variable_names_[index];
    const std::string kind = variable_kinds_[index];
    const tensorflow::Tensor& value = variable_values[index];
    const tensorflow::DataType value_dtype = value.dtype();
    tensorflow::TensorShape value_batch_shape = value.shape();

    if (kind == "mat" && value_dtype == tensorflow::DT_FLOAT) {
      value_batch_shape.RemoveLastDims(2);
    } else if (kind == "buffer" && value_dtype == tensorflow::DT_FLOAT) {
      value_batch_shape.RemoveLastDims(1);
    } else {
      return tensorflow::errors::InvalidArgument(
          "Don't know how to handle variable with name='", name,
          "', kind=", kind, " and type=", value_dtype);
    }
    if (batch_initialized == false) {
      *batch_shape = value_batch_shape;
      batch_initialized = true;
    } else if (*batch_shape != value_batch_shape) {
      return tensorflow::errors::InvalidArgument(
          "Incompatible batch shape for variable with name='", name,
          "', batch shape=", value_batch_shape);
    }
  }
  return tensorflow::Status::OK();
}

// Register kernel with TF
REGISTER_KERNEL_BUILDER(Name("Rasterize").Device(tensorflow::DEVICE_CPU),
                        RasterizeOp);
