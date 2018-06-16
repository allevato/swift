//===--- Array.swift ------------------------------------------*- swift -*-===//
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
//  Three generic, mutable array-like types with value semantics.
//
//  - `Array<Element>` is like `ContiguousArray<Element>` when `Element` is not
//    a reference type or an Objective-C existential.  Otherwise, it may use
//    an `NSArray` bridged from Cocoa for storage.
//
//===----------------------------------------------------------------------===//

/// An ordered, random-access collection.
///
/// Arrays are one of the most commonly used data types in an app. You use
/// arrays to organize your app's data. Specifically, you use the `Array` type
/// to hold elements of a single type, the array's `Element` type. An array
/// can store any kind of elements---from integers to strings to classes.
///
/// Swift makes it easy to create arrays in your code using an array literal:
/// simply surround a comma-separated list of values with square brackets.
/// Without any other information, Swift creates an array that includes the
/// specified values, automatically inferring the array's `Element` type. For
/// example:
///
///     // An array of 'Int' elements
///     let oddNumbers = [1, 3, 5, 7, 9, 11, 13, 15]
///
///     // An array of 'String' elements
///     let streets = ["Albemarle", "Brandywine", "Chesapeake"]
///
/// You can create an empty array by specifying the `Element` type of your
/// array in the declaration. For example:
///
///     // Shortened forms are preferred
///     var emptyDoubles: [Double] = []
///
///     // The full type name is also allowed
///     var emptyFloats: Array<Float> = Array()
///
/// If you need an array that is preinitialized with a fixed number of default
/// values, use the `Array(repeating:count:)` initializer.
///
///     var digitCounts = Array(repeating: 0, count: 10)
///     print(digitCounts)
///     // Prints "[0, 0, 0, 0, 0, 0, 0, 0, 0, 0]"
///
/// Accessing Array Values
/// ======================
///
/// When you need to perform an operation on all of an array's elements, use a
/// `for`-`in` loop to iterate through the array's contents.
///
///     for street in streets {
///         print("I don't live on \(street).")
///     }
///     // Prints "I don't live on Albemarle."
///     // Prints "I don't live on Brandywine."
///     // Prints "I don't live on Chesapeake."
///
/// Use the `isEmpty` property to check quickly whether an array has any
/// elements, or use the `count` property to find the number of elements in
/// the array.
///
///     if oddNumbers.isEmpty {
///         print("I don't know any odd numbers.")
///     } else {
///         print("I know \(oddNumbers.count) odd numbers.")
///     }
///     // Prints "I know 8 odd numbers."
///
/// Use the `first` and `last` properties for safe access to the value of the
/// array's first and last elements. If the array is empty, these properties
/// are `nil`.
///
///     if let firstElement = oddNumbers.first, let lastElement = oddNumbers.last {
///         print(firstElement, lastElement, separator: ", ")
///     }
///     // Prints "1, 15"
///
///     print(emptyDoubles.first, emptyDoubles.last, separator: ", ")
///     // Prints "nil, nil"
///
/// You can access individual array elements through a subscript. The first
/// element of a nonempty array is always at index zero. You can subscript an
/// array with any integer from zero up to, but not including, the count of
/// the array. Using a negative number or an index equal to or greater than
/// `count` triggers a runtime error. For example:
///
///     print(oddNumbers[0], oddNumbers[3], separator: ", ")
///     // Prints "1, 7"
///
///     print(emptyDoubles[0])
///     // Triggers runtime error: Index out of range
///
/// Adding and Removing Elements
/// ============================
///
/// Suppose you need to store a list of the names of students that are signed
/// up for a class you're teaching. During the registration period, you need
/// to add and remove names as students add and drop the class.
///
///     var students = ["Ben", "Ivy", "Jordell"]
///
/// To add single elements to the end of an array, use the `append(_:)` method.
/// Add multiple elements at the same time by passing another array or a
/// sequence of any kind to the `append(contentsOf:)` method.
///
///     students.append("Maxime")
///     students.append(contentsOf: ["Shakia", "William"])
///     // ["Ben", "Ivy", "Jordell", "Maxime", "Shakia", "William"]
///
/// You can add new elements in the middle of an array by using the
/// `insert(_:at:)` method for single elements and by using
/// `insert(contentsOf:at:)` to insert multiple elements from another
/// collection or array literal. The elements at that index and later indices
/// are shifted back to make room.
///
///     students.insert("Liam", at: 3)
///     // ["Ben", "Ivy", "Jordell", "Liam", "Maxime", "Shakia", "William"]
///
/// To remove elements from an array, use the `remove(at:)`,
/// `removeSubrange(_:)`, and `removeLast()` methods.
///
///     // Ben's family is moving to another state
///     students.remove(at: 0)
///     // ["Ivy", "Jordell", "Liam", "Maxime", "Shakia", "William"]
///
///     // William is signing up for a different class
///     students.removeLast()
///     // ["Ivy", "Jordell", "Liam", "Maxime", "Shakia"]
///
/// You can replace an existing element with a new value by assigning the new
/// value to the subscript.
///
///     if let i = students.firstIndex(of: "Maxime") {
///         students[i] = "Max"
///     }
///     // ["Ivy", "Jordell", "Liam", "Max", "Shakia"]
///
/// Growing the Size of an Array
/// ----------------------------
///
/// Every array reserves a specific amount of memory to hold its contents. When
/// you add elements to an array and that array begins to exceed its reserved
/// capacity, the array allocates a larger region of memory and copies its
/// elements into the new storage. The new storage is a multiple of the old
/// storage's size. This exponential growth strategy means that appending an
/// element happens in constant time, averaging the performance of many append
/// operations. Append operations that trigger reallocation have a performance
/// cost, but they occur less and less often as the array grows larger.
///
/// If you know approximately how many elements you will need to store, use the
/// `reserveCapacity(_:)` method before appending to the array to avoid
/// intermediate reallocations. Use the `capacity` and `count` properties to
/// determine how many more elements the array can store without allocating
/// larger storage.
///
/// For arrays of most `Element` types, this storage is a contiguous block of
/// memory. For arrays with an `Element` type that is a class or `@objc`
/// protocol type, this storage can be a contiguous block of memory or an
/// instance of `NSArray`. Because any arbitrary subclass of `NSArray` can
/// become an `Array`, there are no guarantees about representation or
/// efficiency in this case.
///
/// Modifying Copies of Arrays
/// ==========================
///
/// Each array has an independent value that includes the values of all of its
/// elements. For simple types such as integers and other structures, this
/// means that when you change a value in one array, the value of that element
/// does not change in any copies of the array. For example:
///
///     var numbers = [1, 2, 3, 4, 5]
///     var numbersCopy = numbers
///     numbers[0] = 100
///     print(numbers)
///     // Prints "[100, 2, 3, 4, 5]"
///     print(numbersCopy)
///     // Prints "[1, 2, 3, 4, 5]"
///
/// If the elements in an array are instances of a class, the semantics are the
/// same, though they might appear different at first. In this case, the
/// values stored in the array are references to objects that live outside the
/// array. If you change a reference to an object in one array, only that
/// array has a reference to the new object. However, if two arrays contain
/// references to the same object, you can observe changes to that object's
/// properties from both arrays. For example:
///
///     // An integer type with reference semantics
///     class IntegerReference {
///         var value = 10
///     }
///     var firstIntegers = [IntegerReference(), IntegerReference()]
///     var secondIntegers = firstIntegers
///
///     // Modifications to an instance are visible from either array
///     firstIntegers[0].value = 100
///     print(secondIntegers[0].value)
///     // Prints "100"
///
///     // Replacements, additions, and removals are still visible
///     // only in the modified array
///     firstIntegers[0] = IntegerReference()
///     print(firstIntegers[0].value)
///     // Prints "10"
///     print(secondIntegers[0].value)
///     // Prints "100"
///
/// Arrays, like all variable-size collections in the standard library, use
/// copy-on-write optimization. Multiple copies of an array share the same
/// storage until you modify one of the copies. When that happens, the array
/// being modified replaces its storage with a uniquely owned copy of itself,
/// which is then modified in place. Optimizations are sometimes applied that
/// can reduce the amount of copying.
///
/// This means that if an array is sharing storage with other copies, the first
/// mutating operation on that array incurs the cost of copying the array. An
/// array that is the sole owner of its storage can perform mutating
/// operations in place.
///
/// In the example below, a `numbers` array is created along with two copies
/// that share the same storage. When the original `numbers` array is
/// modified, it makes a unique copy of its storage before making the
/// modification. Further modifications to `numbers` are made in place, while
/// the two copies continue to share the original storage.
///
///     var numbers = [1, 2, 3, 4, 5]
///     var firstCopy = numbers
///     var secondCopy = numbers
///
///     // The storage for 'numbers' is copied here
///     numbers[0] = 100
///     numbers[1] = 200
///     numbers[2] = 300
///     // 'numbers' is [100, 200, 300, 4, 5]
///     // 'firstCopy' and 'secondCopy' are [1, 2, 3, 4, 5]
///
/// Bridging Between Array and NSArray
/// ==================================
///
/// When you need to access APIs that require data in an `NSArray` instance
/// instead of `Array`, use the type-cast operator (`as`) to bridge your
/// instance. For bridging to be possible, the `Element` type of your array
/// must be a class, an `@objc` protocol (a protocol imported from Objective-C
/// or marked with the `@objc` attribute), or a type that bridges to a
/// Foundation type.
///
/// The following example shows how you can bridge an `Array` instance to
/// `NSArray` to use the `write(to:atomically:)` method. In this example, the
/// `colors` array can be bridged to `NSArray` because the `colors` array's
/// `String` elements bridge to `NSString`. The compiler prevents bridging the
/// `moreColors` array, on the other hand, because its `Element` type is
/// `Optional<String>`, which does *not* bridge to a Foundation type.
///
///     let colors = ["periwinkle", "rose", "moss"]
///     let moreColors: [String?] = ["ochre", "pine"]
///
///     let url = NSURL(fileURLWithPath: "names.plist")
///     (colors as NSArray).write(to: url, atomically: true)
///     // true
///
///     (moreColors as NSArray).write(to: url, atomically: true)
///     // error: cannot convert value of type '[String?]' to type 'NSArray'
///
/// Bridging from `Array` to `NSArray` takes O(1) time and O(1) space if the
/// array's elements are already instances of a class or an `@objc` protocol;
/// otherwise, it takes O(*n*) time and space.
///
/// When the destination array's element type is a class or an `@objc`
/// protocol, bridging from `NSArray` to `Array` first calls the `copy(with:)`
/// (`- copyWithZone:` in Objective-C) method on the array to get an immutable
/// copy and then performs additional Swift bookkeeping work that takes O(1)
/// time. For instances of `NSArray` that are already immutable, `copy(with:)`
/// usually returns the same array in O(1) time; otherwise, the copying
/// performance is unspecified. If `copy(with:)` returns the same array, the
/// instances of `NSArray` and `Array` share storage using the same
/// copy-on-write optimization that is used when two instances of `Array`
/// share storage.
///
/// When the destination array's element type is a nonclass type that bridges
/// to a Foundation type, bridging from `NSArray` to `Array` performs a
/// bridging copy of the elements to contiguous storage in O(*n*) time. For
/// example, bridging from `NSArray` to `Array<Int>` performs such a copy. No
/// further bridging is required when accessing elements of the `Array`
/// instance.
///
/// - Note: The `ContiguousArray` and `ArraySlice` types are not bridged;
///   instances of those types always have a contiguous block of memory as
///   their storage.
@_fixed_layout
public struct Array<Element>: _DestructorSafeContainer {
  #if _runtime(_ObjC)
    internal typealias _Buffer = _ArrayBuffer<Element>
  #else
    internal typealias _Buffer = _ContiguousArrayBuffer<Element>
  #endif

