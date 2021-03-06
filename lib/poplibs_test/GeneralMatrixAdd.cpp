// Copyright (c) 2017 Graphcore Ltd. All rights reserved.
#include <cassert>
#include <poplibs_test/GeneralMatrixAdd.hpp>

void poplibs_test::axpby::add(const boost::multi_array_ref<double, 1> matA,
                              const boost::multi_array_ref<double, 1> matB,
                              boost::multi_array_ref<double, 1> matC,
                              float alpha, float beta) {
#ifndef NDEBUG
  unsigned matACols = matA.shape()[0];
  unsigned matBCols = matB.shape()[0];
#endif
  unsigned matCCols = matC.shape()[1];

  assert(matACols == matCCols && matBCols == matCCols);

  for (auto c = 0U; c != matCCols; ++c) {
    matC[c] = alpha * matA[c] + beta * matB[c];
  }
}

void poplibs_test::axpby::add(const boost::multi_array_ref<double, 2> matA,
                              const boost::multi_array_ref<double, 2> matB,
                              boost::multi_array_ref<double, 2> matC,
                              float alpha, float beta, bool transposeA,
                              bool transposeB) {
#ifndef NDEBUG
  unsigned matARows = matA.shape()[0];
  unsigned matACols = matA.shape()[1];
  unsigned matBRows = matB.shape()[0];
  unsigned matBCols = matB.shape()[1];
#endif
  unsigned matCRows = matB.shape()[0];
  unsigned matCCols = matB.shape()[1];

  if (transposeA) {
    assert(matARows == matCCols && matACols == matCRows);
  } else {
    assert(matARows == matCRows && matACols == matCCols);
  }

  if (transposeB) {
    assert(matBRows == matCCols && matBCols == matCRows);
  } else {
    assert(matBRows == matCRows && matBCols == matCCols);
  }

  for (auto r = 0U; r != matCRows; ++r) {
    for (auto c = 0U; c != matCCols; ++c) {
      matC[r][c] = alpha * (transposeA ? matA[c][r] : matA[r][c]) +
                   beta * (transposeB ? matB[c][r] : matB[r][c]);
    }
  }
}

void poplibs_test::axpby::add(const boost::multi_array_ref<double, 3> matA,
                              const boost::multi_array_ref<double, 3> matB,
                              boost::multi_array_ref<double, 3> matC,
                              float alpha, float beta) {
  for (unsigned i = 0; i < 3; ++i) {
    assert(matA.shape()[i] == matB.shape()[i]);
    assert(matA.shape()[i] == matC.shape()[i]);
  }
  unsigned matCDepth = matC.shape()[0];
  unsigned matCRows = matC.shape()[1];
  unsigned matCCols = matC.shape()[2];
  for (auto d = 0U; d != matCDepth; ++d) {
    for (auto r = 0U; r != matCRows; ++r) {
      for (auto c = 0U; c != matCCols; ++c) {
        matC[d][r][c] = alpha * matA[d][r][c] + beta * matB[d][r][c];
      }
    }
  }
}
