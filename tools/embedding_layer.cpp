#include "TestDevice.hpp"

#include <poplar/Engine.hpp>
#include <poplar/Graph.hpp>
#include <poplar/IPUModel.hpp>

#include <popops/codelets.hpp>
#include <popops/DynamicSlice.hpp>

#include <poplibs_support/logging.hpp>

#include <poplibs_test/Embedding.hpp>
#include <poplibs_test/Util.hpp>

#include <poputil/exceptions.hpp>

#include <boost/multi_array.hpp>
#include <boost/program_options.hpp>

#include <iostream>
#include <random>

using namespace poplar;
using namespace poplibs_test::util;
using namespace poplar::program;

namespace logging = poplibs_support::logging;

// Default tolerances used in tests
constexpr double FLOAT_REL_TOL = 0.1;
constexpr double HALF_REL_TOL = 0.3;
constexpr double FLOAT_ABS_TOL = 1e-5;
constexpr double HALF_ABS_TOL = 7e-2;

enum class Pass : std::uint8_t {
  FWD, WU, BOTH
};

std::ostream &operator<<(std::ostream &os, const Pass p) {
  switch(p) {
    case Pass::FWD: return os << "fwd";
    case Pass::WU: return os << "wu";
    case Pass::BOTH: return os << "both";
  }

  throw poputil::poplibs_error("Invalid pass");
}

std::istream &operator>>(std::istream &is, Pass &p) {
  std::string token;
  is >> token;

  if (token == "fwd") {
    p = Pass::FWD;
  } else if (token == "wu") {
    p = Pass::WU;
  } else if (token == "both") {
    p = Pass::BOTH;
  } else {
    throw poputil::poplibs_error("Invalid token for pass: " + token);
  }

  return is;
}

bool passEnabled(const Pass opt, const Pass pass) {
  return opt == pass || opt == Pass::BOTH;
}