  @usableFromInline
  internal var _buffer: _Buffer

  /// Initialization from an existing buffer does not have "array.init"
  /// semantics because the caller may retain an alias to buffer.
  @inlinable
  internal init(_buffer: _Buffer) {
    self._buffer = _buffer
  }

}

extension Array: RandomAccessCollection, MutableCollection {
  /// The index type for arrays, `Int`.
  public typealias Index = Int

  /// The type that represents the indices that are valid for subscripting an
  /// array, in ascending order.
  public typealias Indices = Range<Int>

  /// The type that allows iteration over an array's elements.
  public typealias Iterator = IndexingIterator<Array>

  /// The position of the first element in a nonempty array.
  ///
  /// For an instance of `Array`, `startIndex` is always zero. If the array
  /// is empty, `startIndex` is equal to `endIndex`.
  @inlinable
  public var startIndex: Int {
    return 0
  }

  /// The array's "past the end" position---that is, the position one greater
  /// than the last valid subscript argument.
  ///
  /// When you need a range that includes the last element of an array, use the
  /// half-open range operator (`..<`) with `endIndex`. The `..<` operator
  /// creates a range that doesn't include the upper bound, so it's always
  /// safe to use with `endIndex`. For example:
  ///
  ///     let numbers = [10, 20, 30, 40, 50]
  ///     if let i = numbers.firstIndex(of: 30) {
  ///         print(numbers[i ..< numbers.endIndex])
  ///     }
  ///     // Prints "[30, 40, 50]"
  ///
  /// If the array is empty, `endIndex` is equal to `startIndex`.
  @inlinable // FIXME(sil-serialize-all)
  public var endIndex: Int {
    @inlinable
    get {
      return _getCount()
    }
  }

  /// Returns the position immediately after the given index.
  ///
  /// - Parameter i: A valid index of the collection. `i` must be less than
  ///   `endIndex`.
  /// - Returns: The index immediately after `i`.
  @inlinable
  public func index(after i: Int) -> Int {
    // NOTE: this is a manual specialization of index movement for a Strideable
    // index that is required for Array performance.  The optimizer is not
    // capable of creating partial specializations yet.
    // NOTE: Range checks are not performed here, because it is done later by
    // the subscript function.
    return i + 1
  }

  /// Replaces the given index with its successor.
  ///
  /// - Parameter i: A valid index of the collection. `i` must be less than
  ///   `endIndex`.
  @inlinable
  public func formIndex(after i: inout Int) {
    // NOTE: this is a manual specialization of index movement for a Strideable
    // index that is required for Array performance.  The optimizer is not
    // capable of creating partial specializations yet.
    // NOTE: Range checks are not performed here, because it is done later by
    // the subscript function.
    i += 1
  }

  /// Returns the position immediately before the given index.
  ///
  /// - Parameter i: A valid index of the collection. `i` must be greater than
  ///   `startIndex`.
  /// - Returns: The index immediately before `i`.
  @inlinable
  public func index(before i: Int) -> Int {
    // NOTE: this is a manual specialization of index movement for a Strideable
    // index that is required for Array performance.  The optimizer is not
    // capable of creating partial specializations yet.
    // NOTE: Range checks are not performed here, because it is done later by
    // the subscript function.
    return i - 1
  }

  /// Replaces the given index with its predecessor.
  ///
  /// - Parameter i: A valid index of the collection. `i` must be greater than
  ///   `startIndex`.
  @inlinable
  public func formIndex(before i: inout Int) {
    // NOTE: this is a manual specialization of index movement for a Strideable
    // index that is required for Array performance.  The optimizer is not
    // capable of creating partial specializations yet.
    // NOTE: Range checks are not performed here, because it is done later by
    // the subscript function.
    i -= 1
  }

  /// Returns an index that is the specified distance from the given index.
  ///
  /// The following example obtains an index advanced four positions from an
  /// array's starting index and then prints the element at that position.
  ///
  ///     let numbers = [10, 20, 30, 40, 50]
  ///     let i = numbers.index(numbers.startIndex, offsetBy: 4)
  ///     print(numbers[i])
  ///     // Prints "50"
  ///
  /// The value passed as `n` must not offset `i` beyond the bounds of the
  /// collection.
  ///
  /// - Parameters:
  ///   - i: A valid index of the array.
  ///   - n: The distance to offset `i`.
  /// - Returns: An index offset by `n` from the index `i`. If `n` is positive,
  ///   this is the same value as the result of `n` calls to `index(after:)`.
  ///   If `n` is negative, this is the same value as the result of `-n` calls
  ///   to `index(before:)`.
  @inlinable
  public func index(_ i: Int, offsetBy n: Int) -> Int {
    // NOTE: this is a manual specialization of index movement for a Strideable
    // index that is required for Array performance.  The optimizer is not
    // capable of creating partial specializations yet.
    // NOTE: Range checks are not performed here, because it is done later by
    // the subscript function.
    return i + n
  }

