#!/usr/bin/env fish

find tb2c/out -name '*.c' -exec rm {} ';'

cp tb2c/tests/test.c tb2c/out/out1.c

for i in (seq 8)
    set j (math $i + 1)
    bin/cuik -emit-c tb2c/out/out$i.c > tb2c/out/out$j.c
end

bin/cuik -emit-c tb2c/out/out$j.c -O2 > tb2c/out/out.c
