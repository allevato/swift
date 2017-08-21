public struct Foo {
  @available(*, deprecated)
  public static let deprecatedEverywhereByDefault = 1

  @available(*, deprecated private)
  public static let deprecatedEverywhereExplicitly = 1

  @available(*, deprecated fileprivate)
  public static let deprecatedOutsideOfScope = 1

  @available(*, deprecated internal)
  public static let deprecatedOutsideOfFile = 1

  @available(*, deprecated public)
  public static let deprecatedOutsideOfModule = 1

  static func check() {
    _ = deprecatedEverywhereByDefault  // expected-warning{{is deprecated}}
    _ = deprecatedEverywhereExplicitly  // expected-warning{{is deprecated}}
    _ = deprecatedOutsideOfScope
    _ = deprecatedOutsideOfFile
    _ = deprecatedOutsideOfModule
  }
}

extension Foo {
  // Extensions in the same file have private access, so warnings should reflect
  // that.
  static func checkInSameFileExtension() {
    _ = deprecatedEverywhereByDefault  // expected-warning{{is deprecated}}
    _ = deprecatedEverywhereExplicitly  // expected-warning{{is deprecated}}
    _ = deprecatedOutsideOfScope
    _ = deprecatedOutsideOfFile
    _ = deprecatedOutsideOfModule
  }
}

func checkInSameFile() {
  _ = Foo.deprecatedEverywhereByDefault  // expected-warning{{is deprecated}}
  _ = Foo.deprecatedEverywhereExplicitly  // expected-warning{{is deprecated}}
  _ = Foo.deprecatedOutsideOfScope  // expected-warning{{is deprecated}}
  _ = Foo.deprecatedOutsideOfFile
  _ = Foo.deprecatedOutsideOfModule
}