  /// Returns an index that is the specified distance from the given index,
  /// unless that distance is beyond a given limiting index.
  ///
  /// The following example obtains an index advanced four positions from an
  /// array's starting index and then prints the element at that position. The
  /// operation doesn't require going beyond the limiting `numbers.endIndex`
  /// value, so it succeeds.
  ///
  ///     let numbers = [10, 20, 30, 40, 50]
  ///     if let i = numbers.index(numbers.startIndex,
  ///                              offsetBy: 4,
  ///                              limitedBy: numbers.endIndex) {
  ///         print(numbers[i])
  ///     }
  ///     // Prints "50"
  ///
  /// The next example attempts to retrieve an index ten positions from
  /// `numbers.startIndex`, but fails, because that distance is beyond the
  /// index passed as `limit`.
  ///
  ///     let j = numbers.index(numbers.startIndex,
  ///                           offsetBy: 10,
  ///                           limitedBy: numbers.endIndex)
  ///     print(j)
  ///     // Prints "nil"
  ///
  /// The value passed as `n` must not offset `i` beyond the bounds of the
  /// collection, unless the index passed as `limit` prevents offsetting
  /// beyond those bounds.
  ///
  /// - Parameters:
  ///   - i: A valid index of the array.
  ///   - n: The distance to offset `i`.
  ///   - limit: A valid index of the collection to use as a limit. If `n > 0`,
  ///     `limit` has no effect if it is less than `i`. Likewise, if `n < 0`,
  ///     `limit` has no effect if it is greater than `i`.
  /// - Returns: An index offset by `n` from the index `i`, unless that index
  ///   would be beyond `limit` in the direction of movement. In that case,
  ///   the method returns `nil`.
  @inlinable
  public func index(
    _ i: Int, offsetBy n: Int, limitedBy limit: Int
  ) -> Int? {
    // NOTE: this is a manual specialization of index movement for a Strideable
    // index that is required for Array performance.  The optimizer is not
    // capable of creating partial specializations yet.
    // NOTE: Range checks are not performed here, because it is done later by
    // the subscript function.
    let l = limit - i
    if n > 0 ? l >= 0 && l < n : l <= 0 && n < l {
      return nil
    }
    return i + n
  }

  /// Returns the distance between two indices.
  ///
  /// - Parameters:
  ///   - start: A valid index of the collection.
  ///   - end: Another valid index of the collection. If `end` is equal to
  ///     `start`, the result is zero.
  /// - Returns: The distance between `start` and `end`.
  @inlinable
  public func distance(from start: Int, to end: Int) -> Int {
    // NOTE: this is a manual specialization of index movement for a Strideable
    // index that is required for Array performance.  The optimizer is not
    // capable of creating partial specializations yet.
    // NOTE: Range checks are not performed here, because it is done later by
    // the subscript function.
    return end - start
  }

  @inlinable
  public func _failEarlyRangeCheck(_ index: Int, bounds: Range<Int>) {
    // NOTE: This method is a no-op for performance reasons.
  }

  @inlinable
  public func _failEarlyRangeCheck(_ range: Range<Int>, bounds: Range<Int>) {
    // NOTE: This method is a no-op for performance reasons.
  }

  /// Accesses the element at the specified position.
  ///
  /// The following example uses indexed subscripting to update an array's
  /// second element. After assigning the new value (`"Butler"`) at a specific
  /// position, that value is immediately available at that same position.
  ///
  ///     var streets = ["Adams", "Bryant", "Channing", "Douglas", "Evarts"]
  ///     streets[1] = "Butler"
  ///     print(streets[1])
  ///     // Prints "Butler"
  ///
  /// - Parameter index: The position of the element to access. `index` must be
  ///   greater than or equal to `startIndex` and less than `endIndex`.
  ///
  /// - Complexity: Reading an element from an array is O(1). Writing is O(1)
  ///   unless the array's storage is shared with another array, in which case
  ///   writing is O(*n*), where *n* is the length of the array.
  ///   If the array uses a bridged `NSArray` instance as its storage, the
  ///   efficiency is unspecified.
  @inlinable
  public subscript(index: Int) -> Element {
    get {
      // This call may be hoisted or eliminated by the optimizer.  If
      // there is an inout violation, this value may be stale so needs to be
      // checked again below.
      let wasNativeTypeChecked = _hoistableIsNativeTypeChecked()

      // Make sure the index is in range and wasNativeTypeChecked is
      // still valid.
      let token = _checkSubscript(
        index, wasNativeTypeChecked: wasNativeTypeChecked)

      return _getElement(
        index, wasNativeTypeChecked: wasNativeTypeChecked,
        matchingSubscriptCheck: token)
    }
    mutableAddressWithPinnedNativeOwner {
      _makeMutableAndUniqueOrPinned() // makes the array native, too
      _checkSubscript_native(index)
      return (_getElementAddress(index), Builtin.tryPin(_getOwner_native()))
    }
  }

  /// Accesses a contiguous subrange of the array's elements.
  ///
  /// The returned `ArraySlice` instance uses the same indices for the same
  /// elements as the original array. In particular, that slice, unlike an
  /// array, may have a nonzero `startIndex` and an `endIndex` that is not
  /// equal to `count`. Always use the slice's `startIndex` and `endIndex`
  /// properties instead of assuming that its indices start or end at a
  /// particular value.
  ///
  /// This example demonstrates getting a slice of an array of strings, finding
  /// the index of one of the strings in the slice, and then using that index
  /// in the original array.
  ///
  ///     let streets = ["Adams", "Bryant", "Channing", "Douglas", "Evarts"]
  ///     let streetsSlice = streets[2 ..< streets.endIndex]
  ///     print(streetsSlice)
  ///     // Prints "["Channing", "Douglas", "Evarts"]"
  ///
  ///     let i = streetsSlice.firstIndex(of: "Evarts")    // 4
  ///     print(streets[i!])
  ///     // Prints "Evarts"
  ///
  /// - Parameter bounds: A range of integers. The bounds of the range must be
  ///   valid indices of the array.
  @inlinable
  public subscript(bounds: Range<Int>) -> ArraySlice<Element> {
    get {
      _checkIndex(bounds.lowerBound)
      _checkIndex(bounds.upperBound)
      return ArraySlice(_buffer: _buffer[bounds])
    }
    set(rhs) {
      _checkIndex(bounds.lowerBound)
      _checkIndex(bounds.upperBound)
      // If the replacement buffer has same identity, and the ranges match,
      // then this was a pinned in-place modification, nothing further needed.
      if self[bounds]._buffer.identity != rhs._buffer.identity
      || bounds != rhs.startIndex..<rhs.endIndex {
        self.replaceSubrange(bounds, with: rhs)
      }
    }
  }
}

//===--- private helpers---------------------------------------------------===//
extension Array {
  /// Returns `true` if the array is native and does not need a deferred
  /// type check.  May be hoisted by the optimizer, which means its
  /// results may be stale by the time they are used if there is an
  /// inout violation in user code.
  @inlinable
  @_semantics("array.props.isNativeTypeChecked")
  public // @testable
  func _hoistableIsNativeTypeChecked() -> Bool {
   return _buffer.arrayPropertyIsNativeTypeChecked
  }

  @inlinable
  @_semantics("array.get_count")
  internal func _getCount() -> Int {
    return _buffer.count
  }

  @inlinable
  @_semantics("array.get_capacity")
  internal func _getCapacity() -> Int {
    return _buffer.capacity
  }

  /// - Precondition: The array has a native buffer.
  @inlinable
  @_semantics("array.owner")
  internal func _getOwnerWithSemanticLabel_native() -> Builtin.NativeObject {
    return Builtin.unsafeCastToNativeObject(_buffer.nativeOwner)
  }

