gcc main.c -O2 -o sysinfo_log4k.elf

systemctl enable --now task.timer
