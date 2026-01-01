# Simple Mode Filter Specification

## 1. Purpose

Convert terminal output for legacy terminals that lack:
- Unicode support (UTF-8)
- Color/formatting (ANSI SGR)

Filter runs on TERM_OUTPUT packets before writing to stdout.

---

## 2. Design Goals

| Goal | Approach |
|------|----------|
| No allocation | In-place filtering, static state |
| No lag | Output immediately, state handles splits |
| Streaming | Works byte-by-byte across packet boundaries |
| Shrink only | Output <= input, safe to share buffer |

---

## 3. State Machine

```
         +--------------------------------------+
         |                                      |
         v                                      |
    +---------+  0x1B    +-----+  '['    +-----+
    | NORMAL  |--------->| ESC |-------->| CSI |
    +---------+          +-----+         +-----+
         |                  |               |
         | 0x80-0xFF        | other         | 0x40-0x7E
         v                  v               v
    +---------+         (output         (if 'm': discard
    |  UTF8   |          seq)            else: output seq)
    +---------+
         |
         | complete
         v
    (convert to ASCII)
```

**States:**

| State | Meaning | Waiting for |
|-------|---------|-------------|
| NORMAL | Default | Any byte |
| ESC | Saw `0x1B` | `[` or other |
| CSI | In `ESC[...` | Command byte `0x40-0x7E` |
| UTF8 | In multi-byte | Continuation bytes `0x80-0xBF` |

---

## 4. Persistent State (across packets)

```c
struct filter_state {
    int state;              /* NORMAL, ESC, CSI, UTF8 */
    unsigned char seq[32];  /* Incomplete sequence bytes */
    int seq_len;            /* Bytes accumulated in seq[] */
    int utf8_need;          /* Continuation bytes remaining */
    int spinner;            /* 0-3, cycles through -\|/ */
};
```

**Size**: ~40 bytes static. Zero heap allocation.

---

## 5. ESC Sequence Handling

| Sequence Type | Pattern | Action |
|---------------|---------|--------|
| SGR (color) | `ESC[...m` | **Discard** |
| Other CSI | `ESC[...X` (X != m) | Pass through |
| Non-CSI | `ESC` + not `[` | Pass through |
| Too long | `ESC[` + >28 params | Flush as-is |

**SGR examples discarded:**
- `ESC[0m` - reset
- `ESC[1;31m` - bold red
- `ESC[38;5;196m` - 256-color
- `ESC[38;2;255;128;0m` - truecolor

**CSI examples preserved:**
- `ESC[H` - cursor home
- `ESC[2J` - clear screen
- `ESC[?25h` - show cursor
- `ESC[10;20H` - cursor position

---

## 6. UTF-8 Detection

| First byte | Pattern | Total bytes | Need |
|------------|---------|-------------|------|
| `0x00-0x7F` | ASCII | 1 | 0 |
| `0xC0-0xDF` | 110xxxxx | 2 | 1 |
| `0xE0-0xEF` | 1110xxxx | 3 | 2 |
| `0xF0-0xF7` | 11110xxx | 4 | 3 |
| `0x80-0xBF` | Continuation | - | Invalid as start |
| `0xF8-0xFF` | Invalid | - | Output `?` |

**Continuation bytes**: Must match `10xxxxxx` (`0x80-0xBF`)

**Invalid sequence**: Output `?`, reprocess current byte as new start

---

## 7. UTF-8 to ASCII Conversion Table

### 7.1 Box Drawing (E2 94 xx, E2 95 xx)

| Range | Characters | ASCII |
|-------|------------|-------|
| E2 94 80-84 | horizontal lines | `-` |
| E2 94 82-83 | vertical lines | `\|` |
| E2 94 xx other | corners, tees, cross | `+` |
| E2 95 90-94 | double horizontal | `=` |
| E2 95 xx other | double box drawing | `+` |

### 7.2 Arrows (E2 86 xx)

| Bytes | Character | ASCII |
|-------|-----------|-------|
| E2 86 90 | leftwards arrow | `<` |
| E2 86 91 | upwards arrow | `^` |
| E2 86 92 | rightwards arrow | `>` |
| E2 86 93 | downwards arrow | `v` |
| E2 86 xx other | other arrows | `>` |

### 7.3 Geometric Shapes (E2 96 xx, E2 97 xx)

| Bytes | Character | ASCII |
|-------|-----------|-------|
| E2 96 B2-B5 | up triangles | `^` |
| E2 96 B6-B9 | right triangles | `>` |
| E2 96 BA-BD | down triangles | `v` |
| E2 97 80-83 | left triangles | `<` |
| E2 97 8F | black circle | **spinner** |
| E2 97 8B | white circle | `o` |
| E2 97 86-87 | diamonds | `*` |
| other | shapes | `*` |

