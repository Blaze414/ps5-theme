#!/bin/sh
# Run every fuzz target for FUZZ_SECONDS (default 60). Crashing inputs are
# written to /out. Exit status: 0 if no target found anything.
seconds=${FUZZ_SECONDS:-60}
mkdir -p /out
status=0
for t in ${FUZZ_TARGETS:-zstd texture preview tex-encode at9 wav image json svg png png-pixels rco rco-data rco-icon font}; do
  mkdir -p /corpus/$t
  # the new targets start from their neighbours' seeds: textures, and whole interface files
  [ $t = preview ] && cp -n /corpus/texture/* /corpus/preview/ 2>/dev/null
  [ $t = rco-icon ] && cp -n /corpus/rco/* /corpus/rco-icon/ 2>/dev/null
  [ $t = tex-encode ] && cp -n /corpus/texture/* /corpus/tex-encode/ 2>/dev/null
  [ $t = png-pixels ] && cp -n /corpus/png/* /corpus/png-pixels/ 2>/dev/null
  FUZZ_TARGET=$t FUZZ_STOCK_DIR=/stock /fuzz /corpus/$t -max_total_time=$seconds -timeout=10 -rss_limit_mb=2048 \
      -artifact_prefix=/out/$t- -print_final_stats=1 > /out/$t.log 2>&1
  rc=$?
  runs=$(grep -m1 "stat::number_of_executed_units" /out/$t.log | awk '{print $2}')
  cov=$(grep -o "cov: [0-9]*" /out/$t.log | tail -1)
  if [ $rc -eq 0 ]; then
    echo "$t: no findings ($runs runs, $cov)"
  else
    echo "$t: FINDING (exit $rc)"
    grep -m1 -A12 "ERROR\|runtime error\|deadly signal" /out/$t.log
    status=1
  fi
done
exit $status
