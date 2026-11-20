# Phase 3 (47e312b) vs Phase 4 (aa8a990), same CI runner (run 42754408384)

Single run on one 2-vCPU runner. Ratios are warnings only; the build fails on the absolute latency budgets (benchmarks/budgets.json).

Baseline aa8a9901e2cf94672335a61997a2f130fb01f6b6 vs new aa8a9901e2cf94672335a61997a2f130fb01f6b6
threshold 1.5x on median, ignoring base medians < 1000 ns

| dataset | scenario | base median ns | new median ns | ratio | base p95 | new p95 | allocs/op (new) |
|---|---|---:|---:|---:|---:|---:|---:|
| bytes_10mb | hash_blake3 | 1630697 | 1700271 | 1.04 | 1657347 | 1746381 | 0.00 |
| bytes_10mb | hash_fnv1a | 11458976 | 11463563 | 1.00 | 11496523 | 11516382 | 0.00 |
| bytes_1mb | hash_blake3 | 159319 | 160250 | 1.01 | 169344 | 170196 | 0.00 |
| bytes_1mb | hash_fnv1a | 1144317 | 1144107 | 1.00 | 1159610 | 1154944 | 0.00 |
| cargo_ws_150x60kb | edit_during_lsp_indexing | 70435 | 70285 | 1.00 | 2486942 | 2050698 | 57.00 |
| cargo_ws_150x60kb | edit_with_lsp_active | 1766391 | 1990427 | 1.13 | 4021254 | 6217671 | 57.00 |
| cargo_ws_150x60kb | semantic_definition_query | 4425035 | 3575807 | 0.81 | 4919287 | 8198760 | 913.72 |
| code_10mb | byte_to_utf16_point | 781 | 781 | 1.00 | 921 | 921 | 0.00 |
| code_10mb | line_lookup | 1362 | 1322 | 0.97 | 1853 | 1653 | 1.04 |
| code_10mb | multi_cursor_10 | 7190 | 7240 | 1.01 | 9364 | 9334 | 15.05 |
| code_10mb | open | 6036039 | 6248968 | 1.04 | 7021889 | 8014688 | 11626.00 |
| code_10mb | random_delete | 1363 | 1382 | 1.01 | 1632 | 1683 | 5.01 |
| code_10mb | random_insert | 1572 | 1562 | 0.99 | 3455 | 3505 | 6.92 |
| code_10mb | redo | 1252 | 1262 | 1.01 | 1473 | 1462 | 4.00 |
| code_10mb | sequential_typing | 821 | 851 | 1.04 | 1072 | 991 | 6.01 |
| code_10mb | snapshot | 30 | 30 | 1.00 | 30 | 30 | 0.00 |
| code_10mb | undo | 1483 | 1432 | 0.97 | 2213 | 1683 | 4.00 |
| code_10mb | utf16_to_byte | 541 | 541 | 1.00 | 771 | 781 | 0.00 |
| code_1mb | byte_to_utf16_point | 761 | 731 | 0.96 | 1142 | 871 | 0.00 |
| code_1mb | line_lookup | 1112 | 1102 | 0.99 | 1352 | 1352 | 1.04 |
| code_1mb | multi_cursor_10 | 6650 | 6630 | 1.00 | 9014 | 9274 | 15.05 |
| code_1mb | open | 671247 | 678027 | 1.01 | 977176 | 966952 | 1163.00 |
| code_1mb | random_delete | 1281 | 1282 | 1.00 | 1883 | 1813 | 5.01 |
| code_1mb | random_insert | 1422 | 1242 | 0.87 | 3716 | 3345 | 6.46 |
| code_1mb | redo | 1181 | 1182 | 1.00 | 1382 | 1382 | 4.01 |
| code_1mb | sequential_typing | 951 | 951 | 1.00 | 1112 | 1132 | 6.01 |
| code_1mb | snapshot | 30 | 30 | 1.00 | 30 | 30 | 0.00 |
| code_1mb | undo | 1342 | 1292 | 0.96 | 2083 | 1512 | 4.00 |
| code_1mb | utf16_to_byte | 421 | 421 | 1.00 | 511 | 511 | 0.00 |
| code_rust_100kb | parse_full | 13141463 | 13388042 | 1.02 | 13798780 | 13791828 | 0.00 |
| code_rust_100kb | parse_incremental_after_edit | 1109505 | 1133370 | 1.02 | 1791779 | 1215915 | 1.00 |
| code_rust_1mb | document_save_safe | 8178298 | 8145003 | 1.00 | 10066392 | 10588280 | 9.00 |
| code_rust_1mb | journal_append | 1482 | 1483 | 1.00 | 2373 | 2384 | 5.00 |
| code_rust_1mb | journal_append_fsync | 194151 | 186314 | 0.96 | 630867 | 649955 | 5.00 |
| code_rust_1mb | parse_full | 137060653 | 137428033 | 1.00 | 165724022 | 162730415 | 0.00 |
| code_rust_1mb | parse_incremental_after_edit | 13597467 | 13736475 | 1.01 | 21084712 | 16493903 | 1.00 |
| single_file_write | search_after_external_change | 456205 | 451648 | 0.99 | 484868 | 489244 | 43.00 |
| single_file_write | watcher_event_latency | 50302436 | 50305798 | 1.00 | 50417723 | 50328984 | 25416.00 |
| single_line_1mb | byte_to_utf16_point | 481 | 480 | 1.00 | 571 | 561 | 0.00 |
| single_line_1mb | line_lookup | 32830 | 32730 | 1.00 | 42294 | 49905 | 2.00 |
| single_line_1mb | multi_cursor_10 | 6519 | 6570 | 1.01 | 6850 | 7080 | 15.05 |
| single_line_1mb | open | 295623 | 294111 | 0.99 | 595804 | 574983 | 1163.00 |
| single_line_1mb | random_delete | 1272 | 1272 | 1.00 | 1492 | 1503 | 5.02 |
| single_line_1mb | random_insert | 1232 | 1231 | 1.00 | 1723 | 1693 | 6.46 |
| single_line_1mb | redo | 1172 | 1181 | 1.01 | 1372 | 1392 | 4.01 |
| single_line_1mb | sequential_typing | 932 | 931 | 1.00 | 1042 | 1021 | 6.01 |
| single_line_1mb | snapshot | 30 | 30 | 1.00 | 30 | 30 | 0.00 |
| single_line_1mb | undo | 1282 | 1282 | 1.00 | 1472 | 1482 | 4.00 |
| single_line_1mb | utf16_to_byte | 421 | 421 | 1.00 | 511 | 510 | 0.00 |
| titand_stdio | rpc_health_roundtrip | 36224 | 36906 | 1.02 | 42133 | 45338 | 19.00 |
| titand_stdio_1mb | rpc_apply_edit_roundtrip | 46089 | 46550 | 1.01 | 53931 | 54562 | 39.00 |
| unicode_mixed_1mb | byte_to_utf16_point | 772 | 771 | 1.00 | 922 | 922 | 0.00 |
| unicode_mixed_1mb | line_lookup | 1182 | 1172 | 0.99 | 1513 | 1483 | 1.07 |
| unicode_mixed_1mb | multi_cursor_10 | 6911 | 6860 | 0.99 | 7140 | 7241 | 15.05 |
| unicode_mixed_1mb | open | 316495 | 331407 | 1.05 | 629936 | 608423 | 1164.00 |
| unicode_mixed_1mb | random_delete | 1282 | 1292 | 1.01 | 1552 | 1552 | 4.91 |
| unicode_mixed_1mb | random_insert | 1222 | 1262 | 1.03 | 1782 | 1923 | 6.34 |
| unicode_mixed_1mb | redo | 1211 | 1222 | 1.01 | 1432 | 1713 | 4.00 |
| unicode_mixed_1mb | sequential_typing | 952 | 1001 | 1.05 | 1532 | 1722 | 6.01 |
| unicode_mixed_1mb | snapshot | 30 | 30 | 1.00 | 30 | 30 | 0.00 |
| unicode_mixed_1mb | undo | 1312 | 1342 | 1.02 | 1562 | 1943 | 4.00 |
| unicode_mixed_1mb | utf16_to_byte | 441 | 441 | 1.00 | 550 | 551 | 0.00 |
| workspace_100x100kb | workspace_list_files | 135593 | 135082 | 1.00 | 145528 | 143014 | 557.00 |
| workspace_100x100kb | workspace_search | 28587662 | 25429103 | 0.89 | 29013430 | 25746308 | 859.00 |
| ws_150x60kb | cancel_search_latency | 968974 | 740311 | 0.76 | 3685170 | 2444168 | 23.00 |
| ws_150x60kb | edit_during_scan | 44116 | 45028 | 1.02 | 71497 | 66650 | 57.00 |
| ws_150x60kb | edit_during_search_storm | 163675 | 49154 | 0.30 | 1997027 | 995775 | 57.66 |
| ws_150x60kb | edit_idle | 73861 | 45909 | 0.62 | 1020251 | 77476 | 57.00 |
| ws_150x60kb | syntax_symbol_query | 3025761 | 2994855 | 0.99 | 3259422 | 3293913 | 4354.61 |

Latency budgets (fail the build when exceeded):

| dataset | scenario | metric | value ns | budget ns | result |
|---|---|---|---:|---:|---|
| titand_stdio_1mb | rpc_apply_edit_roundtrip | p99 | 63486 | 20000000 | ok |
| ws_150x60kb | edit_idle | p99 | 1485169 | 20000000 | ok |
| ws_150x60kb | edit_during_scan | p99 | 79990 | 50000000 | ok |
| ws_150x60kb | edit_during_search_storm | p99 | 2192781 | 50000000 | ok |
| ws_150x60kb | cancel_search_latency | p95 | 2444168 | 250000000 | ok |
| single_file_write | watcher_event_latency | median | 50305798 | 1000000000 | ok |
| titand_stdio | rpc_health_roundtrip | p99 | 50606 | 10000000 | ok |
| cargo_ws_150x60kb | edit_during_lsp_indexing | p99 | 5889769 | 100000000 | ok |
| cargo_ws_150x60kb | edit_with_lsp_active | p99 | 7879314 | 50000000 | ok |
| cargo_ws_150x60kb | semantic_definition_query | p95 | 8198760 | 2000000000 | ok |