  /// - Precondition: The array has a native buffer.
  @inlinable
  @inline(__always)
  internal func _getOwner_native() -> Builtin.NativeObject {
#if _runtime(_ObjC)
    if _isClassOrObjCExistential(Element.self) {
      // We are hiding the access to '_buffer.owner' behind
      // _getOwner() to help the compiler hoist uniqueness checks in
      // the case of class or Objective-C existential typed array
      // elements.
      return _getOwnerWithSemanticLabel_native()
    }
#endif
    // In the value typed case the extra call to
    // _getOwnerWithSemanticLabel_native hinders optimization.
    return Builtin.unsafeCastToNativeObject(_buffer.owner)
  }

  @inlinable
  @_semantics("array.make_mutable")
  internal mutating func _makeMutableAndUnique() {
    if _slowPath(!_buffer.isMutableAndUniquelyReferenced()) {
      _buffer = _Buffer(copying: _buffer)
    }
  }

  @inlinable
  @_semantics("array.make_mutable")
  internal mutating func _makeMutableAndUniqueOrPinned() {
    if _slowPath(!_buffer.isMutableAndUniquelyReferencedOrPinned()) {
      _buffer = _Buffer(copying: _buffer)
    }
  }


  /// Check that the given `index` is valid for subscripting, i.e.
  /// `0 ≤ index < count`.
  @inlinable
  @inline(__always)
  internal func _checkSubscript_native(_ index: Int) {
    _ = _checkSubscript(index, wasNativeTypeChecked: true)
  }

  /// Check that the given `index` is valid for subscripting, i.e.
  /// `0 ≤ index < count`.
  @inlinable
  @_semantics("array.check_subscript")
  public // @testable
  func _checkSubscript(
    _ index: Int, wasNativeTypeChecked: Bool
  ) -> _DependenceToken {
#if _runtime(_ObjC)
    _buffer._checkInoutAndNativeTypeCheckedBounds(
      index, wasNativeTypeChecked: wasNativeTypeChecked)
#else
    _buffer._checkValidSubscript(index)
#endif
    return _DependenceToken()
  }

  /// Check that the specified `index` is valid, i.e. `0 ≤ index ≤ count`.
  @inlinable
  @_semantics("array.check_index")
  internal func _checkIndex(_ index: Int) {
    _precondition(index <= endIndex, "Array index is out of range")
    _precondition(index >= startIndex, "Negative Array index is out of range")
  }

  @_semantics("array.get_element")
  @inline(__always)
  public // @testable
  func _getElement(
    _ index: Int,
    wasNativeTypeChecked: Bool,
    matchingSubscriptCheck: _DependenceToken
  ) -> Element {
#if _runtime(_ObjC)
    return _buffer.getElement(index, wasNativeTypeChecked: wasNativeTypeChecked)
#else
    return _buffer.getElement(index)
#endif
  }

  @inlinable
  @_semantics("array.get_element_address")
  internal func _getElementAddress(_ index: Int) -> UnsafeMutablePointer<Element> {
    return _buffer.subscriptBaseAddress + index
  }
}

extension Array: ExpressibleByArrayLiteral {
  // Optimized implementation for Array
  /// Creates an array from the given array literal.
  ///
  /// Do not call this initializer directly. It is used by the compiler
  /// when you use an array literal. Instead, create a new array by using an
  /// array literal as its value. To do this, enclose a comma-separated list of
  /// values in square brackets.
  ///
  /// Here, an array of strings is created from an array literal holding
  /// only strings.
  ///
  ///     let ingredients = ["cocoa beans", "sugar", "cocoa butter", "salt"]
  ///
  /// - Parameter elements: A variadic list of elements of the new array.
  @inlinable
  public init(arrayLiteral elements: Element...) {
    self = elements
  }
}

extension Array: RangeReplaceableCollection, ArrayProtocol {
  /// Creates a new, empty array.
  ///
  /// This is equivalent to initializing with an empty array literal.
  /// For example:
  ///
  ///     var emptyArray = Array<Int>()
  ///     print(emptyArray.isEmpty)
  ///     // Prints "true"
  ///
  ///     emptyArray = []
  ///     print(emptyArray.isEmpty)
  ///     // Prints "true"
  @inlinable
  @_semantics("array.init")
  public init() {
    _buffer = _Buffer()
  }

  /// Creates an array containing the elements of a sequence.
  ///
  /// You can use this initializer to create an array from any other type that
  /// conforms to the `Sequence` protocol. For example, you might want to
  /// create an array with the integers from 1 through 7. Use this initializer
  /// around a range instead of typing all those numbers in an array literal.
  ///
  ///     let numbers = Array(1...7)
  ///     print(numbers)
  ///     // Prints "[1, 2, 3, 4, 5, 6, 7]"
  ///
  /// You can also use this initializer to convert a complex sequence or
  /// collection type back to an array. For example, the `keys` property of
  /// a dictionary isn't an array with its own storage, it's a collection
  /// that maps its elements from the dictionary only when they're
  /// accessed, saving the time and space needed to allocate an array. If
  /// you need to pass those keys to a method that takes an array, however,
  /// use this initializer to convert that list from its type of
  /// `LazyMapCollection<Dictionary<String, Int>, Int>` to a simple
  /// `[String]`.
  ///
  ///     func cacheImagesWithNames(names: [String]) {
  ///         // custom image loading and caching
  ///      }
  ///
  ///     let namedHues: [String: Int] = ["Vermillion": 18, "Magenta": 302,
  ///             "Gold": 50, "Cerise": 320]
  ///     let colorNames = Array(namedHues.keys)
  ///     cacheImagesWithNames(colorNames)
  ///
  ///     print(colorNames)
  ///     // Prints "["Gold", "Cerise", "Magenta", "Vermillion"]"
  ///
  /// - Parameter s: The sequence of elements to turn into an array.
  @inlinable
  public init<S: Sequence>(_ s: S)
    where S.Element == Element {

    self = Array(
      _buffer: _Buffer(
        _buffer: s._copyToContiguousArray()._buffer,
        shiftedToStartIndex: 0))
  }

  /// Creates a new array containing the specified number of a single, repeated
  /// value.
  ///
  /// Here's an example of creating an array initialized with five strings
  /// containing the letter *Z*.
  ///
  ///     let fiveZs = Array(repeating: "Z", count: 5)
  ///     print(fiveZs)
  ///     // Prints "["Z", "Z", "Z", "Z", "Z"]"
  ///
  /// - Parameters:
  ///   - repeatedValue: The element to repeat.
  ///   - count: The number of times to repeat the value passed in the
  ///     `repeating` parameter. `count` must be zero or greater.
  @inlinable
  @_semantics("array.init")
  public init(repeating repeatedValue: Element, count: Int) {
    var p: UnsafeMutablePointer<Element>
    (self, p) = Array._allocateUninitialized(count)
    for _ in 0..<count {
      p.initialize(to: repeatedValue)
      p += 1
    }
  }

  @inline(never)
  @usableFromInline
  internal static func _allocateBufferUninitialized(
    minimumCapacity: Int
  ) -> _Buffer {
    let newBuffer = _ContiguousArrayBuffer<Element>(
      _uninitializedCount: 0, minimumCapacity: minimumCapacity)
    return _Buffer(_buffer: newBuffer, shiftedToStartIndex: 0)
  }

  /// Construct a Array of `count` uninitialized elements.
  @inlinable
  internal init(_uninitializedCount count: Int) {
    _precondition(count >= 0, "Can't construct Array with count < 0")
    // Note: Sinking this constructor into an else branch below causes an extra
    // Retain/Release.
    _buffer = _Buffer()
    if count > 0 {
      // Creating a buffer instead of calling reserveCapacity saves doing an
      // unnecessary uniqueness check. We disable inlining here to curb code
      // growth.
      _buffer = Array._allocateBufferUninitialized(minimumCapacity: count)
      _buffer.count = count
    }
    // Can't store count here because the buffer might be pointing to the
    // shared empty array.
  }

  /// Entry point for `Array` literal construction; builds and returns
  /// a Array of `count` uninitialized elements.
  @inlinable
  @_semantics("array.uninitialized")
  internal static func _allocateUninitialized(
    _ count: Int
  ) -> (Array, UnsafeMutablePointer<Element>) {
    let result = Array(_uninitializedCount: count)
    return (result, result._buffer.firstElementAddress)
  }


