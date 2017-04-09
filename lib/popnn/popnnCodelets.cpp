#include <poplar/Vertex.hpp>
#include <poplar/HalfFloat.hpp>
#include <iostream>
#include <iomanip>
#include <cassert>
#include <cmath>
#include <type_traits>
#include <vector>
#include "popnn/Loss.hpp"
#include "popnn/NonLinearity.hpp"
#include "PerformanceEstimation.hpp"

using namespace poplar;

/****************************************************************************/
/*            Auxiliary math functions                                      */
/****************************************************************************/

static float sigmoid(float x)
{
  return (1. / (1. + exp(-x)));
}

static float sigmoid_derivative(float activation)
{
  return activation * (1. - activation);
}

static float relu(float x)
{
  if (x > 0)
    return x;
  return 0;
}

static float relu_derivative(float activation)
{
  if (activation > 0)
    return 1;
  return 0;
}

static float tanh_derivative(float activation)
{
  return 1 - activation * activation;
}


static float nonlinearity(popnn::NonLinearityType t, float x) {
  switch (t) {
  case popnn::NonLinearityType::NON_LINEARITY_SIGMOID:
    return sigmoid(x);
  case popnn::NonLinearityType::NON_LINEARITY_RELU:
    return relu(x);
  case popnn::NonLinearityType::NON_LINEARITY_TANH:
    return tanh(x);
  }
}

static float nonlinearity_derivative(popnn::NonLinearityType t,
                                     float activation) {
  switch (t) {
  case popnn::NonLinearityType::NON_LINEARITY_SIGMOID:
    return sigmoid_derivative(activation);
  case popnn::NonLinearityType::NON_LINEARITY_RELU:
    return relu_derivative(activation);
  case popnn::NonLinearityType::NON_LINEARITY_TANH:
    return tanh_derivative(activation);
  }
}


/****************************************************************************/
/*            Vertices                                                      */
/****************************************************************************/

namespace popnn {

template <typename FPType>
class NonLinearity : public Vertex {
public:
  Vector<InOut<Vector<FPType>>> data;
  NonLinearityType nonLinearityType;

  SimOnlyField<unsigned> dataPathWidth;

  bool compute() {
    for (unsigned i = 0; i < data.size(); ++i) {
      for (unsigned j = 0; j < data[i].size(); ++j) {
        data[i][j] = nonlinearity(nonLinearityType, data[i][j]);
      }
    }
    return true;
  }

  uint64_t getCycleEstimate() const {
    bool isFloat = std::is_same<FPType, float>::value;
    std::vector<unsigned> regionSizes;
    for (const auto region : data)
      regionSizes.push_back(region.size());
    return getNonLinearityCycles(regionSizes, nonLinearityType, isFloat,
                                 dataPathWidth);
  }
};

template class NonLinearity<float>;
template class NonLinearity<half>;

template <typename FPType>
class NonLinearityGrad : public Vertex {
public:
  Vector<Input<Vector<FPType>>> outGrad;
  Vector<Input<Vector<FPType>>> out;
  Vector<Output<Vector<FPType>>> inGrad;
  NonLinearityType nonLinearityType;

  SimOnlyField<unsigned> dataPathWidth;

  bool compute() {
    for (unsigned i = 0; i < inGrad.size(); ++i) {
      assert(outGrad[i].size() == inGrad[i].size());
      assert(outGrad[i].size() == out[i].size());
      for (unsigned j = 0; j < outGrad[i].size(); ++j) {
        inGrad[i][j] =
            outGrad[i][j] * nonlinearity_derivative(nonLinearityType,
                                                    out[i][j]);
      }
    }
    return true;
  }


  uint64_t getCycleEstimate() const {
    bool isFloat = std::is_same<FPType, float>::value;
    uint64_t cycles = 5;
    for (unsigned i = 0; i < inGrad.size(); ++i) {
      unsigned vectorWidth = dataPathWidth / (isFloat ? 32 : 16);
      unsigned numVectors = (inGrad[i].size() + vectorWidth - 1) / vectorWidth;
      switch (nonLinearityType) {
      case NON_LINEARITY_SIGMOID:
        cycles += 5 + numVectors * 3;
        break;
      case NON_LINEARITY_RELU: {
        const unsigned vertexOverhead = 2    // run instruction
                                        + 7; // remaining vertex overhead
        cycles += vertexOverhead + numVectors * 3;
        }
        break;
      case NON_LINEARITY_TANH:
        cycles += 5 + numVectors * 3;
        break;
      default:
        throw std::runtime_error("Invalid nonlinearity type");
      }
    }
    return cycles;
  }
};

template class NonLinearityGrad<float>;
template class NonLinearityGrad<half>;

// AddTensors
// Sum the input tensors into the output
// \a out and \a in1 must have the same sizes. \a in2 may be smaller than the
// output, missing elements are treated as zero
template <class InType, class OutType>
class AddTensors : public Vertex {
public:
  Input<Vector<InType>> in0;
  Input<Vector<InType>> in1;
  Output<Vector<OutType>> out;

