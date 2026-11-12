#include <cassert>
#include <iostream>

#include "krypton/updater/windows/path_sanitizer.hpp"

using namespace kr::updater::windows;

void TestPathSanitizer() {
  std::filesystem::path base_dir = std::filesystem::temp_directory_path() / "KryptonSanitizerTest";
  std::filesystem::create_directories(base_dir);

  // Valid relative paths
  assert(PathSanitizer::IsContainedWithin(base_dir, "file.dll"));
  assert(PathSanitizer::IsContainedWithin(base_dir, "sub/dir/file.pak"));

  // Malicious traversal paths
  assert(!PathSanitizer::IsContainedWithin(base_dir, "../outside.txt"));
  assert(!PathSanitizer::IsContainedWithin(base_dir, "../../Windows/System32/calc.exe"));
  assert(!PathSanitizer::IsContainedWithin(base_dir, "/absolute/path"));
  assert(!PathSanitizer::IsContainedWithin(base_dir, "C:file.txt"));

  std::filesystem::remove_all(base_dir);
}

int main() {
  TestPathSanitizer();
  std::cout << "[PASS] path_sanitizer_test\n";
  return 0;
}
