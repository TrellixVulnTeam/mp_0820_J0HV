// Copyright 2019 The MediaPipe Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <vector>

#include "mediapipe/calculators/image/recolor_calculator.pb.h"
#include "mediapipe/framework/calculator_framework.h"
#include "mediapipe/framework/formats/image_frame.h"
#include "mediapipe/framework/formats/image_frame_opencv.h"
#include "mediapipe/framework/port/opencv_core_inc.h"
#include "mediapipe/framework/port/opencv_imgproc_inc.h"
#include "mediapipe/framework/port/ret_check.h"
#include "mediapipe/framework/port/status.h"
#include "mediapipe/util/color.pb.h"

#if !defined(MEDIAPIPE_DISABLE_GPU)
#include "mediapipe/gpu/gl_calculator_helper.h"
#include "mediapipe/gpu/gl_simple_shaders.h"
#include "mediapipe/gpu/shader_util.h"
#endif  //  !MEDIAPIPE_DISABLE_GPU

namespace {
enum { ATTRIB_VERTEX, ATTRIB_TEXTURE_POSITION, NUM_ATTRIBUTES };
}  // namespace

namespace mediapipe {

// A calculator to recolor a masked area of an image to a specified color.
//
// A mask image is used to specify where to overlay a user defined color.
// The luminance of the input image is used to adjust the blending weight,
// to help preserve image textures.
//
// Inputs:
//   One of the following IMAGE tags:
//   IMAGE: An ImageFrame input image, RGB or RGBA.
//   IMAGE_GPU: A GpuBuffer input image, RGBA.
//   One of the following MASK tags:
//   MASK: An ImageFrame input mask, Gray, RGB or RGBA.
//   MASK_GPU: A GpuBuffer input mask, RGBA.
// Output:
//   One of the following IMAGE tags:
//   IMAGE: An ImageFrame output image.
//   IMAGE_GPU: A GpuBuffer output image.
//
// Options:
//   color_rgb (required): A map of RGB values [0-255].
//   mask_channel (optional): Which channel of mask image is used [RED or ALPHA]
//
// Usage example:
//  node {
//    calculator: "RecolorCalculator"
//    input_stream: "IMAGE_GPU:input_image"
//    input_stream: "MASK_GPU:input_mask"
//    output_stream: "IMAGE_GPU:output_image"
//    node_options: {
//      [mediapipe.RecolorCalculatorOptions] {
//        color { r: 0 g: 0 b: 255 }
//        mask_channel: RED
//      }
//    }
//  }
//
// Note: Cannot mix-match CPU & GPU inputs/outputs.
//       CPU-in & CPU-out <or> GPU-in & GPU-out
class RecolorCalculator : public CalculatorBase {
 public:
  RecolorCalculator() = default;
  ~RecolorCalculator() override = default;

  static ::mediapipe::Status GetContract(CalculatorContract* cc);

  ::mediapipe::Status Open(CalculatorContext* cc) override;
  ::mediapipe::Status Process(CalculatorContext* cc) override;
  ::mediapipe::Status Close(CalculatorContext* cc) override;

 private:
  ::mediapipe::Status LoadOptions(CalculatorContext* cc);
  ::mediapipe::Status InitGpu(CalculatorContext* cc);
  ::mediapipe::Status RenderGpu(CalculatorContext* cc);
  ::mediapipe::Status RenderCpu(CalculatorContext* cc);
  void GlRender();

  bool initialized_ = false;
  std::vector<float> color_;
  mediapipe::RecolorCalculatorOptions::MaskChannel mask_channel_;

