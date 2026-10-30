// math.ls - Numbers for a program: rounding to whole numbers or to places, up, down or to the nearest, and the
// smaller or larger of two.
//
// 1. round(n, places=0, mode="nearest")   -> number: mode nearest, up or down, places below 0 to tens, hundreds...
// 2. min(a, b)                            -> the smaller of two numbers, or the first of two texts
// 3. max(a, b)                            -> the larger of two numbers, or the last of two texts
// 4. abs(n)                               -> number: without its sign

////////// 1-4. NUMBERS //////////

// 1. A number rounded: to the nearest whole number, or always up or always down, like 2.5 to 3, 1.1 up to 2 or -1.5 down
// to -2. places is how many after the point, 2 for 1.25, or below 0 for tens and hundreds. mode is nearest, up or down,
// where nearest takes a half up.
fn round(n, places=0, mode="nearest") {
  if mode != "nearest" and mode != "up" and mode != "down" {fail "mode is nearest, up or down"}
  f = 1
  for i in 1..(places < 0 ? -places : places) {f *= 10}
  x = places < 0 ? n / f : n * f
  if mode == "nearest" {x += 0.5}
  if mode == "up" {x = -x}
  x -= x % 1
  if mode == "up" {x = -x}
  return places < 0 ? x * f : x / f
}

// 2. The smaller of two numbers, or of two texts the one first in order by character code.
fn min(a, b) {return a <= b ? a : b}

// 3. The larger of two numbers, or of two texts the one last in order by character code.
fn max(a, b) {return a >= b ? a : b}

// 4. A number without its sign, like -3 to 3.
fn abs(n) {return n < 0 ? -n : n}
