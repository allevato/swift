// RUN: %target-typecheck-verify-swift -swift-version 4 %S/Inputs/deprecated_accessibility_defs.swift

func checkInDifferentFile() {
  _ = Foo.deprecatedEverywhereByDefault  // expected-warning{{is deprecated}}
  _ = Foo.deprecatedEverywhereExplicitly  // expected-warning{{is deprecated}}
  _ = Foo.deprecatedOutsideOfScope  // expected-warning{{is deprecated}}
  _ = Foo.deprecatedOutsideOfFile  // expected-warning{{is deprecated}}
  _ = Foo.deprecatedOutsideOfModule
}

extension Foo {
  // Extensions in different files don't have private access, so deprecation
  // warnings should reflect that.
  static func checkInDifferentFileExtension() {
    _ = Foo.deprecatedEverywhereByDefault  // expected-warning{{is deprecated}}
    _ = Foo.deprecatedEverywhereExplicitly  // expected-warning{{is deprecated}}
    _ = Foo.deprecatedOutsideOfScope  // expected-warning{{is deprecated}}
    _ = Foo.deprecatedOutsideOfFile  // expected-warning{{is deprecated}}
    _ = Foo.deprecatedOutsideOfModule
  }
}