  bool use_gpu_ = false;
#if !defined(MEDIAPIPE_DISABLE_GPU)
  mediapipe::GlCalculatorHelper gpu_helper_;
  GLuint program_ = 0;
#endif  //  !MEDIAPIPE_DISABLE_GPU
};
REGISTER_CALCULATOR(RecolorCalculator);

// static
::mediapipe::Status RecolorCalculator::GetContract(CalculatorContract* cc) {
  RET_CHECK(!cc->Inputs().GetTags().empty());
  RET_CHECK(!cc->Outputs().GetTags().empty());

  bool use_gpu = false;

#if !defined(MEDIAPIPE_DISABLE_GPU)
  if (cc->Inputs().HasTag("IMAGE_GPU")) {
    cc->Inputs().Tag("IMAGE_GPU").Set<mediapipe::GpuBuffer>();
    use_gpu |= true;
  }
#endif  //  !MEDIAPIPE_DISABLE_GPU
  if (cc->Inputs().HasTag("IMAGE")) {
    cc->Inputs().Tag("IMAGE").Set<ImageFrame>();
  }

#if !defined(MEDIAPIPE_DISABLE_GPU)
  if (cc->Inputs().HasTag("MASK_GPU")) {
    cc->Inputs().Tag("MASK_GPU").Set<mediapipe::GpuBuffer>();
    use_gpu |= true;
  }
#endif  //  !MEDIAPIPE_DISABLE_GPU
  if (cc->Inputs().HasTag("MASK")) {
    cc->Inputs().Tag("MASK").Set<ImageFrame>();
  }

#if !defined(MEDIAPIPE_DISABLE_GPU)
  if (cc->Outputs().HasTag("IMAGE_GPU")) {
    cc->Outputs().Tag("IMAGE_GPU").Set<mediapipe::GpuBuffer>();
    use_gpu |= true;
  }
#endif  //  !MEDIAPIPE_DISABLE_GPU
  if (cc->Outputs().HasTag("IMAGE")) {
    cc->Outputs().Tag("IMAGE").Set<ImageFrame>();
  }

  // Confirm only one of the input streams is present.
  RET_CHECK(cc->Inputs().HasTag("IMAGE") ^ cc->Inputs().HasTag("IMAGE_GPU"));
  // Confirm only one of the output streams is present.
  RET_CHECK(cc->Outputs().HasTag("IMAGE") ^ cc->Outputs().HasTag("IMAGE_GPU"));

  if (use_gpu) {
#if !defined(MEDIAPIPE_DISABLE_GPU)
    MP_RETURN_IF_ERROR(mediapipe::GlCalculatorHelper::UpdateContract(cc));
#endif  //  !MEDIAPIPE_DISABLE_GPU
  }

  return ::mediapipe::OkStatus();
}

::mediapipe::Status RecolorCalculator::Open(CalculatorContext* cc) {
  cc->SetOffset(TimestampDiff(0));

  if (cc->Inputs().HasTag("IMAGE_GPU")) {
    use_gpu_ = true;
#if !defined(MEDIAPIPE_DISABLE_GPU)
    MP_RETURN_IF_ERROR(gpu_helper_.Open(cc));
#endif  //  !MEDIAPIPE_DISABLE_GPU
  }

  MP_RETURN_IF_ERROR(LoadOptions(cc));

  return ::mediapipe::OkStatus();
}

::mediapipe::Status RecolorCalculator::Process(CalculatorContext* cc) {
  if (use_gpu_) {
#if !defined(MEDIAPIPE_DISABLE_GPU)
    MP_RETURN_IF_ERROR(
        gpu_helper_.RunInGlContext([this, &cc]() -> ::mediapipe::Status {
          if (!initialized_) {
            MP_RETURN_IF_ERROR(InitGpu(cc));
            initialized_ = true;
          }
          MP_RETURN_IF_ERROR(RenderGpu(cc));
          return ::mediapipe::OkStatus();
        }));
#endif  //  !MEDIAPIPE_DISABLE_GPU
  } else {
    MP_RETURN_IF_ERROR(RenderCpu(cc));
  }
  return ::mediapipe::OkStatus();
}

::mediapipe::Status RecolorCalculator::Close(CalculatorContext* cc) {
#if !defined(MEDIAPIPE_DISABLE_GPU)
  gpu_helper_.RunInGlContext([this] {
    if (program_) glDeleteProgram(program_);
    program_ = 0;
  });
#endif  //  !MEDIAPIPE_DISABLE_GPU

  return ::mediapipe::OkStatus();
}

::mediapipe::Status RecolorCalculator::RenderCpu(CalculatorContext* cc) {
  if (cc->Inputs().Tag("MASK").IsEmpty()) {
    return ::mediapipe::OkStatus();
  }
  // Get inputs and setup output.
  const auto& input_img = cc->Inputs().Tag("IMAGE").Get<ImageFrame>();
  const auto& mask_img = cc->Inputs().Tag("MASK").Get<ImageFrame>();

  cv::Mat input_mat = formats::MatView(&input_img);
  cv::Mat mask_mat = formats::MatView(&mask_img);

  RET_CHECK(input_mat.channels() == 3);  // RGB only.

  if (mask_mat.channels() > 1) {
    std::vector<cv::Mat> channels;
    cv::split(mask_mat, channels);
    if (mask_channel_ == mediapipe::RecolorCalculatorOptions_MaskChannel_ALPHA)
      mask_mat = channels[3];
    else
      mask_mat = channels[0];
  }
  cv::Mat mask_full;
  cv::resize(mask_mat, mask_full, input_mat.size());

  auto output_img = absl::make_unique<ImageFrame>(
      input_img.Format(), input_mat.cols, input_mat.rows);
  cv::Mat output_mat = mediapipe::formats::MatView(output_img.get());

  // From GPU shader:
  /*
      vec4 weight = texture2D(mask, sample_coordinate);
      vec4 color1 = texture2D(frame, sample_coordinate);
      vec4 color2 = vec4(recolor, 1.0);

      float luminance = dot(color1.rgb, vec3(0.299, 0.587, 0.114));
      float mix_value = weight.MASK_COMPONENT * luminance;

      fragColor = mix(color1, color2, mix_value);
  */
  for (int i = 0; i < output_mat.rows; ++i) {
    for (int j = 0; j < output_mat.cols; ++j) {
      float weight = mask_full.at<uchar>(i, j) * (1.0 / 255.0);
      cv::Vec3f color1 = input_mat.at<cv::Vec3b>(i, j);
      cv::Vec3f color2 = {color_[0], color_[1], color_[2]};

      float luminance =
          (color1[0] * 0.299 + color1[1] * 0.587 + color1[2] * 0.114) / 255;
      float mix_value = weight * luminance;

      cv::Vec3b mix_color = color1 * (1.0 - mix_value) + color2 * mix_value;
      output_mat.at<cv::Vec3b>(i, j) = mix_color;
    }
  }

  cc->Outputs().Tag("IMAGE").Add(output_img.release(), cc->InputTimestamp());

  return ::mediapipe::OkStatus();
}

::mediapipe::Status RecolorCalculator::RenderGpu(CalculatorContext* cc) {
  if (cc->Inputs().Tag("MASK_GPU").IsEmpty()) {
    return ::mediapipe::OkStatus();
  }
#if !defined(MEDIAPIPE_DISABLE_GPU)
  // Get inputs and setup output.
  const Packet& input_packet = cc->Inputs().Tag("IMAGE_GPU").Value();
  const Packet& mask_packet = cc->Inputs().Tag("MASK_GPU").Value();

  const auto& input_buffer = input_packet.Get<mediapipe::GpuBuffer>();
  const auto& mask_buffer = mask_packet.Get<mediapipe::GpuBuffer>();

  auto img_tex = gpu_helper_.CreateSourceTexture(input_buffer);
  auto mask_tex = gpu_helper_.CreateSourceTexture(mask_buffer);
  auto dst_tex =
      gpu_helper_.CreateDestinationTexture(img_tex.width(), img_tex.height());

  // Run recolor shader on GPU.
  {
    gpu_helper_.BindFramebuffer(dst_tex);  // GL_TEXTURE0

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(img_tex.target(), img_tex.name());
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(mask_tex.target(), mask_tex.name());

    GlRender();

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glFlush();
  }

  // Send result image in GPU packet.
  auto output = dst_tex.GetFrame<mediapipe::GpuBuffer>();
  cc->Outputs().Tag("IMAGE_GPU").Add(output.release(), cc->InputTimestamp());

  // Cleanup
  img_tex.Release();
  mask_tex.Release();
  dst_tex.Release();
#endif  //  !MEDIAPIPE_DISABLE_GPU

  return ::mediapipe::OkStatus();
}

void RecolorCalculator::GlRender() {
#if !defined(MEDIAPIPE_DISABLE_GPU)
  static const GLfloat square_vertices[] = {
      -1.0f, -1.0f,  // bottom left
      1.0f,  -1.0f,  // bottom right
      -1.0f, 1.0f,   // top left
      1.0f,  1.0f,   // top right
  };
  static const GLfloat texture_vertices[] = {
      0.0f, 0.0f,  // bottom left
      1.0f, 0.0f,  // bottom right
      0.0f, 1.0f,  // top left
      1.0f, 1.0f,  // top right
  };

  // program
  glUseProgram(program_);

  // vertex storage
  GLuint vbo[2];
  glGenBuffers(2, vbo);
  GLuint vao;
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);

