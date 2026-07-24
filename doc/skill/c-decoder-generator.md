# c-decoder-generator

## Metadata

- **Name**: c-decoder-generator
- **Description**: Generate C protocol decoders from Python decoders using the C Decoder API v4. Translates sigrok Python decoder logic into native C DLL decoders for the PXView/libsigrokdecode engine.
- **Trigger**: When user asks to create/add a new C decoder, port a Python decoder to C, write a C version of a protocol decoder, or similar requests involving C decoder generation.

## Context

PXView uses libsigrokdecode which supports both Python and C protocol decoders. C decoders are compiled as separate DLLs and loaded at runtime. They use the C Decoder API v4 (`SRD_C_DECODER_API_VERSION = 4`) defined in `libsigrokdecode/libsigrokdecode.h`. Each C decoder is a `.c` file in `libsigrokdecode/c_decoders/` compiled as a MODULE library.

The goal is to produce a C decoder whose annotation output is **bit-for-bit identical** to the Python original when fed the same input data.

## Complete Generation Workflow

### Step 1: Read Python Decoder Source

Read the Python decoder files:
- `libsigrokdecode/decoders/<name>/__init__.py` — module docstring
- `libsigrokdecode/decoders/<name>/pd.py` — decoder class with all logic

Extract the full `Decoder` class including:
- Class attributes: `id`, `name`, `longname`, `desc`, `license`, `inputs`, `outputs`, `tags`
- `channels` and `optional_channels` tuples
- `annotations` tuple (each entry is `(id, label)`)
- `annotation_rows` tuple (each entry is `(id, desc, (class_indices,))`)
- `options` tuple (each entry is a dict with `id`, `desc`, `default`, optional `values`)
- `binary` tuple if present
- All methods: `__init__`, `reset`, `start`, `metadata`, `decode`, and any helper methods

### Step 2: Extract Metadata

Map Python metadata to C struct fields:

| Python | C field |
|--------|---------|
| `Decoder.id` | `.id = "<name>_c"` |
| `Decoder.name` | `.name = "<Name>(C)"` |
| `Decoder.longname` | `.longname = "<Long name> (C)"` |
| `Decoder.desc` | `.desc = "<desc> (C implementation)"` |
| `Decoder.license` | `.license` |
| `Decoder.channels` | `static struct srd_channel <name>_channels[]` |
| `Decoder.optional_channels` | `static struct srd_channel <name>_optional_channels[]` |
| `Decoder.annotations` | `enum { ANN_... = 0, ..., NUM_ANN };` + `static const char *<name>_ann_labels[][3]` |
| `Decoder.annotation_rows` | `static const struct srd_c_ann_row <name>_ann_rows[]` |
| `Decoder.options` | `static struct srd_decoder_option <name>_options[]` |
| `Decoder.binary` | `static const struct srd_decoder_binary <name>_binary[]` |
| `Decoder.inputs` | `static const char *<name>_inputs[]` |
| `Decoder.outputs` | `static const char *<name>_outputs[]` |
| `Decoder.tags` | `static const char *<name>_tags[]` |

**Channel struct fields**: `{id, name, desc, order, type, idn}` where:
- `order` = index in the Python channels/optional_channels tuple
- `type` = `SRD_CHANNEL_SCLK` for clock, `SRD_CHANNEL_SDATA` for data, `SRD_CHANNEL_COMMON` for control signals like CS#
- `idn` = the `idn` field from Python (language text source id), or `NULL` if absent

**Annotation labels**: Each entry is `{"", id, label}` — the first field is empty string (reserved).

**Option defaults**: Set in `srd_c_decoder_entry()` function using `g_variant_new_string()`, `g_variant_new_int64()`, `g_variant_new_uint64()`. Option value lists use `GSList` with `g_slist_append()`.

### Step 3: Translate Python Decode Logic to C

This is the core step. Follow these translation rules precisely:

#### State Machine Translation

Python uses `self.state` enum with `if/elif` chains:
```python
# Python
if self.state == 'FIND_START':
    ...
elif self.state == 'FIND_ADDRESS':
    ...
```

C uses `enum` + `switch/case`:
```c
enum <name>_state { STATE_FIND_START, STATE_FIND_ADDRESS, ... };

switch (s->state) {
case STATE_FIND_START:
    ...
    break;
case STATE_FIND_ADDRESS:
    ...
    break;
}
```

#### Decoder State Struct

