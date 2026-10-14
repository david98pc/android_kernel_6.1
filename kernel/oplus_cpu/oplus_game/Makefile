# SPDX-License-Identifier: GPL-2.0-only
obj-y += game_main.o early_detect.o cpufreq_limits.o debug.o
# Keep this older frontend separate from game_opt, including exported helpers.
ccflags-y += -Dearly_detect_dir=cpu_game_early_detect_dir -Dcpufreq_limits_init=cpu_game_cpufreq_limits_init -Dearly_detect_init=cpu_game_early_detect_init -Dearly_detect_exit=cpu_game_early_detect_exit -Ddebug_init=cpu_game_debug_init -Dsystrace_c_printk=cpu_game_systrace_c_printk
