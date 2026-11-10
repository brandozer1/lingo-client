"Dates and times: the date and time now in words for a person, and reading, formatting, adding to and comparing times, how long ago, the start of a day and the day of the week. A time is seconds since 1970, as compute.now() gives the time now."
// 1. parse(text)                                              -> number?: 2026-10-18, or 2026-10-18T14:05:00Z, +02:00 or none
// 2. format(time, pattern="YYYY-MM-DDTHH:mm:ssZ", offset=0)   -> text: by the letters below
// 3. add(time, amount, unit="seconds")                        -> number: seconds to years, back when amount is below 0
// 4. difference(a, b, unit="seconds")                         -> number: from a to b, months and years whole, "days" for days
// 5. ago(time, now=nothing)                                   -> text: 3 minutes ago, in 2 hours, just now
// 6. start_of_day(time, offset=0)                             -> number
// 7. day_of_week(time, offset=0)                              -> text: Monday to Sunday
// 8. now_in_words(zone=nothing)                               -> text: Tuesday 20 October 2026, 3:42 PM, in UTC with none
// A pattern's letters: YYYY 2026, YY 26, MMMM October, MMM Oct, MM 01, M 1, DD 08, D 8, dddd Sunday, ddd Sun, HH 14, H 14,
// hh 02, h 2, mm 05, ss 09, A PM, a pm, Z +02:00 or Z itself in UTC, and [words] kept as they are.

////////// 1-2. READING AND WRITING //////////

fn parse(text) "An ISO date or time, like 2026-10-18 or 2026-10-18T14:05:00+02:00, as seconds since 1970, or nothing when it is not one. One with no zone is UTC. Its clock may have seconds, with a fraction, and a zone is Z or like +02:00." {
  t = std.text(text)
  if std.length(t) < 10 or t[4] != "-" or t[7] != "-" {return nothing}
  date = [_digits(std.slice(t, 0, 4)), _digits(std.slice(t, 5, 7)), _digits(std.slice(t, 8, 10))]
  rest = std.slice(t, 10)
  clock = [0, 0, 0]
  if rest != "" and (rest[0] == "T" or rest[0] == " ") {
    if rest[3] != ":" {return nothing}
    clock = [_digits(std.slice(rest, 1, 3)), _digits(std.slice(rest, 4, 6)), 0]
    rest = std.slice(rest, 6)
    if rest != "" and rest[0] == ":" {
      n = 1
      while rest[n] != nothing and rest[n] in "0123456789." {n += 1}
      clock[2] = rest[1] != nothing and rest[1] in "0123456789" ? std.number(std.slice(rest, 1, n)) : nothing
      rest = std.slice(rest, n)
    }
  }
  zone = 0
  if rest == "Z" {rest = ""}
  if std.length(rest) == 6 and (rest[0] == "+" or rest[0] == "-") and rest[3] == ":" {
    h = _digits(std.slice(rest, 1, 3))
    m = _digits(std.slice(rest, 4, 6))
    zone = h == nothing or m == nothing ? nothing : (rest[0] == "-" ? -1 : 1) * (h * 60 + m)
    rest = ""
  }
  if rest != "" or nothing in date or nothing in clock or zone == nothing or date[1] < 1 or date[1] > 12 or date[2] < 1 or date[2] > 31 {return nothing}
  return ((_days_of(date[0], date[1], date[2]) * 24 + clock[0]) * 60 + clock[1] - zone) * 60 + clock[2]
}