Use `C_DECODER_STATE` macro for auto-generated reset/destroy:
```c
C_DECODER_STATE(<name>, {
    enum <name>_state state;
    int bitcount;
    uint64_t databyte;
    // ... all self.xxx variables from Python
    int out_ann;
    int out_python;
    int out_binary;
    uint64_t samplerate;
});
```

This auto-generates `<name>_s` typedef, `<name>_reset` (calloc), and `<name>_destroy` (free).

For complex state with GArray or non-zero defaults, write custom reset/destroy and suppress the auto-generated ones with `#pragma GCC diagnostic`.

#### self.wait() → c_wait()

| Python | C |
|--------|---|
| `self.wait({0: 'r'})` | `c_wait(di, CW_R(0), CW_END)` |
| `self.wait({0: 'f'})` | `c_wait(di, CW_F(0), CW_END)` |
| `self.wait({0: 'e'})` | `c_wait(di, CW_E(0), CW_END)` |
| `self.wait({0: 'h'})` | `c_wait(di, CW_H(0), CW_END)` |
| `self.wait({0: 'l'})` | `c_wait(di, CW_L(0), CW_END)` |
| `self.wait({0: 'e', 1: 'e'})` | `c_wait(di, CW_E(0), CW_OR, CW_E(1), CW_END)` |
| `self.wait({})` | `c_wait(di, CW_END)` |
| `self.wait([{0: 'r'}, {1: 'f'}])` | `c_wait(di, CW_R(0), CW_OR, CW_F(1), CW_END)` |
| `self.wait([{0: 'r'}, {1: 'f'}, {2: 'e'}])` | `c_wait(di, CW_R(0), CW_OR, CW_F(1), CW_OR, CW_E(2), CW_END)` |

**CRITICAL**: Each condition group in Python's list becomes a CW_OR-separated group in c_wait(). The entire call ends with CW_END.

**Return value check**: Always check `if (ret != SRD_OK) return;` after c_wait().

**No sequential c_wait() calls**: If two c_wait() calls appear back-to-back in the same code path, merge them with CW_OR. Never call c_wait() twice without processing the matched result in between.

#### self.samplenum → di_samplenum(di)

```c
uint64_t samplenum = di_samplenum(di);
```

#### self.matched → di_matched(di)

```python
# Python
if self.matched & (0b1 << cond_reset):
```

```c
// C
if (di_matched(di) & (1ULL << cond_reset)):
```

**IMPORTANT**: `self.matched` is a bitmask. In Python, condition group indices start from 0 in the `condition` list. In C, the bit position corresponds to the CW_OR group index (0-based). So the first CW_OR group = bit 0, second = bit 1, etc.

#### self.put() → c_put()

```python
# Python
self.put(ss, es, self.out_ann, [cls, ['text1', 'text2']])
```

```c
// C
c_put(di, ss, es, s->out_ann, cls, "text1", "text2");
```

For numeric values with hex display:
```python
self.put(ss, es, self.out_ann, [cls, ['0x%02X' % val]])
```

```c
c_put_v(di, ss, es, s->out_ann, cls, val, "0x%02X", (unsigned int)val);
// Or use snprintf + c_put for complex formatting
```

#### self.putp() / self.put(..., self.out_python, [...]) → c_proto()

```python
# Python
self.put(ss, es, self.out_python, ['START', ...])
```

```c
// C — c_proto with C_END sentinel
c_proto(di, ss, es, s->out_python, "START", C_I8(val), C_END);
```

**CRITICAL**: Every c_proto() call MUST end with C_END. Omitting C_END causes undefined behavior.

c_field type mapping:

| Python type | c_field macro |
|-------------|---------------|
| `int` (8-bit) | `C_I8(v)` or `C_U8(v)` |
| `int` (16-bit) | `C_I16(v)` or `C_U16(v)` |
| `int` (32-bit) | `C_I32(v)` or `C_U32(v)` |
| `int` (64-bit) | `C_I64(v)` or `C_U64(v)` |
| `float/double` | `C_F64(v)` |
| `str` | `C_STR(v)` |
| `bytes` | `C_BYTES(data, len)` |

#### self.has_channel() → c_has_ch()

```python
have_miso = self.has_channel(1)
```

```c
int have_miso = c_has_ch(di, 1);
```

#### self.options[] → c_opt_int / c_opt_str / c_opt_dbl / c_opt_bool

```python
cpol = self.options['cpol']
bitorder = self.options['bitorder']
```

