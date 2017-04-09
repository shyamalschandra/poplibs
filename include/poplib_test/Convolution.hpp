#ifndef _poplib_test_Convolution_hpp_
#define _poplib_test_Convolution_hpp_

#include<boost/multi_array.hpp>

namespace poplib_test {
namespace conv {

void convolution(unsigned strideHeight, unsigned strideWidth,
                 unsigned paddingHeight, unsigned paddingWidth,
                 const boost::multi_array<double, 4> &in,
                 const boost::multi_array<double, 4> &weights,
                 const boost::multi_array<double, 1> &biases,
                 boost::multi_array<double, 4> &out);

void convolutionBackward(unsigned strideHeight, unsigned strideWidth,
                         unsigned paddingHeight, unsigned paddingWidth,
                         const boost::multi_array<double, 4> &in,
                         const boost::multi_array<double, 4> &weights,
                         boost::multi_array<double, 4> &out);

void weightUpdate(unsigned strideHeight, unsigned strideWidth,
                  unsigned paddingHeight, unsigned paddingWidth,
                  double learningRate,
                  const boost::multi_array<double, 4> &activations,
                  const boost::multi_array<double, 4> &deltas,
                  boost::multi_array<double, 4> &weights,
                  boost::multi_array<double, 1> &biases);

} // End namespace poplib_test.
} // End namespace conv.

#endif  // _poplib_test_Convolution_hpp_