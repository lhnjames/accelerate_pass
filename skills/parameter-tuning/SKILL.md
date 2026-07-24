# LLVM parameter tuning

Select LLVM 21 compiler/pass parameters using only options proven available by
the supplied `opt-21 --help-hidden` discovery data. Connect each choice to a
specific remark, fired/no-op pass, bottleneck, or measured result. Do not use
removed legacy-pass-manager syntax or invent option names. Respect the exact
JSON schema and budget in the task.
