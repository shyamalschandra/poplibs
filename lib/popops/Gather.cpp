// Copyright (c) 2019 Graphcore Ltd. All rights reserved.
#include "popops/Gather.hpp"
#include "GatherInternal.hpp"

#include "poplibs_support/logging.hpp"
#include "popops/DynamicSlice.hpp"
#include "popops/ElementWise.hpp"
#include "popops/Pad.hpp"
#include "poputil/Broadcast.hpp"
#include "poputil/TileMapping.hpp"
#include "poputil/exceptions.hpp"

#include <algorithm>
#include <iterator>
#include <string>
#include <vector>

#include <boost/range/algorithm.hpp>
#include <boost/range/algorithm_ext.hpp>
#include <boost/range/numeric.hpp>

using namespace poplar;

namespace logging = poplibs_support::logging;

namespace {
// Transposes the given indices such that the indexVectorDim becomes the
// most-minor dimension.
Tensor transposeIndexVectorDimToLast(const Tensor &indices,
                                     unsigned indexVectorDim) {
  if (indices.rank() == indexVectorDim) {
    return indices;
  }

  if (indices.rank() == indexVectorDim + 1) {
    return indices;
  }

  std::vector<unsigned> permutation(indices.rank());

  const auto front = std::begin(permutation);
  const auto mid = std::next(std::begin(permutation), indexVectorDim);
  const auto back = std::end(permutation) - 1;

  std::iota(front, mid, 0);
  std::iota(mid, back, indexVectorDim + 1);
  *back = indexVectorDim;

  return indices.dimShuffle(permutation);
}

// The canonicalized indices is a 2D tensor where each row represents a single
// slice, and each column represents a coordinate into the input tensor.
Tensor canonicalizeGatherIndices(const Tensor &startIndices,
                                 unsigned indexVectorDim,
                                 const std::vector<unsigned> &startIndexMap) {
  // Transpose the non-index-vector dimensions to the front.
  Tensor startIndicesT =
      transposeIndexVectorDimToLast(startIndices, indexVectorDim);

  const bool indicesAreScalar = startIndicesT.rank() == indexVectorDim;

  // The number of dimensions in startIndices that are index dimensions.
  const std::size_t indexDimsInScatterIndices = indicesAreScalar ? 0 : 1;

  // If there is only one index (i.e. indicesAreScalar has rank 1 and this
  // scatter is really just a dynamic update slice) add a leading degenerate
  // dimension for uniformity.  Otherwise create a "collapsed" leading dimension
  // that subsumes all of the non-index-vector dimensions.
  std::vector<std::size_t> shape = startIndicesT.shape();
  if (shape.empty()) {
    startIndicesT = startIndicesT.reshape({1, 1});
  } else if (shape.size() == indexDimsInScatterIndices) {
    shape.insert(shape.begin(), 1);
    startIndicesT = startIndicesT.reshape(shape);
  } else if (indicesAreScalar) {
    startIndicesT = startIndicesT.reshape({startIndicesT.numElements(), 1});
  } else {
    // Collapse all but the dimensions (0 or 1) in startIndices containing
    // the index vectors.
    std::vector<std::size_t> newShape = {
        startIndicesT.numElements() / shape.back(), shape.back()};
    startIndicesT = startIndicesT.reshape(newShape);
  }

  // Reorganise the indices tensor to match the canonicalized input
  // This is kind of like a compile-time matmul with a permutation matrix
  std::vector<unsigned> permutation(startIndicesT.dim(1));
  boost::iota(permutation, 0);
  boost::sort(permutation, [&](unsigned a, unsigned b) {
    return startIndexMap[a] < startIndexMap[b];
  });

  std::vector<Tensor> permutedStartIndices(startIndicesT.dim(1));

  auto permuteIndices = [&](unsigned idx) {
    return startIndicesT.slice(idx, idx + 1, 1);
  };

  boost::transform(permutation, permutedStartIndices.begin(), permuteIndices);

  return concat(permutedStartIndices, 1);
}

// The canonicalized input tensor has its axis permuted such that all of its
// sliced axes are after the non-sliced axes.
Tensor canonicalizeGatherInput(const Tensor &input,
                               const std::vector<unsigned> &startIndexMap) {
  std::vector<unsigned> permutation(input.rank());
  boost::iota(permutation, 0);

  auto dimPred = [&](std::size_t dim) {
    return std::find(startIndexMap.begin(), startIndexMap.end(), dim) !=
           startIndexMap.end();
  };

  boost::stable_partition(permutation, dimPred);
  return input.dimShuffle(permutation);
}

// The canonicalized input tensor has its axis permuted such that all of its
// sliced axes after the non-sliced axes. This function transforms the
// collapsedSliceDims so that it still refers to the same dimensions in the
// canonicalized input tensor.
std::vector<std::size_t> canonicalizeCollapsedSliceDims(
    std::size_t rank, const std::vector<std::size_t> &collapsedSliceDims,
    const std::vector<unsigned> &startIndexMap) {
  std::vector<unsigned> permutation(rank);
  boost::iota(permutation, 0);

  auto dimPred = [&](std::size_t dim) {
    return std::find(startIndexMap.begin(), startIndexMap.end(), dim) !=
           startIndexMap.end();
  };

  boost::stable_partition(permutation, dimPred);

  std::vector<std::size_t> canonCollapsedSliceDims;

  std::vector<unsigned> inversePermutation(permutation.size());
  for (auto i = 0ul; i < permutation.size(); ++i) {
    inversePermutation[permutation[i]] = i;
  }
  for (auto &dim : collapsedSliceDims) {
    canonCollapsedSliceDims.emplace_back(inversePermutation[dim]);
  }

  return canonCollapsedSliceDims;
}

// The canonicalized input tensor has its axis permuted such that all of its
// sliced axes after the non-sliced axes. This function transforms the
// sliceSizes so that it still refers to the same dimensions in the
// canonicalized input tensor.
std::vector<std::size_t>
canonicalizeSliceSizes(const std::vector<std::size_t> &sliceSizes,
                       const std::vector<unsigned> &startIndexMap) {
  std::vector<unsigned> permutation(sliceSizes.size());
  boost::iota(permutation, 0);

  auto dimPred = [&](std::size_t dim) {
    return std::find(startIndexMap.begin(), startIndexMap.end(), dim) !=
           startIndexMap.end();
  };

  boost::stable_partition(permutation, dimPred);

  std::vector<std::size_t> newSliceSizes(sliceSizes.size());
  for (uint i = 0; i < sliceSizes.size(); ++i) {
    newSliceSizes[i] = sliceSizes[permutation[i]];
  }

  return newSliceSizes;
}

// This function "expands" the gather dimensions back to the indices shape.
// For example, if we have an input of shape [2, 3, 4], scalar indices of shape
// [1, 2, 3], and we are taking whole slices from dimension 0 of the input, the
// accumulator would be of shape [6, 3, 4]. We want to reshape this back to
// [1, 2, 3, 3, 4],
Tensor
adjustBatchDimsInAccumulator(const std::vector<std::size_t> &startIndicesShape,
                             const Tensor &accumulator,
                             std::size_t indexVectorDim) {
  std::vector<std::size_t> bounds;

  if (indexVectorDim < startIndicesShape.size()) {
    bounds.resize(startIndicesShape.size() - 1);
  } else {
    bounds.resize(startIndicesShape.size());
  }

  const auto indicesShape = [&startIndicesShape](std::size_t dim) {
    return startIndicesShape[dim];
  };

  const auto begin = std::begin(bounds);
  const auto mid = begin + indexVectorDim;
  const auto end = std::end(bounds);

  std::iota(begin, mid, 0);
  std::iota(mid, end, indexVectorDim + 1);

  std::transform(begin, end, begin, indicesShape);

  if (bounds.empty()) {
    return accumulator.squeeze({0});
  } else {
    const auto shape = accumulator.shape();
    bounds.insert(std::end(bounds), std::begin(shape) + 1, std::end(shape));

    return accumulator.reshape(bounds);
  }
}

// Undo the partitioning of the canonicalization on the accumulator. This
// shuffles the dimensions back to how the users expects for the output.
Tensor permuteBatchAndOffsetDims(const Tensor &accumulator,
                                 const std::vector<std::size_t> &offsetDims,
                                 std::size_t outputRank) {
  std::vector<unsigned> permutation;
  permutation.reserve(outputRank);

  std::size_t batch_idx_counter = 0;
  std::size_t offset_idx_counter = outputRank - offsetDims.size();

  for (std::size_t dim = 0; dim < outputRank; ++dim) {
    bool is_offset_dim = std::find(offsetDims.begin(), offsetDims.end(), dim) !=
                         offsetDims.end();
    if (is_offset_dim) {
      permutation.push_back(offset_idx_counter++);
    } else {
      permutation.push_back(batch_idx_counter++);
    }
  }

  return accumulator.dimShuffle(permutation);
}
} // namespace