```c
s->cpol = (int)c_opt_int(di, "cpol", 0);
const char *bitorder = c_opt_str(di, "bitorder", "msb-first");
```

For boolean string options:
```python
if self.options['show_data_point'] == 'yes':
```

```c
s->show_data_point = c_opt_bool(di, "show_data_point", 1);
```

#### self.register() → c_reg_out()

```python
self.out_ann = self.register(srd.OUTPUT_ANN)
self.out_python = self.register(srd.OUTPUT_PYTHON)
self.out_binary = self.register(srd.OUTPUT_BINARY)
```

```c
s->out_ann    = c_reg_out(di, SRD_OUTPUT_ANN, "<name>");
s->out_python = c_reg_out(di, SRD_OUTPUT_PROTO, "<name>");
s->out_binary = c_reg_out(di, SRD_OUTPUT_BINARY, "<name>");
```

#### Pin Access: self.wait() return values → c_pin()

```python
# Python — wait returns pin values
(clk, miso, mosi, cs) = self.wait(condition)
```

```c
// C — read pins after c_wait
int clk  = c_pin(di, 0);
int miso = c_pin(di, 1);
int mosi = c_pin(di, 2);
int cs   = c_pin(di, 3);
```

#### List Operations

| Python | C |
|--------|---|
| `self.items.append(x)` | `s->items[s->items_cnt++] = x;` (with bounds check) |
| `self.items.insert(0, x)` | `memmove(&s->items[1], &s->items[0], s->items_cnt * sizeof(x)); s->items[0] = x; s->items_cnt++;` |
| `self.items[-1]` | `s->items[s->items_cnt - 1]` |
| `self.items[0]` | `s->items[0]` |
| `len(self.items)` | `s->items_cnt` |
| `self.items = []` | `s->items_cnt = 0` |
| `self.items.pop()` | `s->items_cnt--` |

#### Dict Operations

Python dicts used for state tracking → C struct fields:
```python
self.cmds = {'START': 0, 'STOP': 1}
```

```c
// Use enum or #define constants
#define CMD_START 0
#define CMD_STOP  1
```

Python dicts used for lookup → C arrays or switch statements.

#### String Formatting

| Python | C |
|--------|---|
| `f'{val:02X}'` | `snprintf(buf, sizeof(buf), "%02X", val)` |
| `'%02X' % val` | `snprintf(buf, sizeof(buf), "%02X", val)` |
| `f'{val:d}'` | `snprintf(buf, sizeof(buf), "%d", val)` |
| `f'{val:x}'` | `snprintf(buf, sizeof(buf), "%x", val)` — **lowercase** |
| `'%0*llX' % (width, val)` | `snprintf(buf, sizeof(buf), "%0*llX", width, (unsigned long long)val)` |

**Hex format rule**: Use lowercase `%x`/`%llx` for hex output unless the Python explicitly uses uppercase `%X`/`%02X`.

#### Exception Handling

```python
try:
    ...
except Exception as e:
    ...
```

```c
// C: use return value checking
int ret = c_wait(di, ...);
if (ret != SRD_OK)
    return;
```

#### Bit Operations

Python MSB-first bit assembly:
```python
self.misodata |= miso << (ws - 1 - self.bitcount)
```

C (identical):
```c
s->misodata |= (uint64_t)miso << (s->wordsize - 1 - s->bitcount);
```

Python LSB-first:
```python
self.misodata |= miso << self.bitcount
```

C:
```c
s->misodata |= (uint64_t)miso << s->bitcount;
```

**Hex bit order**: When building hex strings from bits, use MSB-first ordering:
```c
for (int i = 0; i < cnt; i++) {
    int bit_idx = cnt - 1 - i;  // MSB first
    tmp[i] = ((val >> bit_idx) & 1) ? '1' : '0';
}
```

#### Stacking Decoders (decode_upper)

When a Python decoder receives input from a stacked lower decoder via `self.wait()` on a protocol input, the C equivalent uses `decode_upper`:

```c
static void <name>_decode_upper(struct srd_decoder_inst *di,
                                uint64_t start_sample, uint64_t end_sample,
                                const char *cmd, const c_field *fields, int n_fields)
{
    <name>_s *s = (<name>_s *)c_decoder_get_private(di);
    // Parse cmd string and fields to extract protocol data
    // Equivalent to Python's self.wait() on protocol input
}
```

In the decoder struct: `.decode_upper = <name>_decode_upper`.

#### Binary Output