fn format(time, pattern="YYYY-MM-DDTHH:mm:ssZ", offset=0) "A time written by a pattern, like dddd D MMMM YYYY, h:mm a: YYYY 2026, YY 26, MMMM October, MMM Oct, MM 01, M 1, DD 08, D 8, dddd Sunday, ddd Sun, HH 14, H 14, hh 02, h 2, mm 05, ss 09, A PM, a pm, Z +02:00 (Z in UTC), and [words] as they are. time is seconds since 1970, or ISO text like 2026-10-18T14:05:00Z, and offset hours from UTC, like -5." {
  c = _parts(_at(time) + offset * 3600)
  h = c.hour % 12 == 0 ? 12 : c.hour % 12
  hours = math.max(offset, -offset)
  month = ["January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"][c.month - 1]
  day = ["Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"][c.weekday]
  says = {YYYY: std.text(c.year), YY: _two(c.year % 100), MMMM: month, MMM: std.slice(month, 0, 3), MM: _two(c.month), M: std.text(c.month),
    DD: _two(c.day), D: std.text(c.day), dddd: day, ddd: std.slice(day, 0, 3), HH: _two(c.hour), H: std.text(c.hour), hh: _two(h), h: std.text(h),
    mm: _two(c.minute), ss: _two(c.second), A: c.hour < 12 ? "AM" : "PM", a: c.hour < 12 ? "am" : "pm",
    Z: offset == 0 ? "Z" : (offset < 0 ? "-" : "+") + _two(math.round(hours, 0, "down")) + ":" + _two(math.round(hours * 60) % 60)}
  p = std.text(pattern)
  out = []
  i = 0
  while i < std.length(p) {
    if p[i] == "[" {
      words = text.split(std.slice(p, i + 1), "]")[0]
      out += [words]
      i += std.length(words) + 2
      continue
    }
    found = nothing
    for n in [4, 3, 2, 1] {if found == nothing and std.slice(p, i, i + n) in says {found = std.slice(p, i, i + n)}}
    out += [found == nothing ? p[i] : says[found]]
    i += found == nothing ? 1 : std.length(found)
  }
  return text.join(out)
}

////////// 3-5. MOVING AND COMPARING //////////

fn add(time, amount, unit="seconds") "A time moved by an amount of seconds, minutes, hours, days, weeks, months or years, back when the amount is below 0, like a week from now or 3 months ago. Months and years go by the calendar: a month on from the 31st is the last day of a shorter month. time is seconds since 1970, or ISO text like 2026-10-18T14:05:00Z." {
  t = _at(time)
  u = _plural(unit)
  if u != "months" and u != "years" {return t + amount * _seconds(u)}
  c = _parts(t)
  m = c.year * 12 + c.month - 1 + (u == "years" ? amount * 12 : amount)
  y = math.round(m / 12, 0, "down")
  month = m % 12 + 1
  return t + (_days_of(y, month, math.min(c.day, _days_in(y, month))) - _days_of(c.year, c.month, c.day)) * 86400
}

fn difference(a, b, unit="seconds") "How long it is from time a to time b in a unit, like the days between two dates, below 0 when b is first. Months and years are whole ones by the calendar: a month isn't whole until b's day and clock have passed a's. a and b are seconds since 1970, or ISO text like 2026-10-18T14:05:00Z, so the days from one ISO date to another are difference(a, b, \"days\")." {
  x = _at(a)
  y = _at(b)
  u = _plural(unit)
  if u != "months" and u != "years" {return (y - x) / _seconds(u)}
  p = _parts(x)
  q = _parts(y)
  m = (q.year - p.year) * 12 + q.month - p.month
  if m > 0 and add(x, m, "months") > y {m -= 1}
  if m < 0 and add(x, m, "months") < y {m += 1}
  return u == "months" ? m : math.round(m / 12, 0, m >= 0 ? "down" : "up")
}

fn ago(time, now=nothing) "How long ago a time was, or how far off it is, in words: 3 minutes ago, in 2 hours, just now, under a minute either way being just now. now is the time it is said from, now when left out." {
  d = _at(now) - _at(time)
  s = math.max(d, -d)
  if s < 60 {return "just now"}
  said = ""
  for u in [["year", 31536000], ["month", 2592000], ["week", 604800], ["day", 86400], ["hour", 3600], ["minute", 60]] {
    n = math.round(s / u[1], 0, "down")
    if said == "" and n > 0 {said = "{n} {u[0]}" + (n == 1 ? "" : "s")}
  }
  return d > 0 ? said + " ago" : "in " + said
}

////////// 6-7. DAYS //////////

fn start_of_day(time, offset=0) "Midnight at the start of the day a time is on, where the offset is, as seconds since 1970. offset is hours from UTC, like -5." {return math.round((_at(time) + offset * 3600) / 86400, 0, "down") * 86400 - offset * 3600}

fn day_of_week(time, offset=0) "The day of the week a time is on, Monday to Sunday, where the offset is. offset is hours from UTC, like -5." {return format(time, "dddd", offset)}

////////// 8. NOW, IN WORDS //////////

fn now_in_words(zone=nothing) "The time and date now, said in words to tell a person, like Tuesday 20 October 2026, 3:42 PM. Use it for what time is it, what is the time or the date, or what day is it today. zone is their time zone, hours from UTC as a number or text like -4, +05:30 or UTC-4, or their local time with its offset, like 2026-10-19T16:31:07-04:00, as a device gives its time. UTC, and said so, when left out." {
  said = format(compute.now(), "dddd D MMMM YYYY, h:mm A", zone == nothing ? 0 : _hours(zone))
  return zone == nothing ? said + " UTC" : said
}

