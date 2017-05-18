#include "OperationsDef.hpp"
#include "popstd/Operations.hpp"
#include "popstd/exceptions.hpp"
#include "popstd/Util.hpp"
#include "popstd/VertexTemplates.hpp"

using namespace popstd;
using namespace poplar;
using namespace poplar::program;

static std::string outputType(const std::string &inType, enum UnaryOp op) {
  if (op == LOGICAL_NOT) {
    return "bool";
  } else {
    return inType;
  }
}

static std::string outputType(const std::string &inType, enum BinaryOp op) {
  if (op == EQUAL
      || op == GREATER_THAN_EQUAL
      || op == GREATER_THAN
      || op == LESS_THAN_EQUAL
      || op == LOGICAL_AND
      || op == LOGICAL_OR
      || op == LESS_THAN
      || op == NOT_EQUAL) {
    return "bool";
  } else {
    return inType;
  }
}

static std::string outputType(const std::string &inType,
                              enum TernaryOp /*op*/) {
  return inType;
}

static std::string vertexName(enum UnaryOp op) {
  switch(op) {
  case ABSOLUTE:
    return "popstd::Absolute";
  case CEIL:
    return "popstd::Ceil";
  case EXPONENT:
    return "popstd::Exponent";
  case FLOOR:
    return "popstd::Floor";
  case LOGARITHM:
    return "popstd::Logarithm";
  case LOGICAL_NOT:
    return "popstd::LogicalNot";
  case NEGATE:
    return "popstd::Negate";
  case SIGNUM:
    return "popstd::Signum";
  case TANH:
    return "popstd::Tanh";
  }
  throw popstd::poplib_error("Op not supported");
}

static std::string vertexName(enum BinaryOp op) {
  switch(op) {
    case ADD:
      return "popstd::Add";
    case DIVIDE:
      return "popstd::Divide";
    case EQUAL:
      return "popstd::Equal";
    case GREATER_THAN_EQUAL:
      return "popstd::GreaterThanEqual";
    case GREATER_THAN:
      return "popstd::GreaterThan";
    case LESS_THAN_EQUAL:
      return "popstd::LessThanEqual";
    case LOGICAL_AND:
      return "popstd::LogicalAnd";
    case LOGICAL_OR:
      return "popstd::LogicalOr";
    case LESS_THAN:
      return "popstd::LessThan";
    case MAXIMUM:
      return "popstd::Maximum";
    case MINIMUM:
      return "popstd::Minimum";
    case MULTIPLY:
      return "popstd::Multiply";
    case NOT_EQUAL:
      return "popstd::NotEqual";
    case POWER:
      return "popstd::Power";
    case REMAINDER:
      return "popstd::Remainder";
    case SUBTRACT:
      return "popstd::Subtract";
  }
  throw popstd::poplib_error("Op not supported");
}

static std::string vertexName(enum TernaryOp op) {
  switch(op) {
  case CLAMP:
    return "popstd::Clamp";
  case SELECT:
    return "popstd::Select";
  }
  throw popstd::poplib_error("Op not supported");
}

static Tensor unaryOp(Graph &graph, Tensor in, Sequence &prog,
                      enum UnaryOp op, const std::string &debugPrefix) {

  const auto inType = graph.getTensorElementType(in);
  const auto &deviceInfo = graph.getDevice().getDeviceInfo();
  const auto dataPathWidth = deviceInfo.dataPathWidth;
  const auto numTiles = deviceInfo.getNumTiles();
  const auto mapping = graph.getTileMapping(in);
  const auto cs = graph.addComputeSet(debugPrefix);

  const auto outType = outputType(inType, op);

  auto out = graph.addTensor(outType, in.shape(), debugPrefix + "/Out");
  graph.setTileMapping(out, mapping);

  auto inFlat = in.flatten();
  auto outFlat = out.flatten();

  const auto grainSize = deviceInfo.getVectorWidth(inType);

  for (auto tile = 0U; tile != numTiles; ++tile) {
    auto vertexRegions =
      splitRegionsBetweenWorkers(deviceInfo, mapping[tile],
                                 grainSize, 2 * grainSize);

    for (const auto &regions : vertexRegions) {
      auto v = graph.addVertex(cs,
                               templateVertex(vertexName(op), inType),
                               {{"in", inFlat.slices(regions)},
                                {"out", outFlat.slices(regions)}});
      graph.setInitialValue(v["dataPathWidth"], dataPathWidth);
      graph.setTileMapping(v, tile);
    }
  }
  prog.add(Execute(cs));
  return out;
}

