@case LE.CTRLR
@title line editing: ^R redisplays the prompt and the line typed so far
T type "WRITE SYS$OUTPUT 1" gap=0.05
D send "^?"
X send "^R"
F type "2"
R send "\r" expect="\$ $"