```python
self.put(ss, es, self.out_binary, [bin_class, data_bytes])
```

```c
c_put_bin(di, ss, es, s->out_binary, bin_class, data_len, data_ptr);
```

#### Meta Output

```python
self.put(ss, es, self.out_bitrate, bitrate)
```

```c
c_put_meta_int(di, ss, es, s->out_bitrate, bitrate);
```

### Step 4: Generate Complete C Decoder Source File

File location: `libsigrokdecode/c_decoders/<name>_c.c`

File structure (follow this order):

```c
#include "libsigrokdecode.h"
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Channel indices — match Python */
#define CH_XXX 0
#define CH_YYY 1

/* Annotation class indices — match Python annotations tuple */
enum <name>_ann {
    ANN_XXX = 0,
    ANN_YYY,
    NUM_ANN,
};

/* State machine enum (if applicable) */
enum <name>_state {
    STATE_XXX,
    STATE_YYY,
};

/* Decoder state struct */
C_DECODER_STATE(<name>, {
    /* state variables matching Python self.xxx */
    int out_ann;
    int out_python;
    int out_binary;
    uint64_t samplerate;
});

/* Channel definitions */
static struct srd_channel <name>_channels[] = { ... };
static struct srd_channel <name>_optional_channels[] = { ... };

/* Options */
static struct srd_decoder_option <name>_options[] = { ... };

/* Annotation labels */
static const char *<name>_ann_labels[][3] = { ... };

/* Annotation rows */
static const int <name>_row_xxx_classes[] = { ANN_XXX, -1 };
static const struct srd_c_ann_row <name>_ann_rows[] = { ... };

/* Binary output (if any) */
static const struct srd_decoder_binary <name>_binary[] = { ... };

/* Inputs/outputs/tags */
static const char *<name>_inputs[] = { "logic", NULL };
static const char *<name>_outputs[] = { "<proto>", NULL };
static const char *<name>_tags[] = { "Tag", NULL };

/* Helper functions */
static void <name>_format_value(...) { ... }

/* start callback */
static void <name>_start(struct srd_decoder_inst *di) { ... }

/* metadata callback (if needed) */
static void <name>_metadata(struct srd_decoder_inst *di, int key, uint64_t value) { ... }

/* decode callback — main state machine */
static void <name>_decode(struct srd_decoder_inst *di) { ... }

/* decode_upper callback (for stacked decoders) */
static void <name>_decode_upper(struct srd_decoder_inst *di, ...) { ... }

/* Decoder definition struct */
static struct srd_c_decoder <name>_c_def = {
    .id = "<name>_c",
    .name = "<Name>(C)",
    .longname = "<Long name> (C)",
    .desc = "<desc> (C implementation)",
    .license = "gplv2+",
    .channels = <name>_channels,
    .num_channels = N,
    .optional_channels = <name>_optional_channels,
    .num_optional_channels = M,
    .options = <name>_options,
    .num_options = K,
    .num_annotations = NUM_ANN,
    .ann_labels = <name>_ann_labels,
    .num_annotation_rows = R,
    .annotation_rows = <name>_ann_rows,
    .inputs = <name>_inputs,
    .num_inputs = 1,
    .outputs = <name>_outputs,
    .num_outputs = O,
    .binary = <name>_binary,
    .num_binary = B,
    .tags = <name>_tags,
    .num_tags = T,
    .state_size = sizeof(<name>_s),
    .reset = <name>_reset,
    .start = <name>_start,
    .decode = <name>_decode,
    .end = NULL,
    .metadata = <name>_metadata,
    .destroy = <name>_destroy,
    .decode_upper = <name>_decode_upper,
};

/* DLL entry point — sets option defaults and value lists */
SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    <name>_options[0].def = g_variant_new_string("default_val");
    <name>_options[0].idn = "dec_<name>_opt_<option_id>";
    // ... set all option defaults and value lists
    // Use g_slist_append for value lists
    return &<name>_c_def;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}
```

### Step 5: Add Decoder to CMakeLists.txt

Edit `CMakeLists.txt` and add the decoder name to the `C_DECODERS` list (line ~831):

```cmake
set(C_DECODERS spi_c i2c_c ... <new_name>_c)
```

The name must match the file name without extension: `libsigrokdecode/c_decoders/<new_name>_c.c`.

### Step 6: Build and Run Single Decoder Test

Build the project:
```bash
build_incremental.cmd
```

