#ifndef _poplib_test_Pass_hpp_
#define _poplib_test_Pass_hpp_

#include <iosfwd>

namespace poplib_test {

// class to allow the training pass to be specified
enum class Pass {
  FWD,
  BWD,
  WU,
  ALL
};

const char *asString(const Pass &pass);

std::istream &operator>>(std::istream &is, Pass &pass);

std::ostream &operator<<(std::ostream &os, const Pass &pass);

} // End namespace poplib_test.

#endif  // _poplib_test_Pass_hpp_