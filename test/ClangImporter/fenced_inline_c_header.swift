// RUN: %empty-directory(%t)
// TODO: Add imported symbols to the TBD file so we don't need the frontend flag
// RUN: %target-build-swift %s -o %t/a.out -Xfrontend -validate-tbd-against-ir=none
// RUN: %target-codesign %t/a.out
// RUN: %target-run %t/a.out | %FileCheck %s

// REQUIRES: executable_test

```c
typedef struct {
  int numerator;
  int denominator;
} Rational;

double RationalToDouble(Rational rational) {
  return (double)rational.numerator / rational.denominator;
}
```

let r = Rational(numerator: 3, denominator: 4)
print(RationalToDouble(r))

// CHECK: 0.75