  /// Returns an Array of `count` uninitialized elements using the
  /// given `storage`, and a pointer to uninitialized memory for the
  /// first element.
  ///
  /// - Precondition: `storage is _ContiguousArrayStorage`.
  @inlinable
  @_semantics("array.uninitialized")
  internal static func _adoptStorage(
    _ storage: __owned _ContiguousArrayStorage<Element>, count: Int
  ) -> (Array, UnsafeMutablePointer<Element>) {

    let innerBuffer = _ContiguousArrayBuffer<Element>(
      count: count,
      storage: storage)

    return (
      Array(
        _buffer: _Buffer(_buffer: innerBuffer, shiftedToStartIndex: 0)),
        innerBuffer.firstElementAddress)
  }

  /// Entry point for aborting literal construction: deallocates
  /// a Array containing only uninitialized elements.
  @inlinable
  internal mutating func _deallocateUninitialized() {
    // Set the count to zero and just release as normal.
    // Somewhat of a hack.
    _buffer.count = 0
  }

  /// The number of elements in the array.
  @inlinable
  public var count: Int {
    return _getCount()
  }

  /// The total number of elements that the array can contain without
  /// allocating new storage.
  ///
  /// Every array reserves a specific amount of memory to hold its contents.
  /// When you add elements to an array and that array begins to exceed its
  /// reserved capacity, the array allocates a larger region of memory and
  /// copies its elements into the new storage. The new storage is a multiple
  /// of the old storage's size. This exponential growth strategy means that
  /// appending an element happens in constant time, averaging the performance
  /// of many append operations. Append operations that trigger reallocation
  /// have a performance cost, but they occur less and less often as the array
  /// grows larger.
  ///
  /// The following example creates an array of integers from an array literal,
  /// then appends the elements of another collection. Before appending, the
  /// array allocates new storage that is large enough store the resulting
  /// elements.
  ///
  ///     var numbers = [10, 20, 30, 40, 50]
  ///     // numbers.count == 5
  ///     // numbers.capacity == 5
  ///
  ///     numbers.append(contentsOf: stride(from: 60, through: 100, by: 10))
  ///     // numbers.count == 10
  ///     // numbers.capacity == 12
  @inlinable
  public var capacity: Int {
    return _getCapacity()
  }

  /// An object that guarantees the lifetime of this array's elements.
  @inlinable
  public // @testable
  var _owner: AnyObject? {
    return _buffer.owner
  }

  /// If the elements are stored contiguously, a pointer to the first
  /// element. Otherwise, `nil`.
  @inlinable
  public var _baseAddressIfContiguous: UnsafeMutablePointer<Element>? {
    @inline(__always) // FIXME(TODO: JIRA): Hack around test failure
    get { return _buffer.firstElementAddressIfContiguous }
  }

  //===--- basic mutations ------------------------------------------------===//


  /// Reserves enough space to store the specified number of elements.
  ///
  /// If you are adding a known number of elements to an array, use this method
  /// to avoid multiple reallocations. This method ensures that the array has
  /// unique, mutable, contiguous storage, with space allocated for at least
  /// the requested number of elements.
  ///
  /// Calling the `reserveCapacity(_:)` method on an array with bridged storage
  /// triggers a copy to contiguous storage even if the existing storage
  /// has room to store `minimumCapacity` elements.
  ///
  /// For performance reasons, the size of the newly allocated storage might be
  /// greater than the requested capacity. Use the array's `capacity` property
  /// to determine the size of the new storage.
  ///
  /// Preserving an Array's Geometric Growth Strategy
  /// ===============================================
  ///
  /// If you implement a custom data structure backed by an array that grows
  /// dynamically, naively calling the `reserveCapacity(_:)` method can lead
  /// to worse than expected performance. Arrays need to follow a geometric
  /// allocation pattern for appending elements to achieve amortized
  /// constant-time performance. The `Array` type's `append(_:)` and
  /// `append(contentsOf:)` methods take care of this detail for you, but
  /// `reserveCapacity(_:)` allocates only as much space as you tell it to
  /// (padded to a round value), and no more. This avoids over-allocation, but
  /// can result in insertion not having amortized constant-time performance.
  ///
  /// The following code declares `values`, an array of integers, and the
  /// `addTenQuadratic()` function, which adds ten more values to the `values`
  /// array on each call.
  ///
  ///       var values: [Int] = [0, 1, 2, 3]
  ///
  ///       // Don't use 'reserveCapacity(_:)' like this
  ///       func addTenQuadratic() {
  ///           let newCount = values.count + 10
  ///           values.reserveCapacity(newCount)
  ///           for n in values.count..<newCount {
  ///               values.append(n)
  ///           }
  ///       }
  ///
  /// The call to `reserveCapacity(_:)` increases the `values` array's capacity
  /// by exactly 10 elements on each pass through `addTenQuadratic()`, which
  /// is linear growth. Instead of having constant time when averaged over
  /// many calls, the function may decay to performance that is linear in
  /// `values.count`. This is almost certainly not what you want.
  ///
  /// In cases like this, the simplest fix is often to simply remove the call
  /// to `reserveCapacity(_:)`, and let the `append(_:)` method grow the array
  /// for you.
  ///
  ///       func addTen() {
  ///           let newCount = values.count + 10
  ///           for n in values.count..<newCount {
  ///               values.append(n)
  ///           }
  ///       }
  ///
  /// If you need more control over the capacity of your array, implement your
  /// own geometric growth strategy, passing the size you compute to
  /// `reserveCapacity(_:)`.
  ///
  /// - Parameter minimumCapacity: The requested number of elements to store.
  ///
  /// - Complexity: O(*n*), where *n* is the number of elements in the array.
  @inlinable
  @_semantics("array.mutate_unknown")
  public mutating func reserveCapacity(_ minimumCapacity: Int) {
    if _buffer.requestUniqueMutableBackingBuffer(
      minimumCapacity: minimumCapacity) == nil {

      let newBuffer = _ContiguousArrayBuffer<Element>(
        _uninitializedCount: count, minimumCapacity: minimumCapacity)

      _buffer._copyContents(
        subRange: _buffer.indices,
        initializing: newBuffer.firstElementAddress)
      _buffer = _Buffer(
        _buffer: newBuffer, shiftedToStartIndex: _buffer.startIndex)
    }
    _sanityCheck(capacity >= minimumCapacity)
  }

  /// Copy the contents of the current buffer to a new unique mutable buffer.
  /// The count of the new buffer is set to `oldCount`, the capacity of the
  /// new buffer is big enough to hold 'oldCount' + 1 elements.
  @inline(never)
  @inlinable // @specializable
  internal mutating func _copyToNewBuffer(oldCount: Int) {
    let newCount = oldCount + 1
    var newBuffer = _buffer._forceCreateUniqueMutableBuffer(
      countForNewBuffer: oldCount, minNewCapacity: newCount)
    _buffer._arrayOutOfPlaceUpdate(
      &newBuffer, oldCount, 0, _IgnorePointer())
  }

  @inlinable
  @_semantics("array.make_mutable")
  internal mutating func _makeUniqueAndReserveCapacityIfNotUnique() {
    if _slowPath(!_buffer.isMutableAndUniquelyReferenced()) {
      _copyToNewBuffer(oldCount: _buffer.count)
    }
  }

  @inlinable
  @_semantics("array.mutate_unknown")
  internal mutating func _reserveCapacityAssumingUniqueBuffer(oldCount: Int) {
    // This is a performance optimization. This code used to be in an ||
    // statement in the _sanityCheck below.
    //
    //   _sanityCheck(_buffer.capacity == 0 ||
    //                _buffer.isMutableAndUniquelyReferenced())
    //
    // SR-6437
    let capacity = _buffer.capacity == 0

    // Due to make_mutable hoisting the situation can arise where we hoist
    // _makeMutableAndUnique out of loop and use it to replace
    // _makeUniqueAndReserveCapacityIfNotUnique that preceeds this call. If the
    // array was empty _makeMutableAndUnique does not replace the empty array
    // buffer by a unique buffer (it just replaces it by the empty array
    // singleton).
    // This specific case is okay because we will make the buffer unique in this
    // function because we request a capacity > 0 and therefore _copyToNewBuffer
    // will be called creating a new buffer.
    _sanityCheck(capacity ||
                 _buffer.isMutableAndUniquelyReferenced())

    if _slowPath(oldCount + 1 > _buffer.capacity) {
      _copyToNewBuffer(oldCount: oldCount)
    }
  }