static Tensor binaryOp(Graph &graph, Tensor in1, Tensor in2, Sequence &prog,
                       enum BinaryOp op, const std::string &debugPrefix) {
  const auto in1Type = graph.getTensorElementType(in1);
  const auto in2Type = graph.getTensorElementType(in2);

  if (in1Type != in2Type) {
    throw popstd::poplib_error("Binary Op must have same type for "
                               "both operands: " + debugPrefix);
  }

  if (in1.shape() != in2.shape()) {
    throw popstd::poplib_error("Binary Op must have same shape for "
                               "both operands: " + debugPrefix);
  }

  const auto outType = outputType(in1Type, op);
  const auto &deviceInfo = graph.getDevice().getDeviceInfo();
  const auto dataPathWidth = deviceInfo.dataPathWidth;
  const auto numTiles = deviceInfo.getNumTiles();
  const auto mapping = graph.getTileMapping(in1);
  const auto cs = graph.addComputeSet(debugPrefix);

  auto out = graph.addTensor(outType, in1.shape(), debugPrefix + "/Out");
  graph.setTileMapping(out, mapping);

  auto in1Flat = in1.flatten();
  auto in2Flat = in2.flatten();
  auto outFlat = out.flatten();

  const auto grainSize = deviceInfo.getVectorWidth(in1Type);

  for (auto tile = 0U; tile != numTiles; ++tile) {
    auto vertexRegions =
      splitRegionsBetweenWorkers(deviceInfo, mapping[tile],
                                 grainSize, 2 * grainSize);

    for (const auto &regions : vertexRegions) {
      auto v = graph.addVertex(cs,
                               templateVertex(vertexName(op), in1Type),
                               {{"in1", in1Flat.slices(regions)},
                                {"in2", in2Flat.slices(regions)},
                                {"out", outFlat.slices(regions)}});
      graph.setInitialValue(v["dataPathWidth"], dataPathWidth);
      graph.setTileMapping(v, tile);
    }
  }
  prog.add(Execute(cs));
  return out;
}


static Tensor ternaryOp(Graph &graph, Tensor in1, Tensor in2, Tensor in3,
                        Sequence &prog, enum TernaryOp op,
                        const std::string &debugPrefix) {
  const auto in1Type = graph.getTensorElementType(in1);
  const auto in2Type = graph.getTensorElementType(in2);
  const auto in3Type = graph.getTensorElementType(in3);

  if (in1Type != in2Type) {
    throw popstd::poplib_error("Ternary Op must have same type for "
                               "all input operands: " + debugPrefix);
  }

  if (in1.shape() != in2.shape() || in1.shape() != in3.shape()) {
    throw popstd::poplib_error("Ternary Op must have same shape for "
                               "all input operands: " + debugPrefix);
  }

  const auto outType = outputType(in1Type, op);
  const auto &deviceInfo = graph.getDevice().getDeviceInfo();
  const auto dataPathWidth = deviceInfo.dataPathWidth;
  const auto numTiles = deviceInfo.getNumTiles();
  const auto mapping = graph.getTileMapping(in1);
  const auto cs = graph.addComputeSet(debugPrefix);

  auto out = graph.addTensor(outType, in1.shape(), debugPrefix + "/Out");
  graph.setTileMapping(out, mapping);

  auto in1Flat = in1.flatten();
  auto in2Flat = in2.flatten();
  auto in3Flat = in3.flatten();
  auto outFlat = out.flatten();

  const auto grainSize = deviceInfo.getVectorWidth(in1Type);

  for (auto tile = 0U; tile != numTiles; ++tile) {
    auto vertexRegions =
      splitRegionsBetweenWorkers(deviceInfo, mapping[tile],
                                 grainSize, 2 * grainSize);

    for (const auto &regions : vertexRegions) {
      auto v = graph.addVertex(cs,
                               templateVertex(vertexName(op), in1Type),
                               {{"in1", in1Flat.slices(regions)},
                                {"in2", in2Flat.slices(regions)},
                                {"in3", in3Flat.slices(regions)},
                                {"out", outFlat.slices(regions)}});
      graph.setInitialValue(v["dataPathWidth"], dataPathWidth);
      graph.setTileMapping(v, tile);
    }
  }
  prog.add(Execute(cs));
  return out;
}

