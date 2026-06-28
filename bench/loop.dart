// Identical algorithm to loop.aby — for a fair Abyss-vs-Dart comparison.
// Build AOT:  dart compile exe bench/loop.dart -o build/loop_dart
void main() {
  var x = 1;
  for (var i = 0; i < 100000000; i++) {
    x = (x * 1103515245 + 12345) % 2147483647;
  }
  print(x);
}
