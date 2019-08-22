#ifndef poplibs_ExpressionGenerator_hpp_
#define poplibs_ExpressionGenerator_hpp_
#include <poplar/Graph.hpp>
#include <poplar/Program.hpp>
#include <popops/Expr.hpp>
#include <popops/ExprOp.hpp>

#include <unordered_map>
#include <unordered_set>

#include <queue>
#include <stack>
#include <string>
#include <vector>

namespace popops {

// Interface wrapper for the below class.
poplar::Tensor generateAndExecuteMappedOperations(
    poplar::Graph &graph, const expr::Expr &expr,
    const std::vector<poplar::Tensor> &inputs, poplar::program::Sequence &prog,
    bool inPlace, const std::string &debugPrefix = "");

bool isExpressionSupported(const expr::Expr &expr,
                           const std::vector<poplar::Tensor> &ins,
                           bool isForcedOn);

// Traverses the expression tree and converts each into a string from the bottom
// up using Dijkstra's Two-Stack algorithm (using the call stack as the implicit
// second stack) and builds a C++ codelet from the expressions.
class GenerateCodeletFromMapExpr {
public:
  GenerateCodeletFromMapExpr(bool inPlace_,
                             const std::vector<poplar::Tensor> &ins)
      : data(), initalizers(), inputs(ins), numFusedOps(0),
        vectorizationIsSupported(true), inPlace(inPlace_){};

  // Traverse the expression tree and populate the data and initalizers fields.
  void traverseExpressionTree(const expr::Expr &expr);

  // Create the codelet, save it to file, register the codelet to poplar, then
  // remove the file.
  std::string generateCodelet(poplar::Graph &graph);

  poplar::Type deduceReturnType() const { return data.top().second; }

  bool isVectorized() const { return vectorizationIsSupported; }

  size_t getNumFusedOps() const { return numFusedOps; }

private:
  // Add the header section (includes, template traits, helper functions).
  void addHeader(std::stringstream &stream);

  // Add a vectorized loop to the codelet.
  void addVectorizedSection(std::stringstream &stream,
                            size_t vectorizationWidth,
                            std::string &initalizerString,
                            std::string &constantInitalizerStringVector);

  // We always have non-vectorized serial equivalent. We always add this even if
  // we have a vectorized section as we may need to process a remainder as well.
  void addSerialSection(std::stringstream &stream,
                        std::string &initalizerString,
                        std::string &constantInitalizerString);

  // The string "data" which can be either a previously evaluated expression
  // (represented as a C++ variable name), a constant or a placeholder value.
  // We include its type as well for deducing the type of the next expression.
  using StringTypePair = std::pair<std::string, poplar::Type>;

  // At the end of the process this will just contain the variable name of the
  // final result. During the traversal it will contain the intermedate stack
  // of variable names/constants/placeholders which should be poped from upon
  // hitting an operation.
  std::stack<StringTypePair> data;

  // Each expression which is executed is converted to a string and stored as
  // an initalizer.
  std::queue<std::string> initalizers;

  // Each constant is added as its own initalizer. We can't do this lazily
  // because the function might be vectorized later. So we store the "const T
  // c1 = " part and then the actual constant as a string in a pair and
  // combine them later after we know what we are doing with vectorization.
  std::queue<std::pair<std::string, std::string>> constantInitalizers;

  // Hash a poplar::Type by its string representation.
  struct HashType {
    size_t operator()(const poplar::Type &t) const {
      return std::hash<std::string>()(t.toString());
    }
  };

  // Compare a poplar::Type by comparing the string representation.
  struct CompareType {
    bool operator()(const poplar::Type &lhs, const poplar::Type &rhs) const {
      return lhs.toString() == rhs.toString();
    }
  };
  // As we use types to the codelet we add them to this so they can be aliased
  // to a typedef for vectorization.
  std::unordered_set<poplar::Type, HashType, CompareType> TypesNeedingAlias;

  std::unordered_map<poplar::Graph *,
                     std::unordered_map<std::string, std::string>>
      graphToCodelets;

  const std::vector<poplar::Tensor> &inputs;

  // Number of operations we are fusing in this vertex.
  size_t numFusedOps;

  bool vectorizationIsSupported;

  bool inPlace;

  static int GeneratedVertexCount;
};
} // namespace popops

#endif // poplibs_ExpressionGenerator_hpp_