### 7.4 Dingbats (E2 9C xx, E2 9D xx)

| Bytes | Character | ASCII |
|-------|-----------|-------|
| E2 9C 93-94 | check marks | `+` |
| E2 9C 85 | white heavy check | `+` |
| E2 9C 97-98 | ballot X | `x` |
| E2 9D 8C | cross mark | `x` |
| E2 9C A2, B3, B6, BB, BD | stars/asterisks | **spinner** |
| other | dingbats | `*` |

### 7.5 Heavy Arrows (E2 9E xx)

| Bytes | Character | ASCII |
|-------|-----------|-------|
| E2 9E xx | heavy arrows | `>` |

### 7.6 Math Operators (E2 88 xx)

| Bytes | Character | ASCII |
|-------|-----------|-------|
| E2 88 B4 | therefore | **spinner** |
| other | math symbols | `*` |

### 7.7 Technical Symbols (E2 8C-8F xx)

| Bytes | Character | ASCII |
|-------|-----------|-------|
| E2 8C-8F xx | technical symbols | `>` |

### 7.8 General Punctuation (E2 80 xx)

| Bytes | Character | ASCII |
|-------|-----------|-------|
| E2 80 A2 | bullet | `*` |
| E2 80 A3 | triangular bullet | `>` |
| E2 80 93-95 | dashes | `-` |
| E2 80 98-99 | single quotes | `'` |
| E2 80 9C-9D | double quotes | `"` |
| E2 80 A6 | ellipsis | `.` |
| E2 80 B9 | single left angle | `<` |
| E2 80 BA | single right angle | `>` |
| other | punctuation | space |

### 7.9 Latin-1 Supplement (C2 xx, C3 xx)

| Bytes | Character | ASCII |
|-------|-----------|-------|
| C2 A0 | NBSP | space |
| C2 B7 | middle dot | **spinner** |
| other | Latin-1 | `?` |

### 7.10 Emoji (F0 9F xx xx)

| Bytes | Character | ASCII |
|-------|-----------|-------|
| F0 9F xx xx | emoji | `*` |

### 7.11 Fallback

| Condition | ASCII |
|-----------|-------|
| Unknown 2-byte | `?` |
| Unknown 3-byte | `?` |
| Unknown 4-byte | `?` |
| Invalid sequence | `?` |

---

## 8. Spinner Animation

**Characters that animate**: black circle, middle dot, stars, therefore symbol

**Sequence**: `-` `\` `|` `/` (indices 0-3)

**State**: `spinner = (spinner + 1) & 3` after each animated char

**Effect**: Consecutive spinners cycle, creating animation `-\|/-\|/...`

---

## 9. Packet Boundary Handling

**Scenario**: UTF-8 sequence split across packets

```
Packet 1: [data...][E2][94]  <- incomplete, need 1 more byte
          State after: UTF8, seq={E2,94}, seq_len=2, utf8_need=1
          Output: [filtered data...]  (E2 94 held in state)

Packet 2: [80][data...]      <- continuation byte arrives
          Process: seq={E2,94,80} -> complete -> output '-'
          Output: [-][filtered data...]
```

**Scenario**: CSI sequence split

```
Packet 1: [data...][1B][5B][33][31]  <- ESC[31 incomplete
          State after: CSI, seq={1B,5B,33,31}, seq_len=4
          Output: [filtered data...]

Packet 2: [6D][data...]      <- 'm' arrives, completes ESC[31m
          Process: SGR detected, discard entire sequence
          Output: [filtered data...]
```

---

## 10. Integration

```c
/* In main_loop, PKT_TERM_OUTPUT handler */
case PKT_TERM_OUTPUT:
    if (simple_mode) {
        length = filter_simple(payload, length);
    }
    write(STDOUT_FILENO, payload, length);
    break;
```

---

## 11. Edge Cases

| Case | Behavior |
|------|----------|
| Empty packet | Return 0, no state change |
| All-ASCII packet | Pass through unchanged |
| All-SGR packet | Return 0 (all discarded) |
| Truncated at EOF | Flush seq as `?` (or ignore) |
| CSI > 30 bytes | Flush as-is (probably corrupted) |
| Invalid UTF-8 mid-sequence | Output `?`, reprocess byte |

---

## 12. Testing Checklist

- [ ] Pure ASCII passthrough
- [ ] SGR color codes stripped
- [ ] Cursor movement preserved
- [ ] Box drawing to `+-|`
- [ ] Arrows to `<^>v`
- [ ] Checkmarks to `+`
- [ ] X marks to `x`
- [ ] Bullets to spinner
- [ ] Split UTF-8 across packets
- [ ] Split CSI across packets
- [ ] Invalid UTF-8 handling
- [ ] Spinner cycles correctly
