Baseline /tmp/titan-bench/bench-baseline.json vs new a8a22882cbea54ba74376dba7787a87a776aaf8c
threshold 3.0x on median, ignoring base medians < 1000 ns

| dataset | scenario | base median ns | new median ns | ratio | base p95 | new p95 | allocs/op (new) |
|---|---|---:|---:|---:|---:|---:|---:|
| code_10mb | byte_to_utf16_point | 701 | 711 | 1.01 | 821 | 822 | 0.00 |
| code_10mb | line_lookup | 1222 | 1162 | 0.95 | 1512 | 1452 | 1.04 |
| code_10mb | multi_cursor_10 | 5158 | 5107 | 0.99 | 6680 | 6570 | 15.05 |
| code_10mb | open | 5450249 | 4955747 | 0.91 | 6112914 | 5402868 | 11626.00 |
| code_10mb | random_delete | 1302 | 1242 | 0.95 | 1913 | 1492 | 5.01 |
| code_10mb | random_insert | 1312 | 1352 | 1.03 | 2824 | 2844 | 6.92 |
| code_10mb | redo | 1141 | 1152 | 1.01 | 1302 | 1332 | 4.00 |
| code_10mb | sequential_typing | 641 | 661 | 1.03 | 781 | 761 | 6.01 |
| code_10mb | snapshot | 30 | 30 | 1.00 | 40 | 30 | 0.00 |
| code_10mb | undo | 1282 | 1352 | 1.05 | 1532 | 1833 | 4.00 |
| code_10mb | utf16_to_byte | 481 | 461 | 0.96 | 731 | 711 | 0.00 |
| code_1mb | byte_to_utf16_point | 621 | 611 | 0.98 | 731 | 721 | 0.00 |
| code_1mb | line_lookup | 982 | 942 | 0.96 | 1162 | 1121 | 1.04 |
| code_1mb | multi_cursor_10 | 4768 | 4637 | 0.97 | 6519 | 6210 | 15.05 |
| code_1mb | open | 617877 | 545919 | 0.88 | 781593 | 717387 | 1163.00 |
| code_1mb | random_delete | 1142 | 1092 | 0.96 | 1782 | 1462 | 5.01 |
| code_1mb | random_insert | 1051 | 1052 | 1.00 | 2634 | 2604 | 6.46 |
| code_1mb | redo | 1021 | 1002 | 0.98 | 1212 | 1192 | 4.01 |
| code_1mb | sequential_typing | 781 | 741 | 0.95 | 1302 | 871 | 6.01 |
| code_1mb | snapshot | 30 | 30 | 1.00 | 40 | 30 | 0.00 |
| code_1mb | undo | 1132 | 1112 | 0.98 | 1342 | 1322 | 4.00 |
| code_1mb | utf16_to_byte | 351 | 340 | 0.97 | 420 | 401 | 0.00 |
| single_line_1mb | byte_to_utf16_point | 400 | 411 | 1.03 | 470 | 490 | 0.00 |
| single_line_1mb | line_lookup | 24777 | 24267 | 0.98 | 28803 | 28984 | 2.00 |
| single_line_1mb | multi_cursor_10 | 4697 | 4576 | 0.97 | 4887 | 4717 | 15.05 |
| single_line_1mb | open | 245769 | 220651 | 0.90 | 466600 | 423525 | 1163.00 |
| single_line_1mb | random_delete | 1122 | 1141 | 1.02 | 1332 | 1432 | 5.02 |
| single_line_1mb | random_insert | 1041 | 1052 | 1.01 | 1442 | 1452 | 6.46 |
| single_line_1mb | redo | 1022 | 1022 | 1.00 | 1202 | 1212 | 4.01 |
| single_line_1mb | sequential_typing | 732 | 741 | 1.01 | 851 | 802 | 6.01 |
| single_line_1mb | snapshot | 30 | 30 | 1.00 | 40 | 30 | 0.00 |
| single_line_1mb | undo | 1142 | 1122 | 0.98 | 1332 | 1312 | 4.00 |
| single_line_1mb | utf16_to_byte | 360 | 341 | 0.95 | 421 | 401 | 0.00 |
| unicode_mixed_1mb | byte_to_utf16_point | 651 | 651 | 1.00 | 771 | 771 | 0.00 |
| unicode_mixed_1mb | line_lookup | 1032 | 992 | 0.96 | 1282 | 1242 | 1.07 |
| unicode_mixed_1mb | multi_cursor_10 | 4897 | 4768 | 0.97 | 5028 | 4978 | 15.05 |
| unicode_mixed_1mb | open | 278938 | 261382 | 0.94 | 884998 | 429835 | 1164.00 |
| unicode_mixed_1mb | random_delete | 1102 | 1102 | 1.00 | 1332 | 1332 | 4.91 |
| unicode_mixed_1mb | random_insert | 1021 | 1052 | 1.03 | 1383 | 1432 | 6.34 |
| unicode_mixed_1mb | redo | 1042 | 1071 | 1.03 | 1242 | 1272 | 4.00 |
| unicode_mixed_1mb | sequential_typing | 721 | 751 | 1.04 | 811 | 821 | 6.01 |
| unicode_mixed_1mb | snapshot | 30 | 30 | 1.00 | 40 | 30 | 0.00 |
| unicode_mixed_1mb | undo | 1162 | 1191 | 1.02 | 1372 | 1573 | 4.00 |
| unicode_mixed_1mb | utf16_to_byte | 370 | 351 | 0.95 | 450 | 421 | 0.00 |

New scenarios (no baseline):

- code_rust_100kb / parse_full: median 10264124 ns, p95 10522071 ns, allocs/op 0.0
- code_rust_100kb / parse_incremental_after_edit: median 873531 ns, p95 1419931 ns, allocs/op 1.0
- code_rust_1mb / journal_append: median 1152 ns, p95 2054 ns, allocs/op 5.0
- code_rust_1mb / journal_append_fsync: median 254933 ns, p95 369084 ns, allocs/op 5.0
- code_rust_1mb / parse_full: median 105582319 ns, p95 130183980 ns, allocs/op 0.0
- code_rust_1mb / parse_incremental_after_edit: median 9633507 ns, p95 10767068 ns, allocs/op 1.0
- titand_stdio / rpc_health_roundtrip: median 36475 ns, p95 41142 ns, allocs/op 19.0
- titand_stdio_1mb / rpc_apply_edit_roundtrip: median 40531 ns, p95 45037 ns, allocs/op 39.0
- workspace_100x100kb / workspace_list_files: median 113941 ns, p95 119830 ns, allocs/op 557.0
- workspace_100x100kb / workspace_search: median 18891701 ns, p95 19173344 ns, allocs/op 859.0
