// Copyright (c) 2019 Graphcore Ltd. All rights reserved.
#include "ReduceCodelets.hpp"

namespace popops {

template <typename ReduceOp, typename DataType> struct IsExternal {
private:
  template <typename R> constexpr static bool is() {
    return std::is_same<R, ReduceOp>{};
  }

public:
  constexpr bool operator()() const {
    // Current permutations of template parameters that have assembly.
    return (std::is_same<DataType, half>::value ||
            std::is_same<DataType, float>::value) &&
           (is<ReduceAdd>() || is<ReduceSquareAdd>() || is<ReduceMax>() ||
            is<ReduceMin>());
  }
};

template <typename OutType, bool isUpdate>
using ReduceOutput = typename std::conditional<
    isUpdate, poplar::InOut<poplar::Vector<OutType, PTR_ALIGN64, 8>>,
    poplar::Output<poplar::Vector<OutType, PTR_ALIGN64, 8>>>::type;

template <typename PartialsType>
using ReducePartials =
    poplar::Input<poplar::VectorList<PartialsType, DELTAN_TYPE, 8>>;

// common compute method for the reduce down the partial variants.
template <typename ReduceOp, typename OutType, typename PartialsType,
          bool isUpdate>
bool computePartialsEqualSizeReduction(ReduceOutput<OutType, isUpdate> &out,
                                       const ShortType outCount,
                                       const ShortType partialsSize,
                                       ReducePartials<PartialsType> &partials,
                                       const float k) {
  using AccType = AccType<PartialsType, ReduceOp>;
  // outCount is scaled down by however many partials we can fit in 128-bits.
  constexpr auto grainSize = std::is_same<PartialsType, half>::value ? 8 : 4;
  const auto outSize = outCount * grainSize;

  // we walk down the partials height-first, reducing to output.
  for (unsigned o = 0; o < outCount; ++o) {

    // Initialise our internal result
    AccType result[grainSize];
    for (unsigned i = 0; i < grainSize; ++i) {
      result[i] = ReduceOp::template init<AccType>();
    }
    // Along the partials
    for (unsigned p = 0; p < partialsSize; ++p) {
      // Reduce, down the height of the partials
      for (unsigned pg = 0; pg < partials.size(); ++pg) {
        const auto pidx = o * grainSize + p * outSize;
        for (unsigned i = 0; i < grainSize; ++i) {
          ReduceOp::update(result[i],
                           static_cast<AccType>(partials[pg][pidx + i]));
        }
      }
    }
    // scale accordingly.
    for (unsigned i = 0; i < grainSize; ++i) {
      result[i] *= static_cast<AccType>(k);
    }

    // update output.
    const auto oidx = o * grainSize;
    for (unsigned i = 0; i < grainSize; ++i) {
      if (isUpdate) {
        out[oidx + i] += static_cast<OutType>(result[i]);
      } else {
        out[oidx + i] = static_cast<OutType>(result[i]);
      }
    }
  }

  return true;
}

} // namespace popops
