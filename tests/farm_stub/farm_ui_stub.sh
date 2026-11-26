#!/bin/sh
# A stand-in for `emfdtd_farm.py` for the window's tests: canned JSON, and a record of what it was asked.
# EMFDTD_FARM_STUB_LOG (optional) gets one line per call.
[ -n "$EMFDTD_FARM_STUB_LOG" ] && echo "$*" >> "$EMFDTD_FARM_STUB_LOG"
MORE=""
if [ -n "$EMFDTD_FARM_STUB_MANY" ]; then
  i=1
  while [ "$i" -le "$EMFDTD_FARM_STUB_MANY" ]; do
    MORE="$MORE,{\"job\":\"0000000$(printf %09d $i)abcdef0123456789abcd\",\"owner\":\"client:mac-main\",\"label\":\"old-$i\",\"state\":\"done\",\"attempts\":1,\"worker\":\"win-egpu\",\"device\":\"99cb\",\"progress\":\"\",\"percent\":null,\"started\":null,\"reason\":\"\"}"
    i=$((i + 1))
  done
fi
case "$1" in
status)
  echo '{"home":"/stub/farm","enabled":{"coordinator":true,"worker":false,"client":true}}' ;;
queue)
  now=$(date +%s)
  started=$((now - 372))
  cat <<JSON
{"devices":[{"worker":"win-egpu","uuid":"99cb","name":"NVIDIA GeForce RTX 4060 Ti","driver":"NVIDIA","mcells_per_s":1832.0,"state":"busy","ok":true},
            {"worker":"mac-studio","uuid":"190a","name":"Apple M1 Pro","driver":"KosmicKrisp","mcells_per_s":1200.0,"state":"idle","ok":true}],
 "jobs":[{"job":"9cf37054e5064a39b9298f6ec19fe720","owner":"client:mac-main","label":"nd_med_01-on-RTX","state":"running","attempts":1,"worker":"win-egpu","device":"99cb","progress":"progress: 41200 steps, about 37%, 312 s","percent":37,"started":$started,"reason":""},
         {"job":"aaaa0000bbbb1111cccc2222dddd3333","owner":"client:mac-main","label":"patch-farm-2","state":"queued","attempts":0,"worker":null,"device":null,"progress":"","percent":null,"started":null,"reason":""},
         {"job":"d0d0d0d01111222233334444aaaabbbb","owner":"client:mac-main","label":"patch-farm-1","state":"done","attempts":1,"worker":"win-egpu","device":"99cb","progress":"","percent":null,"started":null,"reason":""},
         {"job":"eeee4444ffff5555aaaa6666bbbb7777","owner":"client:mac-main","label":"old failed job","state":"failed","attempts":2,"worker":null,"device":null,"progress":"","percent":null,"started":null,"reason":"worker stopped answering; gave up after 2 attempts"}$MORE]}
JSON
  ;;
members)
  echo '{"members":[{"key":"worker:win-egpu","fingerprint":"b323457cf967618f0000","may":["run"],"revoked":false},{"key":"client:mac-main","fingerprint":"672f81215c9873050000","may":["cancel","results","status","submit"],"revoked":false}]}' ;;
enable|disable|cancel|revoke)
  echo "stub: $*" ;;
submit)
  echo '{"jobs":["bbbb1111cccc2222dddd3333eeee4444"]}' ;;
results)
  # results JOB --dest DIR
  mkdir -p "$4"
  printf '# Hz S RI R 50\n2.4e9 -0.3 0.1\n2.45e9 -0.4 0.2\n2.5e9 -0.5 0.2\n' > "$4/s11.s1p"
  printf 'kicad-em-fdtd run summary\nboard: farm stub\n' > "$4/summary.txt"
  echo "stub: results $2 into $4" ;;
invite)
  echo "invitation for $4 '$2', valid 10 minutes, one use."
  echo "  code:        ABCD-EFGH-JKLM"
  echo "  fingerprint: 1111-2222-3333-4444"
  echo "  listening on 192.0.2.1:7421 until it is used"
  sleep 30 ;;
*)
  echo "stub: unknown command $*" >&2; exit 2 ;;
esac
