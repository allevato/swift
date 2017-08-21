// RUN: %empty-directory(%t)
// RUN: %target-swiftc_driver -swift-version 4 -emit-library -emit-module -emit-module-path %t/deprecated.swiftmodule -module-name deprecated -module-link-name deprecated %S/Inputs/deprecated_accessibility_defs.swift
// RUN: %target-typecheck-verify-swift -swift-version 4 -I %t -ldeprecated

import deprecated

extension Foo {
  // Extensions in different modules should have the same restrictions as other
  // declarations outside of the module.
  static func checkInDifferentFileExtension() {
    _ = Foo.deprecatedEverywhereByDefault  // expected-warning {{is deprecated}}
    _ = Foo.deprecatedEverywhereExplicitly  // expected-warning {{is deprecated}}
    _ = Foo.deprecatedOutsideOfScope  // expected-warning {{is deprecated}}
    _ = Foo.deprecatedOutsideOfFile  // expected-warning {{is deprecated}}
    _ = Foo.deprecatedOutsideOfModule  // expected-warning {{is deprecated}}
  }
}

func checkInDifferentModule() {
  _ = Foo.deprecatedEverywhereByDefault  // expected-warning {{is deprecated}}
  _ = Foo.deprecatedEverywhereExplicitly  // expected-warning {{is deprecated}}
  _ = Foo.deprecatedOutsideOfScope  // expected-warning {{is deprecated}}
  _ = Foo.deprecatedOutsideOfFile  // expected-warning {{is deprecated}}
  _ = Foo.deprecatedOutsideOfModule  // expected-warning {{is deprecated}}
}