  SimOnlyField<unsigned> dataPathWidth;

  bool compute() {
    unsigned outSize = out.size();
    unsigned in1Size = in1.size();
    for (unsigned o = 0; o < in1Size; ++o) {
      out[o] = in0[o] + in1[o];
    }
    // elements not present in \a in2
    for (unsigned o = in1Size; o < outSize; ++o) {
        out[o] = in0[o];
    }
    return true;
  }

  uint64_t getCycleEstimate() const {
    assert(out.size() >= in1.size());
    assert(out.size() == in0.size());
    bool inIsFloat = std::is_same<InType, float>::value;
    bool outIsFloat = std::is_same<InType, float>::value;
    assert(!outIsFloat || inIsFloat && "Output is wider than input");
    const auto inVectorWidth = dataPathWidth / (inIsFloat ? 32 : 16);
    const auto outVectorWidth = dataPathWidth / (outIsFloat ? 32 : 16);
    unsigned numOut = out.size();
    unsigned numIn1 = in1.size();
    const bool separateME = true;
    unsigned numCycles;
    if (separateME) {
      // assume all accesses can occur in a single cycle. Loads are wider
      // so dominate
      // I expect pending IS changes to reduce the overhead from 20->14
      numCycles = 2 + 20 + (numOut + inVectorWidth - 1) / inVectorWidth;
    } else {
      // 2*loads + 1*store per operation, memory accesses will dominate,
      // assume common memory element
      unsigned numCycles = 15
        + (numOut + inVectorWidth - 1) / inVectorWidth   //1*load
        + (numOut + outVectorWidth - 1) / outVectorWidth //1*store
        + (numIn1 + inVectorWidth - 1) / inVectorWidth;  //1*load
      }

    return numCycles;
  }
};

template class AddTensors<float, float>;
template class AddTensors<float, half>;
template class AddTensors<half, half>;

// Acumulate
// Accumulate the input tensors with the output, striding through the output
// vector. Only min(inOut channels, in1 channels) are combined
template <class InType, class OutType>
class Accumulate : public Vertex {
public:
  InOut<Vector<OutType>> outIn0;
  Input<Vector<InType>> in1;

  SimOnlyField<unsigned> dataPathWidth;

  bool compute() {
    unsigned outSize = std::min(outIn0.size(), in1.size());
    for (unsigned o = 0; o < outSize; ++o) {
      outIn0[o] += in1[o];
    }
    return true;
  }


  uint64_t getCycleEstimate() const {
    bool inIsFloat = std::is_same<InType, float>::value;
    bool outIsFloat = std::is_same<InType, float>::value;
    assert(outIn0.size() >= in1.size());
    assert(!outIsFloat || inIsFloat && "Output is wider than input");
    const auto inVectorWidth = dataPathWidth / (inIsFloat ? 32 : 16);
    const auto outVectorWidth = dataPathWidth / (outIsFloat ? 32 : 16);
    unsigned numOut = std::min(outIn0.size(), in1.size());
    // 2*loads + 1*store per operation, memory accesses will dominate,
    unsigned numCycles;
    const bool separateME = true;
    if (separateME) {
      // ideal memory element placing
      const auto maxWidth = std::max(inVectorWidth, outVectorWidth);
      numCycles = 15
        + (numOut + maxWidth - 1) / maxWidth;
    } else {
      // common memory element
      numCycles = 15
        + (numOut + inVectorWidth - 1) / inVectorWidth   //1*load
        + (numOut + outVectorWidth - 1) / outVectorWidth //1*store
        + (numOut + inVectorWidth - 1) / inVectorWidth;  //1*load
    }

    return numCycles;
  }
};
template class Accumulate<float, float>;
template class Accumulate<float, half>;
template class Accumulate<half, half>;

template <typename FPType>
class MaxPooling : public Vertex {
public:
  Vector<Input<Vector<FPType>>> in;
  Vector<Output<Vector<FPType>>> out;
  Vector<unsigned> windowSizes;

  SimOnlyField<unsigned> dataPathWidth;

  bool compute() {
    unsigned inIndex = 0;
    for (unsigned i = 0; i < out.size(); ++i) {
      for (unsigned chan = 0; chan < out[i].size(); ++chan) {
        FPType val;
        for (unsigned w = 0; w < windowSizes[i]; ++w) {
          if (w == 0 || val < in[inIndex + w][chan])
            val = in[inIndex + w][chan];
        }
        out[i][chan] = val;
      }
      inIndex += windowSizes[i];
    }
    return true;
  }

