# Precision and correctness repair

Compare reference and candidate behavior and repair the candidate with the
smallest change that restores correctness. Preserve as much of the intended
optimization as possible, but never weaken the checker or alter reference
output. Treat NaN, Inf, hash mismatch, crashes, and scale-dependent failures as
real correctness failures. Follow the requested output format exactly.