Or for just the decoder DLL:
```bash
cd build && ninja decoder_<new_name>_c
```

Run the decoder test (if test data exists):
```bash
cd libsigrokdecode/tests
decoder_test.exe -d <new_name>_c -t ./testdata/<new_name>_c/<test_case>
```

Or use the batch runner:
```bash
run_tests.cmd <new_name>_c
```

### Step 7: Analyze Deviations and Fix

If the test fails, compare C output with Python output:

1. Run both C and Python decoders on the same input data
2. Compare the JSON output files (`actual_c.json` vs `expected_py.json`)
3. Find the first deviation in annotation position, class, or text
4. Trace back to the C code to find the root cause
5. Fix, rebuild, and retest

See the Debugging Guide section below for systematic deviation resolution.

## Quality Checklist

Before considering a C decoder complete, verify ALL of the following:

1. **All `self.wait()` → `c_wait()`**: Every Python wait call has a corresponding C wait call with correct conditions and CW_END terminator
2. **All `self.put()` → `c_put()`**: Every Python annotation output has a corresponding C annotation output with correct class number and text
3. **All `self.putp()` → `c_proto()` + C_END**: Every Python protocol output has a corresponding C protocol output, and C_END is present
4. **`self.matched` uses bitmask**: `di_matched(di) & (1ULL << N)` — not `==` comparison, not `& 0xN` hex mask
5. **Annotation class numbers match Python**: The enum values must exactly match the order in Python's `annotations` tuple
6. **Multi-text variants match Python**: If Python puts `['long', 'medium', 'short']`, C must put the same strings in the same order
7. **Hex format uses lowercase**: `%x`/`%llx` for lowercase hex unless Python explicitly uses uppercase `%X`
8. **Hex bit order uses MSB-first**: `<< (cnt-1-i)` for bit-to-string conversion
9. **No sequential `c_wait()` calls**: Multiple waits in the same code path must be merged with CW_OR
10. **`c_proto()` ends with C_END**: Every c_proto call has C_END as the last argument
11. **Build passes**: No compile errors, no warnings with `-Wall -Wextra`
12. **Single decoder test passes**: `decoder_test -d <name>_c` returns exit code 0

## Debugging Guide

When the C decoder output deviates from the Python output, follow this systematic process:

### 1. Compare Output Files

Run the test to generate comparison JSON:
```bash
decoder_test.exe -d <name>_c -t ./testdata/<name>_c/<case> -o ./output
```

Compare `actual_c.json` and `expected_py.json`:
- Look for the first annotation that differs
- Note the `start_sample`, `end_sample`, `ann_class`, and `texts` fields

### 2. Locate the c_put() Call

From the deviation's `(start_sample, ann_class)`, find the corresponding `c_put()` call in the C source:
- Search for the ann_class value (e.g., `ANN_MISO_DATA`)
- Check the start_sample and end_sample calculation logic

### 3. Check Text Format

Common text format issues:
- **Hex case mismatch**: Python `'0x%02x'` → C `"%02x"` (lowercase), Python `'0x%02X'` → C `"%02X"` (uppercase)
- **Missing padding**: Python `'%02X'` → C `"%02X"` (not `"%X"`)
- **Integer format**: Python `'%d'` → C `"%d"`, Python `'%lld'` → C `"%lld"`
- **String concatenation**: Python `' '.join(items)` → C loop with `snprintf` + `pos +=`

### 4. Check Sample Positions

Common position issues:
- **Off-by-one in start_sample**: Python `self.samplenum` vs C `di_samplenum(di)` — they should be identical after the same `c_wait()`
- **Wrong end_sample**: Python often uses `self.samplenum` as end_sample; C must use the same value
- **Missing annotation**: If Python emits an annotation that C doesn't, check if a condition branch is missing

### 5. Check c_wait() Conditions

Common wait condition issues:
- **Wrong edge type**: `'r'` → `CW_R()`, `'f'` → `CW_F()`, `'e'` → `CW_E()`, `'h'` → `CW_H()`, `'l'` → `CW_L()`
- **Missing CW_OR**: Python `[{0:'r'}, {1:'f'}]` → C `CW_R(0), CW_OR, CW_F(1), CW_END`
- **Missing CW_END**: Every c_wait() must end with CW_END
- **Wrong channel index**: Python channel indices must match C channel indices exactly

### 6. Check self.matched Usage

