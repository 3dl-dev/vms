@case LE.CTRLR
@title line editing: ^R redisplays the prompt and the line typed so far
# A video terminal, like OVMX's console: the lab VAX's OPA0: is a hardcopy LA36 (rd vms-eda8, operator ruling 2026-10-09).
V0 send "SET TERMINAL/NOHARDCOPY\r" expect="\$ $" quiet
T type "WRITE SYS$OUTPUT 1" gap=0.05
D send "^?"
X send "^R"
F type "2"
R send "\r" expect="\$ $"
