#pragma once
#include <set>
#include <string>
#include <vector>

#include "../filter/filter.h"
#include "proto/manifest.pb.h"
#include <filesystem>
struct WriteOption {
  int64_t max_record_per_file;
};

struct ReadOption {
  std::vector<Filter*> filters;
  std::vector<std::string> columns;  // must have pk and version
  int limit = -1;
  int version = -1;

};

struct SpaceOption {
  std::string uri;
  std::string primary_column;  // could not be null, int64 or string
  std::string version_column;  // could be null, int64
  std::string vector_column;   // could be null, fixed length binary

  bool
  Parse() {
    std::filesystem::path p;
  }
};
