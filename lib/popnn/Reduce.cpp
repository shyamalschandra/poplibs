#include "Reduce.hpp"

#include "Cast.hpp"
#include "Util.hpp"
#include "VertexTemplates.hpp"
#include "Zero.hpp"

using namespace poplar;

void
reduce(Graph &graph,
       Tensor partials,
       Tensor reduced,
       const std::vector<
         std::vector<std::pair<unsigned, unsigned>>
       > &reducedMapping,
       ComputeSet reduceCS) {
  assert(partials[0].dims() == reduced.dims());
  if (partials.dim(0) == 0) {
    zero(graph, reduced, reducedMapping, reduceCS);
    return;
  }
  if (partials.dim(0) == 1) {
    // TODO if the destination type is smaller than the source type it would
    // be better to perform the cast on the source tile to reduce the volume
    // of data that must be exchanged.
    cast(graph, reducedMapping, partials[0], reduced, reduceCS);
    return;
  }
  const auto partialType = graph.getTensorElementType(partials);
  const auto reducedType = graph.getTensorElementType(reduced);
  const auto tilesPerInZGroup = partials.dim(0);
  auto flatPartials =
      partials.reshape({tilesPerInZGroup,
                        partials.numElements() / tilesPerInZGroup});
  auto flatReduced = reduced.flatten();
  const auto &deviceInfo = graph.getDevice().getDeviceInfo();
  const auto dataPathWidth = deviceInfo.dataPathWidth;
  // Accumulate the partial sums.
  const auto numTiles = deviceInfo.getNumTiles();
  std::vector<std::vector<std::pair<unsigned, unsigned>>> vertexRegions;
  for (unsigned tile = 0; tile != numTiles; ++tile) {
    const auto &tileRegions = reducedMapping[tile];
    unsigned vectorWidth;
    if (partialType == "float")
      vectorWidth = deviceInfo.getFloatVectorWidth();
    else
      vectorWidth = deviceInfo.getHalfVectorWidth();
    splitRegionsBetweenWorkers(deviceInfo, tileRegions, vertexRegions,
                               vectorWidth);
    for (const auto &regions : vertexRegions) {
      const auto v = graph.addVertex(reduceCS,
                                     templateVertex("popnn::ConvReduce",
                                                    reducedType,
                                                    partialType));
      graph.setInitialValue(v["dataPathWidth"], dataPathWidth);
      graph.setFieldSize(v["out"], regions.size());
      graph.setFieldSize(v["partials"], regions.size() * tilesPerInZGroup);
      graph.setTileMapping(v, tile);
      const auto numRegions = regions.size();
      for (unsigned i = 0; i != numRegions; ++i) {
        const auto &region = regions[i];
        const auto regionBegin = region.first;
        const auto regionEnd = region.second;
        auto out = flatReduced.slice(regionBegin, regionEnd);
        graph.connect(v["out"][i], out);
        for (unsigned j = 0; j != tilesPerInZGroup; ++j) {
          graph.connect(
            v["partials"][i * tilesPerInZGroup + j],
            flatPartials[j].slice(regionBegin, regionEnd)
          );
        }
      }
    }
  }
}
