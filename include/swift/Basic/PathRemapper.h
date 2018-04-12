//===--- PathRemapper.h - Transforms path prefixes --------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file defines a data structure that stores a string-to-string
//  mapping used to transform file paths based on a prefix mapping.
//
//  This data structure makes some assumptions about the mappings; for
//  example, it is assumed that no source path is a strict prefix of any
//  other source path. If this assumption is violated, the prefix used to
//  remap the path is arbitrary.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_BASIC_PATHREMAPPER_H
#define SWIFT_BASIC_PATHREMAPPER_H

#include "llvm/ADT/Twine.h"

#include <map>
#include <string>

namespace swift {

class PathRemapper {
private:
  std::map<std::string, std::string> PathPrefixMap;

public:
  void addMapping(StringRef FromPrefix, StringRef ToPrefix) {
    PathPrefixMap[FromPrefix] = ToPrefix;
  }

  std::string remapPath(StringRef Path) const {
    for (const auto &Entry : PathPrefixMap)
      if (Path.startswith(Entry.first))
        return (Twine(Entry.second) + Path.substr(Entry.first.size())).str();
    return Path.str();
  }
};

} // end namespace swift

#endif // SWIFT_BASIC_PATHREMAPPER_H