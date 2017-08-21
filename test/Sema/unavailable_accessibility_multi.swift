// RUN: %target-typecheck-verify-swift -swift-version 4 %S/Inputs/unavailable_accessibility_defs_1.swift

func checkInDifferentFile() {
  _ = Foo.unavailableEverywhereByDefault  // expected-error {{is unavailable}}
  _ = Foo.unavailableEverywhereExplicitly  // expected-error {{is unavailable}}
  _ = Foo.unavailableOutsideOfScope  // expected-error {{is unavailable}}
  _ = Foo.unavailableOutsideOfFile  // expected-error {{is unavailable}}
  _ = Foo.unavailableOutsideOfModule
}

extension Foo {
  // Extensions in different files don't have private access, so availability
  // errors should reflect that.
  static func checkInDifferentFileExtension() {
    _ = Foo.unavailableEverywhereByDefault  // expected-error {{is unavailable}}
    _ = Foo.unavailableEverywhereExplicitly  // expected-error {{is unavailable}}
    _ = Foo.unavailableOutsideOfScope  // expected-error {{is unavailable}}
    _ = Foo.unavailableOutsideOfFile  // expected-error {{is unavailable}}
    _ = Foo.unavailableOutsideOfModule
  }
}
