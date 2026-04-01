#pragma once

#include "../util/compat.h"

#include <string>

std::string espCompressorDirname(const std::string &path);
bool espCompressorEnsureDir(fs::FS &fs, const std::string &path);