////////// HELPERS //////////

fn _at(time) "A time as seconds, from seconds or ISO text, or now when it is nothing." {
  if time == nothing {return compute.now()}
  try {return time - 0} catch e {}
  t = parse(time)
  if t == nothing {fail "{time} isn't a time: give seconds since 1970, or text like 2026-10-18T14:05:00Z"}
  return t
}

fn _parts(t) "A time's date and clock by the civil calendar every clock counts by: its days since 1970 as a year, month and day." {
  s = math.round(t, 0, "down")
  days = math.round(s / 86400, 0, "down")
  clock = s - days * 86400
  z = days + 719468
  era = math.round(z / 146097, 0, "down")
  doe = z - era * 146097
  yoe = math.round((doe - math.round(doe / 1460, 0, "down") + math.round(doe / 36524, 0, "down") - math.round(doe / 146096, 0, "down")) / 365, 0, "down")
  doy = doe - (365 * yoe + math.round(yoe / 4, 0, "down") - math.round(yoe / 100, 0, "down"))
  mp = math.round((5 * doy + 2) / 153, 0, "down")
  month = mp < 10 ? mp + 3 : mp - 9
  return {year: yoe + era * 400 + (month <= 2 ? 1 : 0), month: month, day: doy - math.round((153 * mp + 2) / 5, 0, "down") + 1,
    hour: math.round(clock / 3600, 0, "down"), minute: math.round(clock / 60, 0, "down") % 60, second: clock % 60, weekday: (days + 4) % 7}
}

fn _days_of(year, month, day) "The days since 1970 of a date, the other way." {
  y = year - (month <= 2 ? 1 : 0)
  era = math.round(y / 400, 0, "down")
  yoe = y - era * 400
  doy = math.round((153 * (month > 2 ? month - 3 : month + 9) + 2) / 5, 0, "down") + day - 1
  return era * 146097 + yoe * 365 + math.round(yoe / 4, 0, "down") - math.round(yoe / 100, 0, "down") + doy - 719468
}

fn _days_in(year, month) "How many days a month has." {return (month == 12 ? _days_of(year + 1, 1, 1) : _days_of(year, month + 1, 1)) - _days_of(year, month, 1)}

fn _seconds(unit) "Seconds in a unit that is always as long." {
  s = {seconds: 1, minutes: 60, hours: 3600, days: 86400, weeks: 604800}[unit]
  if s == nothing {fail "a unit is seconds, minutes, hours, days, weeks, months or years"}
  return s
}

fn _plural(unit) "A unit as its plural, so minute and minutes are one." {
  u = text.lower(unit)
  return std.slice(u, -1) == "s" ? u : u + "s"
}

fn _hours(zone) "A zone's hours from UTC, from any of the ways now_in_words takes one." {
  z = text.upper(text.trim(zone))
  if "T" in z and std.length(z) > 10 {
    tail = std.slice(z, -6)
    z = std.slice(tail, 0, 1) == "+" or std.slice(tail, 0, 1) == "-" ? tail : ""
  }
  for name in ["UTC", "GMT", "Z"] {if std.slice(z, 0, std.length(name)) == name {z = std.slice(z, std.length(name))}}
  if z == "" {return 0}
  sign = std.slice(z, 0, 1) == "-" ? -1 : 1
  if std.slice(z, 0, 1) == "-" or std.slice(z, 0, 1) == "+" {z = std.slice(z, 1)}
  p = [z, "0"]
  if ":" in z {p = text.split(z, ":")}
  if std.length(z) == 4 and not (":" in z) and not ("." in z) {p = [std.slice(z, 0, 2), std.slice(z, 2)]}
  h = std.number(p[0])
  m = std.number(p[1])
  if h == nothing or m == nothing or h > 14 {fail "{zone} isn't a time zone I can read: give hours from UTC, like -5 or +05:30"}
  return sign * (h + m / 60)
}

fn _two(n) "A number of two digits at least." {return n < 10 ? "0{n}" : "{n}"}

fn _digits(t) "Text of digits only, like 08, as its whole number, or nothing when it is anything else." {
  n = 0
  for c in t {
    if not (c in "0123456789") {return nothing}
    n = n * 10 + std.number(c)
  }
  return t == "" ? nothing : n
}
