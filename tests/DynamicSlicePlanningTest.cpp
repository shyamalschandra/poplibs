#define BOOST_TEST_MODULE DynamicSliceTest
#include "TestDevice.hpp"
#include <boost/test/framework.hpp>
#include <boost/test/unit_test.hpp>
#include <iostream>
#include <numeric>
#include <poplar/Type.hpp>
#include <popops/DynamicSlice.hpp>
#include <popops/DynamicSliceInternal.hpp>
#include <sstream>
#include <vector>

using namespace poplar;
using namespace popops;

void checkPlanner(Type dType, unsigned numEntries, unsigned embeddingSize,
                  const std::vector<std::size_t> numLookups,
                  const sliceInternal::Partition &expectedPartition) {
  BOOST_TEST_MESSAGE(
      "Test " << boost::unit_test::framework::current_test_case().p_name);

  auto device = createTestDevice(TEST_TARGET, 1, 1216);
  Graph graph(device.getTarget());
  auto plan = popops::embedding::plan(graph, dType, numEntries, embeddingSize,
                                      numLookups, {});
  const auto &e = expectedPartition;
  const auto &p = plan.getImpl();
  BOOST_CHECK_EQUAL(e.lookupSplit, p.partition.lookupSplit);
  BOOST_CHECK_EQUAL(e.slicedDimSplit, p.partition.slicedDimSplit);
  BOOST_CHECK_EQUAL(e.unslicedDimSplit, p.partition.unslicedDimSplit);
  BOOST_CHECK_EQUAL(e.unslicedGrainSize, p.partition.unslicedGrainSize);
}

BOOST_AUTO_TEST_CASE(BigEmbedding) {
  checkPlanner(HALF, 100000, 200, {1440}, {1, 12, 100, 2});
}
BOOST_AUTO_TEST_CASE(ManyLookups) {
  checkPlanner(HALF, 1000, 200, {18000}, {24, 1, 50, 2});
}
BOOST_AUTO_TEST_CASE(VeryManyLookups) {
  checkPlanner(HALF, 1000, 200, {40000}, {48, 1, 25, 2});
}
BOOST_AUTO_TEST_CASE(Square) {
  checkPlanner(HALF, 1000, 1000, {1000}, {4, 3, 100, 2});
}
BOOST_AUTO_TEST_CASE(Small) {
  checkPlanner(FLOAT, 50, 50, {20}, {5, 2, 50, 1});
}