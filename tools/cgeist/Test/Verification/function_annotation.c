// RUN: cgeist %s --function=* -S | FileCheck %s

#define FOO __attribute__((annotate("FOO")))

FOO int foo(int a, int b) {
  return a + b;
}

// CHECK:   func.func @foo(%arg0: i32, %arg1: i32) -> i32