  @inlinable
  @_semantics("array.mutate_unknown")
  internal mutating func _appendElementAssumeUniqueAndCapacity(
    _ oldCount: Int,
    newElement: Element
  ) {
    _sanityCheck(_buffer.isMutableAndUniquelyReferenced())
    _sanityCheck(_buffer.capacity >= _buffer.count + 1)

    _buffer.count = oldCount + 1
    (_buffer.firstElementAddress + oldCount).initialize(to: newElement)
  }

  /// Adds a new element at the end of the array.
  ///
  /// Use this method to append a single element to the end of a mutable array.
  ///
  ///     var numbers = [1, 2, 3, 4, 5]
  ///     numbers.append(100)
  ///     print(numbers)
  ///     // Prints "[1, 2, 3, 4, 5, 100]"
  ///
  /// Because arrays increase their allocated capacity using an exponential
  /// strategy, appending a single element to an array is an O(1) operation
  /// when averaged over many calls to the `append(_:)` method. When an array
  /// has additional capacity and is not sharing its storage with another
  /// instance, appending an element is O(1). When an array needs to
  /// reallocate storage before appending or its storage is shared with
  /// another copy, appending is O(*n*), where *n* is the length of the array.
  ///
  /// - Parameter newElement: The element to append to the array.
  ///
  /// - Complexity: Amortized O(1) over many additions. If the array uses a
  ///   bridged `NSArray` instance as its storage, the efficiency is
  ///   unspecified.
  @inlinable
  @_semantics("array.append_element")
  public mutating func append(_ newElement: Element) {
    _makeUniqueAndReserveCapacityIfNotUnique()
    let oldCount = _getCount()
    _reserveCapacityAssumingUniqueBuffer(oldCount: oldCount)
    _appendElementAssumeUniqueAndCapacity(oldCount, newElement: newElement)
  }

  /// Adds the elements of a sequence to the end of the array.
  ///
  /// Use this method to append the elements of a sequence to the end of this
  /// array. This example appends the elements of a `Range<Int>` instance
  /// to an array of integers.
  ///
  ///     var numbers = [1, 2, 3, 4, 5]
  ///     numbers.append(contentsOf: 10...15)
  ///     print(numbers)
  ///     // Prints "[1, 2, 3, 4, 5, 10, 11, 12, 13, 14, 15]"
  ///
  /// - Parameter newElements: The elements to append to the array.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the resulting array.
  @inlinable
  @_semantics("array.append_contentsOf")
  public mutating func append<S: Sequence>(contentsOf newElements: S)
    where S.Element == Element {

    let newElementsCount = newElements.underestimatedCount
    reserveCapacityForAppend(newElementsCount: newElementsCount)

    let oldCount = self.count
    let startNewElements = _buffer.firstElementAddress + oldCount
    let buf = UnsafeMutableBufferPointer(
                start: startNewElements, 
                count: self.capacity - oldCount)

    let (remainder,writtenUpTo) = buf.initialize(from: newElements)
    
    // trap on underflow from the sequence's underestimate:
    let writtenCount = buf.distance(from: buf.startIndex, to: writtenUpTo)
    _precondition(newElementsCount <= writtenCount, 
      "newElements.underestimatedCount was an overestimate")
    // can't check for overflow as sequences can underestimate

    _buffer.count += writtenCount

    if writtenUpTo == buf.endIndex {
      // there may be elements that didn't fit in the existing buffer,
      // append them in slow sequence-only mode
      _buffer._arrayAppendSequence(IteratorSequence(remainder))
    }
  }

  @inlinable
  @_semantics("array.reserve_capacity_for_append")
  internal mutating func reserveCapacityForAppend(newElementsCount: Int) {
    let oldCount = self.count
    let oldCapacity = self.capacity
    let newCount = oldCount + newElementsCount

    // Ensure uniqueness, mutability, and sufficient storage.  Note that
    // for consistency, we need unique self even if newElements is empty.
    self.reserveCapacity(
      newCount > oldCapacity ?
      Swift.max(newCount, _growArrayCapacity(oldCapacity))
      : newCount)
  }

  @inlinable
  public mutating func _customRemoveLast() -> Element? {
    let newCount = _getCount() - 1
    _precondition(newCount >= 0, "Can't removeLast from an empty Array")
    _makeUniqueAndReserveCapacityIfNotUnique()
    let pointer = (_buffer.firstElementAddress + newCount)
    let element = pointer.move()
    _buffer.count = newCount
    return element
  }

  /// Removes and returns the element at the specified position.
  ///
  /// All the elements following the specified position are moved up to
  /// close the gap.
  ///
  ///     var measurements: [Double] = [1.1, 1.5, 2.9, 1.2, 1.5, 1.3, 1.2]
  ///     let removed = measurements.remove(at: 2)
  ///     print(measurements)
  ///     // Prints "[1.1, 1.5, 1.2, 1.5, 1.3, 1.2]"
  ///
  /// - Parameter index: The position of the element to remove. `index` must
  ///   be a valid index of the array.
  /// - Returns: The element at the specified index.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the array.
  @inlinable
  @discardableResult
  public mutating func remove(at index: Int) -> Element {
    _precondition(index < endIndex, "Index out of range")
    _precondition(index >= startIndex, "Index out of range")
    _makeUniqueAndReserveCapacityIfNotUnique()
    let newCount = _getCount() - 1
    let pointer = (_buffer.firstElementAddress + index)
    let result = pointer.move()
    pointer.moveInitialize(from: pointer + 1, count: newCount - index)
    _buffer.count = newCount
    return result
  }

  /// Inserts a new element at the specified position.
  ///
  /// The new element is inserted before the element currently at the specified
  /// index. If you pass the array's `endIndex` property as the `index`
  /// parameter, the new element is appended to the array.
  ///
  ///     var numbers = [1, 2, 3, 4, 5]
  ///     numbers.insert(100, at: 3)
  ///     numbers.insert(200, at: numbers.endIndex)
  ///
  ///     print(numbers)
  ///     // Prints "[1, 2, 3, 100, 4, 5, 200]"
  ///
  /// - Parameter newElement: The new element to insert into the array.
  /// - Parameter i: The position at which to insert the new element.
  ///   `index` must be a valid index of the array or equal to its `endIndex`
  ///   property.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the array.
  @inlinable
  public mutating func insert(_ newElement: Element, at i: Int) {
    _checkIndex(i)
    self.replaceSubrange(i..<i, with: CollectionOfOne(newElement))
  }

  /// Removes all elements from the array.
  ///
  /// - Parameter keepCapacity: Pass `true` to keep the existing capacity of
  ///   the array after removing its elements. The default value is
  ///   `false`.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the array.
  @inlinable
  public mutating func removeAll(keepingCapacity keepCapacity: Bool = false) {
    if !keepCapacity {
      _buffer = _Buffer()
    }
    else {
      self.replaceSubrange(indices, with: EmptyCollection())
    }
  }

  //===--- algorithms -----------------------------------------------------===//

  @inlinable
  public mutating func _withUnsafeMutableBufferPointerIfSupported<R>(
    _ body: (inout UnsafeMutableBufferPointer<Element>) throws -> R
  ) rethrows -> R? {
    return try withUnsafeMutableBufferPointer {
      (bufferPointer) -> R in
      return try body(&bufferPointer)
    }
  }

  @inlinable
  public func _copyToContiguousArray() -> ContiguousArray<Element> {
    if let n = _buffer.requestNativeBuffer() {
      return ContiguousArray(_buffer: n)
    }
    return _copyCollectionToContiguousArray(_buffer)
  }
}

extension Array: CustomReflectable {
  /// A mirror that reflects the array.
  public var customMirror: Mirror {
    return Mirror(
      self,
      unlabeledChildren: self,
      displayStyle: .collection)
  }
}

extension Array: CustomStringConvertible, CustomDebugStringConvertible {
  /// A textual representation of the array and its elements.
  public var description: String {
    return _makeCollectionDescription(for: self, withTypeName: nil)
  }