  // vbo 0
  glBindBuffer(GL_ARRAY_BUFFER, vbo[0]);
  glBufferData(GL_ARRAY_BUFFER, 4 * 2 * sizeof(GLfloat), square_vertices,
               GL_STATIC_DRAW);
  glEnableVertexAttribArray(ATTRIB_VERTEX);
  glVertexAttribPointer(ATTRIB_VERTEX, 2, GL_FLOAT, 0, 0, nullptr);

  // vbo 1
  glBindBuffer(GL_ARRAY_BUFFER, vbo[1]);
  glBufferData(GL_ARRAY_BUFFER, 4 * 2 * sizeof(GLfloat), texture_vertices,
               GL_STATIC_DRAW);
  glEnableVertexAttribArray(ATTRIB_TEXTURE_POSITION);
  glVertexAttribPointer(ATTRIB_TEXTURE_POSITION, 2, GL_FLOAT, 0, 0, nullptr);

  // draw
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  // cleanup
  glDisableVertexAttribArray(ATTRIB_VERTEX);
  glDisableVertexAttribArray(ATTRIB_TEXTURE_POSITION);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);
  glDeleteVertexArrays(1, &vao);
  glDeleteBuffers(2, vbo);
#endif  //  !MEDIAPIPE_DISABLE_GPU
}

