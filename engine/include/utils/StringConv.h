#pragma once
#include <string>

std::wstring CoverString(const std::string& s);      // UTF-8 → UTF-16
std::string  CoverString(const std::wstring& ws);    // UTF-16 → UTF-8
