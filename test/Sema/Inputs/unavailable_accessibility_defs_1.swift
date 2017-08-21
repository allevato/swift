public struct Foo {
  @available(*, unavailable)
  public static let unavailableEverywhereByDefault = 1  // expected-note 2 {{marked unavailable here}}

  @available(*, unavailable private)
  public static let unavailableEverywhereExplicitly = 1  // expected-note 2 {{marked unavailable here}}

  @available(*, unavailable fileprivate)
  public static let unavailableOutsideOfScope = 1  // expected-note 2 {{marked unavailable here}}

  @available(*, unavailable internal)
  public static let unavailableOutsideOfFile = 1  // expected-note 2 {{marked unavailable here}}

  @available(*, unavailable public)
  public static let unavailableOutsideOfModule = 1
}
