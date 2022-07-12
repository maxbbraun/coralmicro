// Copyright 2022 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <cstring>
#include <vector>

#include "libs/base/filesystem.h"
#include "libs/camera/camera.h"
#include "libs/rpc/rpc_http_server.h"
#include "libs/tensorflow/utils.h"
#include "libs/tpu/edgetpu_manager.h"
#include "libs/tpu/edgetpu_op.h"
#include "third_party/freertos_kernel/include/FreeRTOS.h"
#include "third_party/freertos_kernel/include/task.h"
#include "third_party/mjson/src/mjson.h"
#include "third_party/tflite-micro/tensorflow/lite/micro/micro_error_reporter.h"
#include "third_party/tflite-micro/tensorflow/lite/micro/micro_interpreter.h"
#include "third_party/tflite-micro/tensorflow/lite/micro/micro_mutable_op_resolver.h"

// Runs a local server with an endpoint called 'segment_from_camera',
// which will capture an image from the board's camera, run the image through a
// segmentation model and return the results in a JSON response.
//
// The response includes only the top result with a JSON file like this:
//
// {
// 'id': int,
// 'result':
//     {
//     'width': int,
//     'height': int,
//     'base64_data': image_bytes,
//     'output_mask': output_mask,
//      }
// }

// This can theoretically run any supported segmentation model but has only
// been tested with keras_post_training_unet_mv2_128_quant_edgetpu.tflite
// which comes from the tutorial at
// https://www.tensorflow.org/tutorials/images/segmentation. It is trained on
// the Oxford-IIIT Pet Dataset and will segment into three classes:

// Class 1: Pixel belonging to the pet.
// Class 2: Pixel bordering the pet.
// Class 3: None of the above/a surrounding pixel.

namespace coralmicro {
namespace {
constexpr char kModelPath[] =
    "/models/keras_post_training_unet_mv2_128_quant_edgetpu.tflite";
constexpr int kTensorArenaSize = 8 * 1024 * 1024;
STATIC_TENSOR_ARENA_IN_SDRAM(tensor_arena, kTensorArenaSize);

void SegmentFromCamera(struct jsonrpc_request* r) {
    auto* interpreter =
        reinterpret_cast<tflite::MicroInterpreter*>(r->ctx->response_cb_data);

    auto* input_tensor = interpreter->input_tensor(0);
    int model_height = input_tensor->dims->data[1];
    int model_width = input_tensor->dims->data[2];

    coralmicro::CameraTask::GetSingleton()->SetPower(true);
    coralmicro::CameraTask::GetSingleton()->Enable(
        coralmicro::camera::Mode::kStreaming);

    std::vector<uint8_t> image(
        model_width * model_height *
        coralmicro::CameraTask::FormatToBPP(coralmicro::camera::Format::kRgb));
    coralmicro::camera::FrameFormat fmt{
        coralmicro::camera::Format::kRgb,
        coralmicro::camera::FilterMethod::kBilinear,
        coralmicro::camera::Rotation::k0,
        model_width,
        model_height,
        false,
        image.data()};

    // Discard the first frame to ensure no power-on artifacts exist.
    bool ret = coralmicro::CameraTask::GetFrame({fmt});
    ret = coralmicro::CameraTask::GetFrame({fmt});

    coralmicro::CameraTask::GetSingleton()->Disable();
    coralmicro::CameraTask::GetSingleton()->SetPower(false);

    if (!ret) {
        jsonrpc_return_error(r, -1, "Failed to get image from camera.",
                             nullptr);
        return;
    }

    std::memcpy(tflite::GetTensorData<uint8_t>(input_tensor), image.data(),
                image.size());

    if (interpreter->Invoke() != kTfLiteOk) {
        jsonrpc_return_error(r, -1, "Invoke failed", nullptr);
        return;
    }

    const auto& output_tensor = interpreter->output_tensor(0);
    const auto& output_mask = tflite::GetTensorData<uint8_t>(output_tensor);
    const auto mask_size = coralmicro::tensorflow::TensorSize(output_tensor);

    jsonrpc_return_success(r, "{%Q: %d, %Q: %d, %Q: %V, %Q: %V}", "width",
                           model_width, "height", model_height, "base64_data",
                           image.size(), image.data(), "output_mask", mask_size,
                           output_mask);
}

void Main() {
    std::vector<uint8_t> model;
    if (!coralmicro::filesystem::ReadFile(kModelPath, &model)) {
        printf("ERROR: Failed to load %s\r\n", kModelPath);
        vTaskSuspend(nullptr);
    }

    auto tpu_context = coralmicro::EdgeTpuManager::GetSingleton()->OpenDevice();
    if (!tpu_context) {
        printf("ERROR: Failed to get EdgeTpu context\r\n");
        vTaskSuspend(nullptr);
    }

    tflite::MicroErrorReporter error_reporter;
    tflite::MicroMutableOpResolver<3> resolver;
    resolver.AddResizeBilinear();
    resolver.AddArgMax();
    resolver.AddCustom(coralmicro::kCustomOp, coralmicro::RegisterCustomOp());

    tflite::MicroInterpreter interpreter(tflite::GetModel(model.data()),
                                         resolver, tensor_arena,
                                         kTensorArenaSize, &error_reporter);
    if (interpreter.AllocateTensors() != kTfLiteOk) {
        printf("ERROR: AllocateTensors() failed\r\n");
        vTaskSuspend(nullptr);
    }

    if (interpreter.inputs().size() != 1) {
        printf("ERROR: Model must have only one input tensor\r\n");
        vTaskSuspend(nullptr);
    }

    printf("Initializing segmentation server...%p\r\n", &interpreter);
    jsonrpc_init(nullptr, &interpreter);
    jsonrpc_export("segment_from_camera", SegmentFromCamera);
    coralmicro::UseHttpServer(new coralmicro::JsonRpcHttpServer);
    printf("Segmentation server ready!\r\n");
    vTaskSuspend(nullptr);
}
}  // namespace
}  // namespace coralmicro

extern "C" void app_main(void* param) {
    (void)param;
    coralmicro::Main();
    vTaskSuspend(nullptr);
}