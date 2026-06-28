// Identical algorithm to fib.aby — for a fair Abyss-vs-Dart comparison.
// Build AOT:  dart compile exe bench/fib.dart -o build/fib_dart
int fib(int n) {
  if (n < 2) return n;
  return fib(n - 1) + fib(n - 2);
}

void main() {
  print(fib(35));
}