Common matched issues:
- **Wrong bit position**: In Python, `cond_reset = len(condition)` before appending the reset condition. The reset condition is at index `cond_reset`. In C, this maps to `di_matched(di) & (1ULL << cond_reset)`.
- **Using `==` instead of `&`**: `self.matched` is a bitmask, always use bitwise AND
- **Missing matched check**: If Python checks `self.matched & (0b1 << N)`, C must check `di_matched(di) & (1ULL << N)`

### 7. Check State Machine

Common state machine issues:
- **Missing state transition**: Every Python `self.state = 'XXX'` must have a corresponding `s->state = STATE_XXX;`
- **Wrong initial state**: Python's initial state (set in `__init__` or `reset`) must match C's initial state (set by calloc zero-init or explicitly in start)
- **Missing break**: C switch/case requires explicit `break;` statements

### 8. Fix, Rebuild, and Retest

After identifying and fixing the issue:
1. Save the C source file
2. Rebuild: `cd build && ninja decoder_<name>_c`
3. Retest: `decoder_test.exe -d <name>_c -t ./testdata/<name>_c/<case>`
4. If still failing, repeat from step 1

## Common Patterns Reference

### Simple Decoder (no stacking, no protocol output)

See `counter_c.c` as reference. Pattern:
- Single `while(1)` loop with `c_wait()` + `c_put()`
- State stored in flat struct fields
- No `decode_upper`, no `c_proto()`

### Complex Decoder with Stacking

See `spi_c.c` as reference. Pattern:
- Multiple output types: `SRD_OUTPUT_ANN`, `SRD_OUTPUT_PROTO`, `SRD_OUTPUT_BINARY`
- Protocol output via `c_proto()` with typed `c_field` args
- Helper functions for formatting and data assembly
- Binary output via `c_put_bin()`
- Meta output via `c_put_meta_int()`

### Stacked Decoder (receives protocol input)

See `i2c_c.c` as reference. Pattern:
- `decode_upper` callback receives protocol data from lower decoder
- Parse `cmd` string and `fields` array to extract protocol information
- Use `fields[i].type` to determine field type, then access the appropriate union member

### ATK Color Annotations

Many decoders emit ATK (Auto-Tinting Kit) color annotations at the start:
```c
c_put(di, 0, 0, s->out_ann, ANN_ATK_DATA_POINT, "color:#RRGGBB");
c_put(di, 0, 0, s->out_ann, ANN_ATK_RISING_EDGE, "color:#RRGGBB");
c_put(di, 0, 0, s->out_ann, ANN_ATK_FALLING_EDGE, "color:#RRGGBB");
```

These must match the Python decoder's ATK annotations exactly.

### Option Handling in srd_c_decoder_entry()

String options with value lists:
```c
GSList *vals = NULL;
vals = g_slist_append(vals, g_variant_new_string("val1"));
vals = g_slist_append(vals, g_variant_new_string("val2"));
<name>_options[i].values = vals;
<name>_options[i].def = g_variant_new_string("val1");
<name>_options[i].idn = "dec_<name>_opt_<option_id>";
```

Integer options:
```c
<name>_options[i].def = g_variant_new_int64(default_value);
<name>_options[i].idn = "dec_<name>_opt_<option_id>";
```

Integer options with value lists:
```c
GSList *vals = NULL;
vals = g_slist_append(vals, g_variant_new_uint64(0));
vals = g_slist_append(vals, g_variant_new_uint64(1));
<name>_options[i].values = vals;
<name>_options[i].def = g_variant_new_uint64(0);
```

## Important Reminders

- **API Version**: Always use `SRD_C_DECODER_API_VERSION` (currently 4) in `srd_c_decoder_api_version()`
- **Export Macros**: Use `SRD_C_DECODER_EXPORT` for both `srd_c_decoder_entry` and `srd_c_decoder_api_version`
- **No Python.h**: C decoder DLLs must NOT include Python.h — they are pure C
- **Include Order**: `#include "libsigrokdecode.h"` first, then standard headers
- **State Size**: Set `.state_size = sizeof(<name>_s)` in the decoder struct
- **NULL Terminators**: All string arrays (inputs, outputs, tags) must end with NULL
- **Annotation Row Classes**: Class arrays in annotation rows must end with -1 (but `num_ann_classes` field counts the valid entries)
- **Memory Safety**: Use bounds checking on all arrays, especially bit arrays and byte buffers
- **No Dynamic Allocation in Decode Loop**: Pre-allocate buffers in state struct; avoid malloc/free in the hot decode loop