int main(int argc, char **argv) {
  namespace po = boost::program_options;

  struct Options {
    bool profile = false;
    bool showExecutionSteps = false;
    bool showVarStorage = false;

    DeviceType deviceType = DeviceType::IpuModel;
    unsigned numIPUs = IPUModel{}.numIPUs;
    unsigned tilesPerIPU = IPUModel{}.tilesPerIPU;

    Type dataType = FLOAT;
    unsigned grainSize;
    ShapeOption<std::size_t> shape;
    ShapeOption<long unsigned> indices;
    double scale = 1.;

    Pass pass = Pass::BOTH;
    bool ignoreData = false;
  };

  Options opts;

  po::options_description desc("embedding_layer options");
  desc.add_options()
    ("help", "Produce help message")
    ("profile",
     po::value<bool>(&opts.profile)->default_value(opts.profile),
     "Output profiling report")
    ("show-execution-steps",
     po::value<bool>(&opts.showExecutionSteps)
       ->default_value(opts.showExecutionSteps),
     "Show execution steps (requires profiling)")
    ("show-var-storage",
     po::value<bool>(&opts.showVarStorage)->default_value(opts.showVarStorage),
     "Show variable liveness (requires profiling)")
    ("device-type",
     po::value<DeviceType>(&opts.deviceType)->default_value(opts.deviceType),
     "Device type: Cpu | Sim | Hw | IpuModel")
    ("ipus",
     po::value<unsigned>(&opts.numIPUs)->default_value(opts.numIPUs),
     "Number of IPUs")
    ("tiles-per-ipu",
     po::value<unsigned>(&opts.tilesPerIPU)->default_value(opts.tilesPerIPU),
     "Number of tiles per IPU")
    ("data-type",
     po::value<Type>(&opts.dataType)->default_value(opts.dataType),
     "The data type of values stored in the embedding matrix")
    ("grain-size",
     po::value<unsigned>(&opts.grainSize),
     "Minimum elements per slice mapped to each tile. Defaults to the vector "
     "width of the data type for the target chosen.")
    ("shape",
     po::value<ShapeOption<std::size_t>>(&opts.shape)->required(),
     "The shape of the embedding matrix, must be 2D.")
    ("indices",
     po::value<ShapeOption<long unsigned>>(&opts.indices)->required(),
     "List of indices to pull out of the embedding matrix")
    ("scale",
     po::value<double>(&opts.scale)->default_value(opts.scale),
     "Scale applied to the deltas during the update pass")
    ("pass",
     po::value<Pass>(&opts.pass)->default_value(opts.pass),
     "Which pass of the embedding layer to perform: fwd | wu | both")
    ("ignore-data",
     po::value<bool>(&opts.ignoreData)->default_value(opts.ignoreData),
     "Don't upload and download the results from the device. Note that this "
     "means the result is not validated against the model.")
    ;

  po::variables_map vm;
  try {
    po::store(po::parse_command_line(argc, argv, desc), vm);
    if (vm.count("help")) {
      std::cout << desc;
      return 0;
    }

    po::notify(vm);
  } catch (std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }

  if (opts.shape->size() != 2) {
    throw poputil::poplibs_error("The embedding matrix must be 2 dimensions");
  }

  for (const auto &index : opts.indices) {
    if (index >= opts.shape->at(0)) {
      throw poputil::poplibs_error("Index is out-of-bounds.");
    }
  }

  const bool compileIPUCode = true;
  auto device = createTestDevice(opts.deviceType, opts.numIPUs,
                                 opts.tilesPerIPU, compileIPUCode);

  const auto &target = device.getTarget();

  if (!vm.count("grain-size")) {
    opts.grainSize = target.getVectorWidth(opts.dataType);
  }

  logging::info("Embedding matrix shape: {}", opts.shape);
  logging::info("Indices to process: {}", opts.indices);
  logging::info("Performing pass: {}", opts.pass);

  Graph graph(target);
  popops::addCodelets(graph);

  Sequence prog, uploadProg, downloadProg;

  std::unique_ptr<char []> rawExtractedData, rawDeltas;
  std::vector<std::pair<std::string, char *>> tmap;

  logging::info("Graph construction: create embedding matrix, grain size {}",
                opts.grainSize);
  const auto embeddingMatrix =
      popops::createSliceableTensor(graph, opts.dataType, opts.shape, {0}, {1},
                                    opts.grainSize, "embedding");
  const auto rawEmbeddingMatrix =
      allocateHostMemoryForTensor(embeddingMatrix, "embeddingMatrix", graph,
                                  uploadProg, downloadProg, tmap);

  const auto offsets =
      graph.addConstant(UNSIGNED_INT, {opts.indices->size(), 1},
                        opts.indices->data(), "offsets");
  graph.setTileMapping(offsets, 0);

  if (passEnabled(opts.pass, Pass::FWD)) {
    logging::info("Graph construction: create gather operation");
    const auto extractedData =
        popops::multiSlice(graph, embeddingMatrix, offsets, {0}, {1}, prog,
                           "extracted");
    rawExtractedData =
        allocateHostMemoryForTensor(extractedData, "extractedData", graph,
                                    uploadProg, downloadProg, tmap);
  }

  if (passEnabled(opts.pass, Pass::WU)) {
    logging::info("Graph construction: create update operation");
    const auto deltas =
        popops::createUpdateTensor(graph, embeddingMatrix, {0}, {1},
                                   opts.indices->size(), "deltas");
    rawDeltas =
        allocateHostMemoryForTensor(deltas, "deltas", graph, uploadProg,
                                    downloadProg, tmap);

    const auto scale = graph.addConstant(opts.dataType, {}, opts.scale,
                                         "scale");
    graph.setTileMapping(scale, 0);

    popops::multiUpdateAdd(graph, embeddingMatrix, deltas, offsets, scale, {0},
                          {1}, prog, "updated");
  }

  Sequence ctrlProg;
  if (!opts.ignoreData) {
    ctrlProg.add(uploadProg);
  }
  ctrlProg.add(prog);
  if (!opts.ignoreData) {
    ctrlProg.add(downloadProg);
  }

  logging::info("Create engine");
  Engine engine(graph, ctrlProg, {});

  const auto embeddingMatrixExtents =
      boost::extents[opts.shape->at(0)][opts.shape->at(1)];
  boost::multi_array<double, 2> hostEmbeddingMatrix(embeddingMatrixExtents);

  const auto extractedDataExtents =
      boost::extents[opts.indices->size()][opts.shape->at(1)];
  boost::multi_array<double, 2> hostExtractedData(extractedDataExtents);
  boost::multi_array<double, 2> hostDeltas(extractedDataExtents);

  if (!opts.ignoreData) {
    logging::info("Generating the embedding matrix on the host");
    attachStreams(engine, tmap);

    std::mt19937 randomEngine;
    writeRandomValues(target, opts.dataType, hostEmbeddingMatrix, -10., 10.,
                      randomEngine);
    copy(target, hostEmbeddingMatrix, opts.dataType, rawEmbeddingMatrix.get());

    if (passEnabled(opts.pass, Pass::WU)) {
      writeRandomValues(target, opts.dataType, hostDeltas, -1., 1.,
                        randomEngine);
      copy(target, hostDeltas, opts.dataType, rawDeltas.get());
    }
  }

  logging::info("Run program");
  device.bind([&](const Device &d) {
    engine.load(d);
    engine.run(0);
  });

  bool matchesModel = true;

  if (!opts.ignoreData) {
    const double absTol = opts.dataType == FLOAT ? FLOAT_ABS_TOL : HALF_ABS_TOL;
    const double relTol = opts.dataType == FLOAT ? FLOAT_REL_TOL : HALF_REL_TOL;

    boost::multi_array<double, 2> modelExtractedData(extractedDataExtents);
    boost::multi_array<double, 2> modelEmbeddingMatrix(embeddingMatrixExtents);

    if (passEnabled(opts.pass, Pass::FWD)) {
      logging::info("Validate gather operation against model");
      poplibs_test::embedding::multiSlice(hostEmbeddingMatrix, opts.indices,
                                          modelExtractedData);

      copy(target, opts.dataType, rawExtractedData.get(), hostExtractedData);
      matchesModel &= checkIsClose("multiSlice", hostExtractedData,
                                   modelExtractedData, relTol, absTol);
    }

    if (passEnabled(opts.pass, Pass::WU)) {
      logging::info("Validate update operation against model");
      std::copy_n(hostEmbeddingMatrix.data(),
                  hostEmbeddingMatrix.num_elements(),
                  modelEmbeddingMatrix.data());

      poplibs_test::embedding::multiUpdateAdd(hostDeltas, opts.indices,
                                              opts.scale, modelEmbeddingMatrix);

      copy(target, opts.dataType, rawEmbeddingMatrix.get(),
           hostEmbeddingMatrix);
      matchesModel &= checkIsClose("multiUpdateAdd", hostEmbeddingMatrix,
                                   modelEmbeddingMatrix, relTol, absTol);
    }
  }

  if (opts.profile) {
    engine.printProfileSummary(std::cout, {
      {"showExecutionSteps", opts.showExecutionSteps ? "true" : "false"},
      {"showVarStorage", opts.showVarStorage ? "true" : "false"},
    });
  }

  if (!matchesModel) {
    std::cerr << "Validation failed" << std::endl;
  }

  return matchesModel ? 0 : 1;
}