// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -o %t -I %t %S/Inputs/abcde.swift
// RUN: %target-swift-frontend -typecheck -enable-experimental-feature RenamedImports %s -I %t -sdk "" -verify

// REQUIRES: swift_swift_parser

import abcde as vwxyz

let a = abcde.A()