::mediapipe::Status RecolorCalculator::LoadOptions(CalculatorContext* cc) {
  const auto& options = cc->Options<mediapipe::RecolorCalculatorOptions>();

  mask_channel_ = options.mask_channel();

  if (!options.has_color()) RET_CHECK_FAIL() << "Missing color option.";

  color_.push_back(options.color().r());
  color_.push_back(options.color().g());
  color_.push_back(options.color().b());

  return ::mediapipe::OkStatus();
}

::mediapipe::Status RecolorCalculator::InitGpu(CalculatorContext* cc) {
#if !defined(MEDIAPIPE_DISABLE_GPU)
  const GLint attr_location[NUM_ATTRIBUTES] = {
      ATTRIB_VERTEX,
      ATTRIB_TEXTURE_POSITION,
  };
  const GLchar* attr_name[NUM_ATTRIBUTES] = {
      "position",
      "texture_coordinate",
  };

  std::string mask_component;
  switch (mask_channel_) {
    case mediapipe::RecolorCalculatorOptions_MaskChannel_UNKNOWN:
    case mediapipe::RecolorCalculatorOptions_MaskChannel_RED:
      mask_component = "r";
      break;
    case mediapipe::RecolorCalculatorOptions_MaskChannel_ALPHA:
      mask_component = "a";
      break;
  }

  // A shader to blend a color onto an image where the mask > 0.
  // The blending is based on the input image luminosity.
  const std::string frag_src = R"(
  #if __VERSION__ < 130
    #define in varying
  #endif  // __VERSION__ < 130

  #ifdef GL_ES
    #define fragColor gl_FragColor
    precision highp float;
  #else
    #define lowp
    #define mediump
    #define highp
    #define texture2D texture
    out vec4 fragColor;
  #endif  // defined(GL_ES)

    #define MASK_COMPONENT )" + mask_component +
                               R"(

    in vec2 sample_coordinate;
    uniform sampler2D frame;
    uniform sampler2D mask;
    uniform vec3 recolor;

    void main() {
      vec4 weight = texture2D(mask, sample_coordinate);
      vec4 color1 = texture2D(frame, sample_coordinate);
      vec4 color2 = vec4(recolor, 1.0);

      float luminance = dot(color1.rgb, vec3(0.299, 0.587, 0.114));
      float mix_value = weight.MASK_COMPONENT * luminance;

      fragColor = mix(color1, color2, mix_value);
    }
  )";

  // shader program and params
  mediapipe::GlhCreateProgram(mediapipe::kBasicVertexShader, frag_src.c_str(),
                              NUM_ATTRIBUTES, &attr_name[0], attr_location,
                              &program_);
  RET_CHECK(program_) << "Problem initializing the program.";
  glUseProgram(program_);
  glUniform1i(glGetUniformLocation(program_, "frame"), 1);
  glUniform1i(glGetUniformLocation(program_, "mask"), 2);
  glUniform3f(glGetUniformLocation(program_, "recolor"), color_[0] / 255.0,
              color_[1] / 255.0, color_[2] / 255.0);
#endif  //  !MEDIAPIPE_DISABLE_GPU

  return ::mediapipe::OkStatus();
}

}  // namespace mediapipe