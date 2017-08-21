// RUN: %empty-directory(%t)
// RUN: %target-swiftc_driver -swift-version 4 -emit-library -emit-module -emit-module-path %t/unavailable.swiftmodule -module-name unavailable -module-link-name unavailable %S/Inputs/unavailable_accessibility_defs_2.swift
// RUN: %target-typecheck-verify-swift -swift-version 4 -I %t -lunavailable

import unavailable

extension Foo {
  // Extensions in different modules should have the same restrictions as other
  // declarations outside of the module.
  static func checkInDifferentFileExtension() {
    _ = Foo.unavailableEverywhereByDefault  // expected-error {{is unavailable}}
    _ = Foo.unavailableEverywhereExplicitly  // expected-error {{is unavailable}}
    _ = Foo.unavailableOutsideOfScope  // expected-error {{is unavailable}}
    _ = Foo.unavailableOutsideOfFile  // expected-error {{is unavailable}}
    _ = Foo.unavailableOutsideOfModule  // expected-error {{is unavailable}}
  }
}

func checkInDifferentModule() {
  _ = Foo.unavailableEverywhereByDefault  // expected-error {{is unavailable}}
  _ = Foo.unavailableEverywhereExplicitly  // expected-error {{is unavailable}}
  _ = Foo.unavailableOutsideOfScope  // expected-error {{is unavailable}}
  _ = Foo.unavailableOutsideOfFile  // expected-error {{is unavailable}}
  _ = Foo.unavailableOutsideOfModule  // expected-error {{is unavailable}}
}