namespace popops {

poplar::Tensor createGatherInput(poplar::Graph &graph, const poplar::Type &type,
                                 const std::vector<std::size_t> &inputShape,
                                 const std::vector<std::size_t> &sliceSizes,
                                 std::vector<unsigned> startIndexMap,
                                 const std::string &name) {
  std::vector<unsigned> permutation(inputShape.size());
  boost::iota(permutation, 0);

  auto dimPred = [&](std::size_t dim) {
    return std::find(startIndexMap.begin(), startIndexMap.end(), dim) !=
           startIndexMap.end();
  };

  boost::stable_partition(permutation, dimPred);
  std::vector<std::size_t> canonShape;
  for (auto i = 0ul; i < inputShape.size(); ++i) {
    canonShape.emplace_back(inputShape[permutation[i]]);
  }

  std::vector<std::size_t> canonSliceSizes;
  std::sort(startIndexMap.begin(), startIndexMap.end());
  for (unsigned i = 0; i < startIndexMap.size(); ++i) {
    canonSliceSizes.push_back(sliceSizes[startIndexMap[i]]);
  }

  auto input = internal::createGatherInputTensor(graph, type, canonShape,
                                                 canonSliceSizes, name);

  std::vector<unsigned> inversePermutation(inputShape.size());
  for (auto i = 0ul; i < inputShape.size(); ++i) {
    inversePermutation[permutation[i]] = i;
  }

  input = input.dimShuffle(inversePermutation);

  return input;
}

Tensor gather(Graph &graph, const Tensor &input, const Tensor &indices,
              std::size_t indexVectorDim,
              const std::vector<std::size_t> &offsetDims,
              const std::vector<std::size_t> &sliceSizes,
              const std::vector<std::size_t> &collapsedSliceDims,
              const std::vector<unsigned> &startIndexMap,
              program::Sequence &prog, const std::string &debugPrefix) {
  logging::info("gather input={}, indices={}, name={}", input.shape(),
                indices.shape(), debugPrefix);

  auto canonicalizedIndices =
      canonicalizeGatherIndices(indices, indexVectorDim, startIndexMap);
  auto canonicalizedInput = canonicalizeGatherInput(input, startIndexMap);

  auto canonCollapsedSliceDims = canonicalizeCollapsedSliceDims(
      input.rank(), collapsedSliceDims, startIndexMap);
  auto canonSliceSizes = canonicalizeSliceSizes(sliceSizes, startIndexMap);

  for (uint i = canonicalizedIndices.dim(1); i < canonSliceSizes.size(); ++i) {
    canonicalizedInput = canonicalizedInput.slice(0, canonSliceSizes[i], i);
  }

  canonSliceSizes.resize(canonicalizedIndices.dim(1));

  auto result =
      internal::gather(graph, canonicalizedInput, canonicalizedIndices,
                       canonSliceSizes, prog, debugPrefix);

  boost::transform(canonCollapsedSliceDims, canonCollapsedSliceDims.begin(),
                   [](std::size_t dim) { return dim + 1; });
  result = result.squeeze(canonCollapsedSliceDims);

  result =
      adjustBatchDimsInAccumulator(indices.shape(), result, indexVectorDim);

  const auto outputRank = (indices.rank() == indexVectorDim ? 0 : -1) +
                          offsetDims.size() + indices.rank();
  return permuteBatchAndOffsetDims(result, offsetDims, outputRank);
}

Tensor createGatherInput(Graph &graph, const Type &type,
                         const std::vector<std::size_t> &operandShape,
                         unsigned axis, GatherParams params,
                         const std::string &name) {
  if (operandShape[axis] > params.maxElementsPerTile) {
    std::vector<std::size_t> newOperandShape = operandShape;
    if (operandShape[axis] % 2 == 1) {
      newOperandShape[axis] += 1;

      return createGatherInput(graph, type, newOperandShape, axis, params, name)
          .slice(0, operandShape[axis], axis);
    } else {
      newOperandShape[axis] /= 2;
      newOperandShape.insert(newOperandShape.begin() + axis + 1, 2);

      return createGatherInput(graph, type, newOperandShape, axis, params, name)
          .reshape(operandShape);
    }
  } else {
    const std::vector<std::size_t> sliceSizes = {1};

    std::vector<unsigned> permutation(operandShape.size());
    boost::iota(permutation, 0);
    std::swap(permutation.front(), permutation[axis]);

    std::vector<std::size_t> canonShape = operandShape;
    for (unsigned i = 0; i < operandShape.size(); ++i) {
      canonShape[i] = operandShape[permutation[i]];
    }

    auto input = internal::createGatherInputTensor(graph, type, canonShape,
                                                   sliceSizes, name);

    return input.dimShuffle(permutation);
  }
}

Tensor gather(Graph &graph, const Tensor &input, const Tensor &indices,
              unsigned axis, program::Sequence &prog, GatherParams params,
              const std::string &debugPrefix) {
  if (input.dim(axis) > params.maxElementsPerTile) {
    if (input.dim(axis) % 2 == 1) {
      return gather(graph, pad(graph, input, 0, 1, axis), indices, axis, prog,
                    params, debugPrefix);
    }

    auto shape = input.shape();
    shape[axis] /= 2;
    shape.insert(shape.begin() + axis + 1, 2);

    auto one = graph.addConstant(UNSIGNED_INT, {}, 1, debugPrefix + "const_1");
    graph.setTileMapping(one, 0);

    auto indicesDiv = shiftRight(graph, indices, one, prog);
    auto indicesRem = bitwiseAnd(graph, indices, one, prog);
    auto indicesPred = eq(graph, indicesRem, one, prog);

    auto result = gather(graph, input.reshape(shape), indicesDiv, axis, prog,
                         params, debugPrefix + "/halved");

    // The odd and even slice pairs from the split gather
    auto even = result.slice(0, 1, axis + 1);
    auto odd = result.slice(1, 2, axis + 1);

    auto s = odd.shape();
    std::fill(s.begin(), s.end(), 1);
    s[axis] = indicesPred.numElements();
    indicesPred = indicesPred.reshape(s);

    poputil::broadcastToMatch(indicesPred, odd.shape());
    return select(graph, odd, even, indicesPred, prog).squeeze({axis + 1});
  }

  const std::vector<std::size_t> sliceSizes = {1};

  std::vector<unsigned> inputPermutation(input.rank());
  boost::iota(inputPermutation, 0);
  std::swap(inputPermutation.front(), inputPermutation[axis]);

  auto output = internal::gather(graph, input.dimShuffle(inputPermutation),
                                 indices.flatten().expand({1}), sliceSizes,
                                 prog, debugPrefix);
  output = output.squeeze({1});

  std::vector<unsigned> outputPermutation(output.rank());
  boost::iota(outputPermutation, 0);
  std::swap(outputPermutation.front(), outputPermutation[axis]);

  return output.dimShuffle(outputPermutation);
}

} // namespace popops
