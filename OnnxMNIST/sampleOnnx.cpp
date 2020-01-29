#include <vector>
#include <string>
#include <fstream>
#include <iostream>
#include <memory>
#include <cassert>

#include <cuda_runtime_api.h>
#include <NvInfer.h>
#include <NvOnnxParser.h>

#include "bufferManager.h"


using namespace std;

struct SampleParams
{
    int batchSize{1};                  //!< Number of inputs in a batch
    int dlaCore{-1};                   //!< Specify the DLA core to run network on.
    bool int8{false};                  //!< Allow runnning the network in Int8 mode.
    bool fp16{false};                  //!< Allow running the network in FP16 mode.
    std::vector<std::string> inputTensorNames;
    std::vector<std::string> outputTensorNames;
    std::string onnxFilePath;
    std::string inputFilePath;
};

class Logger: public nvinfer1::ILogger
{
    void log(nvinfer1::ILogger::Severity severity, const char* msg) override
    {
        // suppress info-level messages
        if (severity != nvinfer1::ILogger::Severity::kINFO)
            std::cout << msg << std:: endl;
    }
} gLogger;

struct InferDeleter
{
    template <typename T>
    void operator()(T* obj) const
    {
        if (obj)
        {
            obj->destroy();
        }
    }
};

void readPGMFile(const std::string& fileName, uint8_t* buffer, int inH, int inW)
{
    std::ifstream infile(fileName, std::ifstream::binary);
    assert(infile.is_open() && "Attempting to read from a file that is not open.");
    std::string magic, h, w, max;
    infile >> magic >> h >> w >> max;
    infile.seekg(1, infile.cur);
    infile.read(reinterpret_cast<char*>(buffer), inH * inW);
}

class SampleOnnxMNIST
{
    template <typename T>
    using SampleUniquePtr = std::unique_ptr<T, InferDeleter>;

public:
    SampleOnnxMNIST(const SampleParams& params)
        : mParams(params)
    {
    }

    void build();
    void infer();

private:
    SampleParams mParams;

    shared_ptr<nvinfer1::ICudaEngine> mEngine{nullptr}; //!< The TensorRT engine used to run the network
    unique_ptr<BufferManager> mBufManager{nullptr};
};

void SampleOnnxMNIST::build()
{
    // ----------------------------
    // Create builder and network
    // ----------------------------
    auto builder = SampleUniquePtr<nvinfer1::IBuilder>(nvinfer1::createInferBuilder(gLogger));
    const auto explicitBatch = 1U << static_cast<uint32_t>(nvinfer1::NetworkDefinitionCreationFlag::kEXPLICIT_BATCH);     
    auto network = SampleUniquePtr<nvinfer1::INetworkDefinition>(builder->createNetworkV2(explicitBatch));

    // -------------------
    // Create ONNX parser
    // -------------------
    auto parser = SampleUniquePtr<nvonnxparser::IParser>(nvonnxparser::createParser(*network, gLogger));
    parser->parseFromFile(mParams.onnxFilePath.c_str(), static_cast<int>(nvinfer1::ILogger::Severity::kWARNING));

    // -------------
    // Build engine
    // -------------
    auto config = SampleUniquePtr<nvinfer1::IBuilderConfig>(builder->createBuilderConfig());
	builder->setMaxBatchSize(mParams.batchSize);
    /*
    config->setMaxWorkspaceSize(1 << 30);
    if (mParams.fp16) { config->setFlag(nvinfer1::BuilderFlag::kFP16); }
    if (mParams.int8)
    {
        config->setFlag(nvinfer1::BuilderFlag::kINT8);
        samplesCommon::setAllTensorScales(network.get(), 127.0f, 127.0f);
    }
    samplesCommon::enableDLA(builder.get(), config.get(), mParams.dlaCore);
    */

    mEngine = shared_ptr<nvinfer1::ICudaEngine>(builder->buildEngineWithConfig(*network, *config), InferDeleter());

    mBufManager = make_unique<BufferManager>(mEngine);
}

void SampleOnnxMNIST::infer()
{
    // -------------------
    // Prepare Input Data
    // -------------------
    int inputIndex = mEngine->getBindingIndex(mParams.inputTensorNames[0].c_str());
    const int inputH = mEngine->getBindingDimensions(inputIndex).d[2];
    const int inputW = mEngine->getBindingDimensions(inputIndex).d[3];

    std::vector<uint8_t> fileData(inputH * inputW);
    readPGMFile(mParams.inputFilePath, fileData.data(), inputH, inputW);

    std::vector<float> hostInBuffer(inputH * inputW);
    for (int i = 0; i < inputH * inputW; ++i)
    {
        hostInBuffer[i] = 1.0 - float(fileData[i] / 255.0);
    }


    // ----------------------
    // Copy (Host -> Device)
    // ----------------------
    mBufManager->memcpy(mParams.inputTensorNames[0], true, hostInBuffer.data());

    // --------
    // Execute
    // --------
    vector<void*> buffers = mBufManager->getDeviceBindings();

    auto context = SampleUniquePtr<nvinfer1::IExecutionContext>(mEngine->createExecutionContext());
    // context->executeV2(buffers);
    context->execute(1, buffers.data());

    // ----------------------
    // Copy (Device -> Host)
    // ----------------------
    std::vector<float> hostOutBuffer(10);
    mBufManager->memcpy(mParams.outputTensorNames[0], false, hostOutBuffer.data());


    cout << "Result" << endl;
    for (const auto& elem: hostOutBuffer) { cout << elem << endl; }
}

int main()
{
    SampleParams params;
    params.onnxFilePath = "./data/mnist.onnx";
    params.inputFilePath = "./data/8.pgm";
    params.inputTensorNames.push_back("Input3");
    params.outputTensorNames.push_back("Plus214_Output_0");

    SampleOnnxMNIST sample(params);
    sample.build();
    sample.infer();
}