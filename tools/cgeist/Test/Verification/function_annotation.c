// RUN: cgeist %s --function=* -S | FileCheck %s

#define FOO __attribute__((annotate("FOO")))

FOO void foo() {
  return;
}

// CHECK:   func.func @foo() attributes
// CHECK-SAME:  {annotations = ["FOO"], 
