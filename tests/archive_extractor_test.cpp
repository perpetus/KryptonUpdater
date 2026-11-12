#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#include "krypton/updater/archive_extractor.hpp"

using namespace kr::updater;

void TestNonExistentArchive() {
  auto temp_dir = std::filesystem::temp_directory_path() / "Krypton7zTest";
  auto missing_file = temp_dir / "non_existent.7z";

  auto [entries, err] = ArchiveExtractor::InspectArchive(missing_file);
  (void)entries;
  assert(err == ExtractionError::kArchiveNotFound);

  ArchiveExtractor extractor;
  auto extract_err = extractor.ExtractArchive(missing_file, temp_dir);
  (void)extract_err;
  assert(extract_err == ExtractionError::kArchiveNotFound);
}

void TestValid7zArchive() {
  auto temp_dir = std::filesystem::temp_directory_path() / "Krypton7zTestValid";
  std::filesystem::create_directories(temp_dir);

  auto input_file = temp_dir / "payload.txt";
  std::ofstream out(input_file);
  out << "Krypton 7-Zip Payload Content 2026";
  out.close();

  auto archive_file = temp_dir / "test.7z";

  // Create test archive using 7z command if present
  std::string cmd = std::format("7z a -y \"{}\" \"{}\" > nul 2>&1", archive_file.string(), input_file.string());
  int res = std::system(cmd.c_str());

  if (res == 0 && std::filesystem::exists(archive_file)) {
    auto [entries, err] = ArchiveExtractor::InspectArchive(archive_file);
    assert(err == ExtractionError::kNone);
    assert(!entries.empty());

    ThreadPool pool(2);
    ArchiveExtractor extractor(&pool);
    auto extract_dir = temp_dir / "extracted";
    auto extract_err = extractor.ExtractArchive(archive_file, extract_dir);
    (void)extract_err;
    assert(extract_err == ExtractionError::kNone);

    auto extracted_payload = extract_dir / "payload.txt";
    assert(std::filesystem::exists(extracted_payload));

    std::ifstream in(extracted_payload);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    assert(content == "Krypton 7-Zip Payload Content 2026");
  }

  std::filesystem::remove_all(temp_dir);
}

int main() {
  TestNonExistentArchive();
  TestValid7zArchive();
  std::cout << "[PASS] archive_extractor_test (Poco 7-Zip)\n";
  return 0;
}
