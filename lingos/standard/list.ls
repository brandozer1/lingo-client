"Lists for a program: sorting, smallest first or records by one of their parts, reversing, adding up, removing repeats, flattening and finding an item. Built into the lingo client, like the other standard functions."
// 1. sort(list, part=nothing)   -> list: smallest first, records by part, equal ones kept in their order
// 2. reverse(x)                 -> list or text: last first
// 3. sum(list)                  -> number: the numbers added up, 0 for none
// 4. unique(list)               -> list: each item once, first seen first
// 5. flatten(list)              -> list: the items of each list inside it, in order, one level
// 6. index_of(list, item)       -> number: where the item first is, from 0, or nothing

////////// 1-2. ORDER //////////

fn sort(list, part=nothing) "A list sorted smallest first: numbers, or texts by character code, or records by one of their parts, like sort(rows, \"price\"). Equal ones keep the order they had. part is the part of each record to sort by, nothing to sort the items themselves. For largest first, reverse what it gives." {
  n = std.length(list)
  if n < 2 {return std.slice(list, 0)}
  h = (n - n % 2) / 2
  a = sort(std.slice(list, 0, h), part)
  b = sort(std.slice(list, h), part)
  out = []
  i = 0
  j = 0
  while i < h and j < n - h {
    x = part == nothing ? a[i] : a[i][part]
    y = part == nothing ? b[j] : b[j][part]
    if y < x {
      out += [b[j]]
      j += 1
    } else {
      out += [a[i]]
      i += 1
    }
  }
  return out + std.slice(a, i) + std.slice(b, j)
}

fn reverse(x) "A list with its last item first, or text with its last character first." {
  out = []
  for i in 1..std.length(x) {out += [x[-i]]}
  // Text is the one kind that is itself when written as text.
  if "{x}" != x {return out}
  t = ""
  for c in out {t += c}
  return t
}

////////// 3-6. WHAT IS IN IT //////////

fn sum(list) "The numbers of a list added up, 0 when it is empty." {
  total = 0
  for n in list {total += n}
  return total
}

fn unique(list) "A list with each item once, in the order each was first seen." {
  out = []
  for x in list {
    if not (x in out) {out += [x]}
  }
  return out
}

fn flatten(list) "A list with the items of each list inside it in its place, one level. An item that isn't a list stays as it is." {
  out = []
  for x in list {
    if json.kind(x) == "list" {out += x} else {out += [x]}
  }
  return out
}

fn index_of(list, item) "Where an item first is in a list, counting from 0, or nothing when it isn't there." {
  for i in 1..std.length(list) {
    if list[i - 1] == item {return i - 1}
  }
  return nothing
}