namespace popstd {

Tensor add(Graph &graph, Tensor A, Tensor B, Sequence &prog,
           const std::string &debugPrefix) {
  return binaryOp(graph, A, B, prog, BinaryOp::ADD, debugPrefix + "/Op/Add");
}

Tensor abs(Graph &graph, Tensor A, Sequence &prog,
           const std::string &debugPrefix) {
  return unaryOp(graph, A, prog, UnaryOp::ABSOLUTE, debugPrefix + "/Op/Abs");
}

Tensor ceil(Graph &graph, Tensor A, Sequence &prog,
            const std::string &debugPrefix) {
  return unaryOp(graph, A, prog, UnaryOp::CEIL, debugPrefix + "/Op/Ceil");
}

Tensor div(Graph &graph, Tensor A, Tensor B, Sequence &prog,
          const std::string &debugPrefix) {
  return binaryOp(graph, A, B, prog, BinaryOp::DIVIDE, debugPrefix + "/Op/Div");
}

Tensor eq(Graph &graph, Tensor A, Tensor B, Sequence &prog,
          const std::string &debugPrefix) {
  return binaryOp(graph, A, B, prog, BinaryOp::EQUAL,
                  debugPrefix + "/Op/Equal");
}

Tensor exp(Graph &graph, Tensor A, Sequence &prog,
           const std::string &debugPrefix) {
  return unaryOp(graph, A, prog, UnaryOp::EXPONENT, debugPrefix + "/Op/Exp");
}

Tensor floor(Graph &graph, Tensor A, Sequence &prog,
             const std::string &debugPrefix) {
  return unaryOp(graph, A, prog, UnaryOp::FLOOR, debugPrefix + "/Op/Floor");
}

Tensor gt(Graph &graph, Tensor A, Tensor B, Sequence &prog,
          const std::string &debugPrefix) {
  return binaryOp(graph, A, B, prog, BinaryOp::GREATER_THAN,
                  debugPrefix + "/Op/Gteq");
}

Tensor gteq(Graph &graph, Tensor A, Tensor B, Sequence &prog,
            const std::string &debugPrefix) {
  return binaryOp(graph, A, B, prog, BinaryOp::GREATER_THAN_EQUAL,
                  debugPrefix + "/Op/Gteq");
}

Tensor lt(Graph &graph, Tensor A, Tensor B, Sequence &prog,
          const std::string &debugPrefix) {
  return binaryOp(graph, A, B, prog, BinaryOp::LESS_THAN,
                  debugPrefix + "/Op/Lteq");
}

Tensor lteq(Graph &graph, Tensor A, Tensor B, Sequence &prog,
           const std::string &debugPrefix) {
  return binaryOp(graph, A, B, prog, BinaryOp::LESS_THAN_EQUAL,
                  debugPrefix + "/Op/Lteq");
}

Tensor log(Graph &graph, Tensor A, Sequence &prog,
           const std::string &debugPrefix) {
   return unaryOp(graph, A, prog, UnaryOp::LOGARITHM, debugPrefix + "/Op/Log");
}

Tensor logicalAnd(Graph &graph, Tensor A, Tensor B, Sequence &prog,
                  const std::string &debugPrefix) {
  return binaryOp(graph, A, B, prog, BinaryOp::LOGICAL_AND,
                  debugPrefix + "/Op/And");
}

Tensor logicalNot(Graph &graph, Tensor A, Sequence &prog,
                  const std::string &debugPrefix) {
  return unaryOp(graph, A, prog, UnaryOp::LOGICAL_NOT, debugPrefix + "/Op/Not");
}

Tensor logicalOr(Graph &graph, Tensor A, Tensor B, Sequence &prog,
                 const std::string &debugPrefix) {
  return binaryOp(graph, A, B, prog, BinaryOp::LOGICAL_OR,
                  debugPrefix + "/Op/Or");
}

Tensor max(Graph &graph, Tensor A, Tensor B, Sequence &prog,
           const std::string &debugPrefix) {
  return binaryOp(graph, A, B, prog, BinaryOp::MAXIMUM,
                  debugPrefix + "/Op/Max");
}

Tensor min(Graph &graph, Tensor A, Tensor B, Sequence &prog,
           const std::string &debugPrefix) {
  return binaryOp(graph, A, B, prog, BinaryOp::MINIMUM,
                  debugPrefix + "/Op/Min");
}

Tensor mul(Graph &graph, Tensor A, Tensor B, Sequence &prog,
           const std::string &debugPrefix) {
  return binaryOp(graph, A, B, prog, BinaryOp::MULTIPLY,
                  debugPrefix + "/Op/Mul");
}

Tensor neq(Graph &graph, Tensor A, Tensor B, Sequence &prog,
           const std::string &debugPrefix) {
  return binaryOp(graph, A, B, prog, BinaryOp::NOT_EQUAL,
                  debugPrefix + "/Op/Neq");
}

Tensor neg(Graph &graph, Tensor A, Sequence &prog,
           const std::string &debugPrefix) {
  return unaryOp(graph, A, prog, UnaryOp::NEGATE, debugPrefix + "/Op/Neg");
}

Tensor pow(Graph &graph, Tensor A, Tensor B, Sequence &prog,
           const std::string &debugPrefix) {
  return binaryOp(graph, A, B, prog, BinaryOp::POWER, debugPrefix + "/Op/Pow");
}

Tensor rem(Graph &graph, Tensor A, Tensor B, Sequence &prog,
           const std::string &debugPrefix) {
  return binaryOp(graph, A, B, prog, BinaryOp::REMAINDER,
                  debugPrefix + "/Op/Rem");
}

Tensor signum(Graph &graph, Tensor A, Sequence &prog,
            const std::string &debugPrefix) {
  return unaryOp(graph, A, prog, UnaryOp::SIGNUM, debugPrefix + "/Op/Signum");
}

Tensor sub(Graph &graph, Tensor A, Tensor B, Sequence &prog,
           const std::string &debugPrefix) {
  return binaryOp(graph, A, B, prog, BinaryOp::SUBTRACT,
                  debugPrefix + "/Op/Sub");
}

Tensor tanh(Graph &graph, Tensor A, Sequence &prog,
            const std::string &debugPrefix) {
  return unaryOp(graph, A, prog, UnaryOp::TANH, debugPrefix + "/Op/Tanh");
}

Tensor select(Graph &graph, Tensor A, Tensor B, Tensor pred, Sequence &prog,
              const std::string &debugPrefix) {
  return ternaryOp(graph, A, B, pred, prog, TernaryOp::SELECT,
                   debugPrefix + "/Op/Select");
}

Tensor clamp(Graph &graph, Tensor A, Tensor lowerBound, Tensor upperBound,
             Sequence &prog, const std::string &debugPrefix) {
  return ternaryOp(graph, A, lowerBound, upperBound, prog, TernaryOp::CLAMP,
                   debugPrefix + "/Op/Clamp");
}

} // namespace popstd