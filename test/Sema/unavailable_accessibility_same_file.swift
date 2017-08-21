// RUN: %target-typecheck-verify-swift -swift-version 4

public struct Foo {
  @available(*, unavailable)
  public static let unavailableEverywhereByDefault = 1  // expected-note 3 {{marked unavailable here}}

  @available(*, unavailable private)
  public static let unavailableEverywhereExplicitly = 1  // expected-note 3 {{marked unavailable here}}

  @available(*, unavailable fileprivate)
  public static let unavailableOutsideOfScope = 1  // expected-note {{marked unavailable here}}

  @available(*, unavailable internal)
  public static let unavailableOutsideOfFile = 1

  @available(*, unavailable public)
  public static let unavailableOutsideOfModule = 1

  static func check() {
    _ = unavailableEverywhereByDefault  // expected-error {{is unavailable}}
    _ = unavailableEverywhereExplicitly  // expected-error {{is unavailable}}
    _ = unavailableOutsideOfScope
    _ = unavailableOutsideOfFile
    _ = unavailableOutsideOfModule
  }
}

extension Foo {
  // Extensions in the same file have private access, so errors should reflect
  // that.
  static func checkInSameFileExtension() {
    _ = unavailableEverywhereByDefault  // expected-error {{is unavailable}}
    _ = unavailableEverywhereExplicitly  // expected-error {{is unavailable}}
    _ = unavailableOutsideOfScope
    _ = unavailableOutsideOfFile
    _ = unavailableOutsideOfModule
  }
}

func checkInSameFile() {
  _ = Foo.unavailableEverywhereByDefault  // expected-error {{is unavailable}}
  _ = Foo.unavailableEverywhereExplicitly  // expected-error {{is unavailable}}
  _ = Foo.unavailableOutsideOfScope  // expected-error {{is unavailable}}
  _ = Foo.unavailableOutsideOfFile
  _ = Foo.unavailableOutsideOfModule
}