  uint64_t getCycleEstimate() const {
    unsigned numCycles = 10;
    bool isFloat = std::is_same<FPType, float>::value;
    const auto vectorWidth = dataPathWidth / (isFloat ? 32 : 16);
    for (unsigned i = 0; i < out.size(); ++i) {
      auto numVectors = (out[i].size() + vectorWidth - 1) / vectorWidth;
      auto windowSize = windowSizes[i];
      // TODO: This is too optimistic
      numCycles += 1 + numVectors * (1 + windowSize);
    }
    return numCycles;
  }
};

template class MaxPooling<float>;
template class MaxPooling<half>;


template <typename FPType>
class MaxPoolingGrad : public Vertex {
public:
  Vector<Input<Vector<FPType>>> outGrad;
  Vector<Input<Vector<FPType>>> in;
  Vector<Input<Vector<FPType>>> out;
  Vector<Output<Vector<FPType>>> inGrad;
  Vector<unsigned> windowSizes;

  SimOnlyField<unsigned> dataPathWidth;

  bool compute() {
    unsigned inIndex = 0;
    for (unsigned i = 0; i < inGrad.size(); ++i) {
      for (unsigned chan = 0; chan < inGrad[i].size(); ++chan) {
        FPType val = 0;
        for (auto w = 0; w < windowSizes[i]; ++w) {
          if (in[i][chan] == out[inIndex + w][chan])
            val += outGrad[inIndex + w][chan];
        }
        inGrad[i][chan] = val;
      }
      inIndex += windowSizes[i];
    }
    return true;
  }

  uint64_t getCycleEstimate() const {
    unsigned numCycles = 10;
    bool isFloat = std::is_same<FPType, float>::value;
    const auto vectorWidth = dataPathWidth / (isFloat ? 32 : 16);
    // Expected implementation per group:
    // load group of actIn
    // for windowsize:
    // load actOut
    //  compare
    //  res<<=14 (covert to 0.5/0)
    //  mac
    // getacc
    // double
    // store
    for (unsigned i = 0; i < inGrad.size(); ++i) {
      auto numVectors = (inGrad[i].size() + vectorWidth - 1) / vectorWidth;
      auto windowSize = windowSizes[i];
      // TODO: This is too optimistic
      numCycles += 5 + numVectors * (5 + windowSize * 3);
    }
    return numCycles;
  }
};

template class MaxPoolingGrad<float>;
template class MaxPoolingGrad<half>;

template <typename FPType, typename LabelType>
class CalcLoss : public Vertex {
public:
  Vector<Input<Vector<FPType>>> batchIn;
  Input<Vector<LabelType>> label;

  Vector<Output<Vector<FPType>>> batchDeltaOut;
  Vector<Output<FPType>> loss;
  InOut<unsigned> numCorrect;

  Vector<FPType> probs;

  LossType lossType;

  bool compute() {
    const auto batchSize = batchIn.size();
    assert(batchIn.size() == batchDeltaOut.size());
    for (unsigned batchNum = 0; batchNum < batchSize; ++batchNum) {
      auto in = batchIn[batchNum];
      auto deltaOut = batchDeltaOut[batchNum];
      assert(in.size() == deltaOut.size());
      assert(probs.size() == in.size());
      switch (lossType) {
      case SUM_SQUARED_LOSS: {
        /* Calculate the sum-squared error and the partial derivative
           to pass back. */
        FPType sum = 0;
        for (LabelType i = 0;  i < in.size(); ++i) {
          FPType expected = (i == label[batchNum] ? 1 : 0);
          FPType actual = in[i];
          deltaOut[i] = (actual - expected);
          sum += 0.5 * (actual - expected) *  (actual - expected);
        }
        loss[batchNum] = sum;
        }
        break;
      case SOFTMAX_CROSS_ENTROPY_LOSS:
        /* Calculate the softmax probability distribution */
        for (LabelType i = 0;  i < in.size(); ++i) {
          FPType act = in[i];
          probs[i] = exp(act);
        }
        FPType sum = 0;
        for (FPType p : probs)
          sum += p;

        for (LabelType i = 0;  i < in.size(); ++i) {
          probs[i] /= sum;
        }

        /* Calculate the cross-entropy error and the partial derivative
         to pass back. */
        FPType error = 0;
        for (LabelType i = 0;  i < probs.size(); ++i) {
          FPType expected = (i == label[batchNum] ? 1 : 0);
          deltaOut[i] = (probs[i] - expected);
          error += -expected * log(probs[i]);
        }
        loss[batchNum] = error;
        break;
      }

      // Calculate the classification error for reporting test results
      // This assumes that the
      // non-linearity is monotonic, so the max output of the previous
      // layer is the max z-term of the previous layer.
      FPType max = in[0];
      LabelType maxIndex = 0;
      for (LabelType i = 0;  i < in.size(); ++i) {
        if (in[i] > max) {
          max = in[i];
          maxIndex = i;
        }
      }
      bool correct = (maxIndex == label[batchNum]);
      if (correct) {
        *numCorrect += 1;
      }
    }
    return true;
  }

  uint64_t getCycleEstimate() const {
    return 0;
  }
};


template class CalcLoss<float,unsigned int>;
template class CalcLoss<float,int>;
template class CalcLoss<half,unsigned int>;
template class CalcLoss<half,int>;

} // end namespace popnn