  /// A textual representation of the array and its elements, suitable for
  /// debugging.
  public var debugDescription: String {
    // Always show sugared representation for Arrays.
    return _makeCollectionDescription(for: self, withTypeName: nil)
  }
}

extension Array {
  @inlinable
  @_transparent
  internal func _cPointerArgs() -> (AnyObject?, UnsafeRawPointer?) {
    let p = _baseAddressIfContiguous
    if _fastPath(p != nil || isEmpty) {
      return (_owner, UnsafeRawPointer(p))
    }
    let n = ContiguousArray(self._buffer)._buffer
    return (n.owner, UnsafeRawPointer(n.firstElementAddress))
  }
}

extension Array {
  /// Calls a closure with a pointer to the array's contiguous storage.
  ///
  /// Often, the optimizer can eliminate bounds checks within an array
  /// algorithm, but when that fails, invoking the same algorithm on the
  /// buffer pointer passed into your closure lets you trade safety for speed.
  ///
  /// The following example shows how you can iterate over the contents of the
  /// buffer pointer:
  ///
  ///     let numbers = [1, 2, 3, 4, 5]
  ///     let sum = numbers.withUnsafeBufferPointer { buffer -> Int in
  ///         var result = 0
  ///         for i in stride(from: buffer.startIndex, to: buffer.endIndex, by: 2) {
  ///             result += buffer[i]
  ///         }
  ///         return result
  ///     }
  ///     // 'sum' == 9
  ///
  /// The pointer passed as an argument to `body` is valid only during the
  /// execution of `withUnsafeBufferPointer(_:)`. Do not store or return the
  /// pointer for later use.
  ///
  /// - Parameter body: A closure with an `UnsafeBufferPointer` parameter that
  ///   points to the contiguous storage for the array.  If no such storage exists, it is created. If
  ///   `body` has a return value, that value is also used as the return value
  ///   for the `withUnsafeBufferPointer(_:)` method. The pointer argument is
  ///   valid only for the duration of the method's execution.
  /// - Returns: The return value, if any, of the `body` closure parameter.
  @inlinable
  public func withUnsafeBufferPointer<R>(
    _ body: (UnsafeBufferPointer<Element>) throws -> R
  ) rethrows -> R {
    return try _buffer.withUnsafeBufferPointer(body)
  }

  /// Calls the given closure with a pointer to the array's mutable contiguous
  /// storage.
  ///
  /// Often, the optimizer can eliminate bounds checks within an array
  /// algorithm, but when that fails, invoking the same algorithm on the
  /// buffer pointer passed into your closure lets you trade safety for speed.
  ///
  /// The following example shows how modifying the contents of the
  /// `UnsafeMutableBufferPointer` argument to `body` alters the contents of
  /// the array:
  ///
  ///     var numbers = [1, 2, 3, 4, 5]
  ///     numbers.withUnsafeMutableBufferPointer { buffer in
  ///         for i in stride(from: buffer.startIndex, to: buffer.endIndex - 1, by: 2) {
  ///             buffer.swapAt(i, i + 1)
  ///         }
  ///     }
  ///     print(numbers)
  ///     // Prints "[2, 1, 4, 3, 5]"
  ///
  /// The pointer passed as an argument to `body` is valid only during the
  /// execution of `withUnsafeMutableBufferPointer(_:)`. Do not store or
  /// return the pointer for later use.
  ///
  /// - Warning: Do not rely on anything about the array that is the target of
  ///   this method during execution of the `body` closure; it might not
  ///   appear to have its correct value. Instead, use only the
  ///   `UnsafeMutableBufferPointer` argument to `body`.
  ///
  /// - Parameter body: A closure with an `UnsafeMutableBufferPointer`
  ///   parameter that points to the contiguous storage for the array.
  ///    If no such storage exists, it is created. If `body` has a return value, that value is also
  ///   used as the return value for the `withUnsafeMutableBufferPointer(_:)`
  ///   method. The pointer argument is valid only for the duration of the
  ///   method's execution.
  /// - Returns: The return value, if any, of the `body` closure parameter.
  @_semantics("array.withUnsafeMutableBufferPointer")
  @inline(__always) // Performance: This method should get inlined into the
  // caller such that we can combine the partial apply with the apply in this
  // function saving on allocating a closure context. This becomes unnecessary
  // once we allocate noescape closures on the stack.
  public mutating func withUnsafeMutableBufferPointer<R>(
    _ body: (inout UnsafeMutableBufferPointer<Element>) throws -> R
  ) rethrows -> R {
    let count = self.count
    // Ensure unique storage
    _buffer._outlinedMakeUniqueBuffer(bufferCount: count)

    // Ensure that body can't invalidate the storage or its bounds by
    // moving self into a temporary working array.
    // NOTE: The stack promotion optimization that keys of the
    // "array.withUnsafeMutableBufferPointer" semantics annotation relies on the
    // array buffer not being able to escape in the closure. It can do this
    // because we swap the array buffer in self with an empty buffer here. Any
    // escape via the address of self in the closure will therefore escape the
    // empty array.

    var work = Array()
    (work, self) = (self, work)

    // Create an UnsafeBufferPointer over work that we can pass to body
    let pointer = work._buffer.firstElementAddress
    var inoutBufferPointer = UnsafeMutableBufferPointer(
      start: pointer, count: count)

    // Put the working array back before returning.
    defer {
      _precondition(
        inoutBufferPointer.baseAddress == pointer &&
        inoutBufferPointer.count == count,
        "Array withUnsafeMutableBufferPointer: replacing the buffer is not allowed")

      (work, self) = (self, work)
    }

    // Invoke the body.
    return try body(&inoutBufferPointer)
  }

  @inlinable
  public func _copyContents(
    initializing buffer: UnsafeMutableBufferPointer<Element>
  ) -> (Iterator,UnsafeMutableBufferPointer<Element>.Index) {

    guard !self.isEmpty else { return (makeIterator(),buffer.startIndex) }

    // It is not OK for there to be no pointer/not enough space, as this is
    // a precondition and Array never lies about its count.
    guard var p = buffer.baseAddress
      else { _preconditionFailure("Attempt to copy contents into nil buffer pointer") }
    _precondition(self.count <= buffer.count, 
      "Insufficient space allocated to copy array contents")

    if let s = _baseAddressIfContiguous {
      p.initialize(from: s, count: self.count)
      // Need a _fixLifetime bracketing the _baseAddressIfContiguous getter
      // and all uses of the pointer it returns:
      _fixLifetime(self._owner)
    } else {
      for x in self {
        p.initialize(to: x)
        p += 1
      }
    }

    var it = IndexingIterator(_elements: self)
    it._position = endIndex
    return (it,buffer.index(buffer.startIndex, offsetBy: self.count))
  }
}

extension Array {
  /// Replaces a range of elements with the elements in the specified
  /// collection.
  ///
  /// This method has the effect of removing the specified range of elements
  /// from the array and inserting the new elements at the same location. The
  /// number of new elements need not match the number of elements being
  /// removed.
  ///
  /// In this example, three elements in the middle of an array of integers are
  /// replaced by the five elements of a `Repeated<Int>` instance.
  ///
  ///      var nums = [10, 20, 30, 40, 50]
  ///      nums.replaceSubrange(1...3, with: repeatElement(1, count: 5))
  ///      print(nums)
  ///      // Prints "[10, 1, 1, 1, 1, 1, 50]"
  ///
  /// If you pass a zero-length range as the `subrange` parameter, this method
  /// inserts the elements of `newElements` at `subrange.startIndex`. Calling
  /// the `insert(contentsOf:at:)` method instead is preferred.
  ///
  /// Likewise, if you pass a zero-length collection as the `newElements`
  /// parameter, this method removes the elements in the given subrange
  /// without replacement. Calling the `removeSubrange(_:)` method instead is
  /// preferred.
  ///
  /// - Parameters:
  ///   - subrange: The subrange of the array to replace. The start and end of
  ///     a subrange must be valid indices of the array.
  ///   - newElements: The new elements to add to the array.
  ///
  /// - Complexity: O(`subrange.count`) if you are replacing a suffix of the
  ///   array with an empty collection; otherwise, O(*n*), where *n* is the
  ///   length of the array.
  @inlinable
  @_semantics("array.mutate_unknown")
  public mutating func replaceSubrange<C>(
    _ subrange: Range<Int>,
    with newElements: C
  ) where C: Collection, C.Element == Element {
    _precondition(subrange.lowerBound >= self._buffer.startIndex,
      "Array replace: subrange start is negative")

    _precondition(subrange.upperBound <= _buffer.endIndex,
      "Array replace: subrange extends past the end")

    let oldCount = _buffer.count
    let eraseCount = subrange.count
    let insertCount = newElements.count
    let growth = insertCount - eraseCount

    if _buffer.requestUniqueMutableBackingBuffer(
      minimumCapacity: oldCount + growth) != nil {

      _buffer.replaceSubrange(
        subrange, with: insertCount, elementsOf: newElements)
    } else {
      _buffer._arrayOutOfPlaceReplace(subrange, with: newElements, count: insertCount)
    }
  }
}

extension Array: Equatable where Element: Equatable {
  /// Returns a Boolean value indicating whether two arrays contain the same
  /// elements in the same order.
  ///
  /// You can use the equal-to operator (`==`) to compare any two arrays
  /// that store the same, `Equatable`-conforming element type.
  ///
  /// - Parameters:
  ///   - lhs: An array to compare.
  ///   - rhs: Another array to compare.
  @inlinable
  public static func ==(lhs: Array<Element>, rhs: Array<Element>) -> Bool {
    let lhsCount = lhs.count
    if lhsCount != rhs.count {
      return false
    }

    // Test referential equality.
    if lhsCount == 0 || lhs._buffer.identity == rhs._buffer.identity {
      return true
    }


    _sanityCheck(lhs.startIndex == 0 && rhs.startIndex == 0)
    _sanityCheck(lhs.endIndex == lhsCount && rhs.endIndex == lhsCount)

    // We know that lhs.count == rhs.count, compare element wise.
    for idx in 0..<lhsCount {
      if lhs[idx] != rhs[idx] {
        return false
      }
    }

    return true
  }

  /// Returns a Boolean value indicating whether two arrays are not equal.
  ///
  /// Two arrays are equal if they contain the same elements in the same order.
  /// You can use the not-equal-to operator (`!=`) to compare any two arrays
  /// that store the same, `Equatable`-conforming element type.
  ///
  /// - Parameters:
  ///   - lhs: An array to compare.
  ///   - rhs: Another array to compare.
  @inlinable
  public static func !=(lhs: Array<Element>, rhs: Array<Element>) -> Bool {
    return !(lhs == rhs)
  }
}

extension Array: Hashable where Element: Hashable {
  /// Hashes the essential components of this value by feeding them into the
  /// given hasher.
  ///
  /// - Parameter hasher: The hasher to use when combining the components
  ///   of this instance.
  @inlinable
  public func hash(into hasher: inout Hasher) {
    hasher.combine(count) // discriminator
    for element in self {
      hasher.combine(element)
    }
  }
}

extension Array {
  /// Calls the given closure with a pointer to the underlying bytes of the
  /// array's mutable contiguous storage.
  ///
  /// The array's `Element` type must be a *trivial type*, which can be copied
  /// with just a bit-for-bit copy without any indirection or
  /// reference-counting operations. Generally, native Swift types that do not
  /// contain strong or weak references are trivial, as are imported C structs
  /// and enums.
  ///
  /// The following example copies bytes from the `byteValues` array into
  /// `numbers`, an array of `Int`:
  ///
  ///     var numbers: [Int32] = [0, 0]
  ///     var byteValues: [UInt8] = [0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00]
  ///
  ///     numbers.withUnsafeMutableBytes { destBytes in
  ///         byteValues.withUnsafeBytes { srcBytes in
  ///             destBytes.copyBytes(from: srcBytes)
  ///         }
  ///     }
  ///     // numbers == [1, 2]
  ///
  /// The pointer passed as an argument to `body` is valid only for the
  /// lifetime of the closure. Do not escape it from the closure for later
  /// use.
  ///
  /// - Warning: Do not rely on anything about the array that is the target of
  ///   this method during execution of the `body` closure; it might not
  ///   appear to have its correct value. Instead, use only the
  ///   `UnsafeMutableRawBufferPointer` argument to `body`.
  ///
  /// - Parameter body: A closure with an `UnsafeMutableRawBufferPointer`
  ///   parameter that points to the contiguous storage for the array.
  ///    If no such storage exists, it is created. If `body` has a return value, that value is also
  ///   used as the return value for the `withUnsafeMutableBytes(_:)` method.
  ///   The argument is valid only for the duration of the closure's
  ///   execution.
  /// - Returns: The return value, if any, of the `body` closure parameter.
  @inlinable
  public mutating func withUnsafeMutableBytes<R>(
    _ body: (UnsafeMutableRawBufferPointer) throws -> R
  ) rethrows -> R {
    return try self.withUnsafeMutableBufferPointer {
      return try body(UnsafeMutableRawBufferPointer($0))
    }
  }

  /// Calls the given closure with a pointer to the underlying bytes of the
  /// array's contiguous storage.
  ///
  /// The array's `Element` type must be a *trivial type*, which can be copied
  /// with just a bit-for-bit copy without any indirection or
  /// reference-counting operations. Generally, native Swift types that do not
  /// contain strong or weak references are trivial, as are imported C structs
  /// and enums.
  ///
  /// The following example copies the bytes of the `numbers` array into a
  /// buffer of `UInt8`:
  ///
  ///     var numbers = [1, 2, 3]
  ///     var byteBuffer: [UInt8] = []
  ///     numbers.withUnsafeBytes {
  ///         byteBuffer.append(contentsOf: $0)
  ///     }
  ///     // byteBuffer == [1, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, ...]
  ///
  /// - Parameter body: A closure with an `UnsafeRawBufferPointer` parameter
  ///   that points to the contiguous storage for the array.
  ///    If no such storage exists, it is created. If `body` has a return value, that value is also
  ///   used as the return value for the `withUnsafeBytes(_:)` method. The
  ///   argument is valid only for the duration of the closure's execution.
  /// - Returns: The return value, if any, of the `body` closure parameter.
  @inlinable
  public func withUnsafeBytes<R>(
    _ body: (UnsafeRawBufferPointer) throws -> R
  ) rethrows -> R {
    return try self.withUnsafeBufferPointer {
      try body(UnsafeRawBufferPointer($0))
    }
  }
}

#if _runtime(_ObjC)
// We isolate the bridging of the Cocoa Array -> Swift Array here so that
// in the future, we can eagerly bridge the Cocoa array. We need this function
// to do the bridging in an ABI safe way. Even though this looks useless,
// DO NOT DELETE!
@usableFromInline internal
func _bridgeCocoaArray<T>(_ _immutableCocoaArray: _NSArrayCore) -> Array<T> {
  return Array(_buffer: _ArrayBuffer(nsArray: _immutableCocoaArray))
}

extension Array {
  @inlinable
  public // @SPI(Foundation)
  func _bridgeToObjectiveCImpl() -> AnyObject {
    return _buffer._asCocoaArray()
  }

  /// Tries to downcast the source `NSArray` as our native buffer type.
  /// If it succeeds, creates a new `Array` around it and returns that.
  /// Returns `nil` otherwise.
  // Note: this function exists here so that Foundation doesn't have
  // to know Array's implementation details.
  @inlinable
  public static func _bridgeFromObjectiveCAdoptingNativeStorageOf(
    _ source: AnyObject
  ) -> Array? {
    // If source is deferred, we indirect to get its native storage
    let maybeNative = (source as? _SwiftDeferredNSArray)?._nativeStorage ?? source

    return (maybeNative as? _ContiguousArrayStorage<Element>).map {
      Array(_ContiguousArrayBuffer($0))
    }
  }

  /// Private initializer used for bridging.
  ///
  /// Only use this initializer when both conditions are true:
  ///
  /// * it is statically known that the given `NSArray` is immutable;
  /// * `Element` is bridged verbatim to Objective-C (i.e.,
  ///   is a reference type).
  @inlinable
  public init(_immutableCocoaArray: _NSArrayCore) {
    self = _bridgeCocoaArray(_immutableCocoaArray)
  }
}
#endif
