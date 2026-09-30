#!/bin/bash
cd /lab/run-eb3
env JOIN_WAIT_BEATS=200 SETTLE_S=60 bash /lab/run-eb3/matrix.sh /lab/run-eb3/art/fin2 L a-then-b:14:pkt:8109 together:10:pkt:8109 b-then-a:14:pkt:8109 a-then-b:6:pkt:8109 a-then-b:20:pkt:0a a-then-b:14:vax a-then-b:25:pkt:8109 together:14:pkt:8109 a-then-b:10:pkt:8109 b-then-a:6:pkt:0a a-then-b:18:pkt:8109 together:18:member a-then-b:14:pkt:0a b-then-a:20:pkt:8109 a-then-b:16:vax a-then-b:14:pkt:8109 together:6:member a-then-b:12:pkt:8109 b-then-a:22:pkt:8109 together:25:pkt:0a
env JOIN_WAIT_BEATS=200 SETTLE_S=300 bash /lab/run-eb3/matrix.sh /lab/run-eb3/art/fin2 K b-then-a:16:member b-then-a:16:member b-then-a:16:member
