// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/secret)
// RUN: %target-swift-frontend -emit-module -o %t/secret %S/Inputs/struct_with_operators.swift
// RUN: %empty-directory(%t/Frameworks/has_alias.framework/Modules/has_alias.swiftmodule)
// RUN: %target-swift-frontend -emit-module -o %t/Frameworks/has_alias.framework/Modules/has_alias.swiftmodule/%target-swiftmodule-name %S/Inputs/alias.swift -module-name has_alias

// Verify that compilation fails if we don't provide the dependencies' search
// paths, whether or not debugging options are serialized.
// RUN: %target-swift-frontend -emit-module -o %t -I %t/secret -F %t/Frameworks -Fsystem %t/SystemFrameworks -parse-as-library %S/Inputs/has_xref.swift
// RUN: %target-swift-frontend %s -typecheck -I %t -verify -show-diagnostics-after-fatal

// RUN: %target-swift-frontend -emit-module -o %t -I %t/secret -F %t/Frameworks -Fsystem %t/SystemFrameworks -parse-as-library %S/Inputs/has_xref.swift -serialize-debugging-options
// RUN: %target-swift-frontend %s -typecheck -I %t -verify -show-diagnostics-after-fatal

// Verify that compilation succeeds when we do provide the search paths for
// required modules of dependencies.
// RUN: %target-swift-frontend -emit-module -o %t -I %t/secret -F %t/Frameworks -Fsystem %t/SystemFrameworks -parse-as-library %S/Inputs/has_xref.swift
// RUN: %target-swift-frontend %s -typecheck -I %t -I %t/secret -F %t/Frameworks

// RUN: %target-swift-frontend -emit-module -o %t -I %t/secret -F %t/Frameworks -Fsystem %t/SystemFrameworks -parse-as-library %S/Inputs/has_xref.swift -serialize-debugging-options
// RUN: %target-swift-frontend %s -typecheck -I %t -I %t/secret -F %t/Frameworks

// Make sure we don't end up with duplicate search paths.
// RUN: %target-swiftc_driver -emit-module -o %t/has_xref.swiftmodule -I %t/secret -F %t/Frameworks -Fsystem %t/SystemFrameworks -parse-as-library %S/Inputs/has_xref.swift %S/../Inputs/empty.swift -Xfrontend -serialize-debugging-options
// RUN: %target-swift-frontend %s -typecheck -I %t -I %t/secret -F %t/Frameworks
// RUN: llvm-bcanalyzer -dump %t/has_xref.swiftmodule | %FileCheck %s

// RUN: %target-swift-frontend %s -emit-module -o %t/main.swiftmodule -I %t -I %t/secret -F %t/Frameworks -Fsystem %t/SystemFrameworks
// RUN: llvm-bcanalyzer -dump %t/main.swiftmodule | %FileCheck %s

import has_xref // expected-error {{missing required modules: 'has_alias', 'struct_with_operators'}}

numeric(42) // expected-error {{use of unresolved identifier 'numeric'}}

// CHECK: <INPUT_BLOCK
// CHECK-NOT: /secret'
// CHECK-NOT: /Frameworks'
// CHECK: <SEARCH_PATH abbrevid={{[0-9]+}} op0=1 op1=0/> blob data = '{{.+}}/Frameworks'
// CHECK-NOT: /secret'
// CHECK-NOT: /Frameworks'
// CHECK-NOT: /SystemFrameworks'
// CHECK: <SEARCH_PATH abbrevid={{[0-9]+}} op0=1 op1=1/> blob data = '{{.+}}/SystemFrameworks'
// CHECK-NOT: /secret'
// CHECK-NOT: /Frameworks'
// CHECK-NOT: /SystemFrameworks'
// CHECK: <SEARCH_PATH abbrevid={{[0-9]+}} op0=0 op1=0/> blob data = '{{.+}}/secret'
// CHECK-NOT: /secret'
// CHECK-NOT: /Frameworks'
// CHECK-NOT: /SystemFrameworks'
// CHECK: </INPUT_BLOCK>
