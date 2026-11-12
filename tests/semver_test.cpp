#include <cassert>
#include <iostream>

#include "krypton/updater/semver.hpp"

using namespace kr::updater;

void TestSemVerParsing() {
  auto v1 = SemVer::Parse("1.2.3");
  assert(v1.has_value());
  assert(v1->major == 1 && v1->minor == 2 && v1->patch == 3);
  assert(v1->prerelease.empty());

  auto v2 = SemVer::Parse("v0.10.2");
  assert(v2.has_value());
  assert(v2->major == 0 && v2->minor == 10 && v2->patch == 2);

  auto v3 = SemVer::Parse("2.0.0-beta.1");
  assert(v3.has_value());
  assert(v3->major == 2 && v3->minor == 0 && v3->patch == 0);
  assert(v3->prerelease == "beta.1");

  auto v_invalid = SemVer::Parse("not.a.version");
  assert(!v_invalid.has_value());
}

void TestSemVerComparisons() {
  auto v1 = *SemVer::Parse("1.0.0");
  auto v2 = *SemVer::Parse("1.0.1");
  auto v3 = *SemVer::Parse("1.1.0");
  auto v4 = *SemVer::Parse("2.0.0");
  auto v_pre = *SemVer::Parse("1.0.0-alpha");

  assert(v1 < v2);
  assert(v2 < v3);
  assert(v3 < v4);
  assert(v_pre < v1); // Prerelease has lower precedence
  assert(v1 == *SemVer::Parse("1.0.0"));
}

int main() {
  TestSemVerParsing();
  TestSemVerComparisons();
  std::cout << "[PASS] semver_test\n";
  return 0;
}
