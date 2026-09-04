# The on-screen keyboard

What typing on a device with no keys is like. It appears wherever a text field
is tapped, and on a board that has its own keys it never appears at all —
`lcdKeyboardOnScreen()` is false there and every field takes the real ones.

```
    |1|2|3|4|5|6|7|8|9|0|⌫|
    |Q |W |E |R |T |Y |U |I |O |P |
      ␣|A |S |D |F |G |H |J |K |L | ␣
    |fn|Z |X |C |V |B |N |M |, |. |␣
    |⌨̸|shift|ctl|   space   |∷| ⏎ |
```

Five rows staggered like a typewriter's. The bottom row reads left to right as
**put these away, then the modifiers, then the two ways out of what you are
typing**: away in the corner where a thumb finds it without looking, enter in
the far corner, and the key to the other layout just inside enter.

## The caps never change

Every key is printed the way a physical keyboard prints it — upper case, with
what **shift** makes in its top-right corner and what **fn** makes in its
top-left. A modifier changes what a key *produces*, never what it *says*.
Nothing moves under a finger, and the whole character set can be read off the
keyboard without pressing anything.

Between the caps and the two corners that is a US keyboard: every printable
character it has, at or near where a US layout puts it.

## Three modifiers, each sticky for one key

**shift**, **ctl** and **fn** arm on a tap, go blue while they are up, and are
spent by whatever is pressed next.

- **shift** is the only one that latches: a **double tap locks caps**, the key
  reads `caps` and stays blue, and any single tap lets go.
- **fn** never latches. It is lettered in its own blue at rest so that it reads
  with the marks it reaches — the top-left corner of every cap above it.
- **ctl** never latches and carries no marks. A control combination is the
  letter with a bit set, which the terminal decodes and an ordinary text field
  ignores; `Ctrl+C` in the CLI app is what it is for.

**shift + backspace is delete** — rubbing out forwards instead of back. Nothing
on the cap says so; there is no room, and it is where a keyboard with keys puts
it anyway.

## A key can be thrown instead of pressed

Flick a cap **upwards** — anywhere in the quadrant either side of straight up —
and it gives its **top-right** mark, the one shift would have made. Flick it
**any other way** and it gives its **top-left** mark, the one fn would have
made.

So every character on the keyboard is one gesture away without arming anything,
and the marks already printed on the keys are the documentation. Up is the one
direction anybody can aim at without looking, which is why it carries the mark
people reach for most.

Only caps throw. The modifiers, space, enter and the corner keys have no marks
and ignore it.

## Or held, which opens everything else it can make

Hold a key down and a panel opens well above the keys, centred on the screen:
what a tap gives, then the shift and fn alternates, then the accented forms —
one row of them, or two rows of about equal length past five. They are drawn at
twice the legend size, because telling `ä` from `å` from `ā` is the entire point
and at key size they are one smudge under a finger.

**The finger never lifts.** It slides up onto the character it wants, which
highlights under it, and lifting there types it.

**Nothing is chosen until the finger is on a character.** Lift anywhere else and
nothing is typed at all — the panel simply goes. A hold nobody meant costs
nothing.

A **letter** shows neither of its cases, only its fn alternate where it has one
and then its accents: the small letter and the capital are a tap and a shifted
tap, and cells for them would stand in front of the characters that have no
other way in. Every other key shows all three, because for a digit the shifted
and fn forms are not otherwise obvious. With shift on, the panel shows and types
capitals.

A key with nothing to offer that a tap does not already give never opens a
chooser — so **backspace and the cursor keys keep repeating** under a held
finger.

### What is under which letter

| Key | Alternates |
|---|---|
| A | à á â ä ã å æ ā |
| C | ç ć č |
| D | ď đ ð |
| E | è é ê ë ē ę € |
| G | ğ ĝ ģ |
| I | ì í î ï ī į |
| L | ł ľ ĺ |
| M | µ |
| N | ñ ń ň ņ |
| O | ò ó ô ö õ ø œ Ω |
| R | ř ŕ |
| S | ß ś š ş |
| T | ť ţ þ |
| U | ù ú û ü ū ů ű |
| W | ŵ |
| Y | ý ÿ |
| Z | ž ź ż |
| 0 | ° |
| - | – — |
| . | … · |

One set for a family of languages rather than one per language: what a European
Latin alphabet actually puts on a letter, in the order a hand hunting for one
sweeps — the accents first, then the ring, macron and ogonek, then the
characters that are a letter in their own right where they are used at all.
Between them they cover French, German, Spanish, Portuguese, Italian, Dutch,
Catalan, the Nordics, Polish, Czech, Slovak, Slovene, Croatian, Hungarian,
Romanian, Turkish and the Baltics. This is not a claim to write those languages
properly — there is one keyboard here and its layout is a US one — but a name, a
place or a borrowed word in any of them can be typed without leaving it.

Two of them are not letters and are there because the letter is how a hand looks
for them: **µ** under M, **Ω** under O.

## The other layout

The key drawn as a little pad, beside enter, leads to numbers and controls: a
numeric pad, the keys a terminal needs (escape, tab), and the four cursor keys
set around an empty square. The keyboard glyph in that same place leads back.

The bottom row holds still across both — away in one corner, enter in the other,
and the key that changes the layout just inside enter — so the pair is in the
same place whichever layout is up.

A field that accepts only digits gets a **third** layout on open: a plain number
pad, with no letters to leave it for.

## What it does without being asked

- **A capital starts an empty field, and follows a `. `** — as a phone keyboard
  does. Under this keyboard that *is* its shift key: armed and standing blue, so
  one tap refuses it. A field that quietly upper-cased its first letter could
  only be argued with after the fact.
- **Space twice, quickly, ends the sentence** — `. ` and shift armed for the
  letter that starts the next one. Only where a word precedes it.
- **Enter in a one-line field submits and puts the keys away**, which is the
  Enter a device with keys would have sent. In a multi-line field it is a
  newline and the keys stay — unless that field submits on Enter, which some do.
- **A field grows only as far as the screen above the keys allows.** A text box
  grows upwards, so an uncapped one would walk the first lines of what is being
  written off the top edge rather than overflow where it can be seen. Where a
  panel has no room for the eight lines an app asked for, it gets fewer.

## Getting rid of it

The **crossed-out keyboard key** in the corner puts it away at any time.
Anything that clears a dialog does too — the back gesture, an app change, a
tap outside — because it is a tracked dialog like any other. Submitting a field
takes it down as well.

The back gesture's edge zone stands down while the keys are up, so a swipe from
the edge cannot tear them away mid-word; the keyboard's own away key is what
that gesture would have been for.

---

Maintainer detail — the fork's divergence from LVGL's keyboard, the key
handling, the flick geometry and the chooser's construction — is in
`src/lcd_ui/lcd_keys.h` and `lcd_keys.c`, and the platform side (when it opens,
what it opens over, how it is warmed at boot) in `lcd_keyboard.cpp`.
