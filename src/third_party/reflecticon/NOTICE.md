# From reflecticon-224

`analog/analog.hpp`, `analog/filters.hpp` and `analog/filters_224.hpp` are copied unchanged from
[reflecticon-224](https://github.com/joelanders/reflecticon-224) by Joe Landers (formerly reflexion-224), licensed under the
GNU Affero General Public License v3.0. They describe the Lexicon 224 / 224X analog input and output boards (filter chains reduced from
their schematics' SPICE netlists to modal form, the gain-ranging ADC). Reverbicon 224 uses their pole/residue data and converter
thresholds; `sbc/peripherals.hpp` and `sbc/memory_map.hpp` (the control head's 8255 and the SBC's memory map and ports) are
used as they are by `src/machine224.hpp`. `src/analog224.hpp` is a faster implementation of the same chains, checked against these files (test/analog224_compare.cpp).
