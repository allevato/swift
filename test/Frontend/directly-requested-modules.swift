// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -module-name Imported -emit-module-path %t/Imported.swiftmodule %S/../Inputs/empty.swift
// RUN: %target-swift-frontend -emit-module -M %t/Imported.swiftmodule %s

// RUN: %empty-directory(%t/Imported.swiftmodule)
// RUN: %target-swift-frontend -emit-module -module-name Imported -emit-module-path %t/Imported.swiftmodule/%target-swiftmodule-name %S/../Inputs/empty.swift
// RUN: %target-swift-frontend -emit-module -M %t/Imported.swiftmodule %s

import Imported
