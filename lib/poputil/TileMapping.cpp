// Copyright (c) 2017 Graphcore Ltd. All rights reserved.
#include "poputil/TileMapping.hpp"
#include "poplar/Program.hpp"
#include "poplibs_support/gcd.hpp"
#include "poputil/Util.hpp"
#include "poputil/exceptions.hpp"
#include <boost/functional/hash.hpp>
#include <boost/icl/interval_map.hpp>
#include <boost/integer/common_factor.hpp>
#include <set>

namespace poputil {

std::vector<std::vector<poplar::Interval>>
calcLinearTileMapping(const poplar::Graph &graph,
                      std::vector<std::size_t> shape,
                      unsigned minElementsPerTile, unsigned grainSize) {
  const auto numTiles = graph.getTarget().getNumTiles();
  const auto numElements = std::accumulate(shape.begin(), shape.end(), 1UL,
                                           std::multiplies<std::size_t>());
  std::vector<poplar::Interval> regions = {{0, numElements}};
  return splitRegions(regions, grainSize, numTiles, minElementsPerTile);
}

std::vector<std::vector<poplar::Interval>>
calcLinearTileMapping(const poplar::Graph &graph, const poplar::Tensor &t) {
  const auto dType = t.elementType();
  const auto &target = graph.getTarget();
  const auto typeSize = target.getTypeSize(dType);
  unsigned grainSize = target.getVectorWidth(dType);
  const auto minBytesPerTile = 128;
  const auto minElementsPerTile = (minBytesPerTile + typeSize - 1) / typeSize;
  return calcLinearTileMapping(graph, t.shape(), minElementsPerTile, grainSize);
}

void mapTensorLinearly(poplar::Graph &graph, const poplar::Tensor &t,
                       unsigned minElementsPerTile, unsigned grainSize) {
  graph.setTileMapping(t, calcLinearTileMapping(graph, t.shape(),
                                                minElementsPerTile, grainSize));
}

void mapTensorLinearly(poplar::Graph &graph, const poplar::Tensor &t) {
  graph.setTileMapping(t, calcLinearTileMapping(graph, t));
}

unsigned getTileImbalance(const poplar::Graph::TileToTensorMapping &mapping,
                          unsigned minElementsPerTile, unsigned grainSize) {
  unsigned maxElemsPerTile = 0;
  unsigned totalElems = 0;
  for (const auto &regions : mapping) {
    unsigned numElems = std::accumulate(
        regions.begin(), regions.end(), 0U,
        [](unsigned sum, const poplar::Interval &i) { return sum + i.size(); });
    maxElemsPerTile = std::max(numElems, maxElemsPerTile);
    totalElems += numElems;
  }
  unsigned numTiles = mapping.size();
  auto balancedElemsPerTile = (totalElems + numTiles - 1) / numTiles;
  balancedElemsPerTile = std::max(balancedElemsPerTile, minElementsPerTile);
  balancedElemsPerTile = std::max(balancedElemsPerTile, grainSize);
  if (maxElemsPerTile < balancedElemsPerTile)
    return 0;
  return maxElemsPerTile - balancedElemsPerTile;
}

unsigned getTileImbalance(const poplar::Graph &graph, const poplar::Tensor &t,
                          unsigned minElementsPerTile, unsigned grainSize) {
  return getTileImbalance(graph.getTileMapping(t), minElementsPerTile,
                          grainSize);
}

void mapOutputForElementWiseOp(poplar::Graph &graph,
                               const std::vector<poplar::Tensor> &inputs,
                               const poplar::Tensor &output, unsigned grainSize,
                               unsigned minGrainsPerTile) {
  std::vector<unsigned> tilesOccupied(inputs.size());
  std::vector<unsigned> numRegions(inputs.size());
  std::vector<size_t> minTileElements(inputs.size(),
                                      std::numeric_limits<size_t>::max());
  std::vector<size_t> maxTileElements(inputs.size());
  std::vector<size_t> maxCommonGrainSize(inputs.size(), grainSize);
  std::vector<bool> parallelWriteable(inputs.size());

  // Gather info on distribution of inputs.
  for (unsigned i = 0; i < inputs.size(); ++i) {
    if (!inputs[i].isParallelWriteable())
      continue;
    // This may be a broadcast op or similar where the number of
    // elements doesn't match, in which case we can't use this input
    // as a basis for a new mapping.
    if (inputs[i].numElements() != output.numElements())
      continue;
    parallelWriteable[i] = true;
    const auto mapping = graph.getTileMapping(inputs[i]);
    for (const auto &tile : mapping) {
      if (!tile.empty()) {
        tilesOccupied[i]++;
        numRegions[i] += tile.size();
        size_t tileElements = 0;
        for (const auto &interval : tile) {
          tileElements += interval.size();
          maxCommonGrainSize[i] =
              boost::integer::gcd(maxCommonGrainSize[i], interval.size());
        }
        minTileElements[i] = std::min(minTileElements[i], tileElements);
        maxTileElements[i] = std::max(maxTileElements[i], tileElements);
      }
    }
  }

  // If an input tensor has a suitable mapping then map the output to
  // match the most well distributed of these.
  int best = -1;
  for (unsigned i = 0; i < inputs.size(); ++i) {
    // If not parallel writeable, either this has constant elements with
    // indeterminate mapping, or some elements alias others, and likely
    // the resulting tile mapping will not be well distributed.
    if (!parallelWriteable[i])
      continue;

    // If the input does not have a suitable grain size then skip it.
    if (maxCommonGrainSize[i] != grainSize)
      continue;

    // If the input does not have a suitable number of grains per-tile
    // then skip it.
    if (minTileElements[i] / grainSize < minGrainsPerTile)
      continue;

    // Select the tensor with the minimum maximum tile elements
    if (best < 0 || maxTileElements[i] < maxTileElements[best]) {
      best = i;
    } else if (maxTileElements[i] == maxTileElements[best]) {
      // If both have the same maximum, select the tensor which is spread onto
      // the most tiles, or if two tensors share the same number of tiles, then
      // select the one which has the fewest overall regions
      if ((tilesOccupied[i] > tilesOccupied[best]) ||
          (tilesOccupied[i] == tilesOccupied[best] &&
           numRegions[i] < numRegions[best])) {
        best = i;
      }
    }
  }

  // Set output's mapping either based on a suitable input tensor's mapping,
  // or a linear mapping with the given restrictions on grain size and no.
  // of grains per-tile.
  if (best >= 0) {
    graph.setTileMapping(output, graph.getTileMapping(inputs[best]));
  } else {
    poputil::mapTensorLinearly(graph, output, minGrainsPerTile * grainSize,
                               grainSize);
  }
}

poplar::Tensor cloneToIpu(poplar::Graph &masterGraph, const poplar::Tensor &t,
                          unsigned dstIpu, poplar::StringRef name,
                          poplar::TensorCloneMethod method) {
  auto tLocal = masterGraph.clone(t, name, method);
  auto tSimple = t.flatten();
  auto tLocalSimple = tLocal.flatten();
  masterGraph.reorderToSimplify(&tSimple, {&tLocalSimple});
  auto mapping = masterGraph.getTileMapping(tSimple);
  const auto &target = masterGraph.getTarget();
  const auto tilesPerIPU = target.getTilesPerIPU();
  const auto numIPUs = target.getNumIPUs();
  assert(mapping.size() >= target.getNumTiles());
  for (unsigned ipu = 0; ipu != numIPUs; ++ipu) {
    if (ipu == dstIpu)
      continue;
    for (unsigned i = 0; i != tilesPerIPU; ++i) {
      auto &oldTileIntervals = mapping[ipu * tilesPerIPU + i];
      if (oldTileIntervals.empty())
        continue;
      auto &newTileIntervals = mapping[dstIpu * tilesPerIPU + i];
      if (newTileIntervals.empty()) {
        newTileIntervals = std::move(oldTileIntervals);
      } else {
        newTileIntervals.insert(newTileIntervals.end(),
                                oldTileIntervals.begin(),
                                oldTileIntervals.end());
      }
      oldTileIntervals.clear();
    }
  }
  masterGraph.setTileMapping(tLocalSimple, mapping);
  return tLocal;
}

poplar::Tensor createIpuCopy(poplar::Graph &masterGraph,
                             const poplar::Tensor &t, unsigned dstIpu,
                             poplar::Tensor &copySrc, poplar::Tensor &copyDst,
                             poplar::StringRef name,
                             poplar::TensorCloneMethod method) {
  auto tLocal = poputil::cloneToIpu(masterGraph, t, dstIpu, name, method);
  // Create source and destination tensor for the copy. These are different
  // from the source and cloned tensor only if the order and aliases are
  // preserved in the cloned tensor
  copyDst = tLocal;
  copySrc = t;
  if (method == poplar::TensorCloneMethod::PRESERVE_ORDER_AND_ALIASES) {
    // remove all aliased regions in the source and destination tensor
    auto tLocalFlat = tLocal.flatten();
    auto tFlat = t.flatten();
    auto tFlatRegions = masterGraph.getSortedContiguousRegions(
        tFlat, {{0, tFlat.numElements()}}, true);
    copyDst = concat(tLocalFlat.slices(tFlatRegions));
    copySrc = concat(tFlat.slices(tFlatRegions));
  }
  return tLocal;
}

poplar::Tensor copyToIpu(poplar::Graph &graph, const poplar::Tensor &t,
                         poplar::program::Sequence &prog, unsigned dstIpu,
                         poplar::StringRef name,
                         poplar::TensorCloneMethod method) {
  poplar::Tensor tLocalForCopy, tForCopy;
  auto tLocal =
      createIpuCopy(graph, t, dstIpu, tForCopy, tLocalForCopy, name, method);
  prog.add(poplar::program::Copy(tForCopy, tLocalForCopy));
  return tLocal;
}

bool dimIsSplitOverTiles(const poplar::Graph &graph, const poplar::Tensor &t,
                         unsigned dimension) {
  const auto dimElems = t.dim(dimension);
  const auto tShuf = t.dimRoll(dimension, t.rank() - 1);
  const auto tMapping = graph.getTileMapping(tShuf);

  for (unsigned tile = 0; tile < tMapping.size(); ++tile) {
    for (const auto &i : tMapping[tile]) {
      if ((i.begin() % dimElems) || (i.end() % dimElems)) {
        return true;
      }
    }
  }
  return false;
}

bool dimIsSplitOverIPUs(const poplar::Graph &graph, const poplar::Tensor &t,
                        unsigned dimension) {
  const auto &target = graph.getTarget();
  if (target.getNumIPUs() == 1) {
    return false;
  }

  const auto tilesPerIPU = target.getTilesPerIPU();
  const auto dimElems = t.dim(dimension);
  const auto tShuf = t.dimRoll(dimension, t.rank() - 1);
  const auto tMapping = graph.getTileMapping(tShuf);

  using IntervalMap = boost::icl::interval_map<std::size_t, unsigned,
                                               boost::icl::partial_enricher>;
  using Interval = boost::icl::interval<std::size_t>;

  IntervalMap intervalToIPU;
  for (unsigned tile = 0; tile < tMapping.size(); ++tile) {
    const auto ipu = tile / tilesPerIPU;
    for (const auto &i : tMapping[tile]) {
      intervalToIPU +=
          std::make_pair(Interval::right_open(i.begin(), i.end()), ipu);
    }
  }

  // Check each slice of the dimension is not split across multiple IPUs.
  for (const auto &entry : intervalToIPU) {
    const auto &region = entry.first;
    if ((region.lower() % dimElems) || (region.upper() % dimElems)) {
      return true;
    }
  }
  return false;
}

poplar::Tensor createBroadcastOperand(poplar::Graph &graph,
                                      const poplar::Tensor &fullTensor,
                                      const poplar::Type &type, unsigned dim,
                                      bool ditherMapping,
                                      const std::string &name) {
  assert(dim < fullTensor.rank());
  const auto &target = graph.getTarget();
  auto t = fullTensor.dimRoll(dim, fullTensor.rank() - 1);
  const auto numDimElems = fullTensor.dim(dim);
  auto out = graph.addVariable(type, {numDimElems}, name);

  TensorUseTracker useTracker(target.getNumTiles());

  // Find regions of activations or gradients tensors
  const auto mapping = graph.getTileMapping(t);
  for (unsigned tile = 0; tile != mapping.size(); ++tile) {
    for (const auto &region : mapping[tile]) {
      if (region.begin() != region.end()) {
        auto rBegin = region.begin() % numDimElems;
        auto rEnd = region.end() % numDimElems;
        if (region.size() >= numDimElems) {
          useTracker.add(graph, tile, out.slice(0, numDimElems));
        } else {
          if (rBegin < rEnd) {
            useTracker.add(graph, tile, out.slice(rBegin, rEnd));
          } else {
            useTracker.add(graph, tile, out.slice(rBegin, numDimElems));
            useTracker.add(graph, tile, out.slice(0, rEnd));
          }
        }
      }
    }
  }
  const auto grainSize =
      target.getDataPathWidth() / (8 * target.getTypeSize(type));
  useTracker.mapTensorsByUse(
      graph, grainSize, 0, false,
      TensorUseTracker::MappingMethod::ConstrainMappingToUsedTiles);

  // remap with dithering
  if (ditherMapping) {
    // Randomise the start tile for mapping of the variable
    std::size_t seed = 0x9e3779b9UL;
    const auto shape = fullTensor.shape();
    boost::hash_range(seed, shape.begin(), shape.end());

    const auto outMapping = graph.getTileMapping(out);
    const auto numTiles = outMapping.size();

    poplar::Graph::TileToTensorMapping newMapping(numTiles);
    std::size_t dstTile = seed % numTiles;
    for (unsigned tile = 0; tile != numTiles; ++tile) {
      if (!outMapping[tile].empty()) {
        newMapping[dstTile] = std::move(outMapping[tile]);
      }

      if (++dstTile == numTiles) {
        dstTile = 0;
      }
    }
    graph.setTileMapping(out, newMapping);
  }
  return out;
}

} // namespace